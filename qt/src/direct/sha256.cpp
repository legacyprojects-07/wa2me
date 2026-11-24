#include "sha256.h"

#include "compat.h"

#include <cstring>
#include <stdexcept>

namespace wa {
namespace crypto {
namespace {

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        h_[0] = 0x6a09e667; h_[1] = 0xbb67ae85; h_[2] = 0x3c6ef372; h_[3] = 0xa54ff53a;
        h_[4] = 0x510e527f; h_[5] = 0x9b05688c; h_[6] = 0x1f83d9ab; h_[7] = 0x5be0cd19;
        len_ = 0;
        bufLen_ = 0;
    }

    void update(const uint8_t *data, size_t len) {
        len_ += len;
        while (len > 0) {
            const size_t take = std::min(size_t(64 - bufLen_), len);
            std::memcpy(buf_ + bufLen_, data, take);
            bufLen_ += take;
            data += take;
            len -= take;
            if (bufLen_ == 64) {
                compress(buf_);
                bufLen_ = 0;
            }
        }
    }

    Bytes final() {
        const uint64_t bits = len_ * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0x00;
        while (bufLen_ != 56) update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; i++) lenBytes[i] = uint8_t((bits >> (56 - 8 * i)) & 0xff);
        const uint64_t savedLen = len_;
        update(lenBytes, 8);
        len_ = savedLen;

        Bytes out(32);
        for (int i = 0; i < 8; i++) {
            out[4 * i]     = uint8_t((h_[i] >> 24) & 0xff);
            out[4 * i + 1] = uint8_t((h_[i] >> 16) & 0xff);
            out[4 * i + 2] = uint8_t((h_[i] >> 8) & 0xff);
            out[4 * i + 3] = uint8_t(h_[i] & 0xff);
        }
        return out;
    }

private:
    void compress(const uint8_t *block) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(block[4 * i]) << 24) | (uint32_t(block[4 * i + 1]) << 16) |
                   (uint32_t(block[4 * i + 2]) << 8) | uint32_t(block[4 * i + 3]);
        }
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }

    uint32_t h_[8];
    uint64_t len_;
    uint8_t buf_[64];
    size_t bufLen_;
};

} // namespace

Bytes sha256(const uint8_t *data, size_t len) {
    Sha256 s;
    s.update(data, len);
    return s.final();
}

Bytes sha256(const Bytes &data) { return sha256(wa::compat::dataOf(data), data.size()); }

Bytes sha256(const std::string &data) {
    return sha256(reinterpret_cast<const uint8_t *>(wa::compat::dataOf(data)), data.size());
}

// ── SHA-1 (RFC 3174) ──────────────────────────────────────────────────────
//
// Only used to check Sec-WebSocket-Accept during the RFC 6455 handshake.
Bytes sha1(const Bytes &data) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    Bytes msg = data;
    const uint64_t bitLen = uint64_t(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; i--) msg.push_back(uint8_t((bitLen >> (i * 8)) & 0xff));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(msg[chunk + i * 4]) << 24) |
                   (uint32_t(msg[chunk + i * 4 + 1]) << 16) |
                   (uint32_t(msg[chunk + i * 4 + 2]) << 8) | uint32_t(msg[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) {
            const uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    Bytes out;
    out.reserve(20);
    for (int i = 0; i < 5; i++) {
        out.push_back(uint8_t(h[i] >> 24));
        out.push_back(uint8_t(h[i] >> 16));
        out.push_back(uint8_t(h[i] >> 8));
        out.push_back(uint8_t(h[i]));
    }
    return out;
}

Bytes sha1(const std::string &data) {
    return sha1(Bytes(data.begin(), data.end()));
}

Bytes hmacSha256(const Bytes &key, const Bytes &data) {
    Bytes k = key;
    if (k.size() > 64) k = sha256(k);
    k.resize(64, 0);

    Bytes inner(64), outer(64);
    for (int i = 0; i < 64; i++) {
        inner[i] = uint8_t(k[i] ^ 0x36);
        outer[i] = uint8_t(k[i] ^ 0x5c);
    }
    inner.insert(inner.end(), data.begin(), data.end());
    Bytes innerHash = sha256(inner);
    outer.insert(outer.end(), innerHash.begin(), innerHash.end());
    return sha256(outer);
}

Bytes hkdfSha256(const Bytes &ikm, size_t length, const Bytes &salt, const Bytes &info) {
    // Extract
    Bytes prk = hmacSha256(salt.empty() ? Bytes(32, 0) : salt, ikm);
    // Expand
    Bytes out;
    Bytes t;
    uint8_t counter = 1;
    while (out.size() < length) {
        Bytes input = t;
        input.insert(input.end(), info.begin(), info.end());
        input.push_back(counter++);
        t = hmacSha256(prk, input);
        out.insert(out.end(), t.begin(), t.end());
        if (counter == 0 && out.size() < length) break;   // 255 blocks max
    }
    out.resize(length);
    return out;
}

bool constantTimeEquals(const Bytes &a, const Bytes &b) {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= uint8_t(a[i] ^ b[i]);
    return diff == 0;
}

Bytes randomBytes(size_t n) {
    // Seed material: /dev/urandom where the platform has one, plus
    // clock/address/counter entropy, mixed through SHA-256 and used in counter
    // mode. std::random_device is C++11 and missing from Symbian's STLport 5,
    // so the file is read directly (the desktop and the interop tests get real
    // OS randomness from it; Symbian falls back to the timing entropy).
    Bytes seed;
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        uint8_t buf[32];
        const size_t got = fread(buf, 1, sizeof buf, urandom);
        seed.insert(seed.end(), buf, buf + got);
        fclose(urandom);
    }
    // A process-wide counter: two calls inside the same clock tick must still
    // produce different seeds.
    static uint32_t callCounter = 0;
    callCounter++;
    seed.insert(seed.end(), reinterpret_cast<const uint8_t *>(&callCounter),
                reinterpret_cast<const uint8_t *>(&callCounter) + sizeof(callCounter));
    // <chrono> is missing on older toolchains (GCC 4.6 on Symbian); the clock
    // and the address below are mixed into the hash, so the quality is the same.
    const unsigned long long ticks = compat::clockTicks();
    seed.insert(seed.end(), reinterpret_cast<const uint8_t *>(&ticks),
                reinterpret_cast<const uint8_t *>(&ticks) + sizeof(ticks));
    static volatile uintptr_t addressEntropy = 0;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(&addressEntropy);
    seed.insert(seed.end(), reinterpret_cast<const uint8_t *>(&addr),
                reinterpret_cast<const uint8_t *>(&addr) + sizeof(addr));

    Bytes out;
    Bytes block = sha256(seed);
    uint32_t counter = 0;
    while (out.size() < n) {
        Bytes input = block;
        input.insert(input.end(), reinterpret_cast<const uint8_t *>(&counter),
                     reinterpret_cast<const uint8_t *>(&counter) + 4);
        Bytes digest = sha256(input);
        out.insert(out.end(), digest.begin(), digest.end());
        counter++;
    }
    out.resize(n);
    return out;
}

std::string toHex(const Bytes &b) {
    static const char *digits = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) { s += digits[c >> 4]; s += digits[c & 0x0f]; }
    return s;
}

Bytes fromHex(const std::string &hex) {
    Bytes out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = compat::hexNibble(hex[i]);
        const int lo = compat::hexNibble(hex[i + 1]);
        out.push_back(uint8_t(((hi < 0 ? 0 : hi) << 4) | (lo < 0 ? 0 : lo)));
    }
    return out;
}

} // namespace crypto
} // namespace wa
