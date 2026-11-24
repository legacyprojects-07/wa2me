// Noise transport test — replays an operation script recorded from the real
// Baileys NoiseHandler (lib/Utils/noise-handler.js) against the C++ port and
// compares every ciphertext byte-for-byte. This validates the whole key
// schedule: NOISE_MODE hashing, NOISE_HEADER/static-key authentication, HKDF
// salt/info usage, GCM IV counters and AAD (handshake hash) updates.
//
// Build & run:
//   g++ -std=c++17 -I qt/src/direct qt/tests/test_noise.cpp
//       qt/src/direct/noise.cpp qt/src/direct/aes_gcm.cpp
//       qt/src/direct/sha256.cpp qt/src/direct/x25519.cpp
//       -o /tmp/test_noise
//   /tmp/test_noise qt/tests/noise_vectors.txt

#include "noise.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace wa;
using Bytes = std::vector<uint8_t>;

static Bytes hex(const std::string &h) {
    if (h == "-") return Bytes();
    Bytes out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

static std::string toHex(const Bytes &b) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 0xf]; }
    return s;
}

int main(int argc, char **argv) {
    const std::string path = argc > 1 ? argv[1] : "qt/tests/noise_vectors.txt";
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }

    std::string line;
    Bytes priv, pub;
    noise::NoiseHandler *handler = nullptr;
    int pass = 0, fail = 0;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;

        if (kind == "NOISEKEY") {
            std::string privHex, pubHex;
            ss >> privHex >> pubHex;
            priv = hex(privHex);
            pub = hex(pubHex);
            delete handler;
            handler = new noise::NoiseHandler(priv, pub);
        } else if (kind == "OP" && handler) {
            std::string op, dataHex;
            ss >> op >> dataHex;
            if (op == "A") {
                handler->authenticate(hex(dataHex));
                pass++;   // absorbed; correctness shows up in the next ciphertext
            } else if (op == "F") {
                handler->finishInit();
                pass++;
            } else if (op == "E") {
                std::string want;
                ss >> want;
                const Bytes got = handler->encrypt(hex(dataHex));
                if (toHex(got) == want) {
                    pass++;
                } else {
                    fail++;
                    std::fprintf(stderr, "FAIL encrypt  got: %s\n              want: %s\n",
                                 toHex(got).c_str(), want.c_str());
                }
            }
        }
    }
    delete handler;

    // Frame framing: 3-byte big-endian length prefix, WA header on first frame,
    // and correct reassembly of split/coalesced websocket reads.
    {
        noise::NoiseHandler h(Bytes(32, 1), Bytes(32, 2));
        const Bytes frame1 = h.encodeFrame(Bytes{'a', 'b', 'c'});
        const bool headerOk = frame1.size() == 4 + 3 + 3 &&
                              frame1[0] == 87 && frame1[1] == 65 && frame1[2] == 6 && frame1[3] == 3 &&
                              frame1[4] == 0 && frame1[5] == 0 && frame1[6] == 3;
        if (headerOk) pass++; else { fail++; std::fprintf(stderr, "FAIL frame header\n"); }

        const Bytes frame2 = h.encodeFrame(Bytes{'d', 'e'});
        const bool noHeaderAgain = frame2.size() == 3 + 2 && frame2[0] == 0 && frame2[1] == 0 &&
                                   frame2[2] == 2;
        if (noHeaderAgain) pass++; else { fail++; std::fprintf(stderr, "FAIL frame 2 header\n"); }

        // Server->client frames carry no WA header, so strip the first frame's
        // 4-byte intro before feeding them back into the decoder.
        const Bytes stripped1(frame1.begin() + 4, frame1.end());
        std::vector<Bytes> received;
        Bytes all = stripped1;
        all.insert(all.end(), frame2.begin(), frame2.end());
        h.decodeFrames(all, [&](const Bytes &b) { received.push_back(b); });
        const bool coalesced = received.size() == 2 && received[0] == Bytes({'a','b','c'}) &&
                               received[1] == Bytes({'d','e'});
        if (coalesced) pass++; else { fail++; std::fprintf(stderr, "FAIL coalesced frames\n"); }

        received.clear();
        h.decodeFrames(Bytes(stripped1.begin(), stripped1.begin() + 2),
                       [&](const Bytes &b) { received.push_back(b); });
        h.decodeFrames(Bytes(stripped1.begin() + 2, stripped1.end()),
                       [&](const Bytes &b) { received.push_back(b); });
        const bool split = received.size() == 1 && received[0] == Bytes({'a','b','c'});
        if (split) pass++; else { fail++; std::fprintf(stderr, "FAIL split frame\n"); }
    }

    std::printf("\n=== Noise transport ===\n");
    std::printf("scripted ops vs Baileys   : %d passed, %d failed\n", pass, fail);
    std::printf("%s\n", fail == 0 ? "ALL NOISE TESTS PASSED" : "NOISE TESTS FAILED");
    return fail == 0 ? 0 : 1;
}
