// WhatsApp media encryption (media_crypto) — dependency-free C++17.
//
// Every WhatsApp media object (image/audio/document *and* the HistorySync
// blobs and app-state patches) is encrypted with the same scheme, described
// in Baileys' lib/Utils/messages-media.js:
//
//   keys   = HKDF-SHA256(mediaKey, 112, info = "WhatsApp <Type> Keys")
//            -> iv(16) || cipherKey(32) || macKey(32) || refKey(32)
//   blob   = AES-256-CBC(cipherKey, iv, PKCS#7(plaintext))
//            || HMAC-SHA256(macKey, iv || ciphertext)[0..10]
//
// This layer makes history download and (later) media download possible
// without libsodium/OpenSSL. Verified against Node's crypto in
// qt/tests/test_history.cpp.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wa {
namespace media {

using Bytes = std::vector<uint8_t>;

// Types as spelled in MEDIA_HKDF_KEY_MAPPING (Defaults/index.js).
enum class Type {
    Image,
    Video,
    Audio,
    Document,
    History,
    AppState,
    ImageThumbnail,
    VideoThumbnail,
    DocumentThumbnail,
    LinkThumbnail,
    PaymentBackground,
};

// HKDF info string: "WhatsApp <Mapping> Keys" ("WhatsApp  Keys" for ppic).
std::string hkdfInfo(Type type);

// Maps a Baileys media key name ("image", "md-msg-hist", "thumbnail-image"…).
bool typeFromName(const std::string &name, Type &out);

struct Keys {
    Bytes iv;          // 16
    Bytes cipherKey;   // 32
    Bytes macKey;      // 32
    Bytes refKey;      // 32
};

// Expands the 32-byte mediaKey into the four sub-keys.
bool deriveKeys(const Bytes &mediaKey, Type type, Keys &out, std::string &error);

// Decrypts and MAC-verifies a WhatsApp media blob.
bool decryptBlob(const Bytes &blob, const Bytes &mediaKey, Type type,
                 Bytes &plaintext, std::string &error, bool verifyMac = true);

// Encrypts (used by tests and by media upload, which is otherwise out of
// scope). Returns the blob in WhatsApp's ciphertext || 10-byte MAC layout.
bool encryptBlob(const Bytes &plaintext, const Bytes &mediaKey, Type type,
                 Bytes &blob, std::string &error);

// Builds the CDN URL for a message's directPath ("https://mmg.whatsapp.net").
std::string urlFromDirectPath(const std::string &directPath);

} // namespace media
} // namespace wa
