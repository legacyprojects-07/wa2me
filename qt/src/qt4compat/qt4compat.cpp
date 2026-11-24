#include "qt4compat.h"

SignalHook::SignalHook(bool singleShot) : m_singleShot(singleShot) {}

void SignalHook::finishUp() {
    m_f0 = std::function<void()>();
    m_f1s = std::function<void(const QString &)>();
    m_f2ss = std::function<void(const QString &, const QString &)>();
    m_f3i = std::function<void(int, int, int)>();
    m_fsbs = std::function<void(const QString &, bool, const QString &)>();
    if (m_singleShot) deleteLater();
}

void SignalHook::attach(QObject *sender, const char *signal,
                        const std::function<void()> &fn, bool singleShot) {
    if (!sender || !fn) return;
    SignalHook *hook = new SignalHook(singleShot);
    hook->m_f0 = fn;
    connect(sender, signal, hook, SLOT(run0()));
    if (singleShot) connect(sender, SIGNAL(destroyed()), hook, SLOT(deleteLater()));
}

void SignalHook::attach(QObject *sender, const char *signal,
                        const std::function<void(const QString &)> &fn, bool singleShot) {
    if (!sender || !fn) return;
    SignalHook *hook = new SignalHook(singleShot);
    hook->m_f1s = fn;
    connect(sender, signal, hook, SLOT(run1s(QString)));
    if (singleShot) connect(sender, SIGNAL(destroyed()), hook, SLOT(deleteLater()));
}

void SignalHook::attach(QObject *sender, const char *signal,
                        const std::function<void(const QString &, const QString &)> &fn,
                        bool singleShot) {
    if (!sender || !fn) return;
    SignalHook *hook = new SignalHook(singleShot);
    hook->m_f2ss = fn;
    connect(sender, signal, hook, SLOT(run2ss(QString,QString)));
    if (singleShot) connect(sender, SIGNAL(destroyed()), hook, SLOT(deleteLater()));
}

void SignalHook::attach(QObject *sender, const char *signal,
                        const std::function<void(int, int, int)> &fn, bool singleShot) {
    if (!sender || !fn) return;
    SignalHook *hook = new SignalHook(singleShot);
    hook->m_f3i = fn;
    connect(sender, signal, hook, SLOT(run3i(int,int,int)));
    if (singleShot) connect(sender, SIGNAL(destroyed()), hook, SLOT(deleteLater()));
}

void SignalHook::attach(QObject *sender, const char *signal,
                        const std::function<void(const QString &, bool, const QString &)> &fn,
                        bool singleShot) {
    if (!sender || !fn) return;
    SignalHook *hook = new SignalHook(singleShot);
    hook->m_fsbs = fn;
    connect(sender, signal, hook, SLOT(runSBS(QString,bool,QString)));
    if (singleShot) connect(sender, SIGNAL(destroyed()), hook, SLOT(deleteLater()));
}

void SignalHook::run0() {
    const std::function<void()> fn = m_f0;
    finishUp();
    if (fn) fn();
}

void SignalHook::run1s(const QString &a) {
    const std::function<void(const QString &)> fn = m_f1s;
    finishUp();
    if (fn) fn(a);
}

void SignalHook::run2ss(const QString &a, const QString &b) {
    const std::function<void(const QString &, const QString &)> fn = m_f2ss;
    finishUp();
    if (fn) fn(a, b);
}

void SignalHook::run3i(int a, int b, int c) {
    const std::function<void(int, int, int)> fn = m_f3i;
    finishUp();
    if (fn) fn(a, b, c);
}

void SignalHook::runSBS(const QString &a, bool b, const QString &c) {
    const std::function<void(const QString &, bool, const QString &)> fn = m_fsbs;
    finishUp();
    if (fn) fn(a, b, c);
}
