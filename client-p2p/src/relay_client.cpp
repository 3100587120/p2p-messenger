#include "relay_client.h"
#include "local_vault.h"

#include <QJsonDocument>
#include <QUrlQuery>
#include <QUuid>

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
        connected_ = true;
        offlineRecipients_.clear();
        failureNotice_.stop();
        pendingError_.clear();
        failureReported_ = false;
        emit connectedChanged(true);
        resendOutbox();
    });
    connect(&socket_, &QWebSocket::disconnected, this, [this] {
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
        reconnect_.stop();
        retryOutbox_.stop();
        offlineRecipients_.clear();
        failureNotice_.stop();
        pendingError_.clear();
        failureReported_ = false;
        socket_.close();
    }
}

void RelayClient::connectNow()
{
    if (!enabled_ || !isReady() || !hasEndpoint() ||
        socket_.state() != QAbstractSocket::UnconnectedState) return;
    auto url = endpoint_;
    url.setPath(QStringLiteral("/connect"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), identityId());
    url.setQuery(query);
    socket_.open(url);
}

QString RelayClient::send(const QByteArray& recipientPublicKey, const QJsonObject& message)
{
    const auto ids = sendBatch(recipientPublicKey, {message});
    return ids.isEmpty() ? QString {} : ids.first();
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
    if (op == QStringLiteral("relay")) {
        const auto id = frame.value(QStringLiteral("id")).toString();
        const auto status = frame.value(QStringLiteral("status")).toString();
        if (outbox_.contains(id)) {
            const auto recipient = outbox_.value(id).to;
            if (status == QStringLiteral("recipient_offline")) offlineRecipients_.insert(recipient);
            else if (status == QStringLiteral("forwarded")) offlineRecipients_.remove(recipient);
        }
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
