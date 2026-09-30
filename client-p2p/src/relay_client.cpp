#include "relay_client.h"
#include "local_vault.h"

#include <QJsonDocument>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QUrlQuery>
#include <QUuid>
#include <QSslSocket>
#include <QRegularExpression>
#include <QMessageAuthenticationCode>
#include <QDateTime>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>

namespace {
constexpr auto outboxSlot = "__relay_outbox";
constexpr auto seenSlot = "__relay_seen";

QByteArray fromBase64(const QString& value)
{
    return QByteArray::fromBase64(value.toLatin1(), QByteArray::Base64UrlEncoding);
}
}

RelayClient::RelayClient(LocalVault& vault, QObject* parent)
    : QObject(parent), vault_(vault)
{
    crypto_.loadOrCreate(vault_);
    const auto pendingAccount = vault_.loadConversation("__account_registration");
    if (!pendingAccount.isEmpty()) registrationAuth_ = QJsonObject::fromVariantMap(pendingAccount.first().toMap());
    passwordConfigured_ = !vault_.loadConversation("__password_account").isEmpty();
    const auto backupAuth=vault_.loadConversation("__contact_backup_auth");
    if(!backupAuth.isEmpty())backupToken_=backupAuth.first().toMap().value("token").toString();
    backupAllowed_=vault_.loadConversation("__contact_restore_pending").isEmpty();
    if(backupToken_.isEmpty() && !registrationAuth_.isEmpty())backupToken_=registrationAuth_.value("token").toString();
    backupTimer_.setInterval(3000);backupTimer_.setSingleShot(true);
    connect(&backupTimer_,&QTimer::timeout,this,&RelayClient::syncContactBackup);
    directoryDeadline_.setSingleShot(true);
    directoryDeadline_.setInterval(30000);
    connect(&directoryDeadline_, &QTimer::timeout, this, [this] {
        if (!directoryRegistration_.isEmpty() || !directoryQueries_.isEmpty() || loginRequested_)
            emit errorOccurred(tr("账号目录未响应：中继可能尚未升级或无法访问，UID 注册/查找未完成"));
        directoryRegistration_.clear(); directoryQueries_.clear();
        loginRequest_.clear(); loginPassword_.fill(QChar('\0')); loginPassword_.clear(); loginRequested_ = false;
        emit loginStateChanged();
    });
    // Old builds left zero-length metadata files. Recover only those queues;
    // the identity and nonempty unreadable ciphertext must never be reset.
    for (const auto* slot : {outboxSlot, seenSlot})
        if (vault_.hasConversation(QString::fromLatin1(slot)))
            vault_.recoverEmptyConversation(QString::fromLatin1(slot), {});
    for (const auto& item : vault_.loadConversation(QString::fromLatin1(outboxSlot))) {
        const auto row = item.toMap();
        const auto id = row.value(QStringLiteral("id")).toString();
        const auto to = row.value(QStringLiteral("to")).toString();
        const auto envelope = row.value(QStringLiteral("envelope")).toByteArray();
        if (!id.isEmpty() && to.size() == 64 && !envelope.isEmpty())
            outbox_.insert(id, {to, envelope,row.value("scope").toString()});
    }
    for (const auto& item : vault_.loadConversation(QString::fromLatin1(seenSlot)))
        seen_.insert(item.toString());
    reconnect_.setInterval(5000);
    connect(&reconnect_, &QTimer::timeout, this, &RelayClient::connectNow);
    retryOutbox_.setInterval(3000);
    heartbeat_.setInterval(15000);
    pongDeadline_.setSingleShot(true);
    pongDeadline_.setInterval(10000);
    connectDeadline_.setSingleShot(true);
    connectDeadline_.setInterval(15000);
    connect(&heartbeat_, &QTimer::timeout, this, [this] {
        if (!connected_ || pongDeadline_.isActive()) return;
        pongDeadline_.start();
        socket_.ping(QByteArrayLiteral("SD1"));
    });
    connect(&socket_, &QWebSocket::pong, this, [this](quint64, const QByteArray&) {
        pongDeadline_.stop();
    });
    const auto timedOut = [this] {
        if (!enabled_) return;
        if (!failureReported_) {
            failureReported_ = true;
            emit errorOccurred(tr("无法及时连接辅助服务。可能是网络、DNS 或服务不可达；并非好友拒绝。请检查网络连接，已保存的好友申请会在恢复连接后重发。"));
        }
        refreshConnection();
    };
    connect(&pongDeadline_, &QTimer::timeout, this, timedOut);
    connect(&connectDeadline_, &QTimer::timeout, this, timedOut);
    connect(&retryOutbox_, &QTimer::timeout, this, &RelayClient::resendOutbox);
    failureNotice_.setSingleShot(true);
    failureNotice_.setInterval(15000);
    connect(&failureNotice_, &QTimer::timeout, this, [this] {
        if (enabled_ && !connected_ && !failureReported_ && !pendingError_.isEmpty()) {
            failureReported_ = true;
            emit errorOccurred(tr("辅助连接持续失败，正在自动重试：%1").arg(pendingError_));
        }
    });
    connect(&socket_, &QWebSocket::connected, this, [this] {
        connectDeadline_.stop();
        heartbeat_.start();
        connected_ = true;
        offlineRecipients_.clear();
        failureNotice_.stop();
        pendingError_.clear();
        failureReported_ = false;
        emit connectedChanged(true);
        enableDirectory(directoryEnabled_);
        if (loginRequested_) {
            loginCryptoBusy_=false;loginTokenReady_=false;
            loginRequest_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
            socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {{"op","login_info"},{"id",loginRequest_},{"uid",loginUid_}}).toJson(QJsonDocument::Compact)));
            directoryDeadline_.start();
        }
        resendOutbox();
        friendRepairQuery_=QUuid::createUuid().toString(QUuid::WithoutBraces);
        socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op","friend_repair_get"},{"id",friendRepairQuery_}}).toJson(QJsonDocument::Compact)));
        if(backupDirty_)backupTimer_.start();
    });
    connect(&socket_, &QWebSocket::disconnected, this, [this] {
        liveRecipients_.clear();
        heartbeat_.stop();
        pongDeadline_.stop();
        connectDeadline_.stop();
        connected_ = false;
        emit connectedChanged(false);
        if (enabled_) reconnect_.start();
    });
    connect(&socket_, &QWebSocket::textMessageReceived, this, &RelayClient::onFrame);
    connect(&socket_, &QWebSocket::errorOccurred, this, [this] {
        if (!enabled_) return;
        pendingError_ = socket_.errorString();
        if (!failureReported_ && !failureNotice_.isActive()) failureNotice_.start();
    });
}

RelayClient::~RelayClient()
{
    // Socket teardown emits disconnected; disconnect before the member timers
    // are destroyed so the signal cannot access retired timer objects.
    socket_.disconnect(this);
    socket_.abort();
    loginPassword_.fill(QChar('\0'));
    backupToken_.fill(QChar('\0'));pendingBackupToken_.fill(QChar('\0'));
}
bool RelayClient::isReady() const { return !crypto_.identityId().isEmpty(); }
bool RelayClient::isConnected() const { return connected_; }
bool RelayClient::hasEndpoint() const
{
    return (endpoint_.scheme() == QStringLiteral("wss") && !endpoint_.host().isEmpty()) ||
           (endpoint_.scheme() == QStringLiteral("ws") &&
            (endpoint_.host() == QStringLiteral("127.0.0.1") || endpoint_.host() == QStringLiteral("localhost")));
}
QString RelayClient::identityId() const { return crypto_.identityId(); }
QString RelayClient::inviteCode() const { return crypto_.inviteCode(); }
void RelayClient::enableDirectory(bool enabled)
{
    directoryEnabled_ = enabled;
    if (!enabled || !connected_) return;
    if (registrationAuth_.isEmpty()) return; // No numeric registration without a chosen password.
    directoryRegistration_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject registration {{"op", "register"}, {"id", directoryRegistration_}, {"code", inviteCode()},{"auth",registrationAuth_}};
#ifdef Q_OS_WIN
    // Only provisioned on the owner's PC, never included in an installation package.
    // Do not read real credentials from isolated acceptance test profiles.
    const auto ownerRoot=qEnvironmentVariable("P2P_MESSENGER_OWNER_DEVICE_ROOT");
    if (qEnvironmentVariable("P2P_MESSENGER_DATA_ROOT").isEmpty() || !ownerRoot.isEmpty()) {
        LocalVault device(ownerRoot.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : ownerRoot);
        const auto rows = device.loadConversation("__owner_device");
        if (!rows.isEmpty()) {
            const auto secret = QByteArray::fromBase64(rows.first().toMap().value("secret").toString().toLatin1());
            if (secret.size() == 32 && endpoint_.host() == QStringLiteral("shuangdianliao-relay.plastic-gojirasaurus.workers.dev")) {
                if (!rows.first().toMap().value("enabled",false).toBool()) {
                    directoryRegistration_.clear();
                    if (!ownerRegistrationNotice_) {
                        ownerRegistrationNotice_ = true;
                        emit registrationDeferredChanged();
                    }
                    return;
                }
                const auto timestamp = QDateTime::currentSecsSinceEpoch();
                const auto text = QByteArrayLiteral("SD-OWNER|") + identityId().toLatin1() + '|' + QByteArray::number(timestamp);
                const auto mac = QMessageAuthenticationCode::hash(text, secret, QCryptographicHash::Sha256).toHex();
                registration.insert("ownerProof", QJsonObject {{"timestamp", timestamp}, {"mac", QString::fromLatin1(mac)}});
            }
        }
    }
#endif
    socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(registration).toJson(QJsonDocument::Compact)));
    directoryDeadline_.start();
}
bool RelayClient::registerPasswordAccount(const QString& password, const QString& name) {
    if (passwordConfigured_ || registrationPreparing_ || loginRequested_ || !registrationAuth_.isEmpty()) {
        emit errorOccurred(tr("此本机账号已设置密码或正在注册，请勿重复创建")); return false;
    }
    if (password.size()<8 || password.size()>128) return false;
    registrationPreparing_=true; emit loginStateChanged();
    const auto generation=++registrationGeneration_;
    QPointer<RelayClient> guard(this);
    QThreadPool::globalInstance()->start([guard, generation, crypto=crypto_, password=QString(password), name]() mutable {
        auto record=crypto.makeLoginRecord(password,name); password.fill(QChar('\0'));
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, generation, record=std::move(record)] {
            if (!guard || guard->registrationGeneration_!=generation) return;
            auto* self=guard.data(); self->registrationPreparing_=false;
            if (record.isEmpty() || !self->vault_.saveConversation("__account_registration",{record.toVariantMap()})) {
                emit self->loginStateChanged();
                emit self->errorOccurred(tr("密码设置或加密身份备份失败，请检查本机存储")); return;
            }
            self->backupToken_=record.value("token").toString();
            self->registrationAuth_=record; self->enableDirectory(true); emit self->loginStateChanged();
        }, Qt::QueuedConnection);
    });
    return true;
}
bool RelayClient::loginPasswordAccount(const QString& uid, const QString& password) {
    if (loginRequested_ || registrationPreparing_ || passwordConfigured_ || !registrationAuth_.isEmpty() || password.size()<8 || password.size()>128 ||
        !QRegularExpression("^[1-9][0-9]{0,15}$").match(uid).hasMatch()) return false;
    loginUid_=uid; loginPassword_=password; loginRequested_=true; loginCryptoBusy_=false;loginTokenReady_=false;
    emit loginStateChanged();
    if (connected_) {
        loginRequest_=QUuid::createUuid().toString(QUuid::WithoutBraces);
        socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {{"op","login_info"},{"id",loginRequest_},{"uid",uid}}).toJson(QJsonDocument::Compact)));
    }
    directoryDeadline_.start(); return true;
}
bool RelayClient::lookupUid(const QString& uid)
{
    if (!connected_ || !directoryQueries_.isEmpty()) return false;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    directoryQueries_.insert(id, uid);
    socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {
        {"op", "lookup"}, {"id", id}, {"uid", uid}}).toJson(QJsonDocument::Compact)));
    directoryDeadline_.start();emit loginStateChanged();return true;
}
void RelayClient::cancelLogin() {
    backupRestoring_=false;backupRequest_.clear();backupRestoreUid_.clear();backupRestoreName_.clear();
    loginRequested_=false;loginCryptoBusy_=false;loginTokenReady_=false;loginRequest_.clear();loginPassword_.fill(QChar('\0'));loginPassword_.clear();pendingBackupToken_.fill(QChar('\0'));pendingBackupToken_.clear();
    if(directoryRegistration_.isEmpty() && directoryQueries_.isEmpty())directoryDeadline_.stop();
    emit loginStateChanged();
}
QByteArray RelayClient::publicKeyFromCode(const QString& code) { return RelayCrypto::publicKeyFromCode(code); }
QString RelayClient::idForPublicKey(const QByteArray& key) { return RelayCrypto::idForPublicKey(key); }

void RelayClient::setEndpoint(const QUrl& endpoint)
{
    offlineRecipients_.clear();
    failureNotice_.stop();
    pendingError_.clear();
    failureReported_ = false;
    endpoint_ = endpoint;
    socket_.close();
    if (enabled_) connectNow();
}

void RelayClient::setEnabled(bool enabled)
{
    enabled_ = enabled;
    if (enabled_) {
        reconnect_.start();
        retryOutbox_.start();
        connectNow();
    } else {
        // A retired GUI/service session no longer owns the vault writer lock.
        // In-flight password jobs must not commit into the new session's data.
        ++registrationGeneration_;registrationPreparing_=false;
        backupTimer_.stop();backupWrite_.clear();backupRequest_.clear();
        if(loginPending())cancelLogin();
        heartbeat_.stop();
        pongDeadline_.stop();
        connectDeadline_.stop();
        reconnect_.stop();
        retryOutbox_.stop();
        offlineRecipients_.clear();
        failureNotice_.stop();
        pendingError_.clear();
        failureReported_ = false;
        // A released session must not process buffered frames while the service
        // has already acquired the encrypted database's writer lock.
        socket_.abort();
    }
}

void RelayClient::refreshConnection()
{
    if (!enabled_) return;
    heartbeat_.stop();
    pongDeadline_.stop();
    connectDeadline_.stop();
    socket_.abort();
    if (connected_) {
        connected_ = false;
        emit connectedChanged(false);
    }
    connectNow();
}

void RelayClient::connectNow()
{
    if (!enabled_ || !isReady() || !hasEndpoint() ||
        socket_.state() != QAbstractSocket::UnconnectedState) return;
    if (endpoint_.scheme() == QStringLiteral("wss") && !QSslSocket::supportsSsl()) {
        if (!failureReported_) {
            failureReported_ = true;
            emit errorOccurred(tr("安全连接组件未能加载，无法连接中继。请安装包含 TLS 组件的新版安装包；不要关闭证书验证。"));
        }
        return;
    }
    auto url = endpoint_;
    url.setPath(QStringLiteral("/connect"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), identityId());
    url.setQuery(query);
    // Ask the OS about the HTTPS equivalent of a secure WebSocket URL.
    // Windows system proxies often understand https but not the wss scheme.
    auto proxyUrl = url;
    proxyUrl.setScheme(url.scheme() == QStringLiteral("wss")
                           ? QStringLiteral("https") : QStringLiteral("http"));
    const auto proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(proxyUrl));
    for (const auto& proxy : proxies) {
        if (proxy.type() == QNetworkProxy::HttpProxy || proxy.type() == QNetworkProxy::Socks5Proxy) {
            socket_.setProxy(proxy);
            break;
        }
        if (proxy.type() == QNetworkProxy::NoProxy) {
            socket_.setProxy(proxy);
            break;
        }
    }
    connectDeadline_.start();
    socket_.open(url);
}

QString RelayClient::send(const QByteArray& recipientPublicKey, const QJsonObject& message)
{
    const auto ids = sendBatch(recipientPublicKey, {message});
    return ids.isEmpty() ? QString {} : ids.first();
}

bool RelayClient::sendLive(const QByteArray& key, const QJsonObject& message)
{
    if (!connected_ || socket_.bytesToWrite() > 128000) return false;
    const auto to = idForPublicKey(key);
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto envelope = crypto_.seal(key, id, message);
    if (to.isEmpty() || envelope.isEmpty() || envelope.size() > 56000) return false;
    if (liveRecipients_.size() >= 256) liveRecipients_.erase(liveRecipients_.begin());
    liveRecipients_.insert(id,to);
    sendFrame(id, {to, envelope});
    return true;
}

QStringList RelayClient::sendBatch(const QByteArray& recipientPublicKey, const QList<QJsonObject>& messages)
{
    const auto to = idForPublicKey(recipientPublicKey);
    if (to.isEmpty() || !isReady() || messages.isEmpty() || messages.size() > 200) return {};
    QMap<QString, Outgoing> prepared;
    for (const auto& message : messages) {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto envelope = crypto_.seal(recipientPublicKey, id, message);
        if (envelope.isEmpty() || envelope.size() > 56000) return {};
        prepared.insert(id, {to, envelope,message.value("groupId").toString()});
    }
    for (auto it = prepared.cbegin(); it != prepared.cend(); ++it)
        outbox_.insert(it.key(), it.value());
    if (!persistOutbox()) {
        for (auto it = prepared.cbegin(); it != prepared.cend(); ++it) outbox_.remove(it.key());
        emit errorOccurred(tr("发送队列无法保存到本机"));
        return {};
    }
    QStringList ids;
    for (auto it = prepared.cbegin(); it != prepared.cend(); ++it) {
        ids.append(it.key());
        sendFrame(it.key(), it.value());
        emit deliveryState(it.key(), QStringLiteral("queued"));
    }
    return ids;
}

void RelayClient::rejectCurrentPacket()
{
    currentPacketAccepted_ = false;
}

void RelayClient::sendFrame(const QString& id, const Outgoing& outgoing)
{
    if (!connected_) return;
    socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {
        {QStringLiteral("op"), QStringLiteral("send")},
        {QStringLiteral("id"), id},
        {QStringLiteral("to"), outgoing.to},
        {QStringLiteral("envelope"), QString::fromLatin1(outgoing.envelope)}
    }).toJson(QJsonDocument::Compact)));
}

void RelayClient::resendOutbox()
{
    if (!connected_) return;
    QSet<QString> retriedOfflineRecipients;
    for (auto it = outbox_.cbegin(); it != outbox_.cend(); ++it) {
        if (offlineRecipients_.contains(it.value().to)) {
            if (retriedOfflineRecipients.contains(it.value().to)) continue;
            retriedOfflineRecipients.insert(it.value().to);
        }
        sendFrame(it.key(), it.value());
    }
}

void RelayClient::sendAck(const QByteArray& recipientPublicKey, const QString& originalId)
{
    // Acknowledgements are not themselves acknowledged. If one is lost, the
    // original is retried and the receiver sends another acknowledgement.
    const auto to = idForPublicKey(recipientPublicKey);
    if (!connected_ || to.isEmpty()) return;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto envelope = crypto_.seal(recipientPublicKey, id, QJsonObject {
        {QStringLiteral("type"), QStringLiteral("ack")},
        {QStringLiteral("id"), originalId}
    });
    if (!envelope.isEmpty()) sendFrame(id, {to, envelope});
}

void RelayClient::onFrame(const QString& text)
{
    if (!enabled_) return;
    const auto document = QJsonDocument::fromJson(text.toUtf8());
    if (!document.isObject()) return;
    const auto frame = document.object();
    const auto op = frame.value(QStringLiteral("op")).toString();
    const auto directoryId = frame.value(QStringLiteral("id")).toString();
    if(op=="friend_repair_result" && !friendRepairQuery_.isEmpty() && directoryId==friendRepairQuery_){
        friendRepairQuery_.clear();const auto repair=frame.value("repair").toObject();if(repair.isEmpty())return;
        if(repair.value("envelope").toString().size()>8000)return;
        bool ok=false;const auto plain=crypto_.open(repair.value("from").toString(),repair.value("id").toString(),repair.value("envelope").toString().toLatin1(),&ok);
        if(ok && plain.value("type")=="operator_friend_repair" && plain.value("code").toString()==inviteCode())emit friendRepairReceived(repair.value("id").toString(),plain.toVariantMap());
        return;
    }
    if(op=="contacts_get_result" && directoryId==backupRequest_ && backupRestoring_){
        const auto snapshot=frame.value("snapshot").toObject();
        if(!snapshot.isEmpty()){bool ok=false;const auto plain=crypto_.open(identityId(),snapshot.value("id").toString(),snapshot.value("envelope").toString().toLatin1(),&ok);
            if(ok && plain.value("type")=="contact_backup" && plain.value("v").toInt()==1)emit contactBackupRestored(plain.value("profile").toObject().toVariantMap());
            else {backupAllowed_=false;vault_.saveConversation("__contact_restore_pending",{true});emit errorOccurred(tr("好友备份校验失败，未覆盖本机资料"));}
        }
        finishContactRestore();return;
    }
    if(op=="contacts_put_result" && directoryId==backupWrite_){backupWrite_.clear();backupDirty_=false;return;}
    if(op=="directory_error" && directoryId==backupRequest_ && backupRestoring_){backupAllowed_=false;vault_.saveConversation("__contact_restore_pending",{true});finishContactRestore();emit errorOccurred(tr("辅助服务未能恢复好友备份，请保留原设备数据"));return;}
    if (op == "login_info_result" && !loginRequest_.isEmpty() && directoryId == loginRequest_ && frame.value("uid").toString() == loginUid_) {
        if (loginCryptoBusy_ || loginTokenReady_) return;
        loginCryptoBusy_=true;
        QPointer<RelayClient> guard(this);
        QThreadPool::globalInstance()->start([guard, password=loginPassword_, frame, request=loginRequest_]() mutable {
            auto token=RelayCrypto::loginToken(password,frame.value("salt").toString(),frame.value("iterations").toInt());
            password.fill(QChar('\0'));
            QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, request, token=std::move(token)]() mutable {
                if (!guard || !guard->loginRequested_ || guard->loginRequest_!=request) { token.fill(QChar('\0')); return; }
                auto* self=guard.data(); self->loginCryptoBusy_=false;
                if (token.isEmpty()) { self->cancelLogin(); emit self->errorOccurred(tr("登录参数无效或密码格式错误")); return; }
                self->loginTokenReady_=true;
                self->pendingBackupToken_=token;
                self->directoryDeadline_.start();
                self->socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {{"op","login"},{"id",request},{"uid",self->loginUid_},{"token",token}}).toJson(QJsonDocument::Compact)));
                token.fill(QChar('\0'));
            }, Qt::QueuedConnection);
        });
        return;
    }
    if (op == "login_result" && !loginRequest_.isEmpty() && directoryId == loginRequest_) {
        if (loginCryptoBusy_ || !loginTokenReady_) return;
        loginCryptoBusy_=true;
        QPointer<RelayClient> guard(this);
        QThreadPool::globalInstance()->start([guard, password=loginPassword_, frame, request=loginRequest_]() mutable {
            auto secret=RelayCrypto::decryptLoginIdentity(password,frame); password.fill(QChar('\0'));
            QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, frame, request, secret=std::move(secret)]() mutable {
                if (!guard || !guard->loginRequested_ || guard->loginRequest_!=request) { secret.fill('\0'); return; }
                auto* self=guard.data();
                const auto uid=frame.value("uid").toString();
                const bool restored=uid==self->loginUid_ && self->crypto_.installLoginIdentity(self->vault_,secret,frame.value("code").toString());
                secret.fill('\0');
                self->loginPassword_.fill(QChar('\0')); self->loginPassword_.clear(); self->loginRequested_=false; self->loginRequest_.clear(); self->loginCryptoBusy_=false; self->directoryDeadline_.stop();
                if (!restored) { emit self->loginStateChanged();emit self->errorOccurred(tr("账号或密码错误，身份恢复未完成，原有数据未覆盖")); return; }
        // Only fresh login profiles reach here. Old scratch probes must not be resent under the recovered identity.
                self->outbox_.clear(); self->seen_.clear(); self->persistOutbox(); self->persistSeen();
                self->passwordConfigured_=self->vault_.saveConversation("__password_account",{QVariantMap {{"uid",uid}}});
                if (!self->passwordConfigured_) { emit self->loginStateChanged();emit self->errorOccurred(tr("身份已解密，但登录状态无法保存，请检查本机空间后重试")); self->refreshConnection(); return; }
                self->backupToken_=self->pendingBackupToken_;self->pendingBackupToken_.clear();
                self->vault_.saveConversation("__contact_backup_auth",{QVariantMap{{"token",self->backupToken_}}});
                self->backupRestoring_=true;
                self->backupRestoreUid_=uid;self->backupRestoreName_=frame.value("name").toString().left(64);
                self->backupRequest_=QUuid::createUuid().toString(QUuid::WithoutBraces);
                self->socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op","contacts_get"},{"id",self->backupRequest_},{"uid",uid},{"token",self->backupToken_}}).toJson(QJsonDocument::Compact)));
                QTimer::singleShot(10000,self,[self]{if(self->backupRestoring_){self->backupAllowed_=false;self->vault_.saveConversation("__contact_restore_pending",{true});self->finishContactRestore();emit self->errorOccurred(tr("好友备份恢复超时。请勿卸载旧设备，稍后重新登录重试"));}});
            }, Qt::QueuedConnection);
        });
        return;
    }
    if (op == "registered" && !directoryRegistration_.isEmpty() && directoryId == directoryRegistration_) {
        const auto uid = frame.value("uid").toString();
        if (!QRegularExpression(QStringLiteral("^[1-9][0-9]{0,15}$")).match(uid).hasMatch()) return;
        passwordConfigured_=vault_.saveConversation("__password_account",{QVariantMap {{"uid",uid}}});
        if (!passwordConfigured_ || !vault_.saveConversation("__account_registration",{})) { emit errorOccurred(tr("UID 已注册，但本机登录状态未能保存；重试不会占用新 UID")); return; }
        vault_.saveConversation("__contact_backup_auth",{QVariantMap{{"token",backupToken_}}});
        registrationAuth_={}; directoryRegistration_.clear(); emit uidAssigned(uid);if(backupDirty_)backupTimer_.start();
        if (directoryQueries_.isEmpty()) directoryDeadline_.stop(); return;
    }
    if (op == "lookup_result" && directoryQueries_.contains(directoryId)) {
        const auto uid = directoryQueries_.take(directoryId);
        emit loginStateChanged();
        if (frame.value("uid").toString() != uid) return;
        emit uidResolved(uid, frame.value("code").toString(), frame.value("name").toString().left(64));
        if (directoryRegistration_.isEmpty() && directoryQueries_.isEmpty()) directoryDeadline_.stop(); return;
    }
    if (op == "directory_error" && (directoryId == directoryRegistration_ || directoryQueries_.contains(directoryId) || directoryId == loginRequest_)) {
        const bool loginFailed = !loginRequest_.isEmpty() && directoryId == loginRequest_;
        loginPassword_.fill(QChar('\0')); loginPassword_.clear(); loginRequested_=false; loginRequest_.clear();
        directoryRegistration_.clear(); directoryQueries_.clear(); directoryDeadline_.stop();
        emit loginStateChanged();
        emit errorOccurred(loginFailed ? tr("账号或密码错误，或登录尝试过多，请稍后重试") : tr("UID 注册/查询未完成：%1").arg(frame.value("reason").toString())); return;
    }
    if (op == QStringLiteral("relay")) {
        const auto id = frame.value(QStringLiteral("id")).toString();
        const auto status = frame.value(QStringLiteral("status")).toString();
        if (liveRecipients_.contains(id)) {
            const auto peer = liveRecipients_.take(id);
            if (status == "recipient_offline") emit peerUnavailable(peer);
            return;
        }
        // Routing metadata is not an authenticated device receipt. In
        // particular, a relay must never be able to assert "delivered".
        if (!outbox_.contains(id) || (status != QStringLiteral("recipient_offline") &&
                                    status != QStringLiteral("forwarded"))) return;
        const auto recipient = outbox_.value(id).to;
        if (status == QStringLiteral("recipient_offline")) offlineRecipients_.insert(recipient);
        else offlineRecipients_.remove(recipient);
        emit deliveryState(id, status);
        return;
    }
    if (op != QStringLiteral("packet")) return;
    const auto id = frame.value(QStringLiteral("id")).toString();
    const auto from = frame.value(QStringLiteral("from")).toString();
    bool valid = false;
    const auto envelope = frame.value(QStringLiteral("envelope")).toString().toLatin1();
    const auto message = crypto_.open(from, id, envelope, &valid);
    if (!valid) return;
    const auto nested = QJsonDocument::fromJson(fromBase64(QString::fromLatin1(envelope)));
    const auto senderPublic = fromBase64(nested.object().value(QStringLiteral("pk")).toString());
    if (message.value(QStringLiteral("type")).toString().startsWith(QStringLiteral("call_"))) {
        // Live audio/signalling are authenticated, but never saved or replayed
        // as offline messages. The call controller checks session and sequence.
        emit packetReceived(from, senderPublic, message);
        return;
    }
    if (message.value(QStringLiteral("type")) == QStringLiteral("ack")) {
        const auto original = message.value(QStringLiteral("id")).toString();
        if (outbox_.contains(original) && outbox_.value(original).to == from) {
            outbox_.remove(original);
            persistOutbox();
            emit deliveryState(original, QStringLiteral("delivered"));
        }
        return;
    }
    if (!seen_.contains(id)) {
        currentPacketAccepted_ = true;
        emit packetReceived(from, senderPublic, message);
        if (!currentPacketAccepted_) return;
        seen_.insert(id);
        if (!persistSeen()) {
            seen_.remove(id);
            emit errorOccurred(tr("接收记录无法保存到本机"));
            return;
        }
    }
    sendAck(senderPublic, id);
}

bool RelayClient::persistOutbox()
{
    QVariantList rows;
    for (auto it = outbox_.cbegin(); it != outbox_.cend(); ++it)
        rows.append(QVariantMap {{QStringLiteral("id"), it.key()},
                                 {QStringLiteral("to"), it.value().to},
                                 {QStringLiteral("scope"), it.value().scope},
                                 {QStringLiteral("envelope"), it.value().envelope}});
    return vault_.saveConversation(QString::fromLatin1(outboxSlot), rows);
}

bool RelayClient::persistSeen()
{
    QVariantList rows;
    for (const auto& id : seen_) rows.append(id);
    if (rows.size() > 1000) {
        rows = rows.mid(rows.size() - 1000);
        seen_.clear();
        for (const auto& id : rows) seen_.insert(id.toString());
    }
    return vault_.saveConversation(QString::fromLatin1(seenSlot), rows);
}
bool RelayClient::cancelContactPackets(const QString& peerId,const QString& groupId) {
    const auto previous=outbox_;
    for(auto it=outbox_.begin();it!=outbox_.end();) {
        const bool matches=groupId.isEmpty()?(it.value().to==peerId && it.value().scope.isEmpty()):it.value().scope==groupId;
        if(matches)it=outbox_.erase(it);else ++it;
    }
    if(!persistOutbox()){outbox_=previous;return false;}return true;
}
void RelayClient::setContactBackup(const QVariantMap& profile) {
    if(!backupAllowed_ || backupRestoring_ || !passwordConfigured_ || backupToken_.isEmpty())return;
    const auto plain=QJsonDocument(QJsonObject::fromVariantMap(profile)).toJson(QJsonDocument::Compact);
    if(plain==backupPlain_)return;
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto envelope=crypto_.seal(publicKeyFromCode(inviteCode()),id,QJsonObject{{"type","contact_backup"},{"v",1},{"profile",QJsonObject::fromVariantMap(profile)}});
    if(envelope.isEmpty() || envelope.size()>56000){emit errorOccurred(tr("好友资料备份过大，未上传；请保留本机数据"));return;}
    backupPlain_=plain;backupSnapshot_=QJsonObject{{"id",id},{"envelope",QString::fromLatin1(envelope)}};backupDirty_=true;
    if(enabled_ && !backupTimer_.isActive())backupTimer_.start();
}
void RelayClient::syncContactBackup() {
    if(!enabled_ || !connected_ || !passwordConfigured_ || backupRestoring_ || !backupDirty_ || backupSnapshot_.isEmpty())return;
    const auto rows=vault_.loadConversation("__password_account");if(rows.isEmpty())return;
    backupWrite_=QUuid::createUuid().toString(QUuid::WithoutBraces);
    socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op","contacts_put"},{"id",backupWrite_},{"uid",rows.first().toMap().value("uid").toString()},{"token",backupToken_},{"snapshot",backupSnapshot_}}).toJson(QJsonDocument::Compact)));
    backupTimer_.start(10000);
}
void RelayClient::finishContactRestore() {
    backupRestoring_=false;backupRequest_.clear();
    emit passwordAccountRestored(backupRestoreUid_,backupRestoreName_);
    backupRestoreUid_.clear();backupRestoreName_.clear();emit loginStateChanged();refreshConnection();
}
