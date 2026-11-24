// Dependency-free raw-DEFLATE (RFC 1951) decompressor.
//
// WhatsApp compresses binary frames with raw DEFLATE (flag bit 0x02 on the
// frame's first byte). Qt has no raw-DEFLATE API (qUncompress only handles
// zlib-wrapped data), zlib may not be available on Symbian, and libsodium
// does not provide it — so this is a small self-contained implementation
// (stored + fixed-Huffman + dynamic-Huffman blocks), based on the classic
// "puff" reference algorithm.
//
// Verified against Node's zlib.deflateRawSync in qt/tests/test_codec.cpp.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace wa {
namespace binary {

// Returns true on success and fills `out`. Bounded: refuses output larger than
// maxOutput (protects against decompression bombs).
bool inflateRaw(const uint8_t *data, size_t len, std::vector<uint8_t> &out,
                size_t maxOutput = 64u * 1024u * 1024u);

} // namespace binary
} // namespace wa
