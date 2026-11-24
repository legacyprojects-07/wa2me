// A signal::Store backed by plain text, used by qt/tests/test_message.cpp.
// The real client implements the same interface on top of creds.json
// (see WhatsAppDirect's CredsStore); this keeps the test Qt-free.
#pragma once

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "sha256.h"
#include "signal.h"

namespace wa {
namespace test {

class TestStore : public signal::Store {
public:
    void setIdentity(const signal::KeyPair &kp, uint32_t regId) {
        identity_ = kp;
        regId_ = regId;
    }
    void addSignedPreKey(uint32_t id, const signal::PreKeyRecord &rec) {
        signedPreKeys_[id] = rec;
    }
    void addPreKey(uint32_t id, const signal::KeyPair &kp) { preKeys_[id] = kp; }

    signal::KeyPair ourIdentity() override { return identity_; }
    uint32_t ourRegistrationId() override { return regId_; }

    bool loadPreKey(uint32_t id, signal::KeyPair &out) override {
        auto it = preKeys_.find(id);
        if (it == preKeys_.end()) return false;
        out = it->second;
        return true;
    }
    void removePreKey(uint32_t id) override { preKeys_.erase(id); }

    bool loadSignedPreKey(uint32_t id, signal::KeyPair &out) override {
        auto it = signedPreKeys_.find(id);
        if (it == signedPreKeys_.end()) {
            if (signedPreKeys_.empty()) return false;
            it = signedPreKeys_.begin();
        }
        out = it->second.pair;
        return true;
    }

    signal::Record loadSession(const std::string &address) override {
        signal::Record record;
        auto it = sessions_.find(address);
        if (it != sessions_.end()) {
            signal::Record parsed;
            if (signal::deserializeRecord(it->second, parsed)) record = parsed;
        }
        return record;
    }

    void storeSession(const std::string &address, const signal::Record &record) override {
        sessions_[address] = signal::serializeRecord(record);
    }

    // ── Text persistence (one line per entry) ────────────────────────────
    void save(const std::string &path) const {
        std::ofstream out(path);
        out << "IDENTITY " << crypto::toHex(identity_.priv) << ' '
            << crypto::toHex(identity_.pub) << ' ' << regId_ << '\n';
        for (const auto &entry : signedPreKeys_) {
            out << "SKPRE " << entry.first << ' ' << crypto::toHex(entry.second.pair.priv)
                << ' ' << crypto::toHex(entry.second.pair.pub) << ' '
                << crypto::toHex(entry.second.signature) << '\n';
        }
        for (const auto &entry : preKeys_) {
            out << "PREKEY " << entry.first << ' ' << crypto::toHex(entry.second.priv) << ' '
                << crypto::toHex(entry.second.pub) << '\n';
        }
        for (const auto &entry : sessions_) {
            out << "SESSION " << entry.first << ' ' << crypto::toHex(entry.second) << '\n';
        }
    }

    bool load(const std::string &path) {
        std::ifstream in(path);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string kind;
            ss >> kind;
            if (kind == "IDENTITY") {
                std::string priv, pub;
                ss >> priv >> pub >> regId_;
                identity_.priv = crypto::fromHex(priv);
                identity_.pub = crypto::fromHex(pub);
            } else if (kind == "SKPRE") {
                uint32_t id = 0;
                std::string priv, pub, sig;
                ss >> id >> priv >> pub >> sig;
                signal::PreKeyRecord rec;
                rec.id = id;
                rec.pair.priv = crypto::fromHex(priv);
                rec.pair.pub = crypto::fromHex(pub);
                rec.signature = crypto::fromHex(sig);
                signedPreKeys_[id] = rec;
            } else if (kind == "PREKEY") {
                uint32_t id = 0;
                std::string priv, pub;
                ss >> id >> priv >> pub;
                preKeys_[id] = signal::KeyPair{crypto::fromHex(priv), crypto::fromHex(pub)};
            } else if (kind == "SESSION") {
                std::string address, data;
                ss >> address >> data;
                sessions_[address] = crypto::fromHex(data);
            }
        }
        return true;
    }

private:
    signal::KeyPair identity_;
    uint32_t regId_ = 0;
    std::map<uint32_t, signal::PreKeyRecord> signedPreKeys_;
    std::map<uint32_t, signal::KeyPair> preKeys_;
    std::map<std::string, signal::Bytes> sessions_;
};

} // namespace test
} // namespace wa
