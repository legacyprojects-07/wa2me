// Codec test harness — cross-validates the C++ BinaryNode codec and the
// dependency-free DEFLATE decoder against vectors produced by the real
// Baileys 6.7.22 implementation and Node's zlib.
//
// Build & run (no Qt required):
//   g++ -std=c++17 -I qt/src/direct qt/tests/test_codec.cpp
//       qt/src/direct/binary_node.cpp qt/src/direct/inflate.cpp
//       -o /tmp/test_codec && /tmp/test_codec qt/tests/vectors.txt
//
// Checks:
//   1. decode(baileys_bytes) succeeds for every vector
//   2. re-encode(decoded) == original bytes  (byte-for-byte parity)
//   3. inflateRaw(nodeflateOutput) == original raw bytes
//   4. frame reassembly: [0x02][deflate(encodePayload(node))] decodes correctly

#include "binary_node.h"
#include "inflate.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace wa;

static std::vector<uint8_t> fromHex(const std::string &hex) {
    std::vector<uint8_t> out;
    if (hex == "-") return out;   // "-" marks an empty buffer in vectors.txt
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

static std::string toHex(const std::vector<uint8_t> &v) {
    static const char *digits = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (uint8_t b : v) { s += digits[b >> 4]; s += digits[b & 0xf]; }
    return s;
}

static void dump(const BinaryNode &n, int depth, std::string &out) {
    out += std::string(depth * 2, ' ');
    out += n.tag;
    for (const auto &kv : n.attrs) out += " " + kv.first + "=" + kv.second;
    switch (n.contentType) {
    case BinaryNode::Content::None: out += " [no content]\n"; break;
    case BinaryNode::Content::Text: out += " text=\"" + n.text + "\"\n"; break;
    case BinaryNode::Content::Bytes: out += " bytes=" + std::to_string(n.bytes.size()) + "\n"; break;
    case BinaryNode::Content::List:
        out += " children=" + std::to_string(n.children.size()) + "\n";
        for (const auto &c : n.children) dump(c, depth + 1, out);
        break;
    }
}

int main(int argc, char **argv) {
    const std::string path = argc > 1 ? argv[1] : "qt/tests/vectors.txt";
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }

    int nodePass = 0, nodeFail = 0, inflatePass = 0, inflateFail = 0, framePass = 0, frameFail = 0;
    std::string line;
    std::vector<std::vector<uint8_t>> encodedVectors;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.rfind("VECTORS", 0) == 0) continue;   // header
        if (line.rfind("COMPRESSED", 0) == 0) continue;

        if (line[0] == 'V' && line[1] == ' ') {
            std::vector<uint8_t> bytes = fromHex(line.substr(2));
            encodedVectors.push_back(bytes);

            auto res = binary::decode(bytes);
            if (!res.ok) {
                std::fprintf(stderr, "FAIL decode: %s\n", res.error.c_str());
                nodeFail++;
                continue;
            }
            std::vector<uint8_t> re = binary::encode(res.node);
            if (re == bytes) {
                nodePass++;
            } else {
                nodeFail++;
                std::fprintf(stderr, "FAIL round-trip\n  in : %s\n  out: %s\n",
                             line.substr(2).c_str(), toHex(re).c_str());
                std::string d;
                dump(res.node, 0, d);
                std::fprintf(stderr, "%s", d.c_str());
            }
        } else if (line[0] == 'F' && line[1] == ' ') {
            std::istringstream ss(line.substr(2));
            std::string frameHex, payloadHex;
            ss >> frameHex >> payloadHex;
            std::vector<uint8_t> frame = fromHex(frameHex);
            std::vector<uint8_t> payload = fromHex(payloadHex);
            auto res = binary::decode(frame.data(), frame.size());
            if (!res.ok) {
                std::fprintf(stderr, "FAIL compressed frame: %s\n", res.error.c_str());
                frameFail++;
                continue;
            }
            std::vector<uint8_t> re = binary::encodePayload(res.node);
            if (re == payload) {
                framePass++;
            } else {
                frameFail++;
                std::fprintf(stderr, "FAIL compressed frame payload mismatch\n");
            }
        } else if (line[0] == 'C' && line[1] == ' ') {
            std::istringstream ss(line.substr(2));
            std::string rawHex, defHex;
            ss >> rawHex >> defHex;
            std::vector<uint8_t> raw = fromHex(rawHex);
            std::vector<uint8_t> def = fromHex(defHex);
            std::vector<uint8_t> out;
            if (!binary::inflateRaw(def.data(), def.size(), out)) {
                std::fprintf(stderr, "FAIL inflate (returned false), %zu bytes\n", def.size());
                inflateFail++;
                continue;
            }
            if (out == raw) {
                inflatePass++;
            } else {
                inflateFail++;
                std::fprintf(stderr, "FAIL inflate mismatch: got %zu bytes, want %zu\n",
                             out.size(), raw.size());
            }
        }
    }

    std::printf("\n=== BinaryNode codec ===\n");
    std::printf("decode+re-encode byte parity : %d passed, %d failed\n", nodePass, nodeFail);
    std::printf("=== raw DEFLATE ===\n");
    std::printf("inflateRaw vs Node zlib      : %d passed, %d failed\n", inflatePass, inflateFail);
    std::printf("=== compressed frames ===\n");
    std::printf("flag-0x02 frame decode       : %d passed, %d failed\n", framePass, frameFail);

    const int failures = nodeFail + inflateFail + frameFail;
    std::printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
