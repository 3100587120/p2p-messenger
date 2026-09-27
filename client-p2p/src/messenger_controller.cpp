#include "messenger_controller.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QGuiApplication>
#include <QClipboard>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#endif

namespace {
QVariantMap contact(const QString& id, const QString& name, const QString& status)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("initial"), name.left(1).toUpper()},
            {QStringLiteral("status"), status}};
}
}

MessengerController::MessengerController(QObject* parent)
    : QObject(parent)
    , networkStatus_(tr("仅本地模式 — 尚未配置自建服务"))
{
    connect(&daemon_, &DaemonBridge::incomingMessage, this,
            [this](const QString& conversationId, const QString& body,
                   const QString& interactionId, bool outgoing) {
                for (const auto& item : contacts_) {
                    if (item.toMap().value(QStringLiteral("conversationId")).toString() == conversationId) {
                        const auto contactId = item.toMap().value(QStringLiteral("id")).toString();
                        const auto stored = contactId == activeContactId_ ? messages_ : vault_.loadConversation(contactId);
                        for (const auto& previous : stored) {
                            if (!interactionId.isEmpty() &&
                                previous.toMap().value(QStringLiteral("interactionId")).toString() == interactionId)
                                return;
                        }
                        storeMessageForContact(contactId,
                            {{QStringLiteral("body"), body},
                             {QStringLiteral("outgoing"), outgoing},
                             {QStringLiteral("kind"), QStringLiteral("text")},
                             {QStringLiteral("interactionId"), interactionId},
                             {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))}});
                        return;
                    }
                }
            });
    connect(&daemon_, &DaemonBridge::incomingFile, this,
            [this](const QString& conversationId, const QString& interactionId,
                   const QString& fileId, const QString& name) {
                for (const auto& item : contacts_) {
                    const auto contact = item.toMap();
                    if (contact.value(QStringLiteral("conversationId")).toString() != conversationId)
                        continue;
                    const QVariantMap message {{QStringLiteral("body"), tr("文件：%1").arg(name)},
                                               {QStringLiteral("outgoing"), false},
                                               {QStringLiteral("kind"), QStringLiteral("file-offer")},
                                               {QStringLiteral("interactionId"), interactionId},
                                               {QStringLiteral("fileId"), fileId},
                                               {QStringLiteral("name"), name},
                                               {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))}};
                    storeMessageForContact(contact.value(QStringLiteral("id")).toString(), message);
                    break;
                }
            });
    connect(&daemon_, &DaemonBridge::transferChanged, this,
            [this](const QString& conversationId, const QString& fileId,
                   int eventCode, qint64 progress, qint64 total) {
                bool documentWriteFailed = false;
#ifdef Q_OS_ANDROID
                const auto transferKey = conversationId + QLatin1Char('\n') + fileId;
                if (androidDownloadDestinations_.contains(transferKey) &&
                    (eventCode == 6 || eventCode >= 7 || eventCode == 2)) {
                    const auto destination = androidDownloadDestinations_.take(transferKey);
                    const auto localPath = androidDownloadPaths_.take(transferKey);
                    if (eventCode == 6) {
                        const auto context = QNativeInterface::QAndroidApplication::context();
                        const auto javaDestination = QJniObject::fromString(destination);
                        const bool saved = QJniObject::callStaticMethod<jboolean>(
                            "io/p2pmessenger/app/ContentFiles", "copyFromCache",
                            "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z",
                            context.object<jobject>(), QJniObject::fromString(localPath).object<jstring>(),
                            javaDestination.object<jstring>());
                        if (!saved) {
                            documentWriteFailed = true;
                            setError(tr("文件已收到，但无法写入选定位置"));
                        }
                    }
                    QFile::remove(localPath);
                }
#endif
                for (const auto& item : contacts_) {
                    const auto contact = item.toMap();
                    if (contact.value(QStringLiteral("conversationId")).toString() != conversationId)
                        continue;
                    const auto contactId = contact.value(QStringLiteral("id")).toString();
                    auto stored = contactId == activeContactId_ ? messages_ : vault_.loadConversation(contactId);
                    for (auto& value : stored) {
                        auto message = value.toMap();
                        if (message.value(QStringLiteral("fileId")).toString() != fileId) continue;
                        QString state;
                        if (documentWriteFailed) state = tr("保存失败");
                        else if (eventCode == 6) state = tr("已完成");
                        else if (eventCode == 5 && total > 0)
                            state = tr("传输中 %1%").arg(progress * 100 / total);
                        else if (eventCode >= 7 || eventCode == 2)
                            state = tr("传输失败");
                        else state = tr("等待连接");
                        message.insert(QStringLiteral("body"),
                                       tr("文件：%1（%2）").arg(message.value(QStringLiteral("name")).toString(), state));
                        value = message;
                    }
                    vault_.saveConversation(contactId, stored);
                    if (contactId == activeContactId_) {
                        messages_ = stored;
                        emit messagesChanged();
                    }
                    break;
                }
            });
    connect(&daemon_, &DaemonBridge::identityChanged, this,
            [this](const QString& accountId, const QString& code) {
                if (accountId != accountId_ || code == inviteCode_) return;
                inviteCode_ = code;
                emit inviteCodeChanged();
            });
    connect(&daemon_, &DaemonBridge::friendRequestReceived, this,
            [this](const QString& accountId, const QString& contactUri) {
                if (accountId != accountId_ || pendingRequests_.contains(contactUri)) return;
                pendingRequests_.append(contactUri);
                emit pendingRequestsChanged();
            });
    connect(&daemon_, &DaemonBridge::groupRequestReceived, this,
            [this](const QString& accountId, const QString& conversationId) {
                if (accountId != accountId_ || pendingGroupRequests_.contains(conversationId)) return;
                pendingGroupRequests_.append(conversationId);
                emit pendingGroupRequestsChanged();
            });
    connect(&daemon_, &DaemonBridge::contactConfirmed, this,
            [this](const QString& accountId, const QString& contactUri, bool confirmed) {
                if (accountId != accountId_ || !confirmed) return;
                for (auto& item : contacts_) {
                    auto entry = item.toMap();
                    if (entry.value(QStringLiteral("uri")).toString() != contactUri) continue;
                    entry.insert(QStringLiteral("status"), tr("已验证"));
                    entry.insert(QStringLiteral("ready"), true);
                    item = entry;
                    saveProfile();
                    emit contactsChanged();
                    break;
                }
            });
    connect(&daemon_, &DaemonBridge::conversationReady, this,
            [this](const QString& accountId, const QString& conversationId) {
                if (accountId != accountId_) return;
                for (auto& item : contacts_) {
                    auto entry = item.toMap();
                    const auto savedConversation = entry.value(QStringLiteral("conversationId")).toString();
                    if (savedConversation != conversationId) {
                        if (!savedConversation.isEmpty()) continue;
                        const auto uri = entry.value(QStringLiteral("uri")).toString();
                        if (uri.isEmpty() || daemon_.createConversation(accountId_, uri) != conversationId)
                            continue;
                    }
                    entry.insert(QStringLiteral("conversationId"), conversationId);
                    entry.insert(QStringLiteral("ready"), true);
                    if (entry.value(QStringLiteral("status")).toString() == tr("正在加入群聊"))
                        entry.insert(QStringLiteral("status"), tr("群聊"));
                    else if (entry.value(QStringLiteral("status")).toString() == tr("正在建立会话"))
                        entry.insert(QStringLiteral("status"), tr("已验证"));
                    item = entry;
                    saveProfile();
                    emit contactsChanged();
                    break;
                }
            });
    if (daemon_.start()) {
        accountId_ = daemon_.createLocalIdentity(tr("我的设备"));
        inviteCode_ = daemon_.inviteCode(accountId_);
        networkStatus_ = accountId_.isEmpty() ? tr("创建本机身份失败")
                                             : tr("本机身份就绪 — 尚未配置自建引导节点");
    } else {
        networkStatus_ = tr("通信内核未启动");
    }
    const auto savedProfile = vault_.loadConversation(QStringLiteral("__profile"));
    if (!savedProfile.isEmpty()) {
        const auto profile = savedProfile.first().toMap();
        if (profile.value(QStringLiteral("accountId")).toString() == accountId_) {
            contacts_ = profile.value(QStringLiteral("contacts")).toList();
            const auto network = profile.value(QStringLiteral("network")).toMap();
            networkConfig_.bootstrapNode = network.value(QStringLiteral("bootstrapNode")).toString();
            networkConfig_.turnHost = network.value(QStringLiteral("turnHost")).toString();
            networkConfig_.turnPort = static_cast<quint16>(network.value(QStringLiteral("turnPort"), 3478).toUInt());
            networkConfig_.turnUser = network.value(QStringLiteral("turnUser")).toString();
            networkConfig_.turnPassword = network.value(QStringLiteral("turnPassword")).toString();
        }
    }
    if (!accountId_.isEmpty() && networkConfig_.isValid()) {
        daemon_.configurePrivateNetwork(accountId_, networkConfig_);
        if (!networkConfig_.isEmpty())
            networkStatus_ = tr("已设置自建引导节点 — 直连优先");
    }
    if (contacts_.isEmpty())
        contacts_.append(contact(QStringLiteral("welcome"), tr("开始使用"), tr("本设备")));
    if (!accountId_.isEmpty())
        pendingRequests_ = daemon_.pendingFriendRequests(accountId_);
    if (!accountId_.isEmpty())
        pendingGroupRequests_ = daemon_.pendingGroupRequests(accountId_);
    activeContactId_ = QStringLiteral("welcome");
    messages_ = vault_.loadConversation(activeContactId_);
    if (messages_.isEmpty())
        appendMessage(tr("欢迎使用 P2P Messenger。创建或扫描好友邀请码后，即可建立端到端加密连接。"), false);
}

MessengerController::~MessengerController()
{
    daemon_.stop();
}

QVariantList MessengerController::contacts() const { return contacts_; }
QVariantList MessengerController::messages() const { return messages_; }
QString MessengerController::activeContactId() const { return activeContactId_; }
QString MessengerController::activeContactName() const { return contactName(activeContactId_); }
QString MessengerController::networkStatus() const { return networkStatus_; }
QString MessengerController::inviteCode() const { return inviteCode_; }
QString MessengerController::lastError() const { return lastError_; }
QStringList MessengerController::pendingRequests() const { return pendingRequests_; }
QStringList MessengerController::pendingGroupRequests() const { return pendingGroupRequests_; }

void MessengerController::selectContact(const QString& contactId)
{
    if (activeContactId_ == contactId)
        return;
    activeContactId_ = contactId;
    messages_ = vault_.loadConversation(activeContactId_);
    emit activeContactChanged();
    emit messagesChanged();
}

bool MessengerController::addContact(const QString& name, const QString& invite)
{
    const auto trimmedName = name.trimmed();
    if (trimmedName.isEmpty() || invite.trimmed().isEmpty())
        return false;
    if (accountId_.isEmpty() || inviteCode_.isEmpty()) {
        setError(tr("本机身份仍在生成，稍后再添加好友"));
        return false;
    }
    if (!daemon_.addVerifiedContact(accountId_, invite.trimmed())) {
        setError(tr("好友申请未能提交"));
        return false;
    }
    const auto conversationId = daemon_.createConversation(accountId_, invite.trimmed());
    if (conversationId.isEmpty()) {
        setError(tr("无法创建私聊会话"));
        return false;
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto entry = contact(id, trimmedName, tr("等待对方确认"));
    entry.insert(QStringLiteral("uri"), invite.trimmed());
    entry.insert(QStringLiteral("conversationId"), conversationId);
    entry.insert(QStringLiteral("ready"), false);
    contacts_.append(entry);
    saveProfile();
    emit contactsChanged();
    selectContact(id);
    return true;
}

bool MessengerController::createGroup(const QString& name, const QStringList& memberUris)
{
    const auto groupName = name.trimmed();
    if (groupName.isEmpty())
        return false;
    if (accountId_.isEmpty() || inviteCode_.isEmpty()) {
        setError(tr("本机身份仍在生成，稍后再创建群聊"));
        return false;
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto conversationId = daemon_.createEmptyConversation(accountId_);
    if (conversationId.isEmpty()) {
        setError(tr("无法创建群聊会话"));
        return false;
    }
    daemon_.setConversationTitle(accountId_, conversationId, groupName);
    for (const auto& uri : memberUris) {
        const auto member = uri.trimmed();
        if (!member.isEmpty() && !accountId_.isEmpty())
            daemon_.addGroupMember(accountId_, conversationId, member);
    }
    auto entry = contact(id, groupName, tr("群聊"));
    entry.insert(QStringLiteral("initial"), tr("群"));
    entry.insert(QStringLiteral("conversationId"), conversationId);
    entry.insert(QStringLiteral("group"), true);
    entry.insert(QStringLiteral("ready"), true);
    contacts_.append(entry);
    saveProfile();
    emit contactsChanged();
    selectContact(id);
    return true;
}

bool MessengerController::sendMessage(const QString& body)
{
    const auto text = body.trimmed();
    if (text.isEmpty())
        return false;
    for (const auto& item : contacts_) {
        const auto current = item.toMap();
        if (current.value(QStringLiteral("id")).toString() != activeContactId_)
            continue;
        const auto conversationId = current.value(QStringLiteral("conversationId")).toString();
        if (accountId_.isEmpty() || conversationId.isEmpty() ||
            !current.value(QStringLiteral("ready"), true).toBool()) {
            setError(tr("会话尚未就绪，请等对方确认并完成连接"));
            return false;
        }
        if (!daemon_.sendText(accountId_, conversationId, text)) {
            setError(tr("消息未能提交给通信内核"));
            return false;
        }
        return true;
    }
    setError(tr("请选择已经建立的会话"));
    return false;
}

bool MessengerController::queueFile(const QString& path)
{
    auto localPath = path.startsWith(QStringLiteral("file:")) ? QUrl(path).toLocalFile() : path;
#ifdef Q_OS_ANDROID
    if (path.startsWith(QStringLiteral("content:"))) {
        const auto context = QNativeInterface::QAndroidApplication::context();
        const auto javaPath = QJniObject::fromString(path);
        localPath = QJniObject::callStaticObjectMethod(
            "io/p2pmessenger/app/ContentFiles", "copyToCache",
            "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;",
            context.object<jobject>(), javaPath.object<jstring>()).toString();
    }
#endif
    if (localPath.isEmpty() || !QFileInfo::exists(localPath)) {
        setError(tr("找不到要发送的文件"));
        return false;
    }
    for (const auto& item : contacts_) {
        const auto current = item.toMap();
        if (current.value(QStringLiteral("id")).toString() != activeContactId_)
            continue;
        const auto conversationId = current.value(QStringLiteral("conversationId")).toString();
        if (accountId_.isEmpty() || conversationId.isEmpty() ||
            !current.value(QStringLiteral("ready"), true).toBool()) {
            setError(tr("会话尚未就绪，请等对方确认并完成连接"));
            return false;
        }
        if (!daemon_.sendFile(accountId_, conversationId, localPath)) {
            setError(tr("文件未能提交给通信内核"));
            return false;
        }
        appendMessage(tr("文件：%1（等待端到端传输）").arg(QFileInfo(localPath).fileName()), true,
                      QStringLiteral("file"));
        return true;
    }
    setError(tr("请选择已经建立的会话"));
    return false;
}

bool MessengerController::downloadFile(const QString& interactionId, const QString& fileId,
                                       const QString& destination)
{
    auto localPath = destination.startsWith(QStringLiteral("file:"))
        ? QUrl(destination).toLocalFile() : destination;
#ifdef Q_OS_ANDROID
    const bool androidDocument = destination.startsWith(QStringLiteral("content:"));
    if (androidDocument) {
        const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(cache);
        localPath = cache + QStringLiteral("/incoming-") +
                    QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
#endif
    if (localPath.isEmpty() || QFileInfo::exists(localPath)) {
        setError(tr("请选择一个尚不存在的保存路径"));
        return false;
    }
    for (const auto& item : contacts_) {
        const auto contact = item.toMap();
        if (contact.value(QStringLiteral("id")).toString() != activeContactId_) continue;
        const auto conversationId = contact.value(QStringLiteral("conversationId")).toString();
        if (conversationId.isEmpty() || !daemon_.downloadFile(accountId_, conversationId,
                                                               interactionId, fileId, localPath)) {
            setError(tr("无法开始下载文件"));
            return false;
        }
#ifdef Q_OS_ANDROID
        if (androidDocument) {
            const auto key = conversationId + QLatin1Char('\n') + fileId;
            androidDownloadDestinations_.insert(key, destination);
            androidDownloadPaths_.insert(key, localPath);
        }
#endif
        return true;
    }
    return false;
}

void MessengerController::copyInviteCode()
{
    if (!inviteCode_.isEmpty())
        QGuiApplication::clipboard()->setText(inviteCode_);
}

bool MessengerController::acceptFriendRequest(const QString& contactUri)
{
    if (!pendingRequests_.contains(contactUri)) return false;
    if (!daemon_.acceptFriendRequest(accountId_, contactUri)) {
        setError(tr("无法接受好友申请"));
        return false;
    }
    pendingRequests_.removeAll(contactUri);
    emit pendingRequestsChanged();
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto entry = contact(id, contactUri.left(12), tr("正在建立会话"));
    entry.insert(QStringLiteral("uri"), contactUri);
    entry.insert(QStringLiteral("conversationId"), daemon_.createConversation(accountId_, contactUri));
    entry.insert(QStringLiteral("ready"), false);
    contacts_.append(entry);
    saveProfile();
    emit contactsChanged();
    selectContact(id);
    return true;
}

bool MessengerController::acceptGroupRequest(const QString& conversationId)
{
    if (!pendingGroupRequests_.contains(conversationId)) return false;
    auto title = daemon_.groupRequestTitle(accountId_, conversationId).trimmed();
    if (title.isEmpty()) title = tr("群聊 %1").arg(conversationId.left(8));
    if (!daemon_.acceptGroupRequest(accountId_, conversationId)) {
        setError(tr("无法接受群聊邀请"));
        return false;
    }
    pendingGroupRequests_.removeAll(conversationId);
    emit pendingGroupRequestsChanged();
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto entry = contact(id, title, tr("正在加入群聊"));
    entry.insert(QStringLiteral("initial"), tr("群"));
    entry.insert(QStringLiteral("conversationId"), conversationId);
    entry.insert(QStringLiteral("group"), true);
    entry.insert(QStringLiteral("ready"), false);
    contacts_.append(entry);
    saveProfile();
    emit contactsChanged();
    selectContact(id);
    return true;
}

bool MessengerController::configureNetwork(const QString& rendezvous, const QString& turnHost,
                                            int turnPort, const QString& user, const QString& password)
{
    if (turnPort < 1 || turnPort > 65535) {
        setError(tr("TURN 端口必须在 1 到 65535 之间"));
        return false;
    }
    PrivateNetworkConfig config {rendezvous.trimmed(), turnHost.trimmed(), static_cast<quint16>(turnPort), user, password};
    QString reason;
    if (!config.isValid(&reason)) {
        setError(reason);
        return false;
    }
    if (accountId_.isEmpty() || !daemon_.configurePrivateNetwork(accountId_, config)) {
        setError(tr("自建网络设置未能应用"));
        return false;
    }
    networkConfig_ = config;
    saveProfile();
    networkStatus_ = config.isEmpty() ? tr("仅局域网发现") : tr("已设置自建引导节点 — 直连优先");
    emit networkStatusChanged();
    return true;
}

void MessengerController::appendMessage(const QString& body, bool outgoing, const QString& kind)
{
    appendMessageForContact(activeContactId_, body, outgoing, kind);
}

void MessengerController::appendMessageForContact(const QString& contactId, const QString& body,
                                                   bool outgoing, const QString& kind)
{
    const QVariantMap message {{QStringLiteral("body"), body},
                               {QStringLiteral("outgoing"), outgoing},
                               {QStringLiteral("kind"), kind},
                               {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))}};
    storeMessageForContact(contactId, message);
}

void MessengerController::storeMessageForContact(const QString& contactId, const QVariantMap& message)
{
    if (contactId == activeContactId_) {
        messages_.append(message);
        vault_.saveConversation(contactId, messages_);
        emit messagesChanged();
    } else {
        auto stored = vault_.loadConversation(contactId);
        stored.append(message);
        vault_.saveConversation(contactId, stored);
    }
}

void MessengerController::saveProfile()
{
    vault_.saveConversation(QStringLiteral("__profile"),
                            {QVariantMap {{QStringLiteral("accountId"), accountId_},
                                          {QStringLiteral("contacts"), contacts_},
                                          {QStringLiteral("network"), QVariantMap {
                                               {QStringLiteral("bootstrapNode"), networkConfig_.bootstrapNode},
                                               {QStringLiteral("turnHost"), networkConfig_.turnHost},
                                               {QStringLiteral("turnPort"), networkConfig_.turnPort},
                                               {QStringLiteral("turnUser"), networkConfig_.turnUser},
                                               {QStringLiteral("turnPassword"), networkConfig_.turnPassword}}}}});
}

void MessengerController::setError(const QString& error)
{
    lastError_ = error;
    emit lastErrorChanged();
}

QString MessengerController::contactName(const QString& id) const
{
    for (const auto& item : contacts_) {
        const auto map = item.toMap();
        if (map.value(QStringLiteral("id")).toString() == id)
            return map.value(QStringLiteral("name")).toString();
    }
    return {};
}
