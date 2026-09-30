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
    directoryDeadline_.setSingleShot(true);
    directoryDeadline_.setInterval(30000);
    connect(&directoryDeadline_, &QTimer::timeout, this, [this] {
        if (!directoryRegistration_.isEmpty() || !directoryQueries_.isEmpty() || loginRequested_)
            emit errorOccurred(tr("账号目录未响应：中继可能尚未升级或无法访问，UID 注册/查找未完成"));
        directoryRegistration_.clear(); directoryQueries_.clear();
        loginRequest_.clear(); loginPassword_.fill(QChar('\0')); loginPassword_.clear(); loginRequested_ = false;
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
            outbox_.insert(id, {to, envelope});
    }
    for (const auto& item : vault_.loadConversation(QString::fromLatin1(seenSlot)))
        seen_.insert(item.toString());
    reconnect_.setInterval(5000);
    connect(&reconnect_, &QTimer::timeout, this, &RelayClient::connectNow);
    retryOutbox_.setInterval(15000);
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
            emit errorOccurred(tr("无法及时连接中继 %1。可能是网络、DNS 或服务不可达；并非好友拒绝。请使用当前网络可直接访问的中继地址。已保存的好友申请会在恢复连接后重发。").arg(endpoint_.host()));
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
            loginRequest_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
            socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {{"op","login_info"},{"id",loginRequest_},{"uid",loginUid_}}).toJson(QJsonDocument::Compact)));
            directoryDeadline_.start();
        }
        resendOutbox();
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
    if (passwordConfigured_ || !registrationAuth_.isEmpty()) {
        emit errorOccurred(tr("此本机账号已设置密码或正在注册，请勿重复创建")); return false;
    }
    const auto record = crypto_.makeLoginRecord(password,name);
    if (record.isEmpty() || !vault_.saveConversation("__account_registration",{record.toVariantMap()})) {
        emit errorOccurred(tr("密码设置或加密身份备份失败（密码须 8–128 个字符）")); return false;
    }
    registrationAuth_ = record; enableDirectory(true); return true;
}
bool RelayClient::loginPasswordAccount(const QString& uid, const QString& password) {
    if (loginRequested_ || passwordConfigured_ || !registrationAuth_.isEmpty() || password.size()<8 || password.size()>128 ||
        !QRegularExpression("^[1-9][0-9]{0,15}$").match(uid).hasMatch()) return false;
    loginUid_=uid; loginPassword_=password; loginRequested_=true;
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
    directoryDeadline_.start(); return true;
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
        heartbeat_.stop();
        pongDeadline_.stop();
        connectDeadline_.stop();
        reconnect_.stop();
        retryOutbox_.stop();
        offlineRecipients_.clear();
        failureNotice_.stop();
        pendingError_.clear();
        failureReported_ = false;
        socket_.close();
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
        prepared.insert(id, {to, envelope});
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
    const auto document = QJsonDocument::fromJson(text.toUtf8());
    if (!document.isObject()) return;
    const auto frame = document.object();
    const auto op = frame.value(QStringLiteral("op")).toString();
    const auto directoryId = frame.value(QStringLiteral("id")).toString();
    if (op == "login_info_result" && !loginRequest_.isEmpty() && directoryId == loginRequest_ && frame.value("uid").toString() == loginUid_) {
        const auto token=RelayCrypto::loginToken(loginPassword_,frame.value("salt").toString(),frame.value("iterations").toInt());
        if (token.isEmpty()) { loginRequested_=false; loginRequest_.clear(); loginPassword_.fill(QChar('\0')); loginPassword_.clear(); directoryDeadline_.stop(); emit errorOccurred(tr("登录参数无效或密码格式错误")); return; }
        socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {{"op","login"},{"id",loginRequest_},{"uid",loginUid_},{"token",token}}).toJson(QJsonDocument::Compact))); return;
    }
    if (op == "login_result" && !loginRequest_.isEmpty() && directoryId == loginRequest_) {
        const auto uid=frame.value("uid").toString();
        const bool restored=uid==loginUid_ && crypto_.restoreLoginRecord(vault_,loginPassword_,frame);
        loginPassword_.fill(QChar('\0')); loginPassword_.clear(); loginRequested_=false; loginRequest_.clear(); directoryDeadline_.stop();
        if (!restored) { emit errorOccurred(tr("账号或密码错误，身份恢复未完成，原有数据未覆盖")); return; }
        // Only fresh login profiles reach here. Old scratch probes must not be resent under the recovered identity.
        outbox_.clear(); seen_.clear(); persistOutbox(); persistSeen();
        passwordConfigured_=vault_.saveConversation("__password_account",{QVariantMap {{"uid",uid}}});
        if (!passwordConfigured_) { emit errorOccurred(tr("身份已解密，但登录状态无法保存，请检查本机空间后重试")); refreshConnection(); return; }
        emit passwordAccountRestored(uid,frame.value("name").toString().left(64));
        refreshConnection(); return;
    }
    if (op == "registered" && !directoryRegistration_.isEmpty() && directoryId == directoryRegistration_) {
        const auto uid = frame.value("uid").toString();
        if (!QRegularExpression(QStringLiteral("^[1-9][0-9]{0,15}$")).match(uid).hasMatch()) return;
        passwordConfigured_=vault_.saveConversation("__password_account",{QVariantMap {{"uid",uid}}});
        if (!passwordConfigured_ || !vault_.saveConversation("__account_registration",{})) { emit errorOccurred(tr("UID 已注册，但本机登录状态未能保存；重试不会占用新 UID")); return; }
        registrationAuth_={}; directoryRegistration_.clear(); emit uidAssigned(uid);
        if (directoryQueries_.isEmpty()) directoryDeadline_.stop(); return;
    }
    if (op == "lookup_result" && directoryQueries_.contains(directoryId)) {
        const auto uid = directoryQueries_.take(directoryId);
        if (frame.value("uid").toString() != uid) return;
        emit uidResolved(uid, frame.value("code").toString(), frame.value("name").toString().left(64));
        if (directoryRegistration_.isEmpty() && directoryQueries_.isEmpty()) directoryDeadline_.stop(); return;
    }
    if (op == "directory_error" && (directoryId == directoryRegistration_ || directoryQueries_.contains(directoryId) || directoryId == loginRequest_)) {
        const bool loginFailed = !loginRequest_.isEmpty() && directoryId == loginRequest_;
        loginPassword_.fill(QChar('\0')); loginPassword_.clear(); loginRequested_=false; loginRequest_.clear();
        directoryRegistration_.clear(); directoryQueries_.clear(); directoryDeadline_.stop();
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
