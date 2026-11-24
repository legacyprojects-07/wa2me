#include "aes_gcm.h"

#include <cstring>
#include <stdexcept>

namespace wa {
namespace crypto {
namespace {

// ── GF(2^8) helpers — used to derive the S-box at start-up ─────────────────

struct SBox {
    uint8_t enc[256];
    SBox() {
        uint8_t exp[512], log[256];
        uint8_t x = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = x;
            log[x] = uint8_t(i);
            // multiply by generator 3: x*3 = x*2 ^ x
            const uint8_t x2 = uint8_t((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
            x = uint8_t(x2 ^ x);
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];

        log[0] = 0;
        for (int i = 0; i < 256; i++) {
            // multiplicative inverse (0 maps to 0)
            uint8_t inv = (i == 0) ? 0 : exp[255 - log[i]];
            // affine transformation
            auto rotl8 = [](uint8_t v, int n) {
                return uint8_t((v << n) | (v >> (8 - n)));
            };
            const uint8_t s = uint8_t(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^
                                      rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63);
            enc[i] = s;
        }
    }
};

const SBox &sbox() {
    static const SBox box;
    return box;
}

inline uint8_t xtime(uint8_t v) { return uint8_t((v << 1) ^ ((v & 0x80) ? 0x1b : 0x00)); }

inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        const uint8_t hi = uint8_t(a & 0x80);
        a = uint8_t(a << 1);
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

void subBytes(uint8_t state[16]) {
    const SBox &s = sbox();
    for (int i = 0; i < 16; i++) state[i] = s.enc[state[i]];
}

void shiftRows(uint8_t s[16]) {
    // state is column-major: s[4*c + r]
    uint8_t t;
    // row 1 left by 1
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    // row 2 left by 2
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    // row 3 left by 3 (== right by 1)
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

void mixColumns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t *col = s + 4 * c;
        const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = uint8_t(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
        col[1] = uint8_t(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
        col[2] = uint8_t(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
        col[3] = uint8_t(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
    }
}

void invSubBytes(uint8_t state[16]) {
    const SBox &s = sbox();
    static uint8_t inverse[256];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 256; i++) inverse[s.enc[i]] = uint8_t(i);
        built = true;
    }
    for (int i = 0; i < 16; i++) state[i] = inverse[state[i]];
}

void invShiftRows(uint8_t s[16]) {
    uint8_t t;
    // row 1 right by 1
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    // row 2 right by 2
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    // row 3 right by 3
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

void invMixColumns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t *col = s + 4 * c;
        const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = uint8_t(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        col[1] = uint8_t(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        col[2] = uint8_t(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        col[3] = uint8_t(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
    }
}

void addRoundKey(uint8_t s[16], const uint8_t *rk) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

// ── GCM helpers ────────────────────────────────────────────────────────────

// Multiplies x by H in GF(2^128) (GCM's bit-reflected convention).
void ghashMul(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    std::memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        const int byteIdx = i / 8;
        const int bitIdx = 7 - (i % 8);
        if ((x[byteIdx] >> bitIdx) & 1) {
            for (int j = 0; j < 16; j++) z[j] ^= v[j];
        }
        const bool lsb = (v[15] & 1) != 0;
        for (int j = 15; j > 0; j--) v[j] = uint8_t((v[j] >> 1) | ((v[j - 1] & 1) << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    std::memcpy(x, z, 16);
}

void ghash(uint8_t out[16], const uint8_t h[16],
           const Bytes &aad, const Bytes &ciphertext) {
    uint8_t y[16] = {0};
    auto process = [&](const Bytes &data) {
        for (size_t off = 0; off < data.size(); off += 16) {
            const size_t n = std::min<size_t>(16, data.size() - off);
            uint8_t block[16] = {0};
            std::memcpy(block, data.data() + off, n);
            for (int i = 0; i < 16; i++) y[i] ^= block[i];
            ghashMul(y, h);
        }
    };
    process(aad);
    process(ciphertext);

    uint8_t lenBlock[16] = {0};
    const uint64_t aadBits = uint64_t(aad.size()) * 8;
    const uint64_t ctBits = uint64_t(ciphertext.size()) * 8;
    for (int i = 0; i < 8; i++) {
        lenBlock[7 - i] = uint8_t((aadBits >> (8 * i)) & 0xff);
        lenBlock[15 - i] = uint8_t((ctBits >> (8 * i)) & 0xff);
    }
    for (int i = 0; i < 16; i++) y[i] ^= lenBlock[i];
    ghashMul(y, h);
    std::memcpy(out, y, 16);
}

void inc32(uint8_t counter[16]) {
    for (int i = 15; i >= 12; i--) {
        if (++counter[i] != 0) break;
    }
}

Bytes ctrCrypt(const Bytes &input, const Aes256 &aes, const uint8_t j0[16]) {
    Bytes out(input.size());
    uint8_t counter[16];
    std::memcpy(counter, j0, 16);
    uint8_t keystream[16];
    for (size_t off = 0; off < input.size(); off += 16) {
        inc32(counter);
        aes.encryptBlock(counter, keystream);
        const size_t n = std::min<size_t>(16, input.size() - off);
        for (size_t i = 0; i < n; i++) out[off + i] = uint8_t(input[off + i] ^ keystream[i]);
    }
    return out;
}

} // namespace

// ── AES-256 ────────────────────────────────────────────────────────────────

Aes256::Aes256(const Bytes &key) {
    if (key.size() != 32) throw std::invalid_argument("AES-256 key must be 32 bytes");

    std::memcpy(roundKeys_, key.data(), 32);

    uint8_t rcon = 1;
    const int nk = 8;          // 256-bit key
    const int total = 4 * (14 + 1);
    const SBox &s = sbox();

    for (int i = nk; i < total; i++) {
        uint8_t temp[4];
        std::memcpy(temp, roundKeys_ + (i - 1) * 4, 4);
        if (i % nk == 0) {
            // RotWord + SubWord + Rcon
            const uint8_t t0 = temp[0];
            temp[0] = s.enc[temp[1]];
            temp[1] = s.enc[temp[2]];
            temp[2] = s.enc[temp[3]];
            temp[3] = s.enc[t0];
            temp[0] ^= rcon;
            rcon = xtime(rcon);
        } else if (i % nk == 4) {
            for (int j = 0; j < 4; j++) temp[j] = s.enc[temp[j]];
        }
        for (int j = 0; j < 4; j++) {
            roundKeys_[i * 4 + j] = uint8_t(roundKeys_[(i - nk) * 4 + j] ^ temp[j]);
        }
    }
}

void Aes256::encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t state[16];
    std::memcpy(state, in, 16);
    addRoundKey(state, roundKeys_);
    for (int round = 1; round <= 13; round++) {
        subBytes(state);
        shiftRows(state);
        mixColumns(state);
        addRoundKey(state, roundKeys_ + round * 16);
    }
    subBytes(state);
    shiftRows(state);
    addRoundKey(state, roundKeys_ + 14 * 16);
    std::memcpy(out, state, 16);
}

void Aes256::decryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t state[16];
    std::memcpy(state, in, 16);
    addRoundKey(state, roundKeys_ + 14 * 16);
    for (int round = 13; round >= 1; round--) {
        invShiftRows(state);
        invSubBytes(state);
        addRoundKey(state, roundKeys_ + round * 16);
        invMixColumns(state);
    }
    invShiftRows(state);
    invSubBytes(state);
    addRoundKey(state, roundKeys_);
    std::memcpy(out, state, 16);
}

// ── GCM ────────────────────────────────────────────────────────────────────

static void computeJ0(const Bytes &iv, const uint8_t h[16], uint8_t j0[16]) {
    if (iv.size() == 12) {
        std::memcpy(j0, iv.data(), 12);
        j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    } else {
        uint8_t y[16] = {0};
        Bytes data = iv;
        for (size_t off = 0; off < data.size(); off += 16) {
            const size_t n = std::min<size_t>(16, data.size() - off);
            uint8_t block[16] = {0};
            std::memcpy(block, data.data() + off, n);
            for (int i = 0; i < 16; i++) y[i] ^= block[i];
            ghashMul(y, h);
        }
        uint8_t lenBlock[16] = {0};
        const uint64_t ivBits = uint64_t(iv.size()) * 8;
        for (int i = 0; i < 8; i++) lenBlock[7 - i] = uint8_t((ivBits >> (8 * i)) & 0xff);
        for (int i = 0; i < 16; i++) y[i] ^= lenBlock[i];
        ghashMul(y, h);
        std::memcpy(j0, y, 16);
    }
}

Bytes aes256GcmEncrypt(const Bytes &plaintext, const Bytes &key,
                       const Bytes &iv, const Bytes &aad) {
    const Aes256 aes(key);
    uint8_t h[16] = {0};
    uint8_t zero[16] = {0};
    aes.encryptBlock(zero, h);

    uint8_t j0[16];
    computeJ0(iv, h, j0);

    Bytes ciphertext = ctrCrypt(plaintext, aes, j0);

    uint8_t ghashOut[16];
    ghash(ghashOut, h, aad, ciphertext);

    uint8_t tagMask[16];
    aes.encryptBlock(j0, tagMask);

    Bytes out = ciphertext;
    out.resize(ciphertext.size() + 16);
    for (int i = 0; i < 16; i++) out[ciphertext.size() + i] = uint8_t(ghashOut[i] ^ tagMask[i]);
    return out;
}

bool aes256GcmDecrypt(const Bytes &ciphertextAndTag, const Bytes &key,
                      const Bytes &iv, const Bytes &aad, Bytes &plaintext) {
    if (ciphertextAndTag.size() < 16) return false;
    const size_t ctLen = ciphertextAndTag.size() - 16;
    Bytes ciphertext(ciphertextAndTag.begin(), ciphertextAndTag.begin() + ctLen);

    const Aes256 aes(key);
    uint8_t h[16] = {0};
    uint8_t zero[16] = {0};
    aes.encryptBlock(zero, h);

    uint8_t j0[16];
    computeJ0(iv, h, j0);

    uint8_t ghashOut[16];
    ghash(ghashOut, h, aad, ciphertext);

    uint8_t tagMask[16];
    aes.encryptBlock(j0, tagMask);

    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) {
        diff |= uint8_t(ghashOut[i] ^ tagMask[i] ^ ciphertextAndTag[ctLen + i]);
    }
    if (diff != 0) return false;

    plaintext = ctrCrypt(ciphertext, aes, j0);
    return true;
}

// ── CBC / CTR ──────────────────────────────────────────────────────────────

Bytes aes256CbcEncrypt(const Bytes &plaintext, const Bytes &key, const Bytes &iv) {
    if (iv.size() != 16) throw std::invalid_argument("CBC IV must be 16 bytes");
    const Aes256 aes(key);

    const size_t padLen = 16 - (plaintext.size() % 16);
    Bytes data = plaintext;
    data.insert(data.end(), padLen, uint8_t(padLen));

    Bytes out(data.size());
    uint8_t prev[16];
    std::memcpy(prev, iv.data(), 16);
    for (size_t off = 0; off < data.size(); off += 16) {
        uint8_t block[16];
        for (int i = 0; i < 16; i++) block[i] = uint8_t(data[off + i] ^ prev[i]);
        aes.encryptBlock(block, out.data() + off);
        std::memcpy(prev, out.data() + off, 16);
    }
    return out;
}

bool aes256CbcDecrypt(const Bytes &ciphertext, const Bytes &key,
                      const Bytes &iv, Bytes &plaintext) {
    if (iv.size() != 16 || ciphertext.empty() || ciphertext.size() % 16 != 0) return false;
    const Aes256 aes(key);

    Bytes out(ciphertext.size());
    uint8_t prev[16];
    std::memcpy(prev, iv.data(), 16);
    for (size_t off = 0; off < ciphertext.size(); off += 16) {
        uint8_t dec[16];
        aes.decryptBlock(ciphertext.data() + off, dec);
        for (int i = 0; i < 16; i++) out[off + i] = uint8_t(dec[i] ^ prev[i]);
        std::memcpy(prev, ciphertext.data() + off, 16);
    }

    // Strip PKCS#7 padding (validated).
    const uint8_t padLen = out.back();
    if (padLen == 0 || padLen > 16 || padLen > out.size()) return false;
    for (size_t i = out.size() - padLen; i < out.size(); i++) {
        if (out[i] != padLen) return false;
    }
    out.resize(out.size() - padLen);
    plaintext = out;
    return true;
}

Bytes aes256Ctr(const Bytes &input, const Bytes &key, const Bytes &iv) {
    if (iv.size() != 16) throw std::invalid_argument("CTR IV must be 16 bytes");
    const Aes256 aes(key);
    Bytes out(input.size());
    uint8_t counter[16];
    std::memcpy(counter, iv.data(), 16);
    uint8_t keystream[16];
    for (size_t off = 0; off < input.size(); off += 16) {
        aes.encryptBlock(counter, keystream);
        for (int i = 15; i >= 0; i--) {
            if (++counter[i] != 0) break;
        }
        const size_t n = std::min<size_t>(16, input.size() - off);
        for (size_t i = 0; i < n; i++) out[off + i] = uint8_t(input[off + i] ^ keystream[i]);
    }
    return out;
}

} // namespace crypto
} // namespace wa
