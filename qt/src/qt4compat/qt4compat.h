// Small Qt 4.7 compatibility layer.
//
// The client targets the Symbian toolchain, whose newest Qt is 4.7.4. Three
// things that later Qt versions made easy are missing there:
//
//   * functor connections  -> SignalHook runs a std::function from a signal
//   * QJson*               -> qt4compat/json.h
//   * a few QString helpers (QStringLiteral, toHtmlEscaped) and the
//     QDateTime/QNetworkReply spellings used below
#pragma once

#include <QDateTime>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

// QStringLiteral arrived in Qt 5.4.
#if QT_VERSION < QT_VERSION_CHECK(5, 4, 0)
#undef QStringLiteral
#define QStringLiteral(str) QString::fromUtf8(str)
#endif

#if QT_VERSION < QT_VERSION_CHECK(5, 8, 0)
// QDateTime::currentSecsSinceEpoch() / toSecsSinceEpoch() arrived in Qt 5.8.
inline qint64 qt_now_secs() { return qint64(QDateTime::currentDateTime().toTime_t()); }
inline qint64 qt_secs_since_epoch(const QDateTime &dt) { return qint64(dt.toTime_t()); }
#else
inline qint64 qt_now_secs() { return QDateTime::currentSecsSinceEpoch(); }
inline qint64 qt_secs_since_epoch(const QDateTime &dt) { return dt.toSecsSinceEpoch(); }
#endif

// QString::toHtmlEscaped() arrived in Qt 5.0; Qt 4 has Qt::escape() (no quotes).
inline QString htmlEscaped(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); i++) {
        const QChar ch = text.at(i);
        switch (ch.unicode()) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += ch; break;
        }
    }
    return out;
}

// Runs a functor when a signal fires. Qt 4 only connects to real slots, so the
// functor is stored here and invoked from a slot with the signal's argument
// list. One hook per connection; it deletes itself after the call.
class SignalHook : public QObject {
    Q_OBJECT
public:
    static void attach(QObject *sender, const char *signal,
                       const std::function<void()> &fn, bool singleShot = true);
    static void attach(QObject *sender, const char *signal,
                       const std::function<void(const QString &)> &fn,
                       bool singleShot = true);
    static void attach(QObject *sender, const char *signal,
                       const std::function<void(const QString &, const QString &)> &fn,
                       bool singleShot = true);
    static void attach(QObject *sender, const char *signal,
                       const std::function<void(int, int, int)> &fn, bool singleShot = true);
    static void attach(QObject *sender, const char *signal,
                       const std::function<void(const QString &, bool, const QString &)> &fn,
                       bool singleShot = true);

    // Convenience for QNetworkReply::finished().
    static void onFinished(QNetworkReply *reply, const std::function<void()> &fn) {
        if (!reply) return;
        attach(reply, SIGNAL(finished()), fn);
    }

private:
    explicit SignalHook(bool singleShot);

private slots:
    void run0();
    void run1s(const QString &a);
    void run2ss(const QString &a, const QString &b);
    void run3i(int a, int b, int c);
    void runSBS(const QString &a, bool b, const QString &c);

private:
    void finishUp();

    std::function<void()> m_f0;
    std::function<void(const QString &)> m_f1s;
    std::function<void(const QString &, const QString &)> m_f2ss;
    std::function<void(int, int, int)> m_f3i;
    std::function<void(const QString &, bool, const QString &)> m_fsbs;
    bool m_singleShot;
};
