#include "ed25519.h"

#include <cstring>

namespace wa {
namespace crypto {
namespace {

// ── SHA-512 (FIPS 180-4) ───────────────────────────────────────────────────

const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

class Sha512 {
public:
    Sha512() { reset(); }

    void reset() {
        static const uint64_t init[8] = {
            0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
            0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
            0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
        std::memcpy(h_, init, sizeof(h_));
        len_ = 0;
        bufLen_ = 0;
    }

    void update(const uint8_t *data, size_t len) {
        len_ += len;
        while (len > 0) {
            const size_t take = std::min<size_t>(128 - bufLen_, len);
            std::memcpy(buf_ + bufLen_, data, take);
            bufLen_ += take;
            data += take;
            len -= take;
            if (bufLen_ == 128) {
                compress(buf_);
                bufLen_ = 0;
            }
        }
    }

    Bytes final() {
        const uint64_t bitsLo = len_ << 3;
        const uint64_t bitsHi = len_ >> 61;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (bufLen_ != 112) update(&zero, 1);
        uint8_t lenBytes[16];
        for (int i = 0; i < 8; i++) lenBytes[i] = uint8_t((bitsHi >> (56 - 8 * i)) & 0xff);
        for (int i = 0; i < 8; i++) lenBytes[8 + i] = uint8_t((bitsLo >> (56 - 8 * i)) & 0xff);
        const uint64_t saved = len_;
        update(lenBytes, 16);
        len_ = saved;

        Bytes out(64);
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) out[8 * i + j] = uint8_t((h_[i] >> (56 - 8 * j)) & 0xff);
        }
        return out;
    }

private:
    void compress(const uint8_t *block) {
        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | block[8 * i + j];
        }
        for (int i = 16; i < 80; i++) {
            const uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        uint64_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 80; i++) {
            const uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
            const uint64_t ch = (e & f) ^ (~e & g);
            const uint64_t t1 = h + S1 + ch + K512[i] + w[i];
            const uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
            const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint64_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    uint64_t h_[8];
    uint64_t len_;
    uint8_t buf_[128];
    size_t bufLen_;
};

// ── Ed25519 field/point arithmetic (TweetNaCl formulation) ────────────────

typedef int64_t i64;
typedef i64 gf[16];

const gf kGf0 = {0};
const gf kGf1 = {1};

// d = -121665/121666 (Edwards curve constant)
const gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
               0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
const gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
const gf kX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
               0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
const gf kY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
               0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
const gf kI = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
               0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

void set25519(gf r, const gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        const i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

void sel25519(gf p, gf q, int b) {
    const i64 c = ~(i64(b) - 1);
    for (int i = 0; i < 16; i++) {
        const i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack25519(uint8_t *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = int((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = uint8_t(t[i] & 0xff);
        o[2 * i + 1] = uint8_t(t[i] >> 8);
    }
}

int neq25519(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= uint8_t(c[i] ^ d[i]);
    return diff != 0;
}

uint8_t par25519(const gf a) {
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + (i64(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}

void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

void M(gf o, const gf a, const gf b) {
    i64 t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

void S(gf o, const gf a) { M(o, a, a); }

void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void pow2523(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, kD2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

void cswap(gf p[4], gf q[4], uint8_t b) {
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

void pack(uint8_t *r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= uint8_t(par25519(tx) << 7);
}

void scalarmult(gf p[4], gf q[4], const uint8_t *s) {
    set25519(p[0], kGf0);
    set25519(p[1], kGf1);
    set25519(p[2], kGf1);
    set25519(p[3], kGf0);
    for (int i = 255; i >= 0; --i) {
        const uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

void scalarbase(gf p[4], const uint8_t *s) {
    gf q[4];
    set25519(q[0], kX);
    set25519(q[1], kY);
    set25519(q[2], kGf1);
    M(q[3], kX, kY);
    scalarmult(p, q, s);
}

// Group order L, little-endian.
const uint64_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
                         0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};

void modL(uint8_t *r, i64 x[64]) {
    i64 carry;
    int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * i64(kL[j - (i - 32)]);
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * i64(kL[j]);
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; ++j) x[j] -= carry * i64(kL[j]);
    for (i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = uint8_t(x[i] & 255);
    }
}

void reduce(uint8_t *r) {
    i64 x[64];
    for (int i = 0; i < 64; i++) x[i] = i64(r[i]);
    for (int i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}

int unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], kGf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, kD);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);

    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], kI);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], kGf0, r[0]);

    M(r[3], r[0], r[1]);
    return 0;
}

// Montgomery u-coordinate -> Edwards y-coordinate: y = (u-1)/(u+1)
void convertPublicKey(uint8_t *ed, const uint8_t *mont) {
    gf u, one, num, den, inv, y;
    unpack25519(u, mont);
    set25519(one, kGf1);
    A(num, one, u);          // will be swapped: keep numerator/denominator explicit
    Z(den, u, one);          // u - 1
    A(num, u, one);          // u + 1
    // y = (u - 1) / (u + 1)
    inv25519(inv, num);
    M(y, den, inv);
    pack25519(ed, y);
}

void cryptoHash(uint8_t *out, const uint8_t *m, size_t n) {
    const Bytes h = sha512(m, n);
    std::memcpy(out, h.data(), 64);
}

// XEdDSA/curve25519-js "crypto_sign_direct": the secret *scalar* is the
// clamped 32-byte private key itself (no SHA-512 seed expansion, unlike
// standard Ed25519). sk[32..63] holds the public point A.
void edSign(uint8_t *sm, const uint8_t *m, size_t n, const uint8_t *sk) {
    uint8_t h[64], r[64];
    i64 x[64];
    gf p[4];

    for (size_t i = 0; i < n; i++) sm[64 + i] = m[i];
    for (int i = 0; i < 32; i++) sm[32 + i] = sk[i];

    cryptoHash(r, sm + 32, n + 32);   // SHA-512(scalar || message)
    reduce(r);
    scalarbase(p, r);
    pack(sm, p);                      // R

    for (int i = 0; i < 32; i++) sm[i + 32] = sk[i + 32];
    cryptoHash(h, sm, n + 64);        // SHA-512(R || A || m)
    reduce(h);

    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = i64(r[i]);
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i + j] += i64(h[i]) * i64(sk[j]);
    modL(sm + 32, x);                 // S = r + h*a
}

bool edVerify(const uint8_t *m, size_t n, const uint8_t *sig, const uint8_t *pk) {
    uint8_t t[32], h[64];
    gf p[4], q[4];

    if (unpackneg(q, pk) != 0) return false;

    // Hash input is R || A || message (TweetNaCl replaces the signature's
    // S half with the public key before hashing).
    Bytes sm(sig, sig + 32);              // R
    sm.insert(sm.end(), pk, pk + 32);     // A
    sm.insert(sm.end(), m, m + n);

    cryptoHash(h, sm.data(), sm.size());
    reduce(h);
    scalarmult(p, q, h);

    scalarbase(q, sig + 32);
    add(p, q);
    pack(t, p);

    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= uint8_t(t[i] ^ sig[i]);
    return diff == 0;
}

// Clamps a 32-byte X25519 private key the same way curve25519-js does.
Bytes clampedCopy(const Bytes &privateKey) {
    Bytes sk = privateKey;
    sk[0] &= 248;
    sk[31] &= 127;
    sk[31] |= 64;
    return sk;
}

} // namespace

Bytes sha512(const uint8_t *data, size_t len) {
    Sha512 s;
    s.update(data, len);
    return s.final();
}

Bytes sha512(const Bytes &data) { return sha512(data.data(), data.size()); }

Bytes ed25519PublicKeyFromSeed(const Bytes &seed) {
    if (seed.size() != 32) return Bytes();
    // XEdDSA form: A = clamp(seed) * B (curve25519-js does not hash the seed).
    const Bytes sk = clampedCopy(seed);
    gf p[4];
    scalarbase(p, sk.data());
    Bytes pub(32);
    pack(pub.data(), p);
    return pub;
}

Bytes xeddsaSign(const Bytes &privateKey, const Bytes &message) {
    if (privateKey.size() != 32) return Bytes();

    Bytes edsk = clampedCopy(privateKey);   // seed, clamped
    {
        // edsk[32..63] = Ed25519 public key A for this seed
        const Bytes a = ed25519PublicKeyFromSeed(edsk);
        edsk.insert(edsk.end(), a.begin(), a.end());
    }
    const uint8_t signBit = uint8_t(edsk[63] & 128);

    Bytes sm(64 + message.size());
    edSign(sm.data(), message.data(), message.size(), edsk.data());
    Bytes sig(sm.begin(), sm.begin() + 64);
    sig[63] |= signBit;   // carry A's sign bit in the signature
    return sig;
}

bool xeddsaVerify(const Bytes &publicKey, const Bytes &message, const Bytes &signature) {
    if (publicKey.size() != 32 || signature.size() != 64) return false;

    Bytes edpk(32);
    convertPublicKey(edpk.data(), publicKey.data());

    Bytes sig = signature;
    edpk[31] |= uint8_t(sig[63] & 128);   // restore A's sign bit
    sig[63] &= 127;

    return edVerify(message.data(), message.size(), sig.data(), edpk.data());
}

Bytes x25519PublicFromPrivate(const Bytes &privateKey) {
    // Edwards A -> Montgomery u = (1+y)/(1-y); equals X25519 base multiplication.
    Bytes out(32);
    gf p[4];
    const Bytes sk = clampedCopy(privateKey);
    scalarbase(p, sk.data());
    gf y, one, num, den, inv, u;
    set25519(one, kGf1);
    Bytes yBytes(32);
    pack(yBytes.data(), p);
    unpack25519(y, yBytes.data());
    A(num, one, y);
    Z(den, one, y);
    inv25519(inv, den);
    M(u, num, inv);
    pack25519(out.data(), u);
    return out;
}

} // namespace crypto
} // namespace wa
