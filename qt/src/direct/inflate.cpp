#include "inflate.h"

#include <cstring>

namespace wa {
namespace binary {
namespace {

// ── Length/distance tables (RFC 1951 §3.2.5) ────────────────────────────────

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                             4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// Order in which code-length code lengths are stored.
const short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

constexpr int MAXBITS = 15;
constexpr int MAXLCODES = 286;
constexpr int MAXDCODES = 30;
constexpr int MAXCODES = MAXLCODES + MAXDCODES;
constexpr int FIXLCODES = 288;

struct Huffman {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
};

struct State {
    const uint8_t *in;
    size_t inLen;
    size_t inCnt = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    std::vector<uint8_t> *out;
    size_t maxOutput;
};

// Reads `need` bits (LSB-first). Returns -1 at end of input.
int bits(State *s, int need) {
    uint32_t val = s->bitBuf;
    while (s->bitCnt < need) {
        if (s->inCnt >= s->inLen) return -1;
        val |= uint32_t(s->in[s->inCnt++]) << s->bitCnt;
        s->bitCnt += 8;
    }
    s->bitBuf = val >> need;
    s->bitCnt -= need;
    return int(val & ((1u << need) - 1));
}

// Builds a canonical Huffman decoding table. Returns 0 for a complete code,
// >0 for an incomplete code, <0 for an over-subscribed code.
int construct(Huffman *h, const short *length, int n) {
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;   // no symbols: complete but empty

    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;    // over-subscribed
    }

    short offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = short(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++) {
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = short(sym);
    }
    return left;
}

// Decodes one symbol; returns -1 on error.
int decode(State *s, const Huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

bool stored(State *s) {
    s->bitBuf = 0;
    s->bitCnt = 0;
    if (s->inCnt + 4 > s->inLen) return false;
    unsigned len = unsigned(s->in[s->inCnt]) | (unsigned(s->in[s->inCnt + 1]) << 8);
    unsigned nlen = unsigned(s->in[s->inCnt + 2]) | (unsigned(s->in[s->inCnt + 3]) << 8);
    s->inCnt += 4;
    if (len != (~nlen & 0xffff)) return false;
    if (s->inCnt + len > s->inLen) return false;
    if (s->out->size() + len > s->maxOutput) return false;
    s->out->insert(s->out->end(), s->in + s->inCnt, s->in + s->inCnt + len);
    s->inCnt += len;
    return true;
}

bool codes(State *s, const Huffman *lencode, const Huffman *distcode) {
    for (;;) {
        int symbol = decode(s, lencode);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (s->out->size() + 1 > s->maxOutput) return false;
            s->out->push_back(uint8_t(symbol));
        } else if (symbol == 256) {
            return true;   // end of block
        } else {
            symbol -= 257;
            if (symbol >= 29) return false;
            int extra = bits(s, kLenExtra[symbol]);
            if (extra < 0) return false;
            size_t len = size_t(kLenBase[symbol]) + size_t(extra);

            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return false;
            int dextra = bits(s, kDistExtra[dsym]);
            if (dextra < 0) return false;
            size_t dist = size_t(kDistBase[dsym]) + size_t(dextra);
            if (dist > s->out->size()) return false;
            if (s->out->size() + len > s->maxOutput) return false;

            size_t from = s->out->size() - dist;
            for (size_t i = 0; i < len; i++) s->out->push_back((*s->out)[from + i]);
        }
    }
}

bool fixed(State *s) {
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        short lengths[FIXLCODES];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = true;
    }
    return codes(s, &lencode, &distcode);
}

bool dynamic(State *s) {
    short lengths[MAXCODES];
    Huffman lencode, distcode;

    int nlen = bits(s, 5);
    if (nlen < 0) return false;
    nlen += 257;
    int ndist = bits(s, 5);
    if (ndist < 0) return false;
    ndist += 1;
    int ncode = bits(s, 4);
    if (ncode < 0) return false;
    ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return false;

    int index = 0;
    for (; index < ncode; index++) {
        int v = bits(s, 3);
        if (v < 0) return false;
        lengths[kOrder[index]] = short(v);
    }
    for (; index < 19; index++) lengths[kOrder[index]] = 0;

    if (construct(&lencode, lengths, 19) != 0) return false;   // code-length code must be complete

    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = short(symbol);
        } else {
            short len = 0;
            if (symbol == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                int rep = bits(s, 2);
                if (rep < 0) return false;
                symbol = 3 + rep;
            } else if (symbol == 17) {
                int rep = bits(s, 3);
                if (rep < 0) return false;
                symbol = 3 + rep;
            } else {
                int rep = bits(s, 7);
                if (rep < 0) return false;
                symbol = 11 + rep;
            }
            if (index + symbol > nlen + ndist) return false;
            while (symbol--) lengths[index++] = len;
        }
    }

    if (lengths[256] == 0) return false;   // no end-of-block code

    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;

    return codes(s, &lencode, &distcode);
}

} // namespace

bool inflateRaw(const uint8_t *data, size_t len, std::vector<uint8_t> &out, size_t maxOutput) {
    out.clear();
    if (len == 0) return true;   // empty stream -> empty output

    State s{};
    s.in = data;
    s.inLen = len;
    s.out = &out;
    s.maxOutput = maxOutput;

    int last;
    do {
        last = bits(&s, 1);
        if (last < 0) return false;
        int type = bits(&s, 2);
        if (type < 0) return false;
        bool ok = false;
        switch (type) {
        case 0: ok = stored(&s); break;
        case 1: ok = fixed(&s); break;
        case 2: ok = dynamic(&s); break;
        default: return false;
        }
        if (!ok) return false;
    } while (!last);

    return true;
}

} // namespace binary
} // namespace wa
