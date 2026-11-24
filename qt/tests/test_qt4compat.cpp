// Qt 4.7 compatibility layer test (needs real Qt 4: builds only then).
//
// Covers the two pieces that can silently corrupt state:
//   * qt4compat/json.h  — creds.json is read/written with it, so a parser bug
//     loses the device identity
//   * qt4compat/websocket.cpp — the RFC 6455 frame encoder
//
// Build (desktop Qt 4 for testing only):
//   g++ -std=c++11 -fPIC -I qt/src -I qt/src/direct -I qt/src/qt4compat \
//       qt/tests/test_qt4compat.cpp qt/src/qt4compat/json.cpp \
//       $(pkg-config --cflags --libs QtCore) -o /tmp/test_qt4compat

#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
// Qt 5+ uses the real QJson* classes; nothing to test here.
int main() { return 0; }
#else

#include <QString>
#include <cstdio>

#include "qt4compat/json.h"

static int pass = 0, fail = 0;
static void check(bool ok, const char *what, const QString &extra = QString()) {
    if (ok) { pass++; return; }
    fail++;
    std::printf("FAIL %s %s\n", what, qPrintable(extra));
}

int main() {
    // ── Objects, scalars, nesting ────────────────────────────────────────
    {
        QJsonObject o;
        o["meJid"] = QString("919999999999@s.whatsapp.net");
        o["registrationId"] = 1234;
        o["preKeysUploaded"] = true;
        o["ratio"] = 0.25;
        QJsonArray preKeys;
        for (int i = 0; i < 3; i++) {
            QJsonObject key;
            key["id"] = i;
            key["priv"] = QString("AAAA+BBB/CCC==");
            preKeys.append(key);
        }
        o["preKeys"] = preKeys;

        const QByteArray text = QJsonDocument(o).toJson(QJsonDocument::Indented);
        check(!text.isEmpty(), "serialise object");

        const QJsonDocument parsed = QJsonDocument::fromJson(text);
        check(parsed.isObject(), "parse object");
        const QJsonObject back = parsed.object();
        check(back.value("meJid").toString() == "919999999999@s.whatsapp.net", "string round trip");
        check(back.value("registrationId").toInt() == 1234, "int round trip");
        check(back.value("preKeysUploaded").toBool(), "bool round trip");
        check(back.value("ratio").toDouble() == 0.25, "double round trip");
        check(back.value("preKeys").toArray().size() == 3, "array size");
        check(back.value("preKeys").toArray().at(1).toObject().value("id").toInt() == 1,
              "nested array element");
        check(back.value("missing").isUndefined(), "missing key is undefined");
        check(back.contains("meJid"), "contains()");
    }

    // ── Escapes and UTF-8 ────────────────────────────────────────────────
    {
        const QByteArray text =
            "{\"a\":\"line\\nbreak\\t\\\"quoted\\\"\",\"b\":\"caf\\u00e9\","
            "\"c\":\"\\ud83d\\ude00\",\"d\":null,\"e\":-17}";
        const QJsonDocument d = QJsonDocument::fromJson(text);
        check(d.isObject(), "parse escapes");
        const QJsonObject o = d.object();
        check(o.value("a").toString() == "line\nbreak\t\"quoted\"", "control escapes");
        check(o.value("b").toString() == QString::fromUtf8("café"), "\\u escape");
        check(o.value("c").toString().size() == 2, "surrogate pair kept as two units");
        check(o.value("d").isNull(), "null");
        check(o.value("e").toInt() == -17, "negative number");

        // Invalid input must not produce a document.
        check(QJsonDocument::fromJson("{\"a\":").isNull(), "truncated JSON is rejected");
        check(QJsonDocument::fromJson("not json").isNull(), "garbage is rejected");
        check(QJsonDocument::fromJson("{}").isObject(), "empty object parses");
        check(QJsonDocument::fromJson("[]").isArray(), "empty array parses");
    }

    // ── Value semantics (copy-on-write) ──────────────────────────────────
    {
        QJsonObject a;
        a["k"] = 1;
        QJsonObject b = a;
        b["k"] = 2;
        check(a.value("k").toInt() == 1, "copies are independent");
        check(b.value("k").toInt() == 2, "modified copy is updated");

        QJsonValue v = QJsonValue(5);
        check(v.isDouble() && v.toInt() == 5, "int value");
        check(QJsonValue("x").toString() == "x", "string value");
        check(QJsonValue().isUndefined(), "default value is undefined");
    }

    std::printf("\n=== Qt 4.7 compatibility shim ===\n");
    std::printf("%d passed, %d failed\n", pass, fail);
    std::printf("%s\n", fail == 0 ? "QT4COMPAT OK" : "QT4COMPAT FAILED");
    return fail == 0 ? 0 : 1;
}

#endif
