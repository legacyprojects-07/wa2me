#pragma once
// MainWindow — the Qt replacement for the J2ME client.
//
// It can run in two modes:
//   * Proxy mode  (default): talks to the existing OpenKnot REST backend via
//     ApiClient. Works on Symbian Qt 4.7 with a JSON shim; no crypto needed.
//   * Direct mode: talks to wss://web.whatsapp.com via WhatsAppDirect.
//     Transport + pairing are implemented; message encryption still needs
//     libsignal (see QT_PORT.md §6).
//
// The window itself is mode-agnostic: it renders Chat/Message models from
// models.h and routes actions to whichever backend is active.

#include <QMainWindow>
#include <QList>
#include <QHash>

#include "models.h"

class QListWidget;
class QListWidgetItem;
class QTextBrowser;
class QLineEdit;
class QPushButton;
class QLabel;
class QAction;
class QDockWidget;
class QPlainTextEdit;

class ApiClient;
class WhatsAppDirect;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void start(const QString &baseUrl, const QString &apiKey, bool directMode);

private slots:
    void openSettings();
    void refreshChats();
    void onChatSelected();
    void sendCurrentMessage();
    void attachFile();
    void showQr();
    void toggleDirectMode(bool on);
    void searchMessages();

    // ApiClient
    void onChatsReady(const QList<Chat> &chats);
    void onDirectChat(const Chat &chat);
    void onDirectMessage(const Message &message);
    void onMessagesReady(const QString &jid, const QList<Message> &messages);
    void onSendDone(const QJsonObject &obj);
    void onQrReady(const QString &status, const QString &qr);
    void onError(const QString &message, const QString &code, int httpStatus);
    void onSyncReady(qint64 nextSince, bool hasMore, const QList<Chat> &chats,
                     const QList<Message> &messages);

    // WhatsAppDirect
    void onDirectConnection(const QString &state, const QString &reason);
    void onDirectQr(const QString &qr);
    void onDirectPaired(const QString &jid);
    void onDirectLog(const QString &line);

private:
    void buildUi();
    void buildActions();
    void applyMode();
    void appendLog(const QString &line);
    void renderMessages();
    QString currentJid() const;
    void scheduleSyncPoll();

    // backends
    ApiClient *m_api = nullptr;
    WhatsAppDirect *m_direct = nullptr;
    bool m_directMode = false;

    // ui
    QListWidget *m_chatList = nullptr;
    QTextBrowser *m_view = nullptr;
    QLineEdit *m_input = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_attachBtn = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_statusLabel = nullptr;
    QDockWidget *m_logDock = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QAction *m_directAction = nullptr;

    // state
    QString m_baseUrl = "http://127.0.0.1:3000";
    QString m_apiKey;
    QList<Chat> m_chats;
    QHash<QString, QList<Message>> m_messages;
    QHash<QString, QListWidgetItem *> m_chatItems;
    qint64 m_lastSync = 0;
};
