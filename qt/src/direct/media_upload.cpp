#include "media_upload.h"

#include <algorithm>
#include <cctype>

#include "sha256.h"

namespace wa {
namespace media {

namespace {

using wa::crypto::Bytes;
using wa::crypto::sha256;

std::string lower(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::string extensionOf(const std::string &name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return std::string();
    return lower(name.substr(dot + 1));
}

} // namespace

// ── Media connection ─────────────────────────────────────────────────────

// Same shape Baileys sends (lib/Socket/messages-send.js refreshMediaConn):
// <iq type=set xmlns=w:m to=s.whatsapp.net id=…><media_conn/></iq>
BinaryNode buildMediaConnRequest(const std::string &id) {
    BinaryNode iq("iq");
    iq.setAttr("id", id);
    iq.setAttr("type", "set");
    iq.setAttr("xmlns", "w:m");
    iq.addChild(BinaryNode("media_conn"));
    return iq;
}

bool parseMediaConn(const BinaryNode &node, MediaConn &out, std::string &error) {
    out = MediaConn();
    if (const BinaryNode *err = node.child("error")) {
        error = "media_conn failed: " + err->attrOr("text", err->attrOr("code", "?"));
        return false;
    }
    const BinaryNode *conn = node.child("media_conn");
    if (!conn) {
        error = "media_conn response has no <media_conn>";
        return false;
    }
    out.auth = conn->attrOr("auth");
    out.ttlSeconds = std::atoi(conn->attrOr("ttl", "0").c_str());
    for (const BinaryNode *host : conn->childrenNamed("host")) {
        out.hostname = host->attrOr("hostname");
        if (!out.hostname.empty()) break;
    }
    if (out.auth.empty() || out.hostname.empty()) {
        error = "media_conn response is missing auth or host";
        return false;
    }
    return true;
}

// ── URLs ─────────────────────────────────────────────────────────────────

std::string base64Url(const Bytes &data) {
    static const char *kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((data.size() * 4 + 2) / 3);
    size_t i = 0;
    while (i + 2 < data.size()) {
        const uint32_t triple = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) |
                                uint32_t(data[i + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3f]);
        out.push_back(kAlphabet[triple & 0x3f]);
        i += 3;
    }
    if (i + 1 == data.size()) {
        const uint32_t triple = uint32_t(data[i]) << 16;
        out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
    } else if (i + 2 == data.size()) {
        const uint32_t triple = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3f]);
    }
    return out;
}

std::string mediaPath(Type type) {
    switch (type) {
    case Type::Image:
    case Type::ImageThumbnail:
    case Type::LinkThumbnail:
        return "/mms/image";
    case Type::Video:
    case Type::VideoThumbnail:
        return "/mms/video";
    case Type::Audio:
        return "/mms/audio";
    case Type::Document:
    case Type::DocumentThumbnail:
        return "/mms/document";
    case Type::Sticker:
        return "/mms/image";   // MEDIA_PATH_MAP.sticker = /mms/image
    case Type::History:
        return "/mms/md-app-state";
    case Type::AppState:
        return std::string();   // "" in MEDIA_PATH_MAP
    case Type::PaymentBackground:
        return "/mms/image";
    }
    return "/mms/image";
}

std::string uploadUrl(const MediaConn &conn, Type type, const Bytes &fileEncSha256) {
    const std::string encoded = base64Url(fileEncSha256);
    return "https://" + conn.hostname + mediaPath(type) + "/" + encoded + "?auth=" +
           conn.auth + "&token=" + encoded;
}

std::string downloadUrl(const std::string &url, const std::string &directPath) {
    if (url.rfind("https://mmg.whatsapp.net/", 0) == 0) return url;
    return urlFromDirectPath(directPath);
}

// ── Upload preparation ───────────────────────────────────────────────────

bool prepareUpload(const Bytes &plaintext, Type type, const Bytes &mediaKey,
                   int64_t nowSeconds, UploadedMedia &out, std::string &error) {
    out = UploadedMedia();
    if (mediaKey.size() != 32) {
        error = "media key must be 32 bytes";
        return false;
    }
    if (plaintext.empty()) {
        error = "refusing to upload an empty file";
        return false;
    }

    Bytes blob;
    if (!encryptBlob(plaintext, mediaKey, type, blob, error)) return false;

    out.mediaKey = mediaKey;
    out.blob = blob;
    out.fileLength = plaintext.size();
    out.fileSha256 = sha256(plaintext);
    // The CDN is keyed by the hash of ciphertext||MAC, not of the plaintext.
    out.fileEncSha256 = sha256(blob);
    out.mediaKeyTimestamp = nowSeconds;
    return true;
}

bool decryptDownload(const Bytes &blob, const Bytes &mediaKey, Type type, Bytes &plaintext,
                     std::string &error) {
    plaintext.clear();
    if (mediaKey.size() != 32) {
        error = "media key must be 32 bytes";
        return false;
    }
    return decryptBlob(blob, mediaKey, type, plaintext, error);
}

// ── Message assembly ─────────────────────────────────────────────────────

namespace {

void copyCommon(const UploadedMedia &u, waproto::MediaInfo &info) {
    info.mediaKey = u.mediaKey;
    info.fileSha256 = u.fileSha256;
    info.fileEncSha256 = u.fileEncSha256;
    info.fileLength = u.fileLength;
    info.directPath = u.directPath;
    info.url = u.url;
    info.mediaKeyTimestamp = u.mediaKeyTimestamp;
}

} // namespace

bool oggOpusDuration(const Bytes &file, int &seconds) {
    seconds = 0;
    if (file.size() < 28) return false;
    size_t pos = 0;
    uint64_t granule = 0;
    bool sawPage = false;
    const uint64_t kNoGranule = ~uint64_t(0);
    while (pos + 27 <= file.size()) {
        if (!(file[pos] == 'O' && file[pos + 1] == 'g' && file[pos + 2] == 'g' &&
              file[pos + 3] == 'S'))
            break;
        uint64_t pageGranule = 0;
        for (int i = 7; i >= 0; --i) pageGranule = (pageGranule << 8) | file[pos + 6 + i];
        const size_t segments = file[pos + 26];
        const size_t tableEnd = pos + 27 + segments;
        if (tableEnd > file.size()) break;
        size_t payload = 0;
        for (size_t i = 0; i < segments; ++i) payload += file[pos + 27 + i];
        if (tableEnd + payload > file.size()) break;
        if (pageGranule != kNoGranule) granule = pageGranule;
        sawPage = true;
        pos = tableEnd + payload;
    }
    if (!sawPage) return false;
    // Opus granule positions count 48 kHz samples.
    seconds = int((granule + 47999) / 48000);
    return true;
}

void fillMediaMessage(Type type, const UploadedMedia &uploaded, const MediaMeta &meta,
                      waproto::Message &msg) {
    switch (type) {
    case Type::Image:
    case Type::ImageThumbnail:
    case Type::LinkThumbnail: {
        waproto::MediaInfo &info = msg.image;
        copyCommon(uploaded, info);
        info.mimetype = meta.mimetype.empty() ? "image/jpeg" : meta.mimetype;
        info.caption = meta.caption;
        info.height = meta.height;
        info.width = meta.width;
        break;
    }
    case Type::Video:
    case Type::VideoThumbnail: {
        waproto::MediaInfo &info = msg.video;
        copyCommon(uploaded, info);
        info.mimetype = meta.mimetype.empty() ? "video/mp4" : meta.mimetype;
        info.caption = meta.caption;
        info.seconds = meta.seconds;
        info.height = meta.height;
        info.width = meta.width;
        break;
    }
    case Type::Audio: {
        waproto::MediaInfo &info = msg.audio;
        copyCommon(uploaded, info);
        info.mimetype = meta.mimetype.empty() ? "audio/ogg; codecs=opus" : meta.mimetype;
        info.seconds = meta.seconds;
        info.ptt = meta.ptt;
        break;
    }
    case Type::Document:
    case Type::DocumentThumbnail: {
        waproto::MediaInfo &info = msg.document;
        copyCommon(uploaded, info);
        info.mimetype = meta.mimetype.empty() ? "application/octet-stream" : meta.mimetype;
        info.fileName = meta.fileName;
        info.title = meta.title.empty() ? meta.fileName : meta.title;
        info.caption = meta.caption;
        info.pageCount = 1;
        break;
    }
    case Type::Sticker: {
        waproto::MediaInfo &info = msg.sticker;
        copyCommon(uploaded, info);
        info.mimetype = meta.mimetype.empty() ? "image/webp" : meta.mimetype;
        info.height = meta.height;
        info.width = meta.width;
        break;
    }
    case Type::PaymentBackground:
    case Type::History:
    case Type::AppState: {
        waproto::MediaInfo &info = msg.document;
        copyCommon(uploaded, info);
        info.mimetype = meta.mimetype.empty() ? "application/octet-stream" : meta.mimetype;
        info.fileName = meta.fileName;
        break;
    }
    }
}

bool mediaInfoOf(const waproto::Message &msg, Type &typeOut, waproto::MediaInfo &infoOut) {
    if (!msg.image.empty()) { typeOut = Type::Image; infoOut = msg.image; return true; }
    if (!msg.video.empty()) { typeOut = Type::Video; infoOut = msg.video; return true; }
    if (!msg.audio.empty()) { typeOut = Type::Audio; infoOut = msg.audio; return true; }
    if (!msg.document.empty()) { typeOut = Type::Document; infoOut = msg.document; return true; }
    if (!msg.sticker.empty()) { typeOut = Type::Sticker; infoOut = msg.sticker; return true; }
    return false;
}

// ── Types, MIME, file names ──────────────────────────────────────────────

std::string mimeForFileName(const std::string &fileName) {
    const std::string ext = extensionOf(fileName);
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "mp4" || ext == "m4v") return "video/mp4";
    if (ext == "3gp") return "video/3gpp";
    if (ext == "mov") return "video/quicktime";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "m4a") return "audio/mp4";
    if (ext == "aac") return "audio/aac";
    if (ext == "ogg" || ext == "opus") return "audio/ogg; codecs=opus";
    if (ext == "wav") return "audio/wav";
    if (ext == "pdf") return "application/pdf";
    if (ext == "txt") return "text/plain";
    if (ext == "zip") return "application/zip";
    if (ext == "doc") return "application/msword";
    if (ext == "docx")
        return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (ext == "xls") return "application/vnd.ms-excel";
    if (ext == "xlsx")
        return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    return "application/octet-stream";
}

Type typeForMime(const std::string &mime, const std::string &fileName) {
    const std::string m = lower(mime);
    // WhatsApp stickers are WebP; the extension decides, since .webp photos
    // and stickers share the MIME type and the UI sends both.
    if (extensionOf(fileName) == "webp") return Type::Sticker;
    if (m.rfind("image/", 0) == 0) return Type::Image;
    if (m.rfind("video/", 0) == 0) return Type::Video;
    if (m.rfind("audio/", 0) == 0) return Type::Audio;
    if (!m.empty() && m != "application/octet-stream") return Type::Document;

    // Fall back to the extension when the MIME type is generic.
    const std::string ext = extensionOf(fileName);
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp")
        return Type::Image;
    if (ext == "mp4" || ext == "m4v" || ext == "3gp" || ext == "mov") return Type::Video;
    if (ext == "mp3" || ext == "m4a" || ext == "aac" || ext == "ogg" || ext == "opus" ||
        ext == "wav")
        return Type::Audio;
    return Type::Document;
}

std::string suggestedFileName(const waproto::Message &msg, Type type) {
    switch (type) {
    case Type::Image:
    case Type::ImageThumbnail:
    case Type::LinkThumbnail:
        return "image.jpg";
    case Type::Video:
    case Type::VideoThumbnail:
        return "video.mp4";
    case Type::Audio:
        return msg.audio.ptt ? "voice-note.ogg" : "audio.ogg";
    case Type::Sticker:
        return "sticker.webp";
    case Type::Document:
    case Type::DocumentThumbnail:
        return msg.document.fileName.empty() ? std::string("document") : msg.document.fileName;
    case Type::History:
        return "history.json";
    case Type::AppState:
        return "appstate.bin";
    case Type::PaymentBackground:
        return "background.jpg";
    }
    return "file";
}

} // namespace media
} // namespace wa
