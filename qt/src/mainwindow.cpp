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
#include <QInputDialog>

#include "api_client.h"          // also brings in the QJson* shim + Qt4 helpers
#include "direct/whatsapp_direct.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_api = new ApiClient(this);
    m_direct = new WhatsAppDirect(this);

    connect(m_api, SIGNAL(chatsReady(QList<Chat>)), this, SLOT(onChatsReady(QList<Chat>)));
    connect(m_api, SIGNAL(messagesReady(QString,QList<Message>)), this,
            SLOT(onMessagesReady(QString,QList<Message>)));
    connect(m_api, SIGNAL(sendDone(QJsonObject)), this, SLOT(onSendDone(QJsonObject)));
    connect(m_api, SIGNAL(qrReady(QString,QString)), this,
            SLOT(onQrReady(QString,QString)));
    connect(m_api, SIGNAL(syncReady(qint64,bool,QList<Chat>,QList<Message>)), this,
            SLOT(onSyncReady(qint64,bool,QList<Chat>,QList<Message>)));
    connect(m_api, SIGNAL(errorOccurred(QString,QString,int)), this,
            SLOT(onError(QString,QString,int)));
    connect(m_api, SIGNAL(mediaReady(QString,QString,QByteArray,QString)), this,
            SLOT(onMediaReady(QString,QString,QByteArray,QString)));

    connect(m_direct, SIGNAL(connectionUpdate(QString,QString)), this,
            SLOT(onDirectConnection(QString,QString)));
    connect(m_direct, SIGNAL(qrCode(QString)), this, SLOT(onDirectQr(QString)));
    connect(m_direct, SIGNAL(paired(QString)), this, SLOT(onDirectPaired(QString)));
    connect(m_direct, SIGNAL(logLine(QString)), this, SLOT(onDirectLog(QString)));
    SignalHook::attach(m_direct, SIGNAL(streamError(QString,QString)),
                       [this](const QString &code, const QString &text) {
                           appendLog(QString("[direct] stream error %1: %2").arg(code, text));
                       }, false);
    SignalHook::attach(m_direct, SIGNAL(sendResult(QString,bool,QString)),
                       [this](const QString &id, bool ok, const QString &error) {
                           if (!ok)
                               appendLog(QString("[direct] send %1 failed: %2").arg(id, error));
                       }, false);
    connect(m_direct, SIGNAL(chatUpdated(Chat)), this, SLOT(onDirectChat(Chat)));
    connect(m_direct, SIGNAL(messageAdded(Message)), this, SLOT(onDirectMessage(Message)));
    connect(m_direct, SIGNAL(mediaSaved(QString,QString,QString)), this,
            SLOT(onDirectMediaSaved(QString,QString,QString)));
    connect(m_direct, SIGNAL(mediaFailed(QString,QString,QString)), this,
            SLOT(onDirectMediaFailed(QString,QString,QString)));
    connect(m_direct, SIGNAL(groupActionResult(QString,QString,bool,QString)), this,
            SLOT(onGroupAction(QString,QString,bool,QString)));
    connect(m_direct, SIGNAL(groupCreated(QString,QString)), this,
            SLOT(onGroupCreated(QString,QString)));
    connect(m_direct, SIGNAL(groupInviteCode(QString,QString)), this,
            SLOT(onGroupInviteCode(QString,QString)));
    connect(m_direct, SIGNAL(groupNotification(QString,QString,QString)), this,
            SLOT(onGroupNotification(QString,QString,QString)));
    connect(m_api, SIGNAL(groupActionDone(QString,QJsonObject)), this,
            SLOT(onGroupActionDone(QString,QJsonObject)));
    // Metadata answers: REST emits groupReady() too, so one slot serves both.
    connect(m_api, SIGNAL(groupReady(GroupInfo)), this, SLOT(onGroupReady(GroupInfo)));
    connect(m_direct, SIGNAL(groupReady(GroupInfo)), this, SLOT(onGroupReady(GroupInfo)));
    SignalHook::attach(m_direct, SIGNAL(historySyncReceived(int,int,int)),
                       [this](int progress, int chats, int messages) {
                           statusBar()->showMessage(
                               QString("History sync %1% — %2 chats, %3 messages")
                                   .arg(progress).arg(chats).arg(messages), 4000);
                       }, false);

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
    connect(m_chatList, SIGNAL(itemSelectionChanged()), this, SLOT(onChatSelected()));

    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(6, 6, 6, 6);

    m_view = new QTextBrowser(right);
    m_view->setOpenExternalLinks(true);
    rightLayout->addWidget(m_view, 1);

    auto *inputRow = new QHBoxLayout;
    m_input = new QLineEdit(right);
    m_input->setPlaceholderText("Type a message…");
    connect(m_input, SIGNAL(returnPressed()), this, SLOT(sendCurrentMessage()));
    m_attachBtn = new QPushButton("Attach", right);
    connect(m_attachBtn, SIGNAL(clicked()), this, SLOT(attachFile()));
    m_sendBtn = new QPushButton("Send", right);
    m_sendBtn->setDefault(true);
    connect(m_sendBtn, SIGNAL(clicked()), this, SLOT(sendCurrentMessage()));
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
    // QMenu::addAction(text, receiver, method) is Qt 5.6+, so actions are
    // created first and connected the Qt 4 way.
    auto *fileMenu = menuBar()->addMenu("&File");

    QAction *refresh = fileMenu->addAction("Refresh chats");
    refresh->setShortcut(QKeySequence::Refresh);
    connect(refresh, SIGNAL(triggered()), this, SLOT(refreshChats()));

    QAction *attachAction = fileMenu->addAction("Send file…");
    connect(attachAction, SIGNAL(triggered()), this, SLOT(attachFile()));

    QAction *saveMediaAction = fileMenu->addAction("Save media from chat…");
    connect(saveMediaAction, SIGNAL(triggered()), this, SLOT(saveMedia()));

    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction("Quit");
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, SIGNAL(triggered()), qApp, SLOT(quit()));

    // ── Group menu ───────────────────────────────────────────────────────
    auto *groupMenu = menuBar()->addMenu("&Group");
    struct GroupItem { const char *label; const char *slot; };
    static const GroupItem kGroupItems[] = {
        {"New group…", SLOT(groupNew())},
        {"Join with invite code…", SLOT(groupJoin())},
        {0, 0},
        {"Add participant…", SLOT(groupAddParticipant())},
        {"Remove participant…", SLOT(groupRemoveParticipant())},
        {"Make admin…", SLOT(groupPromoteParticipant())},
        {"Dismiss admin…", SLOT(groupDemoteParticipant())},
        {0, 0},
        {"Change subject…", SLOT(groupChangeSubject())},
        {"Change description…", SLOT(groupChangeDescription())},
        {0, 0},
        {"Group info…", SLOT(groupShowInfo())},
        {"Show invite code…", SLOT(groupShowInvite())},
        {"Revoke invite code", SLOT(groupRevokeInvite())},
        {0, 0},
        {"Leave group", SLOT(groupLeave())},
    };
    for (const GroupItem &item : kGroupItems) {
        if (!item.label) {
            groupMenu->addSeparator();
            continue;
        }
        QAction *action = groupMenu->addAction(QString::fromLatin1(item.label));
        connect(action, SIGNAL(triggered()), this, item.slot);
    }

    auto *connMenu = menuBar()->addMenu("&Connection");
    QAction *showQrAction = connMenu->addAction("Show QR / pair device");
    connect(showQrAction, SIGNAL(triggered()), this, SLOT(showQr()));

    m_directAction = connMenu->addAction("Direct WhatsApp Web mode");
    m_directAction->setCheckable(true);
    connect(m_directAction, SIGNAL(toggled(bool)), this, SLOT(toggleDirectMode(bool)));

    connMenu->addSeparator();
    QAction *settings = connMenu->addAction("Settings…");
    connect(settings, SIGNAL(triggered()), this, SLOT(openSettings()));

    auto *tb = addToolBar("Main");
    QAction *tbRefresh = tb->addAction("Refresh");
    connect(tbRefresh, SIGNAL(triggered()), this, SLOT(refreshChats()));
    QAction *tbQr = tb->addAction("QR");
    connect(tbQr, SIGNAL(triggered()), this, SLOT(showQr()));
    QAction *tbSave = tb->addAction("Save media");
    connect(tbSave, SIGNAL(triggered()), this, SLOT(saveMedia()));

    tb->addSeparator();
    m_search = new QLineEdit(tb);
    m_search->setPlaceholderText("Search messages…");
    m_search->setMaximumWidth(240);
    connect(m_search, SIGNAL(returnPressed()), this, SLOT(searchMessages()));
    tb->addWidget(m_search);

    QAction *logAction = tb->addAction("Log");
    connect(logAction, SIGNAL(triggered()), this, SLOT(toggleLogDock()));
}

void MainWindow::applyMode() {
    // Media works in both modes: proxy mode posts to the backend, direct mode
    // encrypts and uploads to the WhatsApp CDN.
    m_attachBtn->setEnabled(true);
    m_statusLabel->setText(m_directMode ? QStringLiteral("Direct mode — WhatsApp Web")
                                        : QString("Proxy mode — %1").arg(m_baseUrl));
}

void MainWindow::setCredentialsPath(const QString &path) {
    m_pendingCredsPath = path;
    if (m_direct && !path.isEmpty()) m_direct->setCredentialsPath(path);
}

void MainWindow::start(const QString &baseUrl, const QString &apiKey, bool directMode) {
    if (!m_pendingCredsPath.isEmpty()) m_direct->setCredentialsPath(m_pendingCredsPath);
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
        connect(poll, SIGNAL(timeout()), this, SLOT(scheduleSyncPoll()));
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
        const QString ts = QDateTime::fromTime_t(uint(m.timestamp))
                               .toString("dd MMM HH:mm");
        html += QString("<div class=\"m %1\">").arg(m.fromMe ? "me" : "");
        html += QString("<div class=\"bubble %1\">").arg(m.fromMe ? "meb" : "");
        if (!m.quoted.id.isEmpty()) {
            html += QString("<div class=\"quoted\">%1</div>")
                        .arg(htmlEscaped(m.quoted.text.isEmpty() ? QString("[media]")
                                                                 : m.quoted.text));
        }
        if (m.deleted) {
            html += "<i>This message was deleted</i>";
        } else if (m.hasMedia) {
            html += QString("[%1] %2").arg(m.mediaType.isEmpty() ? "media" : m.mediaType,
                                           htmlEscaped(m.fileName));
            if (!m.text.isEmpty()) html += "<br>" + htmlEscaped(m.text);
        } else {
            html += htmlEscaped(m.text);
        }
        if (m.edited) html += " <span class=\"meta\">(edited)</span>";
        html += "</div>";
        html += QString("<div class=\"meta\">%1%2%3</div>")
                    .arg(ts,
                         m.senderName.isEmpty() ? QString()
                                                : " · " + htmlEscaped(m.senderName),
                         m.fromMe ? " · " + htmlEscaped(m.status) : QString());
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
    m.timestamp = qt_now_secs();
    m.status = "pending";
    m_messages[jid].append(m);
    renderMessages();
}

void MainWindow::attachFile() {
    const QString jid = currentJid();
    if (jid.isEmpty()) {
        QMessageBox::information(this, "No chat", "Select a chat first.");
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, "Send file");
    if (path.isEmpty()) return;
    const QString caption = m_input->text().trimmed();

    if (m_directMode) {
        // Direct mode encrypts + uploads to the WhatsApp CDN itself.
        m_direct->sendMedia(jid, path, caption);
    } else {
        m_api->sendMedia(jid, path, QString(), caption);
    }
    m_input->clear();
    statusBar()->showMessage("Uploading " + QFileInfo(path).fileName() + "…", 4000);
}

// Downloads the newest media message of the current chat.
void MainWindow::saveMedia() {
    const QString jid = currentJid();
    if (jid.isEmpty()) {
        QMessageBox::information(this, "No chat", "Select a chat first.");
        return;
    }
    QString msgId;
    const QList<Message> messages = m_messages.value(jid);
    for (int i = messages.size() - 1; i >= 0; i--) {
        if (!messages.at(i).hasMedia) continue;
        msgId = messages.at(i).id;
        break;
    }
    if (msgId.isEmpty()) {
        QMessageBox::information(this, "No media",
                                 "This chat has no media messages to download.");
        return;
    }

    QString suggested = QDir::homePath() + "/" + "openknot-media";
    const QString path = QFileDialog::getSaveFileName(this, "Save media", suggested);
    if (path.isEmpty()) return;

    m_pendingSavePath = path;
    m_pendingSaveMsgId = msgId;
    statusBar()->showMessage("Downloading media…", 4000);
    if (m_directMode)
        m_direct->downloadMedia(jid, msgId, path);
    else
        m_api->downloadMedia(jid, msgId);
}

void MainWindow::onMediaReady(const QString &jid, const QString &msgId,
                              const QByteArray &bytes, const QString &mime) {
    Q_UNUSED(jid)
    Q_UNUSED(mime)
    if (msgId != m_pendingSaveMsgId || m_pendingSavePath.isEmpty()) return;
    QFile out(m_pendingSavePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_pendingSavePath.clear();
        m_pendingSaveMsgId.clear();
        QMessageBox::warning(this, "Save failed", "Cannot write " + m_pendingSavePath);
        return;
    }
    out.write(bytes);
    out.close();
    statusBar()->showMessage("Saved " + m_pendingSavePath, 4000);
    m_pendingSavePath.clear();
    m_pendingSaveMsgId.clear();
}

void MainWindow::onDirectMediaSaved(const QString &chatJid, const QString &msgId,
                                    const QString &path) {
    Q_UNUSED(chatJid)
    Q_UNUSED(msgId)
    statusBar()->showMessage("Saved " + path, 4000);
    m_pendingSavePath.clear();
    m_pendingSaveMsgId.clear();
}

void MainWindow::onDirectMediaFailed(const QString &chatJid, const QString &msgId,
                                     const QString &error) {
    Q_UNUSED(chatJid)
    Q_UNUSED(msgId)
    m_pendingSavePath.clear();
    m_pendingSaveMsgId.clear();
    statusBar()->showMessage("Media download failed: " + error, 6000);
    appendLog("[direct] media download failed: " + error);
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

void MainWindow::toggleLogDock() {
    if (m_logDock) m_logDock->setVisible(!m_logDock->isVisible());
}

void MainWindow::onQrDialogCode(const QString &qr) {
    if (m_qrCodeView) m_qrCodeView->setPlainText(qr);
}

void MainWindow::onQrDialogCodeStatus(const QString &status, const QString &qr) {
    if (!m_qrCodeView) return;
    if (!status.isEmpty() && status != "connected")
        m_qrCodeView->setPlainText(QString("status: %1\n%2").arg(status, qr));
    else
        m_qrCodeView->setPlainText(qr.isEmpty() ? QString("Already connected") : qr);
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

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, dlg);
    connect(buttons, SIGNAL(rejected()), dlg, SLOT(reject()));
    lay->addWidget(buttons);

    if (m_directMode) {
        // The raw pairing string. Rendering it as a QR image needs a QR
        // encoder (QZXing / qrcodegen); the string itself is what the phone
        // scans, so it is shown verbatim here.
        m_qrCodeView = code;
        connect(m_direct, SIGNAL(qrCode(QString)), this, SLOT(onQrDialogCode(QString)),
                Qt::UniqueConnection);
        m_direct->requestPairingQr();
    } else {
        m_qrCodeView = code;
        connect(m_api, SIGNAL(qrReady(QString,QString)), this,
                SLOT(onQrDialogCodeStatus(QString,QString)), Qt::UniqueConnection);
        m_api->getQr();
    }
    dlg->exec();
    m_qrCodeView = 0;
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
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                              Qt::Horizontal, &dlg);
    form->addRow(buttons);
    connect(buttons, SIGNAL(accepted()), &dlg, SLOT(accept()));
    connect(buttons, SIGNAL(rejected()), &dlg, SLOT(reject()));

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

// ─── Group administration ──────────────────────────────────────────────────
//
// The same operations exist in both modes: proxy mode posts to /groups/…,
// direct mode sends the <iq xmlns=w:g2> stanzas. The dialogs are deliberately
// plain (Qt 4.7 has no fancy pickers on Symbian either): a group name, a list
// of phone numbers, or a single line of text.

namespace {

// "+91 99999 99999", "919999999999" and "…@s.whatsapp.net" all become a jid.
QString asJid(const QString &input) {
    QString v = input.trimmed();
    if (v.isEmpty() || v.contains('@')) return v;
    if (v.startsWith('+')) v = v.mid(1);
    v.remove(' ').remove('-').remove('(').remove(')');
    if (v.isEmpty()) return v;
    return v + "@s.whatsapp.net";
}

} // namespace

QStringList MainWindow::askParticipants(const QString &title, const QString &prompt) {
    bool ok = false;
    const QString text = QInputDialog::getText(this, title, prompt, QLineEdit::Normal,
                                               QString(), &ok);
    QStringList out;
    if (!ok) return out;
    QString normalized = text;
    normalized.replace(';', ',');
    normalized.replace('\n', ',');
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QStringList parts = normalized.split(',', Qt::SkipEmptyParts);
#else
    const QStringList parts = normalized.split(',', QString::SkipEmptyParts);
#endif
    for (const QString &part : parts) {
        const QString jid = asJid(part);
        if (!jid.isEmpty()) out << jid;
    }
    return out;
}

QString MainWindow::currentGroupJid() const {
    const QString jid = currentJid();
    if (jid.isEmpty() || !jid.endsWith("@g.us")) {
        statusBar()->showMessage("Select a group chat first", 4000);
        return QString();
    }
    return jid;
}

void MainWindow::groupNew() {
    bool ok = false;
    const QString subject = QInputDialog::getText(this, "New group", "Group name:",
                                                  QLineEdit::Normal, QString(), &ok);
    if (!ok || subject.trimmed().isEmpty()) return;
    const QStringList participants =
        askParticipants("New group", "Participants (phone numbers, comma separated):");
    if (participants.isEmpty()) {
        statusBar()->showMessage("A group needs at least one participant", 4000);
        return;
    }
    if (m_directMode) m_direct->createGroup(subject.trimmed(), participants);
    else m_api->createGroup(subject.trimmed(), participants);
    statusBar()->showMessage("Creating group…", 3000);
}

void MainWindow::groupJoin() {
    bool ok = false;
    const QString code = QInputDialog::getText(
        this, "Join group", "Invite code or https://chat.whatsapp.com/… link:",
        QLineEdit::Normal, QString(), &ok);
    if (!ok || code.trimmed().isEmpty()) return;
    if (m_directMode) m_direct->joinGroup(code.trimmed());
    else m_api->joinGroup(code.trimmed());
    statusBar()->showMessage("Joining group…", 3000);
}

void MainWindow::changeGroupParticipants(const QString &action) {
    const QString jid = currentGroupJid();
    if (jid.isEmpty()) return;
    const QString verb = action == QString("add")        ? QString("Add")
                         : action == QString("remove")   ? QString("Remove")
                         : action == QString("promote")  ? QString("Promote")
                                                         : QString("Demote");
    const QStringList participants = askParticipants(
        verb + " participant", "Phone numbers or JIDs (comma separated):");
    if (participants.isEmpty()) return;
    if (m_directMode) m_direct->updateGroupParticipants(jid, participants, action);
    else m_api->updateGroupParticipants(jid, participants, action);
    statusBar()->showMessage(verb + "ing " + QString::number(participants.size()) +
                                 " participant(s)…", 3000);
}

void MainWindow::groupAddParticipant() { changeGroupParticipants("add"); }
void MainWindow::groupRemoveParticipant() { changeGroupParticipants("remove"); }
void MainWindow::groupPromoteParticipant() { changeGroupParticipants("promote"); }
void MainWindow::groupDemoteParticipant() { changeGroupParticipants("demote"); }

void MainWindow::groupChangeSubject() {
    const QString jid = currentGroupJid();
    if (jid.isEmpty()) return;
    bool ok = false;
    const QString subject = QInputDialog::getText(this, "Group subject", "New subject:",
                                                  QLineEdit::Normal, QString(), &ok);
    if (!ok || subject.trimmed().isEmpty()) return;
    if (m_directMode) m_direct->setGroupSubject(jid, subject.trimmed());
    else m_api->updateGroupSubject(jid, subject.trimmed());
}

void MainWindow::groupChangeDescription() {
    const QString jid = currentGroupJid();
    if (jid.isEmpty()) return;
    // QInputDialog::getMultiLineText is Qt 5, so the text box is built here.
    QDialog dialog(this);
    dialog.setWindowTitle("Group description");
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Description (empty removes it):", &dialog));
    QPlainTextEdit *edit = new QPlainTextEdit(&dialog);
    edit->setMinimumSize(360, 140);
    layout->addWidget(edit);
    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
    layout->addWidget(buttons);
    connect(buttons, SIGNAL(accepted()), &dialog, SLOT(accept()));
    connect(buttons, SIGNAL(rejected()), &dialog, SLOT(reject()));
    if (dialog.exec() != QDialog::Accepted) return;
    const QString description = edit->toPlainText();
    if (m_directMode) m_direct->setGroupDescription(jid, description);
    else m_api->updateGroupDescription(jid, description);
}

void MainWindow::groupLeave() {
    const QString jid = currentGroupJid();
    if (jid.isEmpty()) return;
    if (QMessageBox::question(this, "Leave group", "Leave this group?",
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    if (m_directMode) m_direct->leaveGroup(jid);
    else m_api->leaveGroup(jid);
}

void MainWindow::groupShowInfo() {
    const QString jid = currentGroupJid();
    if (jid.isEmpty()) return;
    statusBar()->showMessage("Loading group info…", 3000);
    if (m_directMode) m_direct->requestGroupInfo(jid);
    else m_api->getGroup(jid);
}

void MainWindow::onGroupReady(const GroupInfo &group) {
    QStringList lines;
    lines << "Subject: " + group.subject;
    if (!group.owner.isEmpty()) lines << "Owner: " + group.owner;
    if (!group.description.isEmpty())
        lines << "Description: " + group.description;
    lines << QString("Members: %1").arg(group.participants.size());
    lines << "";
    for (const GroupParticipant &p : group.participants) {
        QString line = "• " + p.jid;
        if (!p.name.isEmpty()) line += " (" + p.name + ")";
        if (p.admin == QString("superadmin")) line += " — superadmin";
        else if (p.admin == QString("admin")) line += " — admin";
        lines << line;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Group info");
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QString("<b>%1</b>").arg(htmlEscaped(group.subject)),
                                 &dialog));
    QPlainTextEdit *view = new QPlainTextEdit(&dialog);
    view->setReadOnly(true);
    view->setMinimumSize(420, 300);
    view->setPlainText(lines.join("\n"));
    layout->addWidget(view);
    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, &dialog);
    layout->addWidget(buttons);
    connect(buttons, SIGNAL(rejected()), &dialog, SLOT(reject()));
    connect(buttons, SIGNAL(clicked(QAbstractButton*)), &dialog, SLOT(accept()));
    dialog.exec();
}

void MainWindow::groupShowInvite() {
    const QString jid = currentGroupJid();
    if (jid.isEmpty()) return;
    if (m_directMode) m_direct->requestGroupInvite(jid);
    else m_api->getGroupInvite(jid);
}

void MainWindow::groupRevokeInvite() {
    const QString jid = currentGroupJid();
    if (jid.isEmpty()) return;
    if (QMessageBox::question(this, "Revoke invite",
                              "Reset the invite link? The old link stops working.",
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    if (m_directMode) m_direct->revokeGroupInvite(jid);
    else m_api->revokeGroupInvite(jid);
}

void MainWindow::onGroupAction(const QString &action, const QString &jid, bool ok,
                               const QString &error) {
    if (!ok) {
        statusBar()->showMessage(QString("Group %1 failed: %2").arg(action, error), 6000);
        appendLog(QString("[group] %1 %2 failed: %3").arg(action, jid, error));
        return;
    }
    statusBar()->showMessage(QString("Group %1 done").arg(action), 3000);
    appendLog(QString("[group] %1 %2 ok").arg(action, jid));
}

void MainWindow::onGroupNotification(const QString &groupJid, const QString &kind,
                                     const QString &detail) {
    appendLog(QString("[group] %1 %2: %3").arg(groupJid, kind, detail));
    if (kind == "subject" || kind == "create")
        statusBar()->showMessage(QString("Group renamed: %1").arg(detail), 4000);
    else
        statusBar()->showMessage(QString("Group update: %1 %2").arg(kind, detail), 4000);
}

void MainWindow::onGroupCreated(const QString &groupJid, const QString &subject) {
    // The direct backend also emits chatUpdated(), which adds the chat; the
    // proxy backend answers with the new group in groupActionDone().
    statusBar()->showMessage(QString("Group \"%1\" ready").arg(subject), 4000);
    appendLog(QString("[group] created %1 (%2)").arg(groupJid, subject));
    refreshChats();
}

void MainWindow::onGroupInviteCode(const QString &groupJid, const QString &code) {
    const QString link = "https://chat.whatsapp.com/" + code;
    QMessageBox::information(this, "Group invite link",
                             QString("Invite code for %1:\n\n%2\n\n%3")
                                 .arg(groupJid, code, link));
    appendLog(QString("[group] invite code %1").arg(code));
}

void MainWindow::onGroupActionDone(const QString &action, const QJsonObject &obj) {
    // Proxy mode: /groups/… answered. Show the invite link, refresh the list.
    const QString code = obj.value("code").toString();
    const QString jid = obj.value("jid").toString();
    if (action == "invite" || action == "revoke-invite") {
        if (!code.isEmpty())
            onGroupInviteCode(jid, code);
        else
            statusBar()->showMessage("No invite code in the reply", 4000);
    } else {
        statusBar()->showMessage(QString("Group %1 done").arg(action), 3000);
    }
    appendLog(QString("[group] %1 %2").arg(action, jid));
    refreshChats();
}
