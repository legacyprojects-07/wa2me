#include "noise.h"
#include "compat.h"

#include "aes_gcm.h"
#include "x25519.h"

#include <cstring>
#include <stdexcept>

namespace wa {
namespace noise {

using crypto::Bytes;
using crypto::hkdfSha256;
using crypto::sha256;

const char *const NOISE_MODE = "Noise_XX_25519_AESGCM_SHA256\0\0\0\0";
const uint8_t NOISE_HEADER[4] = {87, 65, 6, 3};

namespace {

constexpr uint32_t kCertSerialExpected = 0;   // WA_CERT_DETAILS.SERIAL

Bytes concat(const Bytes &a, const Bytes &b) {
    Bytes out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

// Curve.sharedKey(privateKey, publicKey) with WhatsApp's 33-byte-prefixed
// public keys stripped to the raw 32-byte u-coordinate.
bool curveSharedKey(const Bytes &priv, const Bytes &pub, Bytes &out) {
    if (priv.size() != 32) return false;
    Bytes pubKey = pub;
    if (pubKey.size() == 33) pubKey.erase(pubKey.begin());   // strip KEY_BUNDLE_TYPE
    if (pubKey.size() != 32) return false;
    out.resize(32);
    return crypto::x25519(wa::compat::dataOf(priv), wa::compat::dataOf(pubKey), wa::compat::dataOf(out));
}

} // namespace

NoiseHandler::NoiseHandler(Bytes ephPriv, Bytes ephPub, Bytes routingInfo)
    : ephPriv_(compat::move(ephPriv)), ephPub_(compat::move(ephPub)),
      routingInfo_(compat::move(routingInfo)), readCounter_(0), writeCounter_(0),
      finished_(false), sentIntro_(false) {
    // Baileys: hash = NOISE_MODE.byteLength === 32 ? NOISE_MODE : sha256(NOISE_MODE).
    // "Noise_XX_25519_AESGCM_SHA256" is 28 chars + 4 NULs = exactly 32 bytes,
    // so the mode string is used directly as the initial hash.
    Bytes mode(NOISE_MODE, NOISE_MODE + 32);
    hash_ = mode.size() == 32 ? mode : sha256(mode);
    salt_ = hash_;
    encKey_ = hash_;
    decKey_ = hash_;

    authenticate(Bytes(NOISE_HEADER, NOISE_HEADER + 4));
    authenticate(ephPub_);
}

void NoiseHandler::authenticate(const Bytes &data) {
    if (!finished_) hash_ = sha256(concat(hash_, data));
}

Bytes NoiseHandler::generateIV(uint32_t counter) {
    Bytes iv(12, 0);
    iv[8] = uint8_t((counter >> 24) & 0xff);
    iv[9] = uint8_t((counter >> 16) & 0xff);
    iv[10] = uint8_t((counter >> 8) & 0xff);
    iv[11] = uint8_t(counter & 0xff);
    return iv;
}

void NoiseHandler::localHKDF(const Bytes &data, Bytes &writeKey, Bytes &readKey) {
    const Bytes key = hkdfSha256(data, 64, salt_, Bytes());
    writeKey.assign(key.begin(), key.begin() + 32);
    readKey.assign(key.begin() + 32, key.end());
}

void NoiseHandler::mixIntoKey(const Bytes &data) {
    Bytes writeKey, readKey;
    localHKDF(data, writeKey, readKey);
    salt_ = writeKey;
    encKey_ = readKey;
    decKey_ = readKey;
    readCounter_ = 0;
    writeCounter_ = 0;
}

void NoiseHandler::finishInit() {
    Bytes writeKey, readKey;
    localHKDF(Bytes(), writeKey, readKey);
    encKey_ = writeKey;
    decKey_ = readKey;
    hash_.clear();
    readCounter_ = 0;
    writeCounter_ = 0;
    finished_ = true;
}

Bytes NoiseHandler::encrypt(const Bytes &plaintext) {
    const Bytes iv = generateIV(writeCounter_);
    Bytes result = crypto::aes256GcmEncrypt(plaintext, encKey_, iv, hash_);
    writeCounter_++;
    authenticate(result);
    return result;
}

bool NoiseHandler::decrypt(const Bytes &ciphertext, Bytes &plaintext) {
    // Before the handshake completes the read and write counters coincide.
    const Bytes iv = generateIV(finished_ ? readCounter_ : writeCounter_);
    if (!crypto::aes256GcmDecrypt(ciphertext, decKey_, iv, hash_, plaintext)) return false;
    if (finished_) readCounter_++;
    else writeCounter_++;
    authenticate(ciphertext);
    return true;
}

Bytes NoiseHandler::processHandshake(const proto::ServerHello &hello,
                                    const Bytes &noisePriv, const Bytes &noisePub,
                                    uint64_t *certChainSerial,
                                    bool *certValid) {
    if (hello.ephemeral.empty()) throw std::runtime_error("ServerHello without ephemeral key");

    authenticate(hello.ephemeral);

    Bytes shared;
    if (!curveSharedKey(ephPriv_, hello.ephemeral, shared))
        throw std::runtime_error("failed to compute ephemeral shared key");
    mixIntoKey(shared);

    Bytes decStatic;
    if (!decrypt(hello.staticKey, decStatic))
        throw std::runtime_error("failed to decrypt server static key");
    if (!curveSharedKey(ephPriv_, decStatic, shared))
        throw std::runtime_error("failed to compute static shared key");
    mixIntoKey(shared);

    Bytes certDecoded;
    if (!decrypt(hello.payload, certDecoded))
        throw std::runtime_error("failed to decrypt server cert chain");

    // Decoding CertChain fully needs WAProto; parse just the issuer serial.
    uint64_t serial = 0;
    const bool parsed = proto::decodeCertChainIssuerSerial(certDecoded, serial);
    if (certChainSerial) *certChainSerial = serial;
    if (certValid) *certValid = parsed && serial == kCertSerialExpected;

    // The ClientFinish static key is the long-lived noise public key.
    Bytes keyEnc = encrypt(noisePub);

    if (!curveSharedKey(noisePriv, hello.ephemeral, shared))
        throw std::runtime_error("failed to recompute shared key");
    mixIntoKey(shared);

    return keyEnc;
}

Bytes NoiseHandler::encodeFrame(const Bytes &data) {
    Bytes payload = finished_ ? encrypt(data) : data;

    Bytes header;
    if (!routingInfo_.empty()) {
        // ED + flags + 3-byte routing-info length + routing info + WA header
        header.push_back('E');
        header.push_back('D');
        header.push_back(0);
        header.push_back(1);
        header.push_back(uint8_t((routingInfo_.size() >> 16) & 0xff));
        header.push_back(uint8_t((routingInfo_.size() >> 8) & 0xff));
        header.push_back(uint8_t(routingInfo_.size() & 0xff));
        header.insert(header.end(), routingInfo_.begin(), routingInfo_.end());
        header.insert(header.end(), NOISE_HEADER, NOISE_HEADER + 4);
    } else {
        header.assign(NOISE_HEADER, NOISE_HEADER + 4);
    }

    const bool intro = !sentIntro_;
    Bytes frame;
    if (intro) {
        frame = header;
        sentIntro_ = true;
    }
    frame.push_back(uint8_t((payload.size() >> 16) & 0xff));
    frame.push_back(uint8_t((payload.size() >> 8) & 0xff));
    frame.push_back(uint8_t(payload.size() & 0xff));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

void NoiseHandler::decodeFrames(const Bytes &newData,
                                const wa::function<void(const Bytes &)> &onFrame) {
    inBytes_.insert(inBytes_.end(), newData.begin(), newData.end());

    auto frameSize = [this]() -> size_t {
        if (inBytes_.size() < 3) return 0;
        return (size_t(inBytes_[0]) << 16) | (size_t(inBytes_[1]) << 8) | inBytes_[2];
    };

    size_t size = frameSize();
    while (size > 0 && inBytes_.size() >= size + 3) {
        Bytes frame(inBytes_.begin() + 3, inBytes_.begin() + 3 + size);
        inBytes_.erase(inBytes_.begin(), inBytes_.begin() + 3 + size);
        if (finished_) {
            Bytes plain;
            if (!decrypt(frame, plain)) {
                onFrame(Bytes());   // signal authentication failure
                return;
            }
            frame.swap(plain);
        }
        onFrame(frame);
        size = frameSize();
    }
}

} // namespace noise
} // namespace wa
