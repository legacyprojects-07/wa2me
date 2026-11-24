#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QScrollBar>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QJsonObject>

#include "api_client.h"
#include "direct/whatsapp_direct.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_api = new ApiClient(this);
    m_direct = new WhatsAppDirect(this);

    connect(m_api, &ApiClient::chatsReady, this, &MainWindow::onChatsReady);
    connect(m_api, &ApiClient::messagesReady, this, &MainWindow::onMessagesReady);
    connect(m_api, &ApiClient::sendDone, this, &MainWindow::onSendDone);
    connect(m_api, &ApiClient::qrReady, this, &MainWindow::onQrReady);
    connect(m_api, &ApiClient::syncReady, this, &MainWindow::onSyncReady);
    connect(m_api, &ApiClient::errorOccurred, this, &MainWindow::onError);

    connect(m_direct, &WhatsAppDirect::connectionUpdate, this, &MainWindow::onDirectConnection);
    connect(m_direct, &WhatsAppDirect::qrCode, this, &MainWindow::onDirectQr);
    connect(m_direct, &WhatsAppDirect::paired, this, &MainWindow::onDirectPaired);
    connect(m_direct, &WhatsAppDirect::logLine, this, &MainWindow::onDirectLog);
    connect(m_direct, &WhatsAppDirect::streamError, this,
            [this](const QString &code, const QString &text) {
                appendLog(QString("[direct] stream error %1: %2").arg(code, text));
            });
    connect(m_direct, &WhatsAppDirect::sendResult, this,
            [this](const QString &id, bool ok, const QString &error) {
                if (!ok) appendLog(QString("[direct] send %1 failed: %2").arg(id, error));
            });
    connect(m_direct, &WhatsAppDirect::chatUpdated, this, &MainWindow::onDirectChat);
    connect(m_direct, &WhatsAppDirect::messageAdded, this, &MainWindow::onDirectMessage);
    connect(m_direct, &WhatsAppDirect::historySyncReceived, this,
            [this](int progress, int chats, int messages) {
                statusBar()->showMessage(QString("History sync %1% — %2 chats, %3 messages")
                                             .arg(progress).arg(chats).arg(messages), 4000);
            });

    buildUi();
    buildActions();
    applyMode();
}

MainWindow::~MainWindow() = default;

// ─── UI ─────────────────────────────────────────────────────────────────────

void MainWindow::buildUi() {
    setWindowTitle("OpenKnot Qt");
    resize(900, 620);

    auto *splitter = new QSplitter(Qt::Horizontal, this);

    m_chatList = new QListWidget(splitter);
    m_chatList->setMinimumWidth(240);
    m_chatList->setAlternatingRowColors(true);
    connect(m_chatList, &QListWidget::itemSelectionChanged, this, &MainWindow::onChatSelected);

    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(6, 6, 6, 6);

    m_view = new QTextBrowser(right);
    m_view->setOpenExternalLinks(true);
    rightLayout->addWidget(m_view, 1);

    auto *inputRow = new QHBoxLayout;
    m_input = new QLineEdit(right);
    m_input->setPlaceholderText("Type a message…");
    connect(m_input, &QLineEdit::returnPressed, this, &MainWindow::sendCurrentMessage);
    m_attachBtn = new QPushButton("Attach", right);
    connect(m_attachBtn, &QPushButton::clicked, this, &MainWindow::attachFile);
    m_sendBtn = new QPushButton("Send", right);
    m_sendBtn->setDefault(true);
    connect(m_sendBtn, &QPushButton::clicked, this, &MainWindow::sendCurrentMessage);
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_attachBtn);
    inputRow->addWidget(m_sendBtn);
    rightLayout->addLayout(inputRow);

    splitter->addWidget(m_chatList);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_logDock = new QDockWidget("Log", this);
    m_logDock->setWidget(m_log);
    m_logDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_logDock);
    m_logDock->hide();

    m_statusLabel = new QLabel("Not connected", this);
    statusBar()->addPermanentWidget(m_statusLabel);
}

void MainWindow::buildActions() {
    auto *fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("Refresh chats", this, &MainWindow::refreshChats, QKeySequence::Refresh);
    fileMenu->addAction("Save attachments to…", this, &MainWindow::attachFile);
    fileMenu->addSeparator();
    fileMenu->addAction("Quit", qApp, &QApplication::quit, QKeySequence::Quit);

    auto *connMenu = menuBar()->addMenu("&Connection");
    connMenu->addAction("Show QR / pair device", this, &MainWindow::showQr);
    m_directAction = connMenu->addAction("Direct WhatsApp Web mode");
    m_directAction->setCheckable(true);
    connect(m_directAction, &QAction::toggled, this, &MainWindow::toggleDirectMode);
    connMenu->addSeparator();
    connMenu->addAction("Settings…", this, &MainWindow::openSettings);

    auto *tb = addToolBar("Main");
    tb->addAction("Refresh", this, &MainWindow::refreshChats);
    tb->addAction("QR", this, &MainWindow::showQr);
    tb->addSeparator();
    m_search = new QLineEdit(tb);
    m_search->setPlaceholderText("Search messages…");
    m_search->setMaximumWidth(240);
    connect(m_search, &QLineEdit::returnPressed, this, &MainWindow::searchMessages);
    tb->addWidget(m_search);
    tb->addAction("Log", this, [this]() { m_logDock->setVisible(!m_logDock->isVisible()); });
}

void MainWindow::applyMode() {
    const bool proxy = !m_directMode;
    m_attachBtn->setEnabled(proxy);
    m_statusLabel->setText(m_directMode ? QStringLiteral("Direct mode — WhatsApp Web")
                                        : QString("Proxy mode — %1").arg(m_baseUrl));
}

void MainWindow::start(const QString &baseUrl, const QString &apiKey, bool directMode) {
    m_baseUrl = baseUrl;
    m_apiKey = apiKey;
    m_api->setBaseUrl(baseUrl);
    m_api->setApiKey(apiKey);
    m_directMode = directMode;
    m_directAction->setChecked(directMode);
    applyMode();

    if (m_directMode) {
        m_direct->connectToWhatsApp();
    } else {
        m_api->getHealth();
        refreshChats();
        // Delta polling: the J2ME-friendly /sync endpoint, every 5 seconds.
        auto *poll = new QTimer(this);
        poll->setInterval(5000);
        connect(poll, &QTimer::timeout, this, &MainWindow::scheduleSyncPoll);
        poll->start();
    }
}

void MainWindow::scheduleSyncPoll() {
    if (!m_directMode) m_api->sync(m_lastSync);
}

// ─── Chats / messages ──────────────────────────────────────────────────────

void MainWindow::refreshChats() {
    if (m_directMode) {
        // Direct mode fills m_chats from history sync notifications.
        onChatsReady(m_chats);
        return;
    }
    m_api->getChats(200, 0, false);
}

void MainWindow::onChatsReady(const QList<Chat> &chats) {
    m_chats = chats;
    m_chatItems.clear();
    const QString selected = currentJid();

    m_chatList->blockSignals(true);
    m_chatList->clear();
    for (const Chat &c : chats) {
        const QString title = c.name.isEmpty() ? c.jid : c.name;
        auto *item = new QListWidgetItem(m_chatList);
        QString label = title;
        if (c.unreadCount > 0) label = QString("(%1) %2").arg(c.unreadCount).arg(title);
        if (c.pinned) label.prepend("📌 ");
        item->setText(label);
        item->setData(Qt::UserRole, c.jid);
        item->setToolTip(c.jid + (c.lastMessage.isEmpty() ? "" : "\n" + c.lastMessage));
        m_chatItems.insert(c.jid, item);
    }
    m_chatList->blockSignals(false);

    if (!selected.isEmpty() && m_chatItems.contains(selected)) {
        m_chatItems.value(selected)->setSelected(true);
    } else if (m_chatList->count() > 0) {
        m_chatList->item(0)->setSelected(true);
    }
    statusBar()->showMessage(QString("%1 chats").arg(chats.size()), 2000);
}

QString MainWindow::currentJid() const {
    const auto items = m_chatList->selectedItems();
    if (items.isEmpty()) return QString();
    return items.first()->data(Qt::UserRole).toString();
}

void MainWindow::onChatSelected() {
    const QString jid = currentJid();
    if (jid.isEmpty()) return;
    if (m_directMode) {
        // History for this chat arrived with the sync chunks.
        renderMessages();
        return;
    }
    if (!m_messages.contains(jid)) {
        m_api->getMessages(jid, 100);
    } else {
        renderMessages();
    }
}

void MainWindow::onMessagesReady(const QString &jid, const QList<Message> &messages) {
    m_messages.insert(jid, messages);
    if (jid == currentJid()) renderMessages();
}

void MainWindow::renderMessages() {
    const QString jid = currentJid();
    const QList<Message> messages = m_messages.value(jid);

    QString html;
    html += "<style>"
            ".m{font-family:sans-serif;font-size:11pt;margin:3px 0;}"
            ".me{text-align:right;}"
            ".meta{color:#888;font-size:8pt;}"
            ".bubble{display:inline-block;background:#e9f1fb;border-radius:8px;"
            "padding:4px 8px;max-width:75%;}"
            ".meb{background:#d9f2d0;}"
            ".quoted{border-left:3px solid #bbb;padding-left:6px;color:#555;"
            "font-size:9pt;}"
            "</style>";
    for (const Message &m : messages) {
        const QString ts = QDateTime::fromSecsSinceEpoch(m.timestamp)
                               .toString("dd MMM HH:mm");
        html += QString("<div class=\"m %1\">").arg(m.fromMe ? "me" : "");
        html += QString("<div class=\"bubble %1\">").arg(m.fromMe ? "meb" : "");
        if (!m.quoted.id.isEmpty()) {
            html += QString("<div class=\"quoted\">%1</div>")
                        .arg(m.quoted.text.isEmpty() ? "[media]" : m.quoted.text.toHtmlEscaped());
        }
        if (m.deleted) {
            html += "<i>This message was deleted</i>";
        } else if (m.hasMedia) {
            html += QString("[%1] %2").arg(m.mediaType.isEmpty() ? "media" : m.mediaType,
                                           m.fileName.toHtmlEscaped());
            if (!m.text.isEmpty()) html += "<br>" + m.text.toHtmlEscaped();
        } else {
            html += m.text.toHtmlEscaped();
        }
        if (m.edited) html += " <span class=\"meta\">(edited)</span>";
        html += "</div>";
        html += QString("<div class=\"meta\">%1%2%3</div>")
                    .arg(ts,
                         m.senderName.isEmpty() ? QString() : " · " + m.senderName.toHtmlEscaped(),
                         m.fromMe ? " · " + m.status : QString());
        html += "</div>";
    }
    m_view->setHtml(html);
    m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
}

// ─── Sending ──────────────────────────────────────────────────────────────

void MainWindow::sendCurrentMessage() {
    const QString jid = currentJid();
    const QString text = m_input->text().trimmed();
    if (jid.isEmpty() || text.isEmpty()) return;

    if (m_directMode) {
        m_direct->sendText(jid, text);
        m_input->clear();
        return;
    }
    m_api->sendText(jid, text);
    m_input->clear();

    // Optimistic local echo; the server's /sync response reconciles it.
    Message m;
    m.chatJid = jid;
    m.id = QString("local-%1").arg(QDateTime::currentMSecsSinceEpoch());
    m.fromMe = true;
    m.text = text;
    m.timestamp = QDateTime::currentDateTime().toSecsSinceEpoch();
    m.status = "pending";
    m_messages[jid].append(m);
    renderMessages();
}

void MainWindow::attachFile() {
    if (m_directMode) {
        appendLog("[direct] media upload needs the CDN crypto layer (QT_PORT.md §2.6)");
        return;
    }
    const QString jid = currentJid();
    if (jid.isEmpty()) {
        QMessageBox::information(this, "No chat", "Select a chat first.");
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, "Send file");
    if (path.isEmpty()) return;
    m_api->sendMedia(jid, path, QString(), m_input->text().trimmed());
    m_input->clear();
    statusBar()->showMessage("Uploading " + QFileInfo(path).fileName() + "…", 4000);
}

void MainWindow::onSendDone(const QJsonObject &obj) {
    statusBar()->showMessage("Sent", 2000);
    Q_UNUSED(obj)
    // Pull the authoritative row (status/media URL) for the active chat.
    if (!m_directMode && !currentJid().isEmpty()) {
        m_messages.remove(currentJid());
        m_api->getMessages(currentJid(), 100);
    }
}

void MainWindow::searchMessages() {
    const QString q = m_search->text().trimmed();
    if (q.isEmpty() || m_directMode) return;
    m_api->searchMessages(q, QString(), 100, 0);
}

void MainWindow::onSyncReady(qint64 nextSince, bool hasMore, const QList<Chat> &chats,
                             const QList<Message> &messages) {
    m_lastSync = nextSince;
    if (hasMore) m_api->sync(m_lastSync);
    if (!chats.isEmpty()) refreshChats();
    for (const Message &m : messages) {
        if (!m_messages.contains(m.chatJid)) continue;
        auto &list = m_messages[m.chatJid];
        bool replaced = false;
        for (Message &existing : list) {
            if (existing.id == m.id) { existing = m; replaced = true; break; }
        }
        if (!replaced) list.append(m);
    }
    if (!messages.isEmpty()) renderMessages();
}

// ─── QR / pairing ─────────────────────────────────────────────────────────

void MainWindow::showQr() {
    auto *dlg = new QDialog(this);
    dlg->setWindowTitle("Pair device");
    auto *lay = new QVBoxLayout(dlg);
    auto *label = new QLabel("Scan the code in WhatsApp → Linked devices", dlg);
    label->setWordWrap(true);
    lay->addWidget(label);

    auto *code = new QPlainTextEdit(dlg);
    code->setReadOnly(true);
    code->setMinimumSize(420, 120);
    lay->addWidget(code);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    lay->addWidget(buttons);

    if (m_directMode) {
        connect(m_direct, &WhatsAppDirect::qrCode, code, [code](const QString &qr) {
            // The raw pairing string. Rendering it as a QR image needs a QR
            // encoder (QZXing / qrcodegen); the string itself is what the
            // phone scans, so it is shown verbatim here.
            code->setPlainText(qr);
        });
        m_direct->requestPairingQr();
    } else {
        connect(m_api, &ApiClient::qrReady, code, [code](const QString &status,
                                                         const QString &qr) {
            if (!status.isEmpty() && status != "connected")
                code->setPlainText(QString("status: %1\n%2").arg(status, qr));
            else
                code->setPlainText(qr.isEmpty() ? "Already connected" : qr);
        });
        m_api->getQr();
    }
    dlg->exec();
}

void MainWindow::onQrReady(const QString &status, const QString &qr) {
    Q_UNUSED(status)
    Q_UNUSED(qr)
}

void MainWindow::onDirectQr(const QString &qr) {
    appendLog("[direct] pairing QR received (" + QString::number(qr.size()) + " chars)");
    m_statusLabel->setText("Direct mode — scan the QR from Connection ▸ Show QR");
}

void MainWindow::onDirectPaired(const QString &jid) {
    appendLog("[direct] paired as " + jid);
    m_statusLabel->setText("Direct mode — paired as " + jid);
}

void MainWindow::onDirectConnection(const QString &state, const QString &reason) {
    appendLog(QString("[direct] connection %1 (%2)").arg(state, reason));
    m_statusLabel->setText(QString("Direct mode — %1").arg(state));
}

void MainWindow::onDirectLog(const QString &line) { appendLog(line); }

// A chat arrived from history sync: merge it and refresh the list.
void MainWindow::onDirectChat(const Chat &chat) {
    bool replaced = false;
    for (int i = 0; i < m_chats.size(); ++i) {
        if (m_chats[i].jid != chat.jid) continue;
        Chat merged = chat;
        if (merged.name.isEmpty()) merged.name = m_chats[i].name;
        m_chats[i] = merged;
        replaced = true;
        break;
    }
    if (!replaced) m_chats.append(chat);

    // Rebuild without losing the selection.
    const QString selected = currentJid();
    onChatsReady(m_chats);
    if (!selected.isEmpty() && m_chatItems.contains(selected))
        m_chatItems.value(selected)->setSelected(true);
}

// A live message (incoming, or our own echo) for the open chat.
void MainWindow::onDirectMessage(const Message &message) {
    QList<Message> &list = m_messages[message.chatJid];
    for (const Message &existing : list) {
        if (existing.id == message.id) return;   // already rendered
    }
    list.append(message);

    // First traffic from an unknown chat: add it to the list too.
    bool known = false;
    for (const Chat &c : m_chats) {
        if (c.jid == message.chatJid) { known = true; break; }
    }
    if (!known) {
        Chat chat;
        chat.jid = message.chatJid;
        chat.id = chat.jid;
        chat.isGroup = message.chatJid.endsWith("@g.us");
        chat.name = message.senderName;
        chat.lastMessage = message.text;
        chat.lastMessageTs = message.timestamp;
        onDirectChat(chat);
    }

    if (message.chatJid == currentJid()) renderMessages();
}

// ─── Misc ─────────────────────────────────────────────────────────────────

void MainWindow::toggleDirectMode(bool on) {
    m_directMode = on;
    m_chats.clear();
    m_messages.clear();
    m_chatItems.clear();
    m_chatList->clear();
    m_view->setHtml(QString());
    applyMode();
    if (on) {
        m_direct->connectToWhatsApp();
    } else {
        m_direct->disconnectFromWhatsApp();
        m_api->getHealth();
        refreshChats();
    }
}

void MainWindow::openSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("Settings");
    auto *form = new QFormLayout(&dlg);

    auto *url = new QLineEdit(m_baseUrl, &dlg);
    auto *key = new QLineEdit(m_apiKey, &dlg);
    key->setEchoMode(QLineEdit::Password);
    auto *direct = new QCheckBox("Connect directly to WhatsApp Web", &dlg);
    direct->setChecked(m_directMode);
    auto *creds = new QLineEdit(m_direct->credentialsPath(), &dlg);
    auto *fullSync = new QCheckBox("Request full history sync", &dlg);
    fullSync->setChecked(true);

    form->addRow("Backend URL", url);
    form->addRow("API key", key);
    form->addRow(direct);
    form->addRow("Credentials file", creds);
    form->addRow(fullSync);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    m_baseUrl = url->text().trimmed();
    m_apiKey = key->text().trimmed();
    m_api->setBaseUrl(m_baseUrl);
    m_api->setApiKey(m_apiKey);
    m_direct->setCredentialsPath(creds->text().trimmed());
    m_direct->setSyncFullHistory(fullSync->isChecked());
    if (direct->isChecked() != m_directMode) {
        m_directAction->setChecked(direct->isChecked());
        toggleDirectMode(direct->isChecked());
    }
    applyMode();
    refreshChats();
}

void MainWindow::appendLog(const QString &line) {
    if (!m_log) return;
    m_log->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ") + line);
}

void MainWindow::onError(const QString &message, const QString &code, int httpStatus) {
    statusBar()->showMessage(QString("Error %1: %2 (%3)").arg(httpStatus).arg(message, code), 5000);
    appendLog(QString("[proxy] error %1: %2 [%3]").arg(httpStatus).arg(message, code));
}
