#include "messenger_controller.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QSaveFile>
#include <QDir>
#include <QGuiApplication>
#include <QClipboard>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QUuid>
#include <QTimer>
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

QString numericEndpoint(const QString& input)
{
    const auto value = input.trimmed();
    if (value.contains(QLatin1Char('%'))) return {};
    QString host;
    QString portText;
    if (value.startsWith(QLatin1Char('['))) {
        const auto end = value.indexOf(QStringLiteral("]:"));
        if (end < 0) return {};
        host = value.mid(1, end - 1);
        portText = value.mid(end + 2);
    } else {
        const auto separator = value.lastIndexOf(QLatin1Char(':'));
        if (separator < 0) return {};
        host = value.left(separator);
        portText = value.mid(separator + 1);
    }
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,5}$"));
    if (!digits.match(portText).hasMatch()) return {};
    const auto port = portText.toInt();
    if (port < 1 || port > 65535) return {};
    QHostAddress address;
    if (!address.setAddress(host)) return {};
    if (address.isLoopback() || address.isMulticast() ||
        address == QHostAddress::AnyIPv4 || address == QHostAddress::AnyIPv6) return {};
    if (address.protocol() == QAbstractSocket::IPv6Protocol)
        return QStringLiteral("[%1]:%2").arg(address.toString()).arg(port);
    return QStringLiteral("%1:%2").arg(address.toString()).arg(port);
}

QString numericEndpointList(const QString& input)
{
    QStringList endpoints;
    for (const auto& part : input.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const auto endpoint = numericEndpoint(part);
        if (endpoint.isEmpty()) return {};
        if (!endpoints.contains(endpoint)) endpoints.append(endpoint);
        if (endpoints.size() > 4) return {};
    }
    return endpoints.join(QLatin1Char(';'));
}

bool usableGlobalIPv6(const QHostAddress& address)
{
    return address.protocol() == QAbstractSocket::IPv6Protocol &&
           address.isInSubnet(QHostAddress(QStringLiteral("2000::")), 3) &&
           !address.isInSubnet(QHostAddress(QStringLiteral("2001:0::")), 32) &&
           !address.isInSubnet(QHostAddress(QStringLiteral("2002::")), 16) &&
           !address.isInSubnet(QHostAddress(QStringLiteral("2001:db8::")), 32);
}

bool decodePairingCode(const QString& code, QString* peer, QString* endpoint, QString* name)
{
    const QUrl url(code, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QStringLiteral("p2pm") ||
        url.host() != QStringLiteral("pair") || !url.path().isEmpty()) return false;
    const QUrlQuery query(url);
    if (query.queryItemValue(QStringLiteral("v")) != QStringLiteral("1")) return false;
    const auto id = query.queryItemValue(QStringLiteral("peer"));
    static const QRegularExpression identity(QStringLiteral("^[0-9a-fA-F]{40}$"));
    const auto addresses = query.allQueryItemValues(QStringLiteral("node"));
    if (addresses.isEmpty() || addresses.size() > 4) return false;
    const auto address = numericEndpointList(addresses.join(QLatin1Char(';')));
    if (!identity.match(id).hasMatch() || address.isEmpty()) return false;
    *peer = id.toLower();
    *endpoint = address;
    *name = query.queryItemValue(QStringLiteral("name")).trimmed().left(64);
    return true;
}
}

MessengerController::MessengerController(QObject* parent)
    : QObject(parent)
    , networkStatus_(tr("仅本地模式 — 尚未配置自建服务"))
    , relay_(vault_, this)
{
    connect(&relay_, &RelayClient::connectedChanged, this, [this](bool connected) {
        if (!assistedConnection_) return;
        networkStatus_ = connected ? tr("辅助连接已接通 — 等待对方上线")
                                   : tr("辅助连接未接通 — 好友申请会留在本机重试");
        emit networkStatusChanged();
    });
    connect(&relay_, &RelayClient::errorOccurred, this, [this](const QString& error) {
        if (assistedConnection_) setError(error);
    });
    connect(&relay_, &RelayClient::packetReceived, this,
            [this](const QString& senderId, const QByteArray& senderPublic, const QJsonObject& message) {
        const auto type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("friend_request")) {
            pendingRelayRequests_.insert(senderId, QString::fromLatin1(senderPublic.toBase64(
                QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
            if (!pendingRequests_.contains(senderId)) pendingRequests_.append(senderId);
            if (!saveProfile()) { relay_.rejectCurrentPacket(); setError(tr("好友申请无法保存到本机")); }
            emit pendingRequestsChanged();
            return;
        }
        if (type == QStringLiteral("friend_accept")) {
            for (auto& item : contacts_) {
                auto entry = item.toMap();
                if (entry.value(QStringLiteral("transport")).toString() != QStringLiteral("relay") ||
                    entry.value(QStringLiteral("uri")).toString() != senderId) continue;
                entry.insert(QStringLiteral("ready"), true);
                entry.insert(QStringLiteral("status"), tr("已验证"));
                item = entry;
                if (!saveProfile()) { relay_.rejectCurrentPacket(); setError(tr("好友确认无法保存到本机")); }
                emit contactsChanged();
                return;
            }
            relay_.rejectCurrentPacket();
            return;
        }
        if (type == QStringLiteral("group_invite")) {
            const auto groupId = message.value(QStringLiteral("groupId")).toString();
            const auto members = message.value(QStringLiteral("members")).toArray();
            if (groupId.isEmpty() || groupId.size() > 80 || members.isEmpty() || members.size() > 20) return;
            bool senderIncluded = false, selfIncluded = false;
            for (const auto& value : members) {
                const auto key = RelayClient::publicKeyFromCode(value.toString());
                if (key.isEmpty()) return;
                const auto id = RelayClient::idForPublicKey(key);
                senderIncluded |= id == senderId;
                selfIncluded |= id == relay_.identityId();
            }
            if (!senderIncluded || !selfIncluded) return;
            for (const auto& item : contacts_)
                if (item.toMap().value(QStringLiteral("groupId")).toString() == groupId) return;
            pendingRelayGroups_.insert(groupId, message.toVariantMap());
            if (!pendingGroupRequests_.contains(groupId)) pendingGroupRequests_.append(groupId);
            if (!saveProfile()) { relay_.rejectCurrentPacket(); setError(tr("群邀请无法保存到本机")); }
            emit pendingGroupRequestsChanged();
            return;
        }
        if (type == QStringLiteral("group_text")) {
            const auto groupId = message.value(QStringLiteral("groupId")).toString();
            const auto body = message.value(QStringLiteral("body")).toString();
            if (body.isEmpty() || body.size() > 16000) return;
            for (const auto& item : contacts_) {
                const auto entry = item.toMap();
                if (entry.value(QStringLiteral("groupId")).toString() != groupId ||
                    entry.value(QStringLiteral("transport")).toString() != QStringLiteral("relay")) continue;
                bool member = false;
                for (const auto& code : entry.value(QStringLiteral("members")).toList())
                    member |= RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString())) == senderId;
                if (member) {
                    if (!storeMessageForContact(entry.value(QStringLiteral("id")).toString(), QVariantMap {
                        {QStringLiteral("body"), tr("%1：%2").arg(senderId.left(8), body)},
                        {QStringLiteral("outgoing"), false},
                        {QStringLiteral("kind"), QStringLiteral("text")},
                        {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))}
                    })) relay_.rejectCurrentPacket();
                } else relay_.rejectCurrentPacket();
                return;
            }
            relay_.rejectCurrentPacket();
            return;
        }
        if (type == QStringLiteral("file_chunk")) {
            QString contactId;
            for (const auto& item : contacts_) {
                const auto entry = item.toMap();
                if (entry.value(QStringLiteral("transport")).toString() == QStringLiteral("relay") &&
                    !entry.value(QStringLiteral("group")).toBool() &&
                    entry.value(QStringLiteral("uri")).toString() == senderId &&
                    entry.value(QStringLiteral("ready")).toBool()) {
                    contactId = entry.value(QStringLiteral("id")).toString();
                    break;
                }
            }
            if (contactId.isEmpty()) { relay_.rejectCurrentPacket(); return; }
            const auto fileId = message.value(QStringLiteral("fileId")).toString();
            const auto name = QFileInfo(message.value(QStringLiteral("name")).toString()).fileName();
            const auto size = message.value(QStringLiteral("size")).toInteger(-1);
            const auto count = message.value(QStringLiteral("count")).toInt();
            const auto index = message.value(QStringLiteral("index")).toInt(-1);
            const auto hash = message.value(QStringLiteral("sha256")).toString();
            static const QRegularExpression hexHash(QStringLiteral("^[0-9a-f]{64}$"));
            if (fileId.isEmpty() || fileId.size() > 80 || name.isEmpty() || name.size() > 180 ||
                size < 0 || size > 2 * 1024 * 1024 || count != qMax(1, int((size + 12287) / 12288)) ||
                index < 0 || index >= count || !hexHash.match(hash).hasMatch()) return;
            const auto bytes = QByteArray::fromBase64(message.value(QStringLiteral("data")).toString().toLatin1(),
                                                       QByteArray::Base64UrlEncoding);
            if (bytes.size() > 12288 || (size > 0 && bytes.isEmpty())) return;
            const auto key = senderId + QLatin1Char(':') + fileId;
            auto row = incomingRelayFiles_.value(key).toMap();
            if (!row.isEmpty() && (row.value(QStringLiteral("size")).toLongLong() != size ||
                                   row.value(QStringLiteral("hash")).toString() != hash ||
                                   row.value(QStringLiteral("count")).toInt() != count)) return;
            auto chunks = row.value(QStringLiteral("chunks")).toMap();
            chunks.insert(QString::number(index), QString::fromLatin1(bytes.toBase64()));
            row.insert(QStringLiteral("name"), name);
            row.insert(QStringLiteral("size"), size);
            row.insert(QStringLiteral("hash"), hash);
            row.insert(QStringLiteral("count"), count);
            row.insert(QStringLiteral("chunks"), chunks);
            incomingRelayFiles_.insert(key, row);
            if (!vault_.saveConversation(QStringLiteral("__relay_files"), {incomingRelayFiles_})) {
                relay_.rejectCurrentPacket();
                setError(tr("文件块无法保存到本机，请检查存储空间")); return;
            }
            if (chunks.size() != count) return;
            QByteArray file;
            for (int i = 0; i < count; ++i) {
                if (!chunks.contains(QString::number(i))) return;
                file += QByteArray::fromBase64(chunks.value(QString::number(i)).toString().toLatin1());
            }
            if (file.size() != size || QString::fromLatin1(QCryptographicHash::hash(file, QCryptographicHash::Sha256).toHex()) != hash) {
                setError(tr("收到的文件校验失败，请让对方重发")); return;
            }
            if (!storeMessageForContact(contactId, QVariantMap {
                {QStringLiteral("body"), tr("文件：%1（可保存）").arg(name)},
                {QStringLiteral("outgoing"), false},
                {QStringLiteral("kind"), QStringLiteral("file-offer")},
                {QStringLiteral("fileId"), fileId},
                {QStringLiteral("fileData"), QString::fromLatin1(file.toBase64())},
                {QStringLiteral("name"), name},
                {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))}
            })) {
                relay_.rejectCurrentPacket();
                setError(tr("完整文件无法保存到本机，请检查存储空间"));
                return;
            }
            incomingRelayFiles_.remove(key);
            vault_.saveConversation(QStringLiteral("__relay_files"), {incomingRelayFiles_});
            return;
        }
        if (type == QStringLiteral("text")) {
            const auto body = message.value(QStringLiteral("body")).toString();
            if (body.isEmpty()) return;
            for (auto& item : contacts_) {
                auto entry = item.toMap();
                if (entry.value(QStringLiteral("transport")).toString() == QStringLiteral("relay") &&
                    entry.value(QStringLiteral("uri")).toString() == senderId) {
                    if (!entry.value(QStringLiteral("ready")).toBool()) {
                        entry.insert(QStringLiteral("ready"), true);
                        entry.insert(QStringLiteral("status"), tr("已验证"));
                        item = entry;
                        if (!saveProfile()) relay_.rejectCurrentPacket();
                        emit contactsChanged();
                    }
                    if (!storeMessageForContact(entry.value(QStringLiteral("id")).toString(), QVariantMap {
                        {QStringLiteral("body"), body},
                        {QStringLiteral("outgoing"), false},
                        {QStringLiteral("kind"), QStringLiteral("text")},
                        {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))}
                    })) relay_.rejectCurrentPacket();
                    return;
                }
            }
            relay_.rejectCurrentPacket();
        }
    });
    connect(&relay_, &RelayClient::deliveryState, this,
            [this](const QString& packetId, const QString& state) {
        if (state != QStringLiteral("delivered")) return;
        bool contactsChangedLocal = false;
        for (auto& item : contacts_) {
            auto entry = item.toMap();
            if (entry.value(QStringLiteral("requestPacketId")).toString() != packetId) continue;
            entry.insert(QStringLiteral("status"), tr("对方已收到申请，等待确认"));
            item = entry;
            contactsChangedLocal = true;
        }
        if (contactsChangedLocal) { saveProfile(); emit contactsChanged(); }
        for (const auto& item : contacts_) {
            const auto contactId = item.toMap().value(QStringLiteral("id")).toString();
            auto stored = contactId == activeContactId_ ? messages_ : vault_.loadConversation(contactId);
            bool changed = false;
            for (auto& value : stored) {
                auto row = value.toMap();
                if (row.value(QStringLiteral("packetId")).toString() != packetId) continue;
                row.insert(QStringLiteral("delivery"), tr("已送达"));
                value = row;
                changed = true;
            }
            if (changed) {
                vault_.saveConversation(contactId, stored);
                if (contactId == activeContactId_) { messages_ = stored; emit messagesChanged(); }
                break;
            }
        }
    });
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
                if (eventCode >= 7 || eventCode == 2)
                    setError(tr("文件传输中断。请确认两台设备都在线、网络未切换，然后重新发送或接收。"));
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
                emit pairingCodeChanged();
            });
    connect(&gatewayMapper_, &GatewayMapper::endpointChanged, this,
            [this](const QString& endpoint) {
                mappedEndpoint_ = endpoint;
                emit pairingCodeChanged();
            });
    connect(&gatewayMapper_, &GatewayMapper::statusChanged, this,
            [this](const QString& status) {
                if (assistedConnection_) {
                    networkStatus_ = relay_.isConnected() ? tr("辅助连接已接通") : tr("辅助连接正在重试");
                    emit networkStatusChanged();
                    return;
                }
                bool ipv6Available = false;
                for (const auto& address : QNetworkInterface::allAddresses()) {
                    if (usableGlobalIPv6(address)) { ipv6Available = true; break; }
                }
                networkStatus_ = ipv6Available && !gatewayMapper_.busy() &&
                                 gatewayMapper_.endpoint().isEmpty()
                    ? tr("公网 IPv6 可尝试直连；本地路由器的 IPv4 自动映射不可用。")
                    : status;
                emit networkStatusChanged();
            });
    connect(&daemon_, &DaemonBridge::nearbyPeerChanged, this,
            [this](const QString& accountId, const QString& uri,
                   const QString& name, bool present) {
                if (accountId != accountId_ || uri == inviteCode_) return;
                for (qsizetype i = nearbyPeers_.size(); i-- > 0;) {
                    if (nearbyPeers_.at(i).toMap().value(QStringLiteral("uri")).toString() == uri)
                        nearbyPeers_.removeAt(i);
                }
                if (present)
                    nearbyPeers_.append(QVariantMap {{QStringLiteral("uri"), uri},
                                                      {QStringLiteral("name"), name.isEmpty() ? uri.left(8) : name}});
                emit nearbyPeersChanged();
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
    identityRefresh_ = new QTimer(this);
    identityRefresh_->setInterval(2000);
    connect(identityRefresh_, &QTimer::timeout, this, [this] {
        if (accountId_.isEmpty()) {
            identityRefresh_->stop();
            return;
        }
        const auto port = daemon_.listeningPort(accountId_);
        if (port != listeningPort_) {
            listeningPort_ = port;
            emit listeningPortChanged();
            if (port > 0) gatewayMapper_.start(static_cast<quint16>(port));
        }
        const auto code = inviteCode_.isEmpty() ? daemon_.inviteCode(accountId_) : QString {};
        if (!code.isEmpty() && inviteCode_.isEmpty()) {
            inviteCode_ = code;
            emit inviteCodeChanged();
            emit pairingCodeChanged();
        }
        if (!inviteCode_.isEmpty() && listeningPort_ > 0) identityRefresh_->stop();
    });
    if (!accountId_.isEmpty()) identityRefresh_->start();
    const auto savedProfile = vault_.loadConversation(QStringLiteral("__profile"));
    if (!savedProfile.isEmpty()) {
        const auto profile = savedProfile.first().toMap();
        if (profile.value(QStringLiteral("accountId")).toString() == accountId_) {
            profileName_ = profile.value(QStringLiteral("profileName")).toString();
            directEndpoint_ = numericEndpointList(profile.value(QStringLiteral("directEndpoint")).toString());
            contacts_ = profile.value(QStringLiteral("contacts")).toList();
            const auto network = profile.value(QStringLiteral("network")).toMap();
            assistedConnection_ = network.value(QStringLiteral("assistedConnection"), false).toBool();
            pendingRelayRequests_ = profile.value(QStringLiteral("pendingRelayRequests")).toMap();
            pendingRelayGroups_ = profile.value(QStringLiteral("pendingRelayGroups")).toMap();
            QStringList verifiedNodes;
            for (const auto& savedNode : network.value(QStringLiteral("bootstrapNode")).toString()
                                             .split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                const auto node = numericEndpoint(savedNode);
                if (!node.isEmpty() && !verifiedNodes.contains(node)) verifiedNodes.append(node);
            }
            networkConfig_.bootstrapNode = verifiedNodes.join(QLatin1Char(';'));
            networkConfig_.turnHost = network.value(QStringLiteral("turnHost")).toString();
            networkConfig_.turnPort = static_cast<quint16>(network.value(QStringLiteral("turnPort"), 3478).toUInt());
            networkConfig_.turnUser = network.value(QStringLiteral("turnUser")).toString();
            networkConfig_.turnPassword = network.value(QStringLiteral("turnPassword")).toString();
            networkConfig_.turnHost.clear();
            networkConfig_.turnUser.clear();
            networkConfig_.turnPassword.clear();
        }
    }
    if (!accountId_.isEmpty() && networkConfig_.isValid()) {
        daemon_.configurePrivateNetwork(accountId_, networkConfig_, false);
        networkStatus_ = assistedConnection_ ? tr("辅助连接正在建立")
                                             : tr("纯直连 — 不使用公共引导或中继");
    }
    relay_.setEndpoint(QUrl(qEnvironmentVariable("P2P_MESSENGER_RELAY_URL")));
    const auto savedRelayFiles = vault_.loadConversation(QStringLiteral("__relay_files"));
    if (!savedRelayFiles.isEmpty()) incomingRelayFiles_ = savedRelayFiles.first().toMap();
    relay_.setEnabled(assistedConnection_);
    if (assistedConnection_ && !relay_.hasEndpoint()) {
        networkStatus_ = tr("辅助中继尚未部署，不能发送异网申请");
        emit networkStatusChanged();
    }
    if (!accountId_.isEmpty() && !profileName_.isEmpty())
        daemon_.setIdentityAlias(accountId_, profileName_);
    if (contacts_.isEmpty())
        contacts_.append(contact(QStringLiteral("welcome"), tr("开始使用"), tr("本设备")));
    if (!accountId_.isEmpty())
        pendingRequests_ = daemon_.pendingFriendRequests(accountId_);
    for (auto it = pendingRelayRequests_.cbegin(); it != pendingRelayRequests_.cend(); ++it)
        if (!pendingRequests_.contains(it.key())) pendingRequests_.append(it.key());
    pendingRefresh_ = new QTimer(this);
    pendingRefresh_->setInterval(15000);
    connect(pendingRefresh_, &QTimer::timeout, this, &MessengerController::refreshPendingRequests);
    if (!accountId_.isEmpty()) pendingRefresh_->start();
    if (!accountId_.isEmpty())
        pendingGroupRequests_ = daemon_.pendingGroupRequests(accountId_);
    for (auto it = pendingRelayGroups_.cbegin(); it != pendingRelayGroups_.cend(); ++it)
        if (!pendingGroupRequests_.contains(it.key())) pendingGroupRequests_.append(it.key());
    refreshNearbyPeers();
    activeContactId_ = QStringLiteral("welcome");
    messages_ = vault_.loadConversation(activeContactId_);
    if (messages_.isEmpty())
        appendMessage(tr("欢迎使用双点聊。扫描附近设备或粘贴双机配对码，即可尝试端到端连接。"), false);
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
QString MessengerController::inviteCode() const { return assistedConnection_ ? relay_.inviteCode() : inviteCode_; }
QString MessengerController::accountId() const { return accountId_; }
QString MessengerController::profileName() const { return profileName_; }
QString MessengerController::directEndpoint() const { return directEndpoint_; }
int MessengerController::listeningPort() const { return listeningPort_; }
QVariantList MessengerController::nearbyPeers() const { return nearbyPeers_; }
QString MessengerController::pairingCode() const
{
    if (assistedConnection_) return relay_.inviteCode();
    if (inviteCode_.isEmpty()) return {};
    if (directEndpoint_.isEmpty() && mappedEndpoint_.isEmpty()) return inviteCode_;
    QUrl url;
    url.setScheme(QStringLiteral("p2pm"));
    url.setHost(QStringLiteral("pair"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("v"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("peer"), inviteCode_);
    if (!mappedEndpoint_.isEmpty())
        query.addQueryItem(QStringLiteral("node"), mappedEndpoint_);
    for (const auto& endpoint : directEndpoint_.split(QLatin1Char(';'), Qt::SkipEmptyParts))
        query.addQueryItem(QStringLiteral("node"), endpoint);
    if (!profileName_.isEmpty()) query.addQueryItem(QStringLiteral("name"), profileName_);
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}
QString MessengerController::lastError() const { return lastError_; }
QStringList MessengerController::pendingRequests() const { return pendingRequests_; }
QStringList MessengerController::pendingGroupRequests() const { return pendingGroupRequests_; }
bool MessengerController::assistedConnection() const { return assistedConnection_; }

bool MessengerController::setProfileName(const QString& name)
{
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 64) {
        setError(tr("账号名称须为 1 至 64 个字符"));
        return false;
    }
    if (!daemon_.setIdentityAlias(accountId_, trimmed)) {
        setError(tr("无法保存本机账号名称，请检查通信内核"));
        return false;
    }
    profileName_ = trimmed;
    saveProfile();
    setError({});
    emit profileNameChanged();
    emit pairingCodeChanged();
    return true;
}

bool MessengerController::retryIdentity()
{
    if (accountId_.isEmpty()) {
        if (!daemon_.start()) {
            setError(tr("通信内核未启动，无法创建本机账号"));
            return false;
        }
        accountId_ = daemon_.createLocalIdentity(profileName_.isEmpty() ? tr("我的设备") : profileName_);
        emit accountIdChanged();
        if (!accountId_.isEmpty() && identityRefresh_) identityRefresh_->start();
    }
    if (accountId_.isEmpty()) {
        setError(tr("创建本机账号失败"));
        return false;
    }
    const auto code = daemon_.inviteCode(accountId_);
    if (!code.isEmpty() && code != inviteCode_) {
        inviteCode_ = code;
        emit inviteCodeChanged();
        emit pairingCodeChanged();
    }
    setError(inviteCode_.isEmpty() ? tr("账号已创建，邀请码仍在生成；请稍后重试") : QString {});
    return !inviteCode_.isEmpty();
}

bool MessengerController::setDirectEndpoint(const QString& endpoint)
{
    const auto normalized = endpoint.trimmed().isEmpty() ? QString {} : numericEndpointList(endpoint);
    if (!endpoint.trimmed().isEmpty() && normalized.isEmpty()) {
        setError(tr("地址格式不正确。请填写 IP:端口；IPv6 地址须加方括号。"));
        return false;
    }
    if (directEndpoint_ != normalized) {
        directEndpoint_ = normalized;
        saveProfile();
        emit directEndpointChanged();
        emit pairingCodeChanged();
    }
    setError({});
    return true;
}

bool MessengerController::useLocalNetworkAddress()
{
    if (listeningPort_ < 1) {
        setError(tr("本机监听端口尚未就绪，请稍后重试"));
        return false;
    }
    QStringList candidates;
    QString localAddress;
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp) ||
            (interface.flags() & QNetworkInterface::IsLoopBack)) continue;
        for (const auto& entry : interface.addressEntries()) {
            const auto address = entry.ip();
            if (usableGlobalIPv6(address) && candidates.size() < 2)
                candidates.append(QStringLiteral("[%1]:%2").arg(address.toString()).arg(listeningPort_));
            if (address.protocol() != QAbstractSocket::IPv4Protocol || !localAddress.isEmpty()) continue;
            const auto privateAddress = address.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8) ||
                                        address.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12) ||
                                        address.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16);
            if (privateAddress)
                localAddress = QStringLiteral("%1:%2").arg(address.toString()).arg(listeningPort_);
        }
    }
    if (!localAddress.isEmpty()) candidates.append(localAddress);
    if (!candidates.isEmpty()) return setDirectEndpoint(candidates.join(QLatin1Char(';')));
    setError(tr("没有找到可分享的网络地址。请连接 Wi-Fi，或在高级网络设置中填写可达地址。"));
    return false;
}

void MessengerController::copyPairingCode()
{
    if (!pairingCode().isEmpty()) QGuiApplication::clipboard()->setText(pairingCode());
}

bool MessengerController::copyLocalPairingCode()
{
    if (assistedConnection_) {
        if (relay_.inviteCode().isEmpty()) {
            setError(tr("辅助连接身份尚未就绪，请检查本机加密存储。"));
            return false;
        }
        QGuiApplication::clipboard()->setText(relay_.inviteCode());
        setError({});
        return true;
    }
    if (gatewayMapper_.busy() && mappedEndpoint_.isEmpty()) {
        setError(tr("正在向本地路由器申请跨网直连地址，请稍后再试。"));
        return false;
    }
    if (!useLocalNetworkAddress() || pairingCode().isEmpty()) return false;
    copyPairingCode();
    bool hasGlobalIPv6 = false;
    for (const auto& address : QNetworkInterface::allAddresses()) {
        if (usableGlobalIPv6(address)) { hasGlobalIPv6 = true; break; }
    }
    if (mappedEndpoint_.isEmpty() && !hasGlobalIPv6)
        setError(tr("配对码已复制，但当前网络只有局域网地址，无法用于跨网直连。软件会在网络条件允许时自动尝试直连。"));
    return true;
}

void MessengerController::refreshNearbyPeers()
{
    nearbyPeers_.clear();
    const auto discovered = daemon_.nearbyPeers(accountId_);
    for (auto it = discovered.cbegin(); it != discovered.cend(); ++it) {
        if (it.key() == inviteCode_) continue;
        nearbyPeers_.append(QVariantMap {{QStringLiteral("uri"), it.key()},
                                          {QStringLiteral("name"), it.value().isEmpty() ? it.key().left(8) : it.value()}});
    }
    emit nearbyPeersChanged();
}

bool MessengerController::addNearbyPeer(const QString& peerUri)
{
    for (const auto& value : nearbyPeers_) {
        const auto peer = value.toMap();
        if (peer.value(QStringLiteral("uri")).toString() == peerUri)
            return addContact(peer.value(QStringLiteral("name")).toString(), peerUri);
    }
    setError(tr("附近设备已离线，请重新扫描"));
    return false;
}

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
    if (invite.trimmed().isEmpty()) {
        setError(tr("还没有填写配对码。请让对方点击“分享我的配对码”后发给你。"));
        return false;
    }
    if (accountId_.isEmpty() || inviteCode_.isEmpty()) {
        setError(tr("本机身份仍在生成，稍后再添加好友"));
        return false;
    }
    auto peerCode = invite.trimmed();
    if (assistedConnection_) {
        const auto peerKey = RelayClient::publicKeyFromCode(peerCode);
        if (peerKey.isEmpty()) {
            setError(tr("辅助连接需要新的 SD1 配对码。请让对方切到辅助连接，重新复制配对码。"));
            return false;
        }
        const auto peerId = RelayClient::idForPublicKey(peerKey);
        if (peerId == relay_.identityId()) {
            setError(tr("不能添加自己的账号"));
            return false;
        }
        const auto trimmedName = name.trimmed().isEmpty() ? tr("好友 %1").arg(peerId.left(8)) : name.trimmed();
        const auto packetId = relay_.send(peerKey, QJsonObject {
            {QStringLiteral("type"), QStringLiteral("friend_request")},
            {QStringLiteral("name"), profileName_}
        });
        if (packetId.isEmpty()) {
            setError(tr("好友申请无法加密并加入本机发送队列，请检查本机存储。"));
            return false;
        }
        for (auto& item : contacts_) {
            auto entry = item.toMap();
            if (entry.value(QStringLiteral("transport")).toString() != QStringLiteral("relay") ||
                entry.value(QStringLiteral("uri")).toString() != peerId) continue;
            entry.insert(QStringLiteral("requestPacketId"), packetId);
            entry.insert(QStringLiteral("status"), tr("申请已排队，等待对方收到"));
            item = entry;
            saveProfile();
            emit contactsChanged();
            selectContact(entry.value(QStringLiteral("id")).toString());
            return true;
        }
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto entry = contact(id, trimmedName, tr("申请已排队，等待对方收到"));
        entry.insert(QStringLiteral("transport"), QStringLiteral("relay"));
        entry.insert(QStringLiteral("uri"), peerId);
        entry.insert(QStringLiteral("relayPublic"), peerCode);
        entry.insert(QStringLiteral("requestPacketId"), packetId);
        entry.insert(QStringLiteral("ready"), false);
        contacts_.append(entry);
        saveProfile();
        emit contactsChanged();
        selectContact(id);
        return true;
    }
    QString peerEndpoint;
    QString peerName;
    if (peerCode.startsWith(QStringLiteral("p2pm:"))) {
        if (!decodePairingCode(peerCode, &peerCode, &peerEndpoint, &peerName)) {
            setError(tr("配对码无法识别。请让对方重新复制完整配对码，不要截断。"));
            return false;
        }
    }
    static const QRegularExpression identity(QStringLiteral("^[0-9a-fA-F]{40}$"));
    if (!identity.match(peerCode).hasMatch()) {
        setError(tr("设备码不完整。请粘贴对方分享的完整配对码。"));
        return false;
    }
    peerCode = peerCode.toLower();
    if (peerCode.compare(inviteCode_, Qt::CaseInsensitive) == 0) {
        setError(tr("不能添加自己的账号"));
        return false;
    }
    const auto trimmedName = name.trimmed().isEmpty()
        ? (peerName.isEmpty() ? tr("好友 %1").arg(peerCode.left(8)) : peerName)
        : name.trimmed();
    if (!peerEndpoint.isEmpty()) {
        auto directConfig = networkConfig_;
        auto nodes = directConfig.bootstrapNode.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const auto& endpoint : peerEndpoint.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            if (!nodes.contains(endpoint)) nodes.append(endpoint);
        }
        directConfig.bootstrapNode = nodes.join(QLatin1Char(';'));
        directConfig.turnHost.clear();
        directConfig.turnUser.clear();
        directConfig.turnPassword.clear();
        if (!daemon_.configurePrivateNetwork(accountId_, directConfig, assistedConnection_)) {
            setError(tr("无法保存对方的直连地址。请重启应用后重新粘贴配对码。"));
            return false;
        }
        networkConfig_ = directConfig;
        networkStatus_ = assistedConnection_ ? tr("辅助连接正在建立")
                                             : tr("已配置 %1 台直连设备 — 不使用中继").arg(nodes.size());
        emit networkStatusChanged();
        saveProfile();
    }
    for (const auto& value : contacts_) {
        const auto existing = value.toMap();
        if (existing.value(QStringLiteral("uri")).toString() == peerCode) {
            if (!existing.value(QStringLiteral("ready"), false).toBool()) {
                if (!daemon_.addVerifiedContact(accountId_, peerCode)) {
                    setError(tr("好友申请未重新提交：本机通信网络尚未就绪。请保持应用打开并联网，稍后重试。"));
                    return false;
                }
            }
            selectContact(existing.value(QStringLiteral("id")).toString());
            setError({});
            return true;
        }
    }
    if (!daemon_.addVerifiedContact(accountId_, peerCode)) {
        setError(tr("好友申请未提交：本机通信网络尚未就绪。请保持应用打开并联网，稍后重新发送。"));
        return false;
    }
    const auto conversationId = daemon_.createConversation(accountId_, peerCode);
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto entry = contact(id, trimmedName, tr("等待对方确认"));
    entry.insert(QStringLiteral("uri"), peerCode);
    entry.insert(QStringLiteral("conversationId"), conversationId);
    entry.insert(QStringLiteral("ready"), false);
    contacts_.append(entry);
    saveProfile();
    emit contactsChanged();
    selectContact(id);
    QTimer::singleShot(60000, this, [this, id] {
        for (const auto& value : contacts_) {
            const auto candidate = value.toMap();
            if (candidate.value(QStringLiteral("id")).toString() == id &&
                !candidate.value(QStringLiteral("ready"), false).toBool()) {
                setError(assistedConnection_
                    ? tr("申请已提交，但尚未收到对方确认。请让对方打开双点聊并保持联网；公共节点或中继不可用时也可能送达失败。")
                    : tr("申请已提交，但尚未收到对方确认。请让对方打开双点聊；跨网纯直连可能因双方网络限制而无法送达。"));
                break;
            }
        }
    });
    setError({});
    return true;
}

bool MessengerController::createGroup(const QString& name, const QStringList& memberUris)
{
    const auto groupName = name.trimmed();
    if (groupName.isEmpty())
        return false;
    if (assistedConnection_) {
        if (groupName.size() > 64 || memberUris.size() > 19) {
            setError(tr("群名过长或群成员超过 20 人"));
            return false;
        }
        QStringList members {relay_.inviteCode()};
        QList<QByteArray> destinations;
        for (const auto& uri : memberUris) {
            if (uri.trimmed().isEmpty()) continue;
            const auto key = RelayClient::publicKeyFromCode(uri.trimmed());
            if (key.isEmpty()) { setError(tr("群成员需使用辅助连接的 SD1 配对码")); return false; }
            if (!members.contains(uri.trimmed())) {
                members.append(uri.trimmed());
                destinations.append(key);
            }
        }
        if (destinations.isEmpty()) { setError(tr("请至少填写一位群成员的 SD1 配对码")); return false; }
        const auto groupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto payload = QJsonObject {
            {QStringLiteral("type"), QStringLiteral("group_invite")},
            {QStringLiteral("groupId"), groupId},
            {QStringLiteral("name"), groupName},
            {QStringLiteral("members"), QJsonArray::fromStringList(members)}
        };
        for (const auto& key : destinations)
            if (relay_.send(key, payload).isEmpty()) {
                setError(tr("群邀请未能保存到本机发送队列")); return false;
            }
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto entry = contact(id, groupName, tr("群聊"));
        entry.insert(QStringLiteral("initial"), tr("群"));
        entry.insert(QStringLiteral("transport"), QStringLiteral("relay"));
        entry.insert(QStringLiteral("group"), true);
        entry.insert(QStringLiteral("groupId"), groupId);
        entry.insert(QStringLiteral("members"), members);
        entry.insert(QStringLiteral("ready"), true);
        contacts_.append(entry);
        saveProfile(); emit contactsChanged(); selectContact(id);
        return true;
    }
    if (accountId_.isEmpty() || inviteCode_.isEmpty()) {
        setError(tr("本机身份仍在生成，稍后再创建群聊"));
        return false;
    }
    static const QRegularExpression identity(QStringLiteral("^[0-9a-fA-F]{40}$"));
    for (const auto& uri : memberUris) {
        const auto member = uri.trimmed();
        if (!member.isEmpty() && !identity.match(member).hasMatch()) {
            setError(tr("群成员邀请码必须是 40 位设备 ID"));
            return false;
        }
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
    if (assistedConnection_ && text.size() > 16000) {
        setError(tr("辅助连接单条消息最多 16000 个字符，请拆分后发送。"));
        return false;
    }
    for (const auto& item : contacts_) {
        const auto current = item.toMap();
        if (current.value(QStringLiteral("id")).toString() != activeContactId_)
            continue;
        if (current.value(QStringLiteral("transport")).toString() == QStringLiteral("relay")) {
            if (current.value(QStringLiteral("group")).toBool()) {
                int queued = 0;
                for (const auto& code : current.value(QStringLiteral("members")).toList()) {
                    const auto key = RelayClient::publicKeyFromCode(code.toString());
                    if (RelayClient::idForPublicKey(key) == relay_.identityId()) continue;
                    if (!relay_.send(key, QJsonObject {
                        {QStringLiteral("type"), QStringLiteral("group_text")},
                        {QStringLiteral("groupId"), current.value(QStringLiteral("groupId")).toString()},
                        {QStringLiteral("body"), text}
                    }).isEmpty()) ++queued;
                }
                if (queued == 0) { setError(tr("群消息未能保存到发送队列")); return false; }
                appendMessage(text, true);
                return true;
            }
            if (!current.value(QStringLiteral("ready")).toBool()) {
                setError(tr("请等待对方接受好友申请后再发送消息。"));
                return false;
            }
            const auto peerKey = RelayClient::publicKeyFromCode(
                current.value(QStringLiteral("relayPublic")).toString());
            const auto packetId = relay_.send(peerKey, QJsonObject {
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("body"), text}
            });
            if (packetId.isEmpty()) {
                setError(tr("消息未能加密并保存到本机发送队列。"));
                return false;
            }
            storeMessageForContact(activeContactId_, QVariantMap {
                {QStringLiteral("body"), text},
                {QStringLiteral("outgoing"), true},
                {QStringLiteral("kind"), QStringLiteral("text")},
                {QStringLiteral("packetId"), packetId},
                {QStringLiteral("delivery"), tr("等待对方收到")},
                {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))}
            });
            return true;
        }
        const auto conversationId = current.value(QStringLiteral("conversationId")).toString();
        if (accountId_.isEmpty() || conversationId.isEmpty() ||
            !current.value(QStringLiteral("ready"), true).toBool()) {
            setError(tr("还不能发送：请先等待对方接受好友申请并保持应用在线。"));
            return false;
        }
        if (!daemon_.sendText(accountId_, conversationId, text)) {
            setError(tr("消息未发送。请检查网络连接，并确认对方设备在线后重试。"));
            return false;
        }
        return true;
    }
    setError(tr("请先选择已连接的好友或群聊，再发送消息。"));
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
        setError(tr("无法读取所选文件。请确认文件仍存在，并允许应用访问它。"));
        return false;
    }
    for (const auto& item : contacts_) {
        const auto current = item.toMap();
        if (current.value(QStringLiteral("id")).toString() != activeContactId_)
            continue;
        if (current.value(QStringLiteral("transport")).toString() == QStringLiteral("relay")) {
            if (current.value(QStringLiteral("group")).toBool()) {
                setError(tr("辅助连接群聊暂不支持文件；请先发送给单个好友。")); return false;
            }
            if (!current.value(QStringLiteral("ready")).toBool()) {
                setError(tr("请等待对方接受好友申请后再发送文件。")); return false;
            }
            QFile input(localPath);
            if (!input.open(QIODevice::ReadOnly) || input.size() > 2 * 1024 * 1024) {
                setError(tr("辅助连接目前仅支持不超过 2 MB 的文件。")); return false;
            }
            const auto file = input.readAll();
            if (file.size() != input.size()) { setError(tr("文件读取不完整，请重新选择。")); return false; }
            const auto key = RelayClient::publicKeyFromCode(current.value(QStringLiteral("relayPublic")).toString());
            const auto fileId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto hash = QString::fromLatin1(QCryptographicHash::hash(file, QCryptographicHash::Sha256).toHex());
            const auto count = qMax(1, int((file.size() + 12287) / 12288));
            QList<QJsonObject> packets;
            for (int i = 0; i < count; ++i) {
                packets.append(QJsonObject {
                    {QStringLiteral("type"), QStringLiteral("file_chunk")},
                    {QStringLiteral("fileId"), fileId},
                    {QStringLiteral("name"), QFileInfo(localPath).fileName()},
                    {QStringLiteral("size"), file.size()},
                    {QStringLiteral("count"), count},
                    {QStringLiteral("index"), i},
                    {QStringLiteral("sha256"), hash},
                    {QStringLiteral("data"), QString::fromLatin1(file.mid(i * 12288, 12288).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))}
                });
            }
            if (relay_.sendBatch(key, packets).size() != count) {
                setError(tr("文件未能完整加入本机发送队列，请检查存储空间后重试。")); return false;
            }
            appendMessage(tr("文件：%1（已排队，等待对方上线）").arg(QFileInfo(localPath).fileName()), true,
                          QStringLiteral("file"));
            return true;
        }
        const auto conversationId = current.value(QStringLiteral("conversationId")).toString();
        if (accountId_.isEmpty() || conversationId.isEmpty() ||
            !current.value(QStringLiteral("ready"), true).toBool()) {
            setError(tr("还不能发送文件：请等待对方接受好友申请并保持应用在线。"));
            return false;
        }
        if (!daemon_.sendFile(accountId_, conversationId, localPath)) {
            setError(tr("文件未开始传输。请检查网络和文件权限后重试。"));
            return false;
        }
        appendMessage(tr("文件：%1（等待端到端传输）").arg(QFileInfo(localPath).fileName()), true,
                      QStringLiteral("file"));
        return true;
    }
    setError(tr("请先选择已连接的好友或群聊，再发送文件。"));
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
        if (contact.value(QStringLiteral("transport")).toString() == QStringLiteral("relay")) {
            const auto stored = activeContactId_ == contact.value(QStringLiteral("id")).toString()
                ? messages_ : vault_.loadConversation(activeContactId_);
            for (const auto& value : stored) {
                const auto message = value.toMap();
                if (message.value(QStringLiteral("fileId")).toString() != fileId ||
                    message.value(QStringLiteral("kind")).toString() != QStringLiteral("file-offer")) continue;
                const auto data = QByteArray::fromBase64(message.value(QStringLiteral("fileData")).toString().toLatin1());
                QSaveFile output(localPath);
                if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit()) {
                    setError(tr("文件无法保存到所选位置，请检查权限和剩余空间。")); return false;
                }
#ifdef Q_OS_ANDROID
                if (androidDocument) {
                    const auto context = QNativeInterface::QAndroidApplication::context();
                    const bool copied = QJniObject::callStaticMethod<jboolean>(
                        "io/p2pmessenger/app/ContentFiles", "copyFromCache",
                        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z",
                        context.object<jobject>(), QJniObject::fromString(localPath).object<jstring>(),
                        QJniObject::fromString(destination).object<jstring>());
                    QFile::remove(localPath);
                    if (!copied) { setError(tr("文件已接收，但无法写入选定位置。")); return false; }
                }
#endif
                return true;
            }
            setError(tr("本机尚未收到完整文件，请等待传输完成后再保存。")); return false;
        }
        const auto conversationId = contact.value(QStringLiteral("conversationId")).toString();
        if (conversationId.isEmpty() || !daemon_.downloadFile(accountId_, conversationId,
                                                               interactionId, fileId, localPath)) {
            setError(tr("无法开始接收文件。请确认发送方在线、文件仍可用，然后重试。"));
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
    setError(tr("请先选择收到文件的会话，再尝试保存。"));
    return false;
}

void MessengerController::copyInviteCode()
{
    if (!inviteCode().isEmpty())
        QGuiApplication::clipboard()->setText(inviteCode());
}

bool MessengerController::acceptFriendRequest(const QString& contactUri)
{
    if (!pendingRequests_.contains(contactUri)) {
        refreshPendingRequests();
        setError(tr("这条好友申请已不在待处理列表中。请让对方重新发送申请。"));
        return false;
    }
    if (pendingRelayRequests_.contains(contactUri)) {
        const auto peerCode = QStringLiteral("SD1-") + pendingRelayRequests_.value(contactUri).toString();
        const auto peerKey = RelayClient::publicKeyFromCode(peerCode);
        if (peerKey.isEmpty() || relay_.send(peerKey, QJsonObject {
                {QStringLiteral("type"), QStringLiteral("friend_accept")},
                {QStringLiteral("name"), profileName_}
            }).isEmpty()) {
            setError(tr("接受申请失败：无法加密回复或保存到本机发送队列。"));
            return false;
        }
        pendingRelayRequests_.remove(contactUri);
        pendingRequests_.removeAll(contactUri);
        emit pendingRequestsChanged();
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto entry = contact(id, tr("好友 %1").arg(contactUri.left(8)), tr("已验证"));
        entry.insert(QStringLiteral("transport"), QStringLiteral("relay"));
        entry.insert(QStringLiteral("uri"), contactUri);
        entry.insert(QStringLiteral("relayPublic"), peerCode);
        entry.insert(QStringLiteral("ready"), true);
        contacts_.append(entry);
        saveProfile();
        emit contactsChanged();
        selectContact(id);
        return true;
    }
    if (!daemon_.acceptFriendRequest(accountId_, contactUri)) {
        setError(tr("接受好友申请失败：申请可能已过期，或通信内核尚未同步。请保持联网，刷新申请后重试。"));
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

void MessengerController::refreshPendingRequests()
{
    if (accountId_.isEmpty()) return;
    auto current = daemon_.pendingFriendRequests(accountId_);
    for (auto it = pendingRelayRequests_.cbegin(); it != pendingRelayRequests_.cend(); ++it)
        if (!current.contains(it.key())) current.append(it.key());
    if (current == pendingRequests_) return;
    pendingRequests_ = current;
    emit pendingRequestsChanged();
}

bool MessengerController::acceptGroupRequest(const QString& conversationId)
{
    if (!pendingGroupRequests_.contains(conversationId)) return false;
    if (pendingRelayGroups_.contains(conversationId)) {
        const auto invite = pendingRelayGroups_.take(conversationId).toMap();
        pendingGroupRequests_.removeAll(conversationId);
        auto name = invite.value(QStringLiteral("name")).toString().trimmed().left(64);
        if (name.isEmpty()) name = tr("群聊 %1").arg(conversationId.left(8));
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto entry = contact(id, name, tr("群聊"));
        entry.insert(QStringLiteral("initial"), tr("群"));
        entry.insert(QStringLiteral("transport"), QStringLiteral("relay"));
        entry.insert(QStringLiteral("group"), true);
        entry.insert(QStringLiteral("groupId"), conversationId);
        entry.insert(QStringLiteral("members"), invite.value(QStringLiteral("members")).toList());
        entry.insert(QStringLiteral("ready"), true);
        contacts_.append(entry);
        saveProfile(); emit pendingGroupRequestsChanged(); emit contactsChanged(); selectContact(id);
        return true;
    }
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

bool MessengerController::configureNetwork(const QString& rendezvous)
{
    const auto normalized = rendezvous.trimmed().isEmpty() ? QString {} : numericEndpoint(rendezvous);
    if (!rendezvous.trimmed().isEmpty() && normalized.isEmpty()) {
        setError(tr("直连入口须为数字 IP:端口"));
        return false;
    }
    PrivateNetworkConfig config;
    config.bootstrapNode = normalized;
    QString reason;
    if (!config.isValid(&reason)) {
        setError(reason);
        return false;
    }
    if (accountId_.isEmpty() || !daemon_.configurePrivateNetwork(accountId_, config, false)) {
        setError(tr("自建网络设置未能应用"));
        return false;
    }
    networkConfig_ = config;
    saveProfile();
    networkStatus_ = assistedConnection_ ? tr("辅助连接由加密中继处理，Jami 保持纯直连")
                                         : (config.isEmpty() ? tr("仅局域网发现")
                                                             : tr("仅尝试连接指定设备 — 不使用中继"));
    emit networkStatusChanged();
    setError({});
    return true;
}

bool MessengerController::setAssistedConnection(bool enabled)
{
    if (assistedConnection_ == enabled) return true;
    if (enabled && !relay_.isReady()) {
        setError(tr("辅助连接身份未能创建，请检查本机加密存储。"));
        return false;
    }
    if (enabled && !relay_.hasEndpoint()) {
        setError(tr("辅助中继尚未部署，暂时不能切换到辅助连接。"));
        return false;
    }
    if (accountId_.isEmpty() || !daemon_.configurePrivateNetwork(accountId_, networkConfig_, false)) {
        setError(tr("连接模式切换失败：本机通信内核尚未就绪，请稍后重试。"));
        return false;
    }
    assistedConnection_ = enabled;
    relay_.setEnabled(enabled);
    saveProfile();
    networkStatus_ = enabled ? tr("辅助连接正在建立 — 通过你的加密中继")
                             : tr("纯直连 — 不使用公共引导或中继");
    emit assistedConnectionChanged();
    emit inviteCodeChanged();
    emit pairingCodeChanged();
    emit networkStatusChanged();
    setError({});
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

bool MessengerController::storeMessageForContact(const QString& contactId, const QVariantMap& message)
{
    if (contactId == activeContactId_) {
        messages_.append(message);
        if (!vault_.saveConversation(contactId, messages_)) { messages_.removeLast(); return false; }
        emit messagesChanged();
    } else {
        auto stored = vault_.loadConversation(contactId);
        stored.append(message);
        if (!vault_.saveConversation(contactId, stored)) return false;
    }
    return true;
}

bool MessengerController::saveProfile()
{
    return vault_.saveConversation(QStringLiteral("__profile"),
                            {QVariantMap {{QStringLiteral("accountId"), accountId_},
                                          {QStringLiteral("profileName"), profileName_},
                                          {QStringLiteral("directEndpoint"), directEndpoint_},
                                          {QStringLiteral("contacts"), contacts_},
                                          {QStringLiteral("pendingRelayRequests"), pendingRelayRequests_},
                                          {QStringLiteral("pendingRelayGroups"), pendingRelayGroups_},
                                          {QStringLiteral("network"), QVariantMap {
                                               {QStringLiteral("assistedConnection"), assistedConnection_},
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
