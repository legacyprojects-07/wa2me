#include "media_crypto.h"

#include "aes_gcm.h"
#include "sha256.h"

namespace wa {
namespace media {

namespace {

using wa::crypto::Bytes;

std::string mappingFor(Type type) {
    switch (type) {
    case Type::Image: return "Image";
    case Type::Video: return "Video";
    case Type::Audio: return "Audio";
    case Type::Document: return "Document";
    case Type::History: return "History";
    case Type::AppState: return "App State";
    case Type::ImageThumbnail: return "Image Thumbnail";
    case Type::VideoThumbnail: return "Video Thumbnail";
    case Type::DocumentThumbnail: return "Document Thumbnail";
    case Type::Sticker: return "Image";   // MEDIA_HKDF_KEY_MAPPING.sticker = Image
    case Type::LinkThumbnail: return "Link Thumbnail";
    case Type::PaymentBackground: return "Payment Background";
    }
    return "Image";
}

Bytes sliceOf(const Bytes &b, size_t from, size_t to) {
    if (from > b.size()) return Bytes();
    if (to > b.size()) to = b.size();
    return Bytes(b.begin() + from, b.begin() + to);
}

} // namespace

std::string hkdfInfo(Type type) { return "WhatsApp " + mappingFor(type) + " Keys"; }

bool typeFromName(const std::string &name, Type &out) {
    if (name == "image" || name == "product-catalog-image" || name == "product") {
        out = Type::Image;
    } else if (name == "video" || name == "gif" || name == "ptv") {
        out = Type::Video;
    } else if (name == "audio" || name == "ptt") {
        out = Type::Audio;
    } else if (name == "document") {
        out = Type::Document;
    } else if (name == "sticker") {
        out = Type::Sticker;
    } else if (name == "md-msg-hist") {
        out = Type::History;
    } else if (name == "md-app-state") {
        out = Type::AppState;
    } else if (name == "thumbnail-image") {
        out = Type::ImageThumbnail;
    } else if (name == "thumbnail-video") {
        out = Type::VideoThumbnail;
    } else if (name == "thumbnail-document") {
        out = Type::DocumentThumbnail;
    } else if (name == "thumbnail-link") {
        out = Type::LinkThumbnail;
    } else if (name == "payment-bg-image") {
        out = Type::PaymentBackground;
    } else {
        return false;
    }
    return true;
}

bool deriveKeys(const Bytes &mediaKey, Type type, Keys &out, std::string &error) {
    if (mediaKey.size() != 32) {
        error = "media key must be 32 bytes";
        return false;
    }
    // Note: keep the string alive — begin()/end() from two separate temporary
    // std::strings is undefined behaviour.
    const std::string infoStr = hkdfInfo(type);
    const Bytes info(infoStr.begin(), infoStr.end());
    const Bytes expanded = crypto::hkdfSha256(mediaKey, 112, Bytes(), info);
    if (expanded.size() != 112) {
        error = "hkdf expansion failed";
        return false;
    }
    out.iv = sliceOf(expanded, 0, 16);
    out.cipherKey = sliceOf(expanded, 16, 48);
    out.macKey = sliceOf(expanded, 48, 80);
    out.refKey = sliceOf(expanded, 80, 112);
    return true;
}

bool decryptBlob(const Bytes &blob, const Bytes &mediaKey, Type type,
                 Bytes &plaintext, std::string &error, bool verifyMac) {
    plaintext.clear();
    if (blob.size() < 10 + 16) {
        error = "media blob too short";
        return false;
    }
    Keys keys;
    if (!deriveKeys(mediaKey, type, keys, error)) return false;

    const Bytes ciphertext(blob.begin(), blob.end() - 10);
    const Bytes mac(blob.end() - 10, blob.end());

    if (verifyMac) {
        Bytes macInput = keys.iv;
        macInput.insert(macInput.end(), ciphertext.begin(), ciphertext.end());
        const Bytes full = crypto::hmacSha256(keys.macKey, macInput);
        if (full.size() < 10 ||
            !crypto::constantTimeEquals(Bytes(full.begin(), full.begin() + 10), mac)) {
            error = "media MAC mismatch";
            return false;
        }
    }

    Bytes raw;
    if (!crypto::aes256CbcDecryptRaw(ciphertext, keys.cipherKey, keys.iv, raw)) {
        error = "media AES-CBC decryption failed";
        return false;
    }
    // WhatsApp pads media with a 1..16 byte tail whose *contents* are random
    // (only the final byte carries the length), exactly like Baileys'
    // unpadRandomMax16 — so the padding bytes themselves cannot be validated.
    const uint8_t padLen = raw.back();
    if (padLen == 0 || padLen > 16 || padLen > raw.size()) {
        error = "media padding out of range";
        return false;
    }
    plaintext.assign(raw.begin(), raw.end() - padLen);
    return true;
}

bool encryptBlob(const Bytes &plaintext, const Bytes &mediaKey, Type type,
                 Bytes &blob, std::string &error) {
    blob.clear();
    Keys keys;
    if (!deriveKeys(mediaKey, type, keys, error)) return false;

    const Bytes ciphertext = crypto::aes256CbcEncrypt(plaintext, keys.cipherKey, keys.iv);
    Bytes macInput = keys.iv;
    macInput.insert(macInput.end(), ciphertext.begin(), ciphertext.end());
    const Bytes full = crypto::hmacSha256(keys.macKey, macInput);
    if (full.size() < 10) {
        error = "hmac failed";
        return false;
    }
    blob = ciphertext;
    blob.insert(blob.end(), full.begin(), full.begin() + 10);
    return true;
}

std::string urlFromDirectPath(const std::string &directPath) {
    if (directPath.empty()) return std::string();
    if (directPath.compare(0, 8, "https://") == 0) return directPath;
    return "https://mmg.whatsapp.net" + directPath;
}

} // namespace media
} // namespace wa
