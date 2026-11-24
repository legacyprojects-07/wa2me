// Media upload/download test, checked against Baileys' own encryptedStream()
// and protobuf encoders (qt/tests/media_vectors.txt).
//
//   phase 1  test_media <vectors> <writeDir>
//              parses a <media_conn> reply, rebuilds the exact upload URL,
//              re-encrypts the same file with the same mediaKey and compares
//              ciphertext/hashes byte-for-byte, decrypts Baileys' blob, decodes
//              their ImageMessage/DocumentMessage, and writes one of our own
//              image message for the Node side to decode.
//   phase 2  node qt/tests/verify_media.mjs <vectors> <dir>
//              Baileys' proto.Message decodes ours; Baileys' download path
//              decrypts our blob; a document upload comes back our way.
//   phase 3  test_media <vectors> <dir> <dir>
//              decodes + decrypts what Baileys sent, proving the receive path
//              (including the MIME/extension mapping used by the UI).

#include "binary_node.h"
#include "media_crypto.h"
#include "media_upload.h"
#include "sha256.h"
#include "wa_proto.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace wa;
using Bytes = std::vector<uint8_t>;

static Bytes hex(const std::string &h) {
    Bytes out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

static int pass = 0, fail = 0;
static void check(bool ok, const char *what, const std::string &extra = "") {
    if (ok) { pass++; return; }
    fail++;
    std::fprintf(stderr, "FAIL %s %s\n", what, extra.c_str());
}

int main(int argc, char **argv) {
    const std::string vectorsPath = argc > 1 ? argv[1] : "qt/tests/media_vectors.txt";
    const std::string writeDir = argc > 2 ? argv[2] : "/tmp";
    const std::string readDir = argc > 3 ? argv[3] : std::string();

    std::ifstream in(vectorsPath);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", vectorsPath.c_str()); return 2; }

    Bytes plaintext, mediaKey, blob, fileSha256, fileEncSha256, imageBox, docBox;
    Bytes mediaConnFrame;
    std::string uploadPath;
    std::vector<std::pair<std::string, std::string>> downloads;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const size_t sp = line.find(' ');
        const std::string kind = line.substr(0, sp);
        const std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
        if (kind == "MEDIAFILE") plaintext = hex(rest);
        else if (kind == "MEDIAKEY") mediaKey = hex(rest);
        else if (kind == "MEDIABLOB") blob = hex(rest);
        else if (kind == "MEDIAPATH") uploadPath = rest;
        else if (kind == "MEDIACONN") mediaConnFrame = hex(rest);
        else if (kind == "IMAGEBOX") imageBox = hex(rest);
        else if (kind == "DOCBOX") docBox = hex(rest);
        else if (kind == "MEDIAHASH") {
            std::istringstream ss(rest);
            std::string a, b;
            ss >> a >> b;
            fileSha256 = hex(a);
            fileEncSha256 = hex(b);
        } else if (kind == "MEDIADOWNLOAD") {
            std::istringstream ss(rest);
            std::string url, directPath;
            ss >> url >> directPath;
            downloads.emplace_back(url, directPath);
        }
    }
    check(!plaintext.empty() && mediaKey.size() == 32 && !blob.empty(), "media vectors parsed");
    if (fail) return 1;

    if (readDir.empty()) {
        // ── Phase 1 ───────────────────────────────────────────────────────
        // The media_conn reply came out of Baileys' node encoder.
        const binary::DecodeResult decoded = binary::decode(mediaConnFrame);
        check(decoded.ok, "decode media_conn frame", decoded.error);
        media::MediaConn conn;
        std::string error;
        check(media::parseMediaConn(decoded.node, conn, error), "parse media_conn", error);
        check(conn.auth == "AUTH-TOKEN", "media auth token", conn.auth);
        check(conn.hostname == "mmg.whatsapp.net", "media host", conn.hostname);
        check(conn.ttlSeconds == 3600, "media ttl", std::to_string(conn.ttlSeconds));
        check(conn.usable(conn.fetchedAt + 10), "fresh media connection is usable");
        check(!conn.usable(conn.fetchedAt + 4000), "expired media connection is not");

        const BinaryNode request = media::buildMediaConnRequest("MEDIAIQ1");
        check(request.attrOr("xmlns") == "w:m", "media_conn request namespace");
        check(request.attrOr("type") == "set", "media_conn request type");
        check(request.child("media_conn") != nullptr, "media_conn request child");

        // Re-encrypt the same bytes with the same key: must be identical.
        media::UploadedMedia uploaded;
        check(media::prepareUpload(plaintext, media::Type::Image, mediaKey, 1700000000,
                                   uploaded, error),
              "prepare upload", error);
        check(uploaded.blob == blob, "encrypted blob matches Baileys",
              std::to_string(uploaded.blob.size()) + " vs " + std::to_string(blob.size()));
        check(uploaded.fileSha256 == fileSha256, "plaintext SHA-256 matches");
        check(uploaded.fileEncSha256 == fileEncSha256, "encrypted SHA-256 matches");
        check(uploaded.fileLength == plaintext.size(), "file length recorded");
        check(uploaded.mediaKeyTimestamp == 1700000000, "mediaKeyTimestamp recorded");

        // The CDN path component is base64url of the *encrypted* hash.
        check(media::base64Url(fileEncSha256) == uploadPath, "upload path matches Baileys",
              media::base64Url(fileEncSha256) + " vs " + uploadPath);
        check(media::base64Url(Bytes{0xfb, 0xef, 0xbe}) == "----",
              "base64url uses -_ and never pads",
              media::base64Url(Bytes{0xfb, 0xef, 0xbe}));
        check(media::mediaPath(media::Type::Image) == "/mms/image", "image path");
        check(media::mediaPath(media::Type::Document) == "/mms/document", "document path");
        check(media::mediaPath(media::Type::Audio) == "/mms/audio", "audio path");
        check(media::mediaPath(media::Type::Video) == "/mms/video", "video path");

        const std::string url = media::uploadUrl(conn, media::Type::Image,
                                                uploaded.fileEncSha256);
        const std::string expected = "https://mmg.whatsapp.net/mms/image/" + uploadPath +
                                     "?auth=AUTH-TOKEN&token=" + uploadPath;
        check(url == expected, "upload URL composition", url);

        // ── Download half ────────────────────────────────────────────────
        Bytes downloaded;
        check(media::decryptDownload(blob, mediaKey, media::Type::Image, downloaded, error),
              "decrypt download", error);
        check(downloaded == plaintext, "downloaded bytes match the original");

        Bytes tampered = blob;
        tampered.back() ^= 0xff;
        Bytes ignored;
        const bool tamperedOk =
            media::decryptDownload(tampered, mediaKey, media::Type::Image, ignored, error);
        check(!tamperedOk, "tampered blob is rejected", error);

        const bool wrongType =
            media::decryptDownload(blob, mediaKey, media::Type::Document, ignored, error);
        check(!wrongType, "wrong HKDF type cannot decrypt");

        // URL selection: only mmg.whatsapp.net URLs are trusted.
        if (downloads.size() == 2) {
            check(media::downloadUrl(downloads[0].first, downloads[0].second) ==
                      downloads[0].first,
                  "trusted CDN url is used as-is");
            check(media::downloadUrl(downloads[1].first, downloads[1].second) ==
                      "https://mmg.whatsapp.net" + downloads[1].second,
                  "untrusted url falls back to directPath",
                  media::downloadUrl(downloads[1].first, downloads[1].second));
        }

        // ── Messages produced by Baileys ─────────────────────────────────
        {
            waproto::Message msg;
            check(waproto::decodeMessage(imageBox, msg), "decode Baileys ImageMessage");
            check(msg.image.url == "https://mmg.whatsapp.net/mms/image/abc", "image url");
            check(msg.image.mimetype == "image/jpeg", "image mimetype");
            check(msg.image.caption == "vector caption", "image caption");
            check(msg.image.fileLength == 4096, "image file length");
            check(msg.image.height == 480 && msg.image.width == 640, "image dimensions");
            check(msg.image.mediaKey.size() == 32, "image media key");
            check(msg.image.fileEncSha256.size() == 32, "image enc hash");
            check(msg.image.directPath == "/mms/image/xyz", "image direct path");
            check(msg.image.mediaKeyTimestamp == 1700000000, "image media key timestamp");
            check(msg.mediaType() == "image", "image media type");
            check(media::suggestedFileName(msg, media::Type::Image) == "image.jpg",
                  "image file name");
        }
        {
            waproto::Message msg;
            check(waproto::decodeMessage(docBox, msg), "decode Baileys DocumentMessage");
            check(msg.document.title == "vector.pdf", "document title");
            check(msg.document.fileName == "vector.pdf", "document file name");
            check(msg.document.caption == "doc caption", "document caption");
            check(msg.document.pageCount == 2, "document page count");
            check(msg.document.directPath == "/mms/document/xyz", "document direct path");
            check(msg.document.mediaKeyTimestamp == 1700000001, "document key timestamp");
            check(msg.mediaType() == "document", "document media type");
            check(media::suggestedFileName(msg, media::Type::Document) == "vector.pdf",
                  "document file name for saving");
        }

        // ── Our own outgoing message ─────────────────────────────────────
        uploadPath.size();   // (already compared above)
        uploaded.directPath = "/mms/image/OURS";
        uploaded.url = "https://mmg.whatsapp.net/mms/image/OURS";
        media::MediaMeta meta;
        meta.mimetype = "image/jpeg";
        meta.caption = "sent from qt";
        meta.height = 120;
        meta.width = 160;
        waproto::Message outgoing;
        media::fillMediaMessage(media::Type::Image, uploaded, meta, outgoing);

        waproto::Message reparsed;
        check(waproto::decodeMessage(waproto::encodeMessage(outgoing), reparsed),
              "our message round-trips");
        check(reparsed.image.caption == "sent from qt", "caption survives");
        check(reparsed.image.fileLength == plaintext.size(), "length survives");
        check(reparsed.image.mediaKey == mediaKey, "media key survives");
        check(reparsed.image.mediaKeyTimestamp == 1700000000, "timestamp survives");
        check(reparsed.image.directPath == "/mms/image/OURS", "direct path survives");
        check(!reparsed.image.fileEncSha256.empty(), "enc hash survives");

        // Documents get a name/title and a page count.
        media::UploadedMedia docUpload;
        check(media::prepareUpload(plaintext, media::Type::Document, mediaKey, 1700000000,
                                   docUpload, error),
              "prepare document upload", error);
        media::MediaMeta docMeta;
        docMeta.mimetype = "application/pdf";
        docMeta.fileName = "notes.pdf";
        waproto::Message docMsg;
        media::fillMediaMessage(media::Type::Document, docUpload, docMeta, docMsg);
        check(docMsg.document.fileName == "notes.pdf", "document name set");
        check(docMsg.document.title == "notes.pdf", "document title defaults to the name");
        check(docMsg.document.pageCount == 1, "document page count set");
        check(docMsg.mediaType() == "document", "document media type set");

        // MIME/type mapping used by the attach dialog.
        check(media::mimeForFileName("photo.JPG") == "image/jpeg", "mime for .JPG");
        check(media::mimeForFileName("clip.mp4") == "video/mp4", "mime for .mp4");
        check(media::mimeForFileName("song.mp3") == "audio/mpeg", "mime for .mp3");
        check(media::mimeForFileName("note.pdf") == "application/pdf", "mime for .pdf");
        check(media::mimeForFileName("blob.xyz") == "application/octet-stream", "unknown mime");
        check(media::typeForMime("image/png", "") == media::Type::Image, "png is an image");
        check(media::typeForMime("video/3gpp", "") == media::Type::Video, "3gp is a video");
        check(media::typeForMime("application/octet-stream", "a.mp3") == media::Type::Audio,
              "extension decides when the mime is generic");
        check(media::typeForMime("application/octet-stream", "a.bin") == media::Type::Document,
              "unknown files are documents");

        std::ofstream out(writeDir + "/cpp_media.txt");
        out << "CPPMEDIAKEY " << wa::crypto::toHex(mediaKey) << "\n";
        out << "CPPMEDIABLOB " << wa::crypto::toHex(blob) << "\n";
        out << "CPPMEDIAPLAIN " << wa::crypto::toHex(plaintext) << "\n";
        out << "CPPMEDIAMSG " << wa::crypto::toHex(waproto::encodeMessage(outgoing)) << "\n";
        out << "CPPMEDIADOC " << wa::crypto::toHex(waproto::encodeMessage(docMsg)) << "\n";
        out.close();

        std::printf("\n=== Media (phase 1) ===\n");
        std::printf("%d passed, %d failed\n", pass, fail);
        std::printf("%s\n", fail == 0 ? "MEDIA BUILD OK" : "MEDIA BUILD FAILED");
        return fail == 0 ? 0 : 1;
    }

    // ── Phase 3: what Baileys sent back ─────────────────────────────────
    std::ifstream nodeFile(readDir + "/node_media.txt");
    check(static_cast<bool>(nodeFile), "node_media.txt exists");
    if (!nodeFile) return 1;

    Bytes nodeMediaKey, nodeBlob, nodePlain, nodeDocMsg, decodedCheck;
    while (std::getline(nodeFile, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind, a, b;
        ss >> kind;
        if (kind == "NODEMEDIAKEY") { ss >> a; nodeMediaKey = hex(a); }
        else if (kind == "NODEMEDIABLOB") { ss >> a; nodeBlob = hex(a); }
        else if (kind == "NODEMEDIAPLAIN") { ss >> a; nodePlain = hex(a); }
        else if (kind == "NODEDOC") { ss >> a; nodeDocMsg = hex(a); }
        else if (kind == "NODEDECRYPTED") { ss >> a; decodedCheck = hex(a); }
    }

    // Baileys decrypted the blob we produced from its own ciphertext.
    check(!decodedCheck.empty(), "Baileys decrypted our blob");
    check(decodedCheck == plaintext, "plaintext recovered by Baileys matches");

    // And we decrypt a document blob encrypted on their side.
    Bytes document;
    std::string error;
    check(media::decryptDownload(nodeBlob, nodeMediaKey, media::Type::Document, document, error),
          "decrypt a document uploaded by Baileys", error);
    check(document == nodePlain, "document bytes match");

    waproto::Message docMsg;
    check(waproto::decodeMessage(nodeDocMsg, docMsg), "decode their document message");
    check(docMsg.document.fileName == "reply.pdf", "document name from Baileys",
          docMsg.document.fileName);
    check(docMsg.document.mediaKey == nodeMediaKey, "document media key from Baileys");
    media::Type kind = media::Type::Document;
    waproto::MediaInfo info;
    check(media::mediaInfoOf(docMsg, kind, info), "media info located");
    check(kind == media::Type::Document, "media info is a document");
    check(info.fileLength == document.size(), "media info length matches");

    std::printf("\n=== Media (phase 3) ===\n");
    std::printf("%d passed, %d failed\n", pass, fail);
    std::printf("%s\n", fail == 0 ? "MEDIA INTEROP OK" : "MEDIA INTEROP FAILED");
    return fail == 0 ? 0 : 1;
}
