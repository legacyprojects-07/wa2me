// Protobuf/handshake test harness — validates the hand-written protobuf
// encoders against Baileys' generated WAProto code, and the ServerHello
// decoder against a Baileys-encoded ServerHello.
//
// Build & run:
//   g++ -std=c++17 -I qt/src/direct qt/tests/test_proto.cpp
//       qt/src/direct/proto_min.cpp -o /tmp/test_proto
//   /tmp/test_proto qt/tests/proto_vectors.txt

#include "proto_min.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace wa;
using proto::Bytes;

namespace {
int pass = 0, fail = 0;

Bytes hex(const std::string &h) {
    if (h == "-") return Bytes();
    Bytes out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

std::string toHex(const Bytes &b) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 0xf]; }
    return s;
}

void check(bool ok, const char *what, const std::string &extra = "") {
    if (ok) { pass++; return; }
    fail++;
    std::fprintf(stderr, "FAIL %s %s\n", what, extra.c_str());
}
} // namespace

int main(int argc, char **argv) {
    const std::string path = argc > 1 ? argv[1] : "qt/tests/proto_vectors.txt";
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }

    std::string line;
    int md5n = 0, helloN = 0, serverN = 0, finishN = 0, dpN = 0, regN = 0, loginN = 0;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;

        if (kind == "MD5") {
            std::string inHex, want;
            ss >> inHex >> want;
            check(toHex(proto::md5(hex(inHex))) == want, "MD5");
            md5n++;
        } else if (kind == "HELLO") {
            std::string ephHex, want;
            ss >> ephHex >> want;
            proto::ClientHello h;
            h.ephemeral = hex(ephHex);
            const Bytes got = proto::encodeClientHello(h);
            check(toHex(got) == want, "ClientHello", toHex(got) + " != " + want);
            helloN++;
        } else if (kind == "SERVERHELLO") {
            std::string msgHex, ephHex, staticHex, payloadHex;
            ss >> msgHex >> ephHex >> staticHex >> payloadHex;
            proto::ServerHello sh;
            const bool ok = proto::decodeServerHello(hex(msgHex), sh);
            check(ok && toHex(sh.ephemeral) == ephHex &&
                      toHex(sh.staticKey) == staticHex &&
                      toHex(sh.payload) == payloadHex,
                  "ServerHello decode");
            serverN++;
        } else if (kind == "FINISH") {
            std::string msgHex;
            ss >> msgHex;
            // Re-encode check: decode fields out of the Baileys message and
            // re-encode with our writer.
            proto::Reader r(hex(msgHex));
            proto::Field f;
            proto::ClientFinish finish;
            while (r.next(f)) {
                if (f.number != 4) continue;
                proto::Reader inner(r.slice(f));
                proto::Field g;
                while (inner.next(g)) {
                    if (g.number == 1) finish.staticKey = g.bytes;
                    if (g.number == 2) finish.payload = g.bytes;
                }
            }
            const Bytes got = proto::encodeClientFinish(finish);
            check(toHex(got) == msgHex, "ClientFinish", toHex(got) + " != " + msgHex);
            finishN++;
        } else if (kind == "DEVICEPROPS") {
            std::string want;
            ss >> want;
            proto::HistorySyncConfig hs;
            const Bytes got = proto::encodeDeviceProps("Ubuntu", 15, true, hs);
            check(toHex(got) == want, "DeviceProps");
            dpN++;
        } else if (kind == "REGPAYLOAD") {
            std::string user, device, tertiary, ident, skeyId, skeyVal, skeySig, devProps, want;
            ss >> user >> device >> tertiary >> ident >> skeyId >> skeyVal >> skeySig >> devProps >> want;
            (void)device;

            proto::ClientPayload p;
            p.connectType = 1;
            p.connectReason = 1;
            p.hasUserAgent = true;
            p.userAgent.platform = 1;
            p.userAgent.appVersion = {2, 3000, uint32_t(std::stoul(tertiary))};
            p.webSubPlatform = 0;
            p.hasDevicePairingData = true;
            p.devicePairingData.eRegid = proto::encodeBigEndian(1234567890ull);
            p.devicePairingData.eKeytype = Bytes{5};
            p.devicePairingData.eIdent = hex(ident);
            p.devicePairingData.eSkeyId = proto::encodeBigEndian(std::stoull(skeyId), 3);
            p.devicePairingData.eSkeyVal = hex(skeyVal);
            p.devicePairingData.eSkeySig = hex(skeySig);
            p.devicePairingData.buildHash = proto::md5(Bytes{'2','.','3','0','0','0','.','1','0','3','5','9','3','9','1','8','7'});
            p.devicePairingData.deviceProps = hex(devProps);

            const Bytes got = proto::encodeClientPayload(p);
            check(toHex(got) == want, "registration ClientPayload",
                  "\n  got: " + toHex(got) + "\n  want:" + want);
            regN++;
        } else if (kind == "LOGINPAYLOAD") {
            std::string user, device, secondary, tertiary, want;
            ss >> user >> device >> secondary >> tertiary >> want;
            (void)secondary;

            proto::ClientPayload p;
            p.connectType = 1;
            p.connectReason = 1;
            p.username = std::stoull(user);
            p.hasDevice = true;
            p.device = uint32_t(std::stoul(device));
            p.hasPull = true;
            p.pull = true;
            p.hasUserAgent = true;
            p.userAgent.platform = 1;
            p.userAgent.appVersion = {2, 3000, uint32_t(std::stoul(tertiary))};
            p.webSubPlatform = 0;

            const Bytes got = proto::encodeClientPayload(p);
            check(toHex(got) == want, "login ClientPayload",
                  "\n  got: " + toHex(got) + "\n  want:" + want);
            loginN++;
        }
    }

    std::printf("\n=== protobuf / handshake messages ===\n");
    std::printf("MD5                       : %d vectors\n", md5n);
    std::printf("ClientHello encode        : %d vectors\n", helloN);
    std::printf("ServerHello decode        : %d vectors\n", serverN);
    std::printf("ClientFinish encode       : %d vectors\n", finishN);
    std::printf("DeviceProps encode        : %d vectors\n", dpN);
    std::printf("registration ClientPayload: %d vectors\n", regN);
    std::printf("login ClientPayload       : %d vectors\n", loginN);
    std::printf("\n%d passed, %d failed\n", pass, fail);
    std::printf("%s\n", fail == 0 ? "ALL PROTO TESTS PASSED" : "PROTO TESTS FAILED");
    return fail == 0 ? 0 : 1;
}
