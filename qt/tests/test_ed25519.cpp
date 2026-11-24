// SHA-512 / Ed25519 / XEdDSA test.
//
// The vectors come from Node's crypto (SHA-512) and from curve25519-js, which
// is the implementation libsignal calls for Signal signatures — so this pins
// the WhatsApp XEdDSA variant (clamped X25519 key as Ed25519 scalar, sign bit
// moved into bit 7 of the last signature byte).
//
// Build & run:
//   g++ -std=c++17 -I qt/src/direct qt/tests/test_ed25519.cpp
//       qt/src/direct/ed25519.cpp qt/src/direct/x25519.cpp -o /tmp/test_ed25519
//   /tmp/test_ed25519 qt/tests/sig_vectors.txt

#include "ed25519.h"
#include "x25519.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace wa::crypto;

static Bytes hx(const std::string &h) {
    if (h == "-") return Bytes();
    Bytes out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

static std::string th(const Bytes &b) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

int main(int argc, char **argv) {
    const std::string path = argc > 1 ? argv[1] : "qt/tests/sig_vectors.txt";
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }

    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char *what, const std::string &extra = "") {
        if (ok) { pass++; return; }
        fail++;
        std::fprintf(stderr, "FAIL %s %s\n", what, extra.c_str());
    };

    std::string line;
    int sha = 0, sig = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;

        if (kind == "SHA512") {
            std::string msgHex, wantHex;
            ss >> msgHex >> wantHex;
            check(th(sha512(hx(msgHex))) == wantHex, "sha512", msgHex);
            sha++;
        } else if (kind == "XEDDSA") {
            std::string privHex, msgHex, sigHex;
            ss >> privHex >> msgHex >> sigHex;
            const Bytes priv = hx(privHex);
            const Bytes msg = hx(msgHex);
            const Bytes expected = hx(sigHex);

            // Deterministic signing: byte-identical to curve25519-js.
            const Bytes mine = xeddsaSign(priv, msg);
            check(mine == expected, "xeddsa sign", th(mine) + " != " + sigHex);

            const Bytes pub = x25519PublicFromPrivate(priv);
            check(pub.size() == 32, "x25519 public key");
            check(xeddsaVerify(pub, msg, expected), "xeddsa verify");
            check(xeddsaVerify(pub, msg, mine), "self-verify");
            // A tampered message must not verify.
            Bytes tampered = msg;
            if (!tampered.empty()) tampered[0] ^= 0x01;
            else tampered.push_back(0x01);
            check(!xeddsaVerify(pub, tampered, expected), "reject tampered message");
            sig++;
        } else if (kind == "XEDDSA_BAD") {
            std::string privHex, msgHex, sigHex;
            ss >> privHex >> msgHex >> sigHex;
            if (msgHex == "-") msgHex.clear();
            const Bytes pub = x25519PublicFromPrivate(hx(privHex));
            check(!xeddsaVerify(pub, hx(msgHex), hx(sigHex)), "reject bad signature");
        }
    }

    // The public key derivation must match X25519's own base-point multiply.
    {
        const Bytes priv = hx("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        uint8_t pk[32];
        x25519Base(priv.data(), pk);
        check(x25519PublicFromPrivate(priv) == Bytes(pk, pk + 32), "public key derivation");
    }

    std::printf("\n=== SHA-512 / Ed25519 / XEdDSA ===\n");
    std::printf("SHA-512 vectors : %d\n", sha);
    std::printf("XEdDSA vectors  : %d\n", sig);
    std::printf("\n%d passed, %d failed\n", pass, fail);
    std::printf("%s\n", fail == 0 ? "ALL ED25519 TESTS PASSED" : "ED25519 TESTS FAILED");
    return fail == 0 ? 0 : 1;
}
