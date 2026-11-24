// WhatsApp media upload/download — the CDN half of media messages.
//
// Sending a picture is three steps (see Baileys lib/Utils/messages-media.js and
// lib/Socket/messages-send.js):
//
//   1. ask the server where to upload:
//        <iq type=set xmlns="w:m" id=..><media_conn/></iq>
//      ->  <media_conn auth=… ttl=…><host hostname="mmg.whatsapp.net"/></media_conn>
//   2. encrypt the file with a fresh 32-byte mediaKey and POST the blob to
//        https://<hostname><path>/<b64url(sha256(ciphertext||mac))>?auth=…&token=…
//      ->  {"url": "…", "direct_path": "/mms/image/…"}
//   3. put url/directPath/mediaKey/fileSha256/fileEncSha256/fileLength into an
//      ImageMessage/DocumentMessage/… and send it as a normal (or group) message.
//
// Downloading reverses step 3 and 2: GET the message's url (or
// https://mmg.whatsapp.net + directPath) and decryptBlob() the result.
//
// Everything here is Qt-free so the test suite can check it against Node.

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "binary_node.h"
#include "media_crypto.h"
#include "wa_proto.h"

namespace wa {
namespace media {

using Bytes = std::vector<uint8_t>;

// <media_conn> reply, cached for ttl seconds like Baileys' mediaConn refresh.
struct MediaConn {
    std::string auth;
    std::string hostname;   // first <host hostname="…"/>
    int ttlSeconds;
    int64_t fetchedAt;  // unix seconds

    bool usable(int64_t now) const { return !hostname.empty() && now < fetchedAt + ttlSeconds; }
    MediaConn() : ttlSeconds(0), fetchedAt(0) {}
};

// <iq type=set xmlns="w:m" id=…><media_conn/></iq>
BinaryNode buildMediaConnRequest(const std::string &id);
bool parseMediaConn(const BinaryNode &node, MediaConn &out, std::string &error);

// base64url without padding (encodeBase64EncodedStringForUpload).
std::string base64Url(const Bytes &data);
// "/mms/image", "/mms/document", … (MEDIA_PATH_MAP).
std::string mediaPath(Type type);
// The exact upload URL from getWAUploadToServer.
std::string uploadUrl(const MediaConn &conn, Type type, const Bytes &fileEncSha256);
// https://mmg.whatsapp.net + directPath (getUrlFromDirectPath).
std::string downloadUrl(const std::string &url, const std::string &directPath);

// Result of encrypting a file for upload.
struct UploadedMedia {
    Bytes mediaKey;        // 32 bytes, random
    Bytes fileSha256;      // SHA-256 of the plaintext
    Bytes fileEncSha256;   // SHA-256 of ciphertext||mac (what the CDN is keyed by)
    Bytes blob;            // what gets POSTed
    size_t fileLength;
    std::string directPath;   // filled in from the upload response
    std::string url;          // filled in from the upload response
    int64_t mediaKeyTimestamp;
    UploadedMedia() : fileLength(0), mediaKeyTimestamp(0) {}
};

// Encrypt-only half of an upload (no network): derive keys, CBC-encrypt, hash.
bool prepareUpload(const Bytes &plaintext, Type type, const Bytes &mediaKey,
                   int64_t nowSeconds, UploadedMedia &out, std::string &error);

// Download half: verify the MAC and decrypt. `type` must match the sender's.
bool decryptDownload(const Bytes &blob, const Bytes &mediaKey, Type type,
                     Bytes &plaintext, std::string &error);

// Human-facing metadata for the outgoing message.
struct MediaMeta {
    std::string mimetype;
    std::string fileName;      // documents
    std::string caption;
    std::string title;         // documents (defaults to the file name)
    uint32_t seconds;
    bool ptt;
    uint32_t height;
    uint32_t width;
    MediaMeta() : seconds(0), ptt(false), height(0), width(0) {}
};

// Duration of an Ogg/Opus file in seconds, read from the last Ogg page's
// granule position (48 kHz). Voice notes need it; returns false when the file
// is not Ogg or has no readable page.
bool oggOpusDuration(const Bytes &file, int &seconds);

// Fills the right field of `msg` (image/video/audio/document/sticker).
void fillMediaMessage(Type type, const UploadedMedia &uploaded, const MediaMeta &meta,
                      waproto::Message &msg);

// Which media a message carries (empty when it is not a media message).
bool mediaInfoOf(const waproto::Message &msg, Type &typeOut, waproto::MediaInfo &infoOut);

// MIME guessing for the attach dialog / receive path.
std::string mimeForFileName(const std::string &fileName);
Type typeForMime(const std::string &mime, const std::string &fileName);

// A file name for a downloaded message ("document.pdf", "image.jpg", …).
std::string suggestedFileName(const waproto::Message &msg, Type type);

} // namespace media
} // namespace wa
