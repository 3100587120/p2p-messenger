#include "messenger_controller.h"
#include "relay_build_config.h"
#include "photo_picker.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QSaveFile>
#include <QDir>
#include <QGuiApplication>
#include <QClipboard>
#include <QScreen>
#include <QWindow>
#include <QPixmap>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QUuid>
#include <QTimer>
#include <QImageReader>
#include <QBuffer>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#endif

namespace {
QString safeAvatar(const QString& input) {
    if (input.isEmpty() || input.size() > 32768) return {};
    auto raw = QByteArray::fromBase64(input.toLatin1());
    QBuffer buffer(&raw); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer, "PNG");
    const auto size = reader.size();
    return size.isValid() && size.width() <= 128 && size.height() <= 128 && !reader.read().isNull() ? input : QString {};
}
QVariantMap memberSnapshot(const QVariantMap& group,const QString& sender,const QByteArray& key) {
    auto details=group.value("memberProfiles").toMap().value(sender).toMap();
    details.insert("uri",sender);details.insert("relayPublic","SD1-"+QString::fromLatin1(key.toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals)));
    if(details.value("name").toString().isEmpty())details.insert("name",QStringLiteral("群成员 ")+sender.left(8));
    return details;
}
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
    if (auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(gui, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && assistedConnection_ && !relay_.isConnected()) relay_.refreshConnection();
        });
    }
    connect(&relay_, &RelayClient::connectedChanged, this, [this](bool connected) {
        if (!assistedConnection_) return;
        if (connected) {
            selfTestPassed_ = false;
            networkStatus_ = tr("中继已连接，正在验证本机加密收发");
            if (selfTestPacketId_.isEmpty())
                selfTestPacketId_ = relay_.send(RelayClient::publicKeyFromCode(relay_.inviteCode()),
                    QJsonObject {{QStringLiteral("type"), QStringLiteral("probe")}});
            if (selfTestPacketId_.isEmpty())
                setError(tr("本机加密收发自检无法排队，请检查本地存储"));
            const auto expected = selfTestPacketId_;
            QTimer::singleShot(20000, this, [this, expected] {
                if (assistedConnection_ && relay_.isConnected() && !selfTestPassed_ &&
                    !expected.isEmpty() && expected == selfTestPacketId_) {
                    networkStatus_ = tr("中继已连接，但本机加密收发自检未通过");
                    emit networkStatusChanged();
                    setError(tr("辅助连接自检超时：中继已连接，但加密数据没有返回本机；请勿发送好友申请"));
                }
            });
        } else {
            selfTestPassed_ = false;
            networkStatus_ = tr("辅助连接未接通 — 好友申请会留在本机重试");
            bool changed = false;
            for (auto& item : contacts_) {
                auto entry = item.toMap();
                if (entry.value(QStringLiteral("transport")).toString() != QStringLiteral("relay") ||
                    entry.value(QStringLiteral("ready")).toBool() ||
                    entry.value(QStringLiteral("requestPacketId")).toString().isEmpty() ||
                    entry.value(QStringLiteral("requestDelivery")).toString() == QStringLiteral("delivered")) continue;
                entry.insert(QStringLiteral("status"), tr("尚未确认送达：本机中继连接已断开，恢复后自动重试"));
                item = entry;
                changed = true;
            }
            if (changed) emit contactsChanged();
        }
        emit networkStatusChanged();
    });
    connect(&relay_, &RelayClient::errorOccurred, this, [this](const QString& error) {
        if (assistedConnection_) setError(error);
    });
    connect(&relay_, &RelayClient::registrationDeferredChanged, this, &MessengerController::registrationStatusChanged);
    connect(&relay_, &RelayClient::loginStateChanged, this, &MessengerController::registrationStatusChanged);
    connect(this, &MessengerController::uidChanged, this, &MessengerController::registrationStatusChanged);
    connect(this, &MessengerController::profileNameChanged, this, &MessengerController::registrationStatusChanged);
    connect(this, &MessengerController::networkStatusChanged, this, &MessengerController::registrationStatusChanged);
    connect(this, &MessengerController::assistedConnectionChanged, this, &MessengerController::registrationStatusChanged);
    connect(&relay_, &RelayClient::uidAssigned, this, [this](const QString& uid) {
        const auto old = uid_; uid_ = uid;
        if (!saveProfile()) { uid_ = old; setError(tr("UID 已注册但无法保存，请检查本机存储")); return; }
        emit uidChanged();
    });
    connect(&relay_, &RelayClient::passwordAccountRestored, this, [this](const QString& uid,const QString& name) {
        uid_=uid; profileName_=name; selfTestPassed_=false; selfTestPacketId_.clear();
        if (!saveProfile()) { setError(tr("登录身份已恢复，但本机资料保存失败，请重试")); return; }
        emit uidChanged(); emit profileNameChanged(); emit inviteCodeChanged(); emit pairingCodeChanged(); emit notificationPermissionRequested();
    });
    connect(&relay_, &RelayClient::uidResolved, this, [this](const QString& uid, const QString& code, const QString& name) {
        if (RelayClient::publicKeyFromCode(code).isEmpty()) { setError(tr("没有找到 UID %1，请确认对方已在同一服务注册").arg(uid)); return; }
        if (addContact(uidRemark_.isEmpty() ? name : uidRemark_, code)) {
            for (auto& item : contacts_) { auto row=item.toMap(); if (row.value("uri") == RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code))) { row.insert("hasRemark",!uidRemark_.isEmpty());row.insert("peerUid",uid);item=row; } }
            if(!saveProfile())setError(tr("好友申请已排队，但 UID 资料未能保存"));
            emit contactsChanged();emit activeContactChanged();
            emit operationCompleted(tr("好友申请已排队"),tr("等待对方设备接收并确认，可在会话中查看送达状态"));
        }
    });
    connect(&relay_, &RelayClient::packetReceived, this,
            [this](const QString& senderId, const QByteArray& senderPublic, const QJsonObject& message) {
        const auto type = message.value(QStringLiteral("type")).toString();
        if (type.startsWith(QStringLiteral("call_"))) { receiveCall(senderId, senderPublic, message); return; }
        // The relay client sends an authenticated receipt for this probe.
        // It must not create a contact or appear in chat history.
        if (type == QStringLiteral("probe")) return;
        if (type == QStringLiteral("friend_request")) {
            for(const auto& value:contacts_){const auto row=value.toMap();if(row.value("uri")==senderId && row.value("transport")=="relay" && !row.value("group").toBool() && row.value("ready").toBool())return;}
            const auto previousRequests = pendingRelayRequests_;
            const auto previousPending = pendingRequests_;
            const auto previousProfiles = pendingRelayProfiles_;
            const auto avatar = safeAvatar(message.value(QStringLiteral("avatar")).toString());
            pendingRelayProfiles_.insert(senderId, QVariantMap {
                {QStringLiteral("name"), message.value(QStringLiteral("name")).toString().left(64)},
                {QStringLiteral("avatar"), avatar.size() <= 32768 ? avatar : QString {}}});
            pendingRelayRequests_.insert(senderId, QString::fromLatin1(senderPublic.toBase64(
                QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
            if (!pendingRequests_.contains(senderId)) pendingRequests_.append(senderId);
            if (!saveProfile()) {
                pendingRelayRequests_ = previousRequests;
                pendingRequests_ = previousPending;
                pendingRelayProfiles_ = previousProfiles;
                relay_.rejectCurrentPacket();
                setError(tr("好友申请未能保存，因此未确认收到；对方会重试。原因：%1").arg(vault_.error()));
                return;
            }
            emit pendingRequestsChanged();
            if (!previousRequests.contains(senderId)) emit incomingNotice(tr("新的好友申请"),message.value("name").toString().left(64));
            return;
        }
        if (type == QStringLiteral("friend_accept")) {
            for (auto& item : contacts_) {
                auto entry = item.toMap();
                if (entry.value(QStringLiteral("transport")).toString() != QStringLiteral("relay") ||
                    entry.value(QStringLiteral("uri")).toString() != senderId) continue;
                const auto previous = item;
                entry.insert(QStringLiteral("ready"), true);
                entry.insert(QStringLiteral("status"), tr("已验证"));
                entry.insert("avatar", safeAvatar(message.value("avatar").toString()));
                entry.insert("peerName", message.value("name").toString().left(64));
                if (!entry.value("hasRemark").toBool() && !entry.value("peerName").toString().trimmed().isEmpty()) entry.insert("name",entry.value("peerName"));
                item = entry;
                if (!saveProfile()) {
                    item = previous;
                    relay_.rejectCurrentPacket();
                    setError(tr("好友确认未能保存，尚未成为可聊天的好友。原因：%1").arg(vault_.error()));
                    return;
                }
                emit contactsChanged();
                emit activeContactChanged();
                return;
            }
            relay_.rejectCurrentPacket();
            return;
        }
        if (type == "profile_update") {
            for (auto& item : contacts_) {
                auto entry = item.toMap();
                if (entry.value("transport") != "relay" || entry.value("uri") != senderId || !entry.value("ready").toBool() || entry.value("group").toBool()) continue;
                const auto previous = item;
                entry.insert("avatar", safeAvatar(message.value("avatar").toString()));
                entry.insert("peerName", message.value("name").toString().left(64));
                if (!entry.value("hasRemark").toBool() && !entry.value("peerName").toString().trimmed().isEmpty()) entry.insert("name",entry.value("peerName"));
                item = entry;
                if (!saveProfile()) { item = previous; relay_.rejectCurrentPacket(); return; }
                emit contactsChanged(); emit activeContactChanged(); return;
            }
            relay_.rejectCurrentPacket(); return;
        }
        if (type == QStringLiteral("group_invite")) {
            const auto groupId = message.value(QStringLiteral("groupId")).toString();
            if(leftGroups_.contains(groupId))return;
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
            const auto previousGroups = pendingRelayGroups_;
            const auto previousPending = pendingGroupRequests_;
            pendingRelayGroups_.insert(groupId, message.toVariantMap());
            if (!pendingGroupRequests_.contains(groupId)) pendingGroupRequests_.append(groupId);
            if (!saveProfile()) {
                pendingRelayGroups_ = previousGroups;
                pendingGroupRequests_ = previousPending;
                relay_.rejectCurrentPacket();
                setError(tr("群邀请未能保存，因此未确认收到。原因：%1").arg(vault_.error()));
                return;
            }
            emit pendingGroupRequestsChanged();
            return;
        }
        if(type=="group_leave") {
            for(auto& value:contacts_){auto row=value.toMap();if(!row.value("group").toBool() || row.value("groupId").toString()!=message.value("groupId").toString())continue;
                auto members=row.value("members").toList();bool found=false;for(qsizetype i=members.size();i>0;--i)if(RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(members[i-1].toString()))==senderId){members.removeAt(i-1);found=true;}
                if(!found){relay_.rejectCurrentPacket();return;}const auto previous=value;row.insert("members",members);auto profiles=row.value("memberProfiles").toMap();profiles.remove(senderId);row.insert("memberProfiles",profiles);value=row;
                if(!saveProfile()){value=previous;relay_.rejectCurrentPacket();return;}emit contactsChanged();emit activeContactChanged();return;
            }return;
        }
        if(type=="group_member_profile") {
            const auto groupId=message.value("groupId").toString();
            for(auto& value:contacts_) {
                auto row=value.toMap();if(!row.value("group").toBool() || row.value("groupId").toString()!=groupId)continue;
                bool member=false;for(const auto& code:row.value("members").toList())member|=RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()))==senderId;
                if(!member){relay_.rejectCurrentPacket();return;}
                auto profiles=row.value("memberProfiles").toMap();const auto previous=value;
                profiles.insert(senderId,QVariantMap{{"name",message.value("name").toString().trimmed().left(64)},{"avatar",safeAvatar(message.value("avatar").toString())},{"nickname",message.value("nickname").toString().trimmed().left(64)}});
                row.insert("memberProfiles",profiles);value=row;
                if(!saveProfile()){value=previous;relay_.rejectCurrentPacket();return;}
                emit contactsChanged();emit activeContactChanged();emit messagesChanged();return;
            }
            relay_.rejectCurrentPacket();return;
        }
        if(type=="group_profile") {
            const auto groupId=message.value("groupId").toString();if(groupId.isEmpty())return;
            for(auto& value:contacts_) {
                auto row=value.toMap();if(!row.value("group").toBool() || row.value("groupId").toString()!=groupId)continue;
                bool member=false;for(const auto& code:row.value("members").toList())member|=RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()))==senderId;
                if(!member){relay_.rejectCurrentPacket();return;}
                const auto name=message.value("name").toString().trimmed();if(name.isEmpty()||name.size()>64)return;
                const auto previous=value;row.insert("name",name);row.insert("avatar",safeAvatar(message.value("avatar").toString()));value=row;
                if(!saveProfile()){value=previous;relay_.rejectCurrentPacket();return;}
                emit contactsChanged();emit activeContactChanged();return;
            }
            relay_.rejectCurrentPacket();return;
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
                        {QStringLiteral("body"), tr("%1：%2").arg(message.value(QStringLiteral("nickname")).toString().trimmed().left(64).isEmpty()
                            ? senderId.left(8) : message.value(QStringLiteral("nickname")).toString().trimmed().left(64), body)},
                        {QStringLiteral("outgoing"), false},
                        {QStringLiteral("senderId"), senderId},
                        {QStringLiteral("senderDetails"),memberSnapshot(entry,senderId,senderPublic)},
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
            const auto groupId = message.value("groupId").toString();
            for (const auto& item : contacts_) {
                const auto entry = item.toMap();
                if (!groupId.isEmpty() && entry.value("transport") == "relay" && entry.value("group").toBool() && entry.value("groupId").toString() == groupId) {
                    for (const auto& code : entry.value("members").toList()) if (RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString())) == senderId) contactId = entry.value("id").toString();
                    if (!contactId.isEmpty()) break;
                }
                if (entry.value(QStringLiteral("transport")).toString() == QStringLiteral("relay") &&
                    groupId.isEmpty() &&
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
            const auto mediaKind = message.value(QStringLiteral("mediaKind")).toString();
            if (!mediaKind.isEmpty() && mediaKind != "voice" && mediaKind != "sticker" && mediaKind != "photo") { relay_.rejectCurrentPacket(); return; }
            const auto size = message.value(QStringLiteral("size")).toInteger(-1);
            const auto count = message.value(QStringLiteral("count")).toInt();
            const auto index = message.value(QStringLiteral("index")).toInt(-1);
            const auto hash = message.value(QStringLiteral("sha256")).toString();
            static const QRegularExpression hexHash(QStringLiteral("^[0-9a-f]{64}$"));
            if (fileId.isEmpty() || fileId.size() > 80 || name.isEmpty() || name.size() > 180 ||
                size < 0 || size > 2 * 1024 * 1024 || count != qMax(1, int((size + 12287) / 12288)) ||
                index < 0 || index >= count || !hexHash.match(hash).hasMatch()) { relay_.rejectCurrentPacket(); return; }
            const auto bytes = QByteArray::fromBase64(message.value(QStringLiteral("data")).toString().toLatin1(),
                                                       QByteArray::Base64UrlEncoding);
            if (bytes.size() > 12288 || (size > 0 && bytes.isEmpty())) { relay_.rejectCurrentPacket(); return; }
            const auto key = senderId + QLatin1Char(':') + fileId;
            auto row = incomingRelayFiles_.value(key).toMap();
            if (!row.isEmpty() && (row.value(QStringLiteral("size")).toLongLong() != size ||
                                   row.value(QStringLiteral("hash")).toString() != hash ||
                                   row.value(QStringLiteral("count")).toInt() != count ||
                                   row.value(QStringLiteral("mediaKind")).toString() != mediaKind ||
                                   row.value("contactId").toString() != contactId)) { relay_.rejectCurrentPacket(); return; }
            auto chunks = row.value(QStringLiteral("chunks")).toMap();
            chunks.insert(QString::number(index), QString::fromLatin1(bytes.toBase64()));
            row.insert(QStringLiteral("name"), name);
            row.insert(QStringLiteral("mediaKind"), mediaKind);
            row.insert("contactId", contactId);
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
                relay_.rejectCurrentPacket(); setError(tr("收到的文件校验失败，请让对方重发")); return;
            }
            if (mediaKind == "voice" && (file.isEmpty() || file.size() > 1920000 || file.size() % 2)) { relay_.rejectCurrentPacket(); return; }
            if (mediaKind == "sticker" || mediaKind == "photo") {
                QBuffer imageBuffer(&file); imageBuffer.open(QIODevice::ReadOnly); QImageReader imageReader(&imageBuffer);
                const auto dims = imageReader.size();
                const int limit=mediaKind=="photo"?1600:512;
                if (!dims.isValid() || dims.width()>limit || dims.height()>limit || (mediaKind=="sticker" && file.size()>196608) || imageReader.read().isNull()) { relay_.rejectCurrentPacket(); return; }
            }
            if (!storeMessageForContact(contactId, QVariantMap {
                {QStringLiteral("body"), mediaKind == "voice" ? tr("语音 %1 秒").arg(qMax(1, file.size() / 32000)) : mediaKind == "photo" ? tr("[照片]") : mediaKind == "sticker" ? tr("[表情图片]") : tr("文件：%1（可保存）").arg(name)},
                {QStringLiteral("outgoing"), false},
                {QStringLiteral("senderId"), senderId},
                {QStringLiteral("senderDetails"), groupId.isEmpty()?QVariantMap():memberSnapshot([&]{for(const auto& v:contacts_)if(v.toMap().value("id")==contactId)return v.toMap();return QVariantMap();}(),senderId,senderPublic)},
                {QStringLiteral("kind"), mediaKind.isEmpty() ? QStringLiteral("file-offer") : mediaKind},
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
                        // A message is not an acceptance of a friend request.
                        relay_.rejectCurrentPacket();
                        return;
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
        updateMessageDelivery(packetId,state);
        if (assistedConnection_ && relay_.isConnected() &&
            packetId == selfTestPacketId_ && state == QStringLiteral("delivered")) {
            selfTestPassed_ = true;
            selfTestPacketId_.clear();
            networkStatus_ = tr("辅助连接自检通过 — 等待对方上线");
            emit networkStatusChanged();
        }
        if (packetId == peerProbePacketId_) {
            QString result;
            if (state == QStringLiteral("queued"))
                result = tr("检测已加入本机队列，正在等待中继响应");
            else if (state == QStringLiteral("recipient_offline"))
                result = tr("对方未连到同一个中继，或配对码已失效；将自动重试");
            else if (state == QStringLiteral("forwarded"))
                result = tr("中继已转发，等待对方设备确认");
            else if (state == QStringLiteral("delivered"))
                result = tr("端到端连接成功，可以发送好友申请");
            if (!result.isEmpty() && result != peerProbeStatus_) {
                peerProbeStatus_ = result;
                emit peerProbeStatusChanged();
                if (state == QStringLiteral("recipient_offline")) setError(result);
            }
        }
        bool contactsChangedLocal = false;
        for (auto& item : contacts_) {
            auto entry = item.toMap();
            if (entry.value(QStringLiteral("requestPacketId")).toString() != packetId) continue;
            if (entry.value(QStringLiteral("ready")).toBool()) continue;
            if (state == QStringLiteral("recipient_offline"))
                entry.insert(QStringLiteral("status"), tr("对方未连接当前中继，申请将自动重试"));
            else if (state == QStringLiteral("forwarded"))
                entry.insert(QStringLiteral("status"), tr("已转发，等待对方设备确认"));
            else if (state == QStringLiteral("delivered"))
                entry.insert(QStringLiteral("status"), tr("对方已收到申请，等待确认"));
            else continue;
            entry.insert(QStringLiteral("requestDelivery"), state);
            item = entry;
            contactsChangedLocal = true;
        }
        if (contactsChangedLocal) { saveProfile(); emit contactsChanged(); }
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
                    networkStatus_ = !relay_.isConnected() ? tr("辅助连接正在重试")
                        : (selfTestPassed_ ? tr("辅助连接自检通过 — 等待对方上线")
                                           : tr("中继已连接，正在验证本机加密收发"));
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
    auto savedProfile = vault_.loadConversation(QStringLiteral("__profile"));
    bool recoveredEmptyProfile = false;
    if (savedProfile.isEmpty() && vault_.hasConversation(QStringLiteral("__profile"))) {
        recoveredEmptyProfile = vault_.recoverEmptyConversation(QStringLiteral("__profile"),
            {QVariantMap {{QStringLiteral("accountId"), accountId_}}});
        if (recoveredEmptyProfile) savedProfile = vault_.loadConversation(QStringLiteral("__profile"));
    }
    profileReadable_ = !savedProfile.isEmpty() || !vault_.hasConversation(QStringLiteral("__profile"));
    const auto profileLoadError = vault_.error();
    if (!savedProfile.isEmpty()) {
        const auto profile = savedProfile.first().toMap();
        // The encrypted vault (and relay key), not the optional direct engine,
        // owns this profile. An unavailable/recreated engine must not discard it.
        {
            profileName_ = profile.value(QStringLiteral("profileName")).toString();
            profileAvatar_ = safeAvatar(profile.value(QStringLiteral("avatar")).toString());
            uid_ = profile.value(QStringLiteral("uid")).toString();
            for (const auto& value : profile.value("stickers").toList()) {
                if (stickers_.size() >= 50) break;
                const auto row = value.toMap(); const auto data = row.value("data").toString();
                if (data.size() > 262144) continue;
                auto bytes = QByteArray::fromBase64(data.toLatin1()); QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer,"PNG");
                const auto dims = reader.size();
                if (dims.isValid() && dims.width() <= 256 && dims.height() <= 256 && !reader.read().isNull()) stickers_.append(row);
            }
            directEndpoint_ = numericEndpointList(profile.value(QStringLiteral("directEndpoint")).toString());
            contacts_ = profile.value(QStringLiteral("contacts")).toList();
            leftGroups_=profile.value("leftGroups").toStringList();
            for(auto& value:contacts_){auto row=value.toMap();if(row.value("transport")=="relay"){row.insert("group",!row.value("groupId").toString().isEmpty());value=row;}}
            const auto network = profile.value(QStringLiteral("network")).toMap();
            assistedConnection_ = network.value(QStringLiteral("assistedConnection"), true).toBool();
            relayEndpoint_ = network.value(QStringLiteral("relayEndpoint")).toString();
            pendingRelayRequests_ = profile.value(QStringLiteral("pendingRelayRequests")).toMap();
            pendingRelayProfiles_ = profile.value(QStringLiteral("pendingRelayProfiles")).toMap();
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
            if (profile.value(QStringLiteral("accountId")).toString() != accountId_) {
                for (auto& item : contacts_) {
                    auto entry = item.toMap();
                    if (entry.value(QStringLiteral("transport")).toString() == QStringLiteral("relay") ||
                        entry.value(QStringLiteral("id")).toString() == QStringLiteral("welcome")) continue;
                    entry.insert(QStringLiteral("ready"), false);
                    entry.insert(QStringLiteral("status"), tr("直连身份已变化，请重新配对；聊天记录已保留"));
                    entry.remove(QStringLiteral("conversationId"));
                    item = entry;
                }
            }
        }
    }
    if (!accountId_.isEmpty() && networkConfig_.isValid()) {
        daemon_.configurePrivateNetwork(accountId_, networkConfig_, false);
        networkStatus_ = assistedConnection_ ? tr("辅助连接正在建立")
                                             : tr("纯直连 — 不使用公共引导或中继");
    }
    const auto builtInRelay = QString::fromLatin1(P2P_MESSENGER_DEFAULT_RELAY_URL);
    // Quick Tunnel addresses from earlier test builds expire. They were saved
    // with the profile and otherwise override the permanent relay forever.
    const auto savedRelayHost = QUrl(relayEndpoint_).host();
    const bool expiredQuickTunnel = !builtInRelay.isEmpty() &&
        savedRelayHost.endsWith(QStringLiteral(".trycloudflare.com"), Qt::CaseInsensitive);
    if (relayEndpoint_.isEmpty() || expiredQuickTunnel)
        relayEndpoint_ = builtInRelay;
    const auto environmentRelay = qEnvironmentVariable("P2P_MESSENGER_RELAY_URL").trimmed();
    if (!environmentRelay.isEmpty()) relayEndpoint_ = environmentRelay;
    relay_.setEndpoint(QUrl(relayEndpoint_));
    const auto savedRelayFiles = vault_.loadConversation(QStringLiteral("__relay_files"));
    if (!savedRelayFiles.isEmpty()) incomingRelayFiles_ = savedRelayFiles.first().toMap();
    relay_.setEnabled(assistedConnection_ && profileReadable_);
    if (!vault_.isReady() || !profileReadable_ || !relay_.isReady()) {
        networkStatus_ = tr("本机加密账号未就绪 — 原有数据已保留");
        setError(!vault_.isReady() ? vault_.error() : !profileReadable_
            ? tr("本地账号资料无法解密，已保留原文件，未重置账号或聊天记录。%1").arg(profileLoadError)
            : tr("端到端加密身份无法读取或生成，已保留原有数据。"));
    } else if (recoveredEmptyProfile) {
        setError(tr("原账号配置文件为 0 字节，已保留空文件备份并恢复可用配置。现有密钥和聊天记录未删除；若没有有效配置备份，账号名称和好友列表需要重新设置。"));
    }
    if (assistedConnection_ && !relay_.hasEndpoint()) {
        networkStatus_ = tr("辅助中继尚未部署，不能发送异网申请");
        emit networkStatusChanged();
    }
    if (!accountId_.isEmpty() && !profileName_.isEmpty())
        daemon_.setIdentityAlias(accountId_, profileName_);
    // Retire the old built-in onboarding conversation, including saved entries.
    // Its legacy vault file is left untouched; real conversations are retained.
    for (qsizetype i = contacts_.size(); i > 0; --i)
        if (contacts_[i-1].toMap().value(QStringLiteral("id")) == QStringLiteral("welcome"))
            contacts_.removeAt(i-1);
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
    activeContactId_ = contacts_.isEmpty() ? QString() : contacts_.first().toMap().value(QStringLiteral("id")).toString();
    if (!activeContactId_.isEmpty()) messages_ = vault_.loadConversation(activeContactId_);
    if (expiredQuickTunnel) saveProfile();
    relay_.enableDirectory(!profileName_.isEmpty());
    callDeadline_.setSingleShot(true);
    connect(&callDeadline_, &QTimer::timeout, this, [this] { endCall(); setError(tr("对方未接听或通话连接已中断")); });
    callHeartbeat_.setInterval(3000);
    connect(&callHeartbeat_, &QTimer::timeout, this, [this] {
        if (callState_ == "idle") return;
        callPing_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!relay_.sendLive(callKey_, QJsonObject {{"type","call_ping"},{"callId",callId_},{"nonce",callPing_}})) {
            endCall(); setError(tr("通话连接已断开，已自动挂断"));
        }
    });
    connect(&relay_, &RelayClient::peerUnavailable, this, [this](const QString& peer) {
        if (callState_ != "idle" && peer == callPeerId_) { endCall(); setError(tr("对方已离线，语音通话已自动挂断")); }
    });
    connect(&relay_, &RelayClient::connectedChanged, this, [this](bool online) { if (!online && callState_ != "idle") endCall(); });
    if (auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) connect(gui, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if ((state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden) && !voice_.permissionPending()) { finishVoice(false); endCall(); }
    });
    connect(&voice_, &VoiceEngine::recordingChanged, this, [this](bool active) {
        recording_ = active && callState_ == "idle";
        if (active && callState_ == "connecting") callState_ = "active";
        emit voiceChanged();
    });
    connect(&voice_, &VoiceEngine::recordingReady, this, [this](const QByteArray& pcm) { sendMedia(pcm, "voice", recordingContact_); });
    connect(&voice_, &VoiceEngine::playbackChanged, this, &MessengerController::voiceChanged);
    connect(&voice_, &VoiceEngine::errorOccurred, this, [this](const QString& reason) { finishVoice(false); endCall(); setError(reason); });
    connect(&voice_, &VoiceEngine::pcmReady, this, [this](const QByteArray& pcm) {
        if (callState_ == "active") relay_.sendLive(callKey_, QJsonObject {{"type", "call_audio"}, {"callId", callId_}, {"seq", callSequence_++}, {"data", QString::fromLatin1(pcm.toBase64())}});
    });
}

MessengerController::~MessengerController()
{
    endCall(); finishVoice(false);
    daemon_.stop();
}

QVariantMap MessengerController::activeEntry() const {
    for (const auto& item : contacts_) if (item.toMap().value("id").toString() == activeContactId_) return item.toMap();
    return {};
}
bool MessengerController::sendMedia(const QByteArray& data, const QString& kind, const QString& contactId,const QString& fileName) {
    QVariantMap target;
    for (const auto& item : contacts_) if (item.toMap().value("id").toString() == contactId) target = item.toMap();
    if (!assistedConnection_ || target.value("transport") != "relay" || !target.value("ready").toBool()) {
        setError(tr("请在辅助连接模式选择已确认的好友或群聊")); return false;
    }
    const bool isFile=kind=="file";
    if ((!isFile && data.isEmpty()) || data.size() > (isFile?2*1024*1024:1920000)) { setError(tr("文件为空或超过允许大小（文件 2 MB，语音/图片约 1.8 MB）")); return false; }
    const auto key = RelayClient::publicKeyFromCode(target.value("relayPublic").toString());
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QList<QJsonObject> packets;
    const auto count = qMax(1,int((data.size() + 12287) / 12288));
    const auto hash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    for (int i = 0; i < count; ++i) packets.append(QJsonObject {{"type", "file_chunk"}, {"mediaKind", kind}, {"fileId", id}, {"name", kind == "voice" ? "voice.pcm" : kind=="photo" ? "photo.jpg" : "sticker.png"}, {"size", int(data.size())}, {"count", count}, {"index", i}, {"sha256", hash}, {"data", QString::fromLatin1(data.mid(i * 12288, 12288).toBase64(QByteArray::Base64UrlEncoding))}});
    if(isFile)for(auto& packet:packets){packet.remove("mediaKind");packet.insert("name",QFileInfo(fileName).fileName().left(180));}
    QStringList packetIds;
    if (target.value("group").toBool()) {
        broadcastGroupMemberProfile(target);
        for (auto& packet : packets) { packet.insert("groupId", target.value("groupId").toString()); packet.insert("nickname", target.value("myNickname",profileName_).toString()); }
        bool sent = false;
        for (const auto& code : target.value("members").toList()) {
            const auto memberKey = RelayClient::publicKeyFromCode(code.toString());
            if (RelayClient::idForPublicKey(memberKey) == relay_.identityId()) continue;
            const auto ids=relay_.sendBatch(memberKey,packets);packetIds.append(ids);
            if (ids.size()!=count) { setError(tr("群内媒体发送未全部排队，请勿重复发送")); return false; }
            sent = true;
        }
        if (!sent) { setError(tr("群里没有其他成员")); return false; }
    } else {packetIds=relay_.sendBatch(key, packets);if(packetIds.size()!=count){setError(tr("文件或媒体未能保存到发送队列"));return false;}}
    const bool saved=storeMessageForContact(contactId, QVariantMap {{"body", isFile?tr("文件：%1").arg(QFileInfo(fileName).fileName()):kind == "voice" ? tr("语音 %1 秒").arg(qMax(1, data.size()/32000)) : kind=="photo" ? tr("[照片]") : tr("[表情图片]")}, {"kind", isFile?"file-offer":kind}, {"outgoing", true}, {"fileData", QString::fromLatin1(data.toBase64())},{"fileId",id},{"name",QFileInfo(fileName).fileName()},{"packetIds",packetIds},{"delivery",tr("已排队")}, {"time", QDateTime::currentDateTime().toString("HH:mm")}});
    if(!saved)setError(tr("媒体已排队，但本机聊天记录未能保存，请勿重复发送"));return saved;
}
bool MessengerController::sendPhoto(const QString& path) {
    const auto local=QUrl(path).isLocalFile()?QUrl(path).toLocalFile():path;
    QImageReader reader(local); reader.setAutoTransform(true); const auto dims=reader.size();
    if (!dims.isValid() || dims.width()>30000 || dims.height()>30000 || QFileInfo(local).size()>20*1024*1024) { setError(tr("照片无法读取或超过 20 MB")); return false; }
    reader.setScaledSize(dims.scaled(1600,1600,Qt::KeepAspectRatio)); auto image=reader.read();
    if (image.isNull()) { setError(tr("照片格式不支持，请换一张照片")); return false; }
    image=image.scaled(1600,1600,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    QByteArray bytes;
    for (int quality : {85,70,55}) { bytes.clear(); QBuffer output(&bytes); output.open(QIODevice::WriteOnly); if (!image.save(&output,"JPEG",quality)) return false; if (bytes.size()<=1920000) break; }
    return sendMedia(bytes,"photo",activeContactId_);
}
void MessengerController::choosePhoto(bool camera) {
#ifdef Q_OS_ANDROID
    const auto contact=activeContactId_;
    if (!openAvatarGallery(this,[this,contact](QString file,QString error) {
        if (!error.isEmpty()) { setError(error); return; } if (file.isEmpty()) return;
        if (activeContactId_!=contact) { setError(tr("会话已切换，请重新选择照片")); return; }
        sendPhoto(file);
    },camera?4:3)) setError(tr("系统图片选择器正在使用，或无法启动"));
#else
    Q_UNUSED(camera); emit photoPickerRequested();
#endif
}
void MessengerController::suspendForBackground() {
    endCall(); finishVoice(false);
    if (identityRefresh_) identityRefresh_->stop();
    if (pendingRefresh_) pendingRefresh_->stop();
    relay_.setEnabled(false);
    relay_.blockSignals(true); daemon_.blockSignals(true); gatewayMapper_.blockSignals(true);
    daemon_.stop();
}
bool MessengerController::sendSticker(const QString& path) {
    auto local = QUrl(path).isLocalFile() ? QUrl(path).toLocalFile() : path;
#ifdef Q_OS_ANDROID
    if (path.startsWith("content:")) local = QJniObject::callStaticObjectMethod("io/p2pmessenger/app/ContentFiles", "copyToCache", "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;", QNativeInterface::QAndroidApplication::context().object<jobject>(), QJniObject::fromString(path).object<jstring>()).toString();
#endif
    QImageReader reader(local); const auto size = reader.size();
    if (!size.isValid() || size.width()>10000 || size.height()>10000 || QFileInfo(local).size()>10*1024*1024) { setError(tr("请选择小于 10 MB 的表情图片")); return false; }
    reader.setScaledSize(size.scaled(256,256,Qt::KeepAspectRatio)); const auto image = reader.read();
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    if (image.isNull() || !image.save(&buffer,"PNG") || bytes.size()>196608) { setError(tr("表情图片无法读取或压缩")); return false; }
    const auto data = QString::fromLatin1(bytes.toBase64());
    const auto hash = QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
    bool exists = false; for (const auto& value : stickers_) exists |= value.toMap().value("hash") == hash;
    if (!exists && stickers_.size() < 50) {
        stickers_.append(QVariantMap {{"hash",hash},{"data",data}});
        if (!saveProfile()) { stickers_.removeLast(); setError(tr("表情未能保存到本机")); return false; }
        emit stickersChanged();
    }
    return sendMedia(bytes,"sticker",activeContactId_);
}
bool MessengerController::sendSavedSticker(int index) {
    if (index < 0 || index >= stickers_.size()) return false;
    return sendMedia(QByteArray::fromBase64(stickers_[index].toMap().value("data").toString().toLatin1()),"sticker",activeContactId_);
}
void MessengerController::removeSticker(int index) {
    if (index < 0 || index >= stickers_.size()) return;
    const auto previous = stickers_; stickers_.removeAt(index);
    if (!saveProfile()) { stickers_ = previous; setError(tr("表情删除未能保存")); return; } emit stickersChanged();
}
void MessengerController::recordVoice() {
    const auto entry = activeEntry();
    if (callState_ != "idle" || recording_) { setError(tr("请先结束当前录音或通话")); return; }
    if (!assistedConnection_ || !entry.value("ready").toBool() || entry.value("transport") != "relay") { setError(tr("请先选择辅助连接的已确认好友或群聊")); return; }
    recordingContact_ = activeContactId_; voice_.capture(false);
}
void MessengerController::finishVoice(bool send) { if (callState_ == "idle") voice_.stopCapture(send); }
void MessengerController::playVoice(const QString& data) { if (callState_ != "idle") { setError(tr("请先结束通话")); return; } voice_.stopPlayback();playingVoiceData_=data;voice_.play(QByteArray::fromBase64(data.toLatin1())); }
void MessengerController::captureScreenshot() {
#ifdef Q_OS_WIN
    discardScreenshot();
    if(activeContactId_.isEmpty()){setError(tr("请先选择好友或群聊"));return;}
    auto* window=QGuiApplication::focusWindow();auto* screen=window?window->screen():QGuiApplication::primaryScreen();
    if(!screen){setError(tr("没有可截图的显示器"));return;}
    const auto image=screen->grabWindow(0).toImage();
    if(image.isNull()){setError(tr("屏幕截图失败，请检查远程桌面或显示权限"));return;}
    QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
    if(!image.scaled(1600,1600,Qt::KeepAspectRatio,Qt::SmoothTransformation).save(&buffer,"JPEG",85)){setError(tr("截图转换失败"));return;}
    screenshotPreview_="data:image/jpeg;base64,"+QString::fromLatin1(bytes.toBase64());emit screenshotChanged();
#else
    setError(tr("此设备不支持桌面截图"));
#endif
}
bool MessengerController::sendScreenshot() {
    if(screenshotPreview_.isEmpty())return false;
    const auto raw=QByteArray::fromBase64(screenshotPreview_.section(',',1).toLatin1());
    if(!sendMedia(raw,"photo",activeContactId_))return false;
    discardScreenshot();return true;
}
void MessengerController::startCall() {
    const auto entry = activeEntry();
    if (callState_ != "idle" || recording_) { setError(tr("请先结束当前录音或通话")); return; }
    if (!assistedConnection_ || !relay_.isConnected() || !entry.value("ready").toBool() || entry.value("group").toBool() || entry.value("transport") != "relay") { setError(tr("语音通话需要辅助连接已接通，并选择已确认私聊好友")); return; }
    callKey_ = RelayClient::publicKeyFromCode(entry.value("relayPublic").toString()); callPeerId_ = RelayClient::idForPublicKey(callKey_);
    callId_ = QUuid::createUuid().toString(QUuid::WithoutBraces); callPeerName_ = entry.value("name").toString(); callState_ = "dialing";
    callSequence_ = 0; receivedSequence_ = -1;
    if (!relay_.sendLive(callKey_, QJsonObject {{"type","call_offer"},{"callId",callId_},{"expires",QDateTime::currentSecsSinceEpoch()+45}})) { endCall(); setError(tr("通话请求未能发出")); return; }
    callDeadline_.start(45000); callHeartbeat_.start(); emit voiceChanged();
}
void MessengerController::answerCall() {
    if (callState_ != "ringing") return;
    if (!relay_.sendLive(callKey_,QJsonObject {{"type","call_accept"},{"callId",callId_}})) { endCall(); setError(tr("无法接通通话")); return; }
    callState_ = "connecting"; callDeadline_.start(15000); emit voiceChanged(); voice_.capture(true);
}
void MessengerController::endCall() {
    if (callState_ == "idle") return;
    relay_.sendLive(callKey_,QJsonObject {{"type","call_end"},{"callId",callId_}});
    callDeadline_.stop(); callHeartbeat_.stop(); callPing_.clear();
    callState_ = "idle"; callId_.clear(); callKey_.clear(); callPeerId_.clear();
    voice_.stopCapture(false); voice_.stopPlayback(); emit voiceChanged();
}
void MessengerController::receiveCall(const QString& sender, const QByteArray& key, const QJsonObject& msg) {
    const auto type = msg.value("type").toString(), id = msg.value("callId").toString();
    if (id.size()!=36) return;
    if (type == "call_offer") {
        const auto expires = msg.value("expires").toInteger(), now = QDateTime::currentSecsSinceEpoch();
        if (expires < now || expires > now+60 || callState_ != "idle" || recording_) return;
        for (const auto& item : contacts_) {
            const auto entry = item.toMap();
            if (entry.value("uri") != sender || entry.value("transport") != "relay" || !entry.value("ready").toBool() || entry.value("group").toBool()) continue;
            callId_ = id; callKey_ = key; callPeerId_ = sender; callPeerName_ = entry.value("name").toString(); callState_ = "ringing"; callSequence_ = 0; receivedSequence_ = -1;
            callDeadline_.start(45000); callHeartbeat_.start(); emit voiceChanged(); return;
        }
        return;
    }
    if (sender != callPeerId_ || id != callId_ || callState_ == "idle") return;
    if (type == "call_ping") {
        const auto nonce = msg.value("nonce").toString();
        if (nonce.size() == 36) relay_.sendLive(key,QJsonObject {{"type","call_pong"},{"callId",id},{"nonce",nonce}});
        return;
    }
    if (type == "call_pong") {
        if (!callPing_.isEmpty() && msg.value("nonce").toString() == callPing_ && callState_ == "active") { callPing_.clear(); callDeadline_.start(15000); }
        return;
    }
    if (type == "call_end") { endCall(); return; }
    if (type == "call_accept" && callState_ == "dialing") { callState_ = "connecting"; callDeadline_.start(15000); emit voiceChanged(); voice_.capture(true); return; }
    if (type == "call_audio" && callState_ == "active") {
        const auto seq = msg.value("seq").toInt(-1); const auto data = QByteArray::fromBase64(msg.value("data").toString().toLatin1());
        if (seq <= receivedSequence_ || data.size() != 3200) return;
        receivedSequence_ = seq; callDeadline_.start(15000); voice_.play(data,true);
    }
}

QVariantList MessengerController::contacts() const { return contacts_; }
QVariantList MessengerController::messages() const { return messages_; }
QString MessengerController::activeContactId() const { return activeContactId_; }
QString MessengerController::activeContactName() const { return contactName(activeContactId_); }
QString MessengerController::networkStatus() const { return networkStatus_; }
QString MessengerController::inviteCode() const { return assistedConnection_ ? relay_.inviteCode() : inviteCode_; }
QString MessengerController::userCode() const { return uid_; }
QString MessengerController::registrationStatus() const {
    if (profileName_.isEmpty()) return tr("请输入昵称，创建本机账号");
    if (!uid_.isEmpty()) return tr("UID：%1 · 已注册").arg(uid_);
    if (relay_.registrationDeferred()) return tr("本机账号已创建 · 预留 UID 暂缓领取（1–10 未绑定）");
    if (!passwordConfigured() && !registrationPending()) return tr("本机昵称已保存 · 请设置密码并注册 UID");
    if (!assistedConnection_) return tr("本机账号已创建 · 切换辅助连接后注册 UID");
    return relay_.isConnected() ? tr("本机账号已创建 · 正在注册 UID") : tr("本机账号已创建 · 等待连接后注册 UID");
}
void MessengerController::copyUid() { QGuiApplication::clipboard()->setText(uid_); }
void MessengerController::broadcastProfile() {
    if (!assistedConnection_) return;
    for (const auto& item : contacts_) {
        const auto row = item.toMap();
        if (row.value("transport") == "relay" && row.value("ready").toBool() && !row.value("group").toBool())
            relay_.send(RelayClient::publicKeyFromCode(row.value("relayPublic").toString()), QJsonObject {{"type","profile_update"},{"name",profileName_},{"avatar",profileAvatar_}});
        else if(row.value("transport")=="relay" && row.value("ready").toBool() && row.value("group").toBool())broadcastGroupMemberProfile(row);
    }
}
bool MessengerController::addFriendByUid(const QString& uid, const QString& remark)
{
    const auto number = uid.trimmed();
    if (!QRegularExpression(QStringLiteral("^[1-9][0-9]{0,15}$")).match(number).hasMatch() || number == uid_) {
        setError(tr("请输入其他账号的有效数字 UID")); return false;
    }
    if (!assistedConnection_ || !relay_.isConnected()) { setError(tr("UID 查找需要接通辅助连接，当前中继未接通")); return false; }
    if(relay_.lookupPending()){setError(tr("正在查找账号，请等待本次查询完成"));return false;}
    for(const auto& value:contacts_){const auto row=value.toMap();if(!row.value("group").toBool() && row.value("peerUid").toString()==number){setError(row.value("ready").toBool()?tr("对方已经是你的好友，无需再次验证"):tr("好友申请正在等待确认"));return false;}}
    uidRemark_ = remark.trimmed().left(64);
    if (!relay_.lookupUid(number)) { setError(tr("已有查找正在进行，请稍候")); return false; }
    setError({}); return true;
}
QString MessengerController::avatarUrl() const { return profileAvatar_.isEmpty() ? QString {} : QStringLiteral("data:image/png;base64,") + profileAvatar_; }
QVariantList MessengerController::friendRequests() const
{
    QVariantList rows;
    for (const auto& id : pendingRequests_) {
        auto row = pendingRelayProfiles_.value(id).toMap();
        row.insert(QStringLiteral("id"), id);
        if (row.value(QStringLiteral("name")).toString().isEmpty()) row.insert(QStringLiteral("name"), tr("好友 %1").arg(id.left(8)));
        rows.append(row);
    }
    return rows;
}

void MessengerController::chooseAttachment(bool sticker) {
#ifdef Q_OS_ANDROID
    const auto contact = activeContactId_;
    if (!openAvatarGallery(this,[this,sticker,contact](const QString& file,const QString& error) {
        if (!error.isEmpty()) setError(error);
        else if (!file.isEmpty()) {
            if (contact != activeContactId_) { setError(tr("选择文件期间会话已改变，请重新选择")); return; }
            if (sticker) sendSticker(file); else queueFile(file);
        }
    },sticker ? 1 : 2)) setError(tr("文件选择器未能打开或已有选择窗口"));
#else
    emit attachmentPickerRequested(sticker);
#endif
}
void MessengerController::chooseAvatar() {
    if(uid_.isEmpty() || !passwordConfigured() || loginPending()){setError(tr("请先登录账号，再上传头像"));return;}
#ifdef Q_OS_ANDROID
    if (!openAvatarGallery(this,[this](const QString& file,const QString& error) {
        if (!error.isEmpty()) setError(error);
        else if (!file.isEmpty()) setAvatar(file);
    })) setError(tr("相册未能打开或已有选择窗口，请稍后重试"));
#else
    emit avatarPickerRequested();
#endif
}
bool MessengerController::setAvatar(const QString& path)
{
    if(uid_.isEmpty() || !passwordConfigured() || loginPending()){setError(tr("请先登录账号，再上传头像"));return false;}
    auto localPath = QUrl(path).isLocalFile() ? QUrl(path).toLocalFile() : path;
#ifdef Q_OS_ANDROID
    if (path.startsWith(QStringLiteral("content:"))) {
        const auto context = QNativeInterface::QAndroidApplication::context();
        localPath = QJniObject::callStaticObjectMethod("io/p2pmessenger/app/ContentFiles", "copyToCache",
            "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;",
            context.object<jobject>(), QJniObject::fromString(path).object<jstring>()).toString();
    }
#endif
    QImageReader reader(localPath);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 10000 || size.height() > 10000 || QFileInfo(localPath).size() > 10 * 1024 * 1024) {
        setError(tr("请选择不超过 10 MB、尺寸不超过 10000 像素的头像图片")); return false;
    }
    reader.setScaledSize(size.scaled(128, 128, Qt::KeepAspectRatio));
    const auto image = reader.read();
    QByteArray encoded;
    if (image.isNull()) { setError(tr("头像读取失败：%1").arg(reader.errorString())); return false; }
    for (int edge : {128,96,80,64}) {
        encoded.clear(); QBuffer buffer(&encoded); buffer.open(QIODevice::WriteOnly);
        const auto thumbnail = image.scaled(edge,edge,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        if (!thumbnail.save(&buffer,"PNG")) { setError(tr("头像转换 PNG 失败")); return false; }
        if (encoded.toBase64().size() <= 24000) break;
    }
    if (encoded.toBase64().size() > 24000) { setError(tr("头像压缩后仍然过大，请换一张图片")); return false; }
    const auto previous = profileAvatar_;
    profileAvatar_ = QString::fromLatin1(encoded.toBase64());
    if (!saveProfile()) { profileAvatar_ = previous; setError(tr("头像未保存：%1").arg(vault_.error())); return false; }
    emit profileNameChanged();
    broadcastProfile();
    setError({});
    return true;
}

bool MessengerController::setGroupNickname(const QString& name)
{
    const auto value = name.trimmed();
    if (value.isEmpty() || value.size() > 64) { setError(tr("群昵称须为 1 至 64 个字符")); return false; }
    for (auto& item : contacts_) {
        auto row = item.toMap();
        if (row.value(QStringLiteral("id")).toString() != activeContactId_ || !row.value(QStringLiteral("group")).toBool()) continue;
        const auto previous = item;
        row.insert(QStringLiteral("myNickname"), value); item = row;
        if (!saveProfile()) { item = previous; setError(tr("群昵称未保存：%1").arg(vault_.error())); return false; }
        emit contactsChanged(); setError({}); return true;
    }
    setError(tr("请先进入一个群聊再设置群昵称")); return false;
}
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
QString MessengerController::relayEndpoint() const { return relayEndpoint_; }
QString MessengerController::peerProbeStatus() const { return peerProbeStatus_; }

bool MessengerController::testPeerConnection(const QString& invite)
{
    if (!assistedConnection_ || !relay_.hasEndpoint()) {
        setError(tr("请先在两台设备上开启辅助连接"));
        return false;
    }
    if (!relay_.isConnected()) {
        setError(tr("本机还未连上中继，请检查网络和辅助连接状态"));
        return false;
    }
    if (!selfTestPassed_) {
        setError(tr("本机加密收发自检尚未通过，请等待状态显示“辅助连接自检通过”"));
        return false;
    }
    const auto peerKey = RelayClient::publicKeyFromCode(invite);
    if (peerKey.isEmpty() || RelayClient::idForPublicKey(peerKey) == relay_.identityId()) {
        setError(tr("请粘贴对方当前显示的完整 SD1 配对码，不能使用自己的配对码"));
        return false;
    }
    peerProbePacketId_ = relay_.send(peerKey, QJsonObject {
        {QStringLiteral("type"), QStringLiteral("probe")}
    });
    if (peerProbePacketId_.isEmpty()) {
        setError(tr("连接检测无法加入本机队列，请检查本机存储"));
        return false;
    }
    peerProbeStatus_ = tr("检测已发送，等待对方设备确认");
    emit peerProbeStatusChanged();
    setError({});
    return true;
}

bool MessengerController::registerAccount(const QString& name, const QString& password) {
    if (password.size()<8 || password.size()>128) { setError(tr("登录密码需要 8–128 个字符")); return false; }
    if (!setProfileName(name)) return false;
    if (!assistedConnection_ && !setAssistedConnection(true)) return false;
    if (!relay_.registerPasswordAccount(password,profileName_)) return false;
    emit registrationStatusChanged(); emit notificationPermissionRequested(); return true;
}
bool MessengerController::loginAccount(const QString& uid, const QString& password) {
    if (!profileName_.isEmpty() || !uid_.isEmpty() || !contacts_.isEmpty() || passwordConfigured()) {
        setError(tr("登录需使用新的本机账号资料，不能覆盖当前账号和聊天记录")); return false;
    }
    if (!assistedConnection_ && !setAssistedConnection(true)) return false;
    if (!relay_.loginPasswordAccount(uid.trimmed(),password)) { setError(tr("UID 或密码格式错误，或已经有登录操作")); return false; }
    return true;
}
bool MessengerController::setProfileName(const QString& name)
{
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 64) {
        setError(tr("账号名称须为 1 至 64 个字符"));
        return false;
    }
    const auto previous = profileName_;
    profileName_ = trimmed;
    if (!saveProfile()) {
        profileName_ = previous;
        setError(tr("账号名称无法保存到本机：%1").arg(profileReadable_ ? vault_.error()
            : tr("原账号配置无法读取，已阻止覆盖")));
        return false;
    }
    if (!accountId_.isEmpty()) daemon_.setIdentityAlias(accountId_, trimmed);
    setError({});
    emit profileNameChanged();
    emit pairingCodeChanged();
    relay_.enableDirectory(true);
    broadcastProfile();
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
    bool found = false;
    for (const auto& row : contacts_) found |= row.toMap().value(QStringLiteral("id")).toString() == contactId;
    if (!found) return;
    if (activeContactId_ == contactId)
        return;
    activeContactId_ = contactId;
    messages_ = vault_.loadConversation(activeContactId_);
    emit activeContactChanged();
    emit messagesChanged();
}
bool MessengerController::copyMessage(int index) {
    if(index<0 || index>=messages_.size())return false;
    QGuiApplication::clipboard()->setText(messages_[index].toMap().value("body").toString());emit operationCompleted(tr("已复制"),tr("消息已复制到剪贴板"));return true;
}
QVariantMap MessengerController::messageSenderDetails(int index) const {
    if(index<0 || index>=messages_.size())return {};
    const auto message=messages_[index].toMap();
    if(message.value("outgoing").toBool())return QVariantMap{{"name",profileName_},{"avatar",profileAvatar_},{"self",true},{"peerUid",uid_}};
    if(!activeIsGroup())return activeEntry();
    const auto sender=message.value("senderId").toString();
    auto details=message.value("senderDetails").toMap();const auto latest=activeEntry().value("memberProfiles").toMap().value(sender).toMap();
    for(const auto& value:contacts_){const auto row=value.toMap();if(!row.value("group").toBool() && row.value("uri")==sender){for(auto it=row.cbegin();it!=row.cend();++it)details.insert(it.key(),it.value());break;}}
    for(auto it=latest.cbegin();it!=latest.cend();++it)details.insert(it.key(),it.value());
    if(details.value("name").toString().isEmpty())details.insert("name",tr("群成员"));
    details.insert("uri",sender);return details;
}
bool MessengerController::addMessageSender(int index) {
    const auto details=messageSenderDetails(index);
    if(details.isEmpty() || details.value("self").toBool())return false;
    return addContact(details.value("name").toString(),details.value("relayPublic").toString());
}
bool MessengerController::deleteLocalMessage(int index) {
    if(index<0 || index>=messages_.size())return false;
    auto updated=messages_;updated.removeAt(index);
    if(!vault_.saveConversation(activeContactId_,updated)){setError(tr("消息删除未能保存，原消息已保留"));return false;}
    messages_=updated;emit messagesChanged();return true;
}
bool MessengerController::forwardMessage(int index,const QString& targetId) {
    if(index<0 || index>=messages_.size())return false;
    const auto message=messages_[index].toMap();const auto kind=message.value("kind").toString();
    if(kind=="photo" || kind=="voice" || kind=="sticker")return sendMedia(QByteArray::fromBase64(message.value("fileData").toString().toLatin1()),kind,targetId);
    if(kind=="file-offer")return sendMedia(QByteArray::fromBase64(message.value("fileData").toString().toLatin1()),"file",targetId,message.value("name").toString());
    if(kind!="text" && !kind.isEmpty()){setError(tr("这类消息暂不支持转发，请先保存文件后重新发送"));return false;}
    const auto previous=activeContactId_;selectContact(targetId);
    if(activeContactId_!=targetId)return false;
    const bool sent=sendMessage(message.value("body").toString());selectContact(previous);return sent;
}
bool MessengerController::removeActiveContact() {
    const auto previous=contacts_;const auto id=activeContactId_;const auto removed=activeEntry();const auto previousLeft=leftGroups_;const auto groupId=removed.value("groupId").toString();
    if(!groupId.isEmpty() && !leftGroups_.contains(groupId))leftGroups_.append(groupId);
    for(qsizetype i=contacts_.size();i>0;--i)if(contacts_[i-1].toMap().value("id").toString()==id)contacts_.removeAt(i-1);
    if(contacts_==previous)return false;
    if(!saveProfile()){contacts_=previous;leftGroups_=previousLeft;setError(tr("删除未能保存，原会话已保留"));return false;}
    if(removed.value("transport")=="relay" && !relay_.cancelContactPackets(removed.value("uri").toString(),groupId)){contacts_=previous;leftGroups_=previousLeft;saveProfile();setError(tr("待发送申请或消息无法取消，尚未删除"));return false;}
    if(!groupId.isEmpty())for(const auto& code:removed.value("members").toList()){const auto key=RelayClient::publicKeyFromCode(code.toString());if(RelayClient::idForPublicKey(key)!=relay_.identityId())relay_.send(key,QJsonObject{{"type","group_leave"},{"groupId",groupId}});}
    endCall();finishVoice(false);activeContactId_.clear();messages_.clear();emit contactsChanged();emit activeContactChanged();emit messagesChanged();return true;
}
bool MessengerController::renameGroup(const QString& name) {
    if(!activeIsGroup() || name.trimmed().isEmpty() || name.size()>64)return false;
    const auto previous=contacts_;
    for(auto& value:contacts_){auto row=value.toMap();if(row.value("id").toString()==activeContactId_){row.insert("name",name.trimmed());value=row;}}
    if(!saveProfile()){contacts_=previous;setError(tr("群名称无法保存"));return false;}
    emit contactsChanged();emit activeContactChanged();broadcastGroupProfile();return true;
}
void MessengerController::broadcastGroupProfile() {
    const auto group=activeEntry();if(!activeIsGroup()||group.value("transport")!="relay")return;
    for(const auto& code:group.value("members").toList()) {
        const auto key=RelayClient::publicKeyFromCode(code.toString());if(RelayClient::idForPublicKey(key)==relay_.identityId())continue;
        if(relay_.send(key,QJsonObject {{"type","group_profile"},{"groupId",group.value("groupId").toString()},{"name",group.value("name").toString()},{"avatar",group.value("avatar").toString()}}).isEmpty())setError(tr("群资料已保存，但同步未能排队，请稍后重试"));
    }
}
void MessengerController::broadcastGroupMemberProfile(const QVariantMap& group) {
    for(const auto& code:group.value("members").toList()) {
        const auto key=RelayClient::publicKeyFromCode(code.toString());if(RelayClient::idForPublicKey(key)==relay_.identityId())continue;
        relay_.send(key,QJsonObject{{"type","group_member_profile"},{"groupId",group.value("groupId").toString()},{"name",profileName_},{"avatar",profileAvatar_},{"nickname",group.value("myNickname",profileName_).toString()}});
    }
}
void MessengerController::chooseGroupAvatar() {
    if(!activeIsGroup())return;
#ifdef Q_OS_ANDROID
    const auto id=activeContactId_;
    if(!openAvatarGallery(this,[this,id](QString path,QString error){if(!error.isEmpty())setError(error);else if(!path.isEmpty()&&activeContactId_==id)setGroupAvatar(path);}))setError(tr("相册正在使用或无法打开，请检查系统相册权限"));
#else
    emit groupAvatarPickerRequested();
#endif
}
bool MessengerController::setGroupAvatar(const QString& path) {
    if(!activeIsGroup()||uid_.isEmpty()||!passwordConfigured())return false;
    const auto local=QUrl(path).isLocalFile()?QUrl(path).toLocalFile():path;
    QImageReader reader(local);const auto dims=reader.size();
    if(!dims.isValid()||dims.width()>10000||dims.height()>10000){setError(tr("群头像无法读取"));return false;}
    reader.setAutoTransform(true);reader.setScaledSize(dims.scaled(128,128,Qt::KeepAspectRatio));auto image=reader.read();QByteArray bytes;
    if(image.isNull())return false;
    for(int size:{128,96,64}){bytes.clear();QBuffer output(&bytes);output.open(QIODevice::WriteOnly);if(!image.scaled(size,size,Qt::KeepAspectRatio,Qt::SmoothTransformation).save(&output,"PNG"))return false;if(bytes.size()<=18000)break;}
    if(bytes.size()>18000){setError(tr("群头像过大，请换一张图片"));return false;}
    const auto previous=contacts_;for(auto& value:contacts_){auto row=value.toMap();if(row.value("id").toString()==activeContactId_){row.insert("avatar",QString::fromLatin1(bytes.toBase64()));value=row;}}
    if(!saveProfile()){contacts_=previous;setError(tr("群头像无法保存"));return false;}
    emit contactsChanged();emit activeContactChanged();broadcastGroupProfile();return true;
}

bool MessengerController::addContact(const QString& name, const QString& invite)
{
    if (invite.trimmed().isEmpty()) {
        setError(tr("还没有填写配对码。请让对方点击“分享我的配对码”后发给你。"));
        return false;
    }
    auto peerCode = invite.trimmed();
    if (assistedConnection_) {
        if (!selfTestPassed_) {
            setError(tr("本机加密收发自检尚未通过，不能发送好友申请"));
            return false;
        }
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
        for(const auto& value:contacts_){const auto row=value.toMap();if(row.value("uri").toString()==peerId && !row.value("group").toBool()){setError(row.value("ready").toBool()?tr("对方已经是你的好友，无需再次验证"):tr("已发送好友申请，请等待对方确认"));return false;}}
        const auto packetId = relay_.send(peerKey, QJsonObject {
            {QStringLiteral("type"), QStringLiteral("friend_request")},
            {QStringLiteral("name"), profileName_},
            {QStringLiteral("avatar"), profileAvatar_}
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
        entry.insert("hasRemark", !name.trimmed().isEmpty());
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
    if (accountId_.isEmpty() || inviteCode_.isEmpty()) {
        setError(tr("直连身份仍在生成，稍后重试，或切换到辅助连接"));
        return false;
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
            if (!assistedConnection_) {
                setError(tr("此会话使用辅助连接。请先在本机账号中切换到辅助连接，再发送消息。"));
                return false;
            }
            if (current.value(QStringLiteral("group")).toBool()) {
                broadcastGroupMemberProfile(current);
                int queued = 0;
                for (const auto& code : current.value(QStringLiteral("members")).toList()) {
                    const auto key = RelayClient::publicKeyFromCode(code.toString());
                    if (RelayClient::idForPublicKey(key) == relay_.identityId()) continue;
                    if (!relay_.send(key, QJsonObject {
                        {QStringLiteral("type"), QStringLiteral("group_text")},
                        {QStringLiteral("groupId"), current.value(QStringLiteral("groupId")).toString()},
                        {QStringLiteral("nickname"), current.value(QStringLiteral("myNickname"), profileName_).toString()},
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
            if (!assistedConnection_) {
                setError(tr("此会话使用辅助连接。请先在本机账号中切换到辅助连接，再发送文件。"));
                return false;
            }
            if (current.value(QStringLiteral("group")).toBool()) {
                QFile input(localPath);
                if(!input.open(QIODevice::ReadOnly) || input.size()>2*1024*1024){setError(tr("文件无法读取或超过 2 MB"));return false;}
                const auto data=input.readAll();if(data.size()!=input.size()){setError(tr("文件读取不完整"));return false;}
                return sendMedia(data,"file",activeContactId_,QFileInfo(localPath).fileName());
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
            return sendMedia(file,"file",activeContactId_,QFileInfo(localPath).fileName());
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
        if (peerKey.isEmpty()) { setError(tr("好友申请的公钥无效")); return false; }
        const auto previousContacts = contacts_;
        const auto previousPending = pendingRequests_;
        const auto previousRequests = pendingRelayRequests_, previousProfiles = pendingRelayProfiles_;
        const auto peerProfile = pendingRelayProfiles_.value(contactUri).toMap();
        QString selectedId;
        pendingRelayRequests_.remove(contactUri);
        pendingRelayProfiles_.remove(contactUri);
        pendingRequests_.removeAll(contactUri);
        // Both users can send a request at the same time. Upgrade the existing
        // outgoing row instead of creating two sessions for the same peer.
        for (auto& item : contacts_) {
            auto entry = item.toMap();
            if (entry.value(QStringLiteral("transport")).toString() != QStringLiteral("relay") ||
                entry.value(QStringLiteral("group")).toBool() ||
                entry.value(QStringLiteral("uri")).toString() != contactUri) continue;
            entry.insert(QStringLiteral("relayPublic"), peerCode);
            entry.insert(QStringLiteral("ready"), true);
            entry.insert(QStringLiteral("status"), tr("已验证"));
            entry.insert("avatar", safeAvatar(peerProfile.value("avatar").toString()));
            item = entry;
            selectedId = entry.value("id").toString(); break;
        }
        if (selectedId.isEmpty()) {
        selectedId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto entry = contact(selectedId, peerProfile.value(QStringLiteral("name")).toString().isEmpty()
            ? tr("好友 %1").arg(contactUri.left(8)) : peerProfile.value(QStringLiteral("name")).toString(), tr("已验证"));
        entry.insert(QStringLiteral("transport"), QStringLiteral("relay"));
        entry.insert(QStringLiteral("uri"), contactUri);
        entry.insert(QStringLiteral("relayPublic"), peerCode);
        entry.insert("avatar", safeAvatar(peerProfile.value("avatar").toString()));
        entry.insert(QStringLiteral("ready"), true);
        contacts_.append(entry);
        }
        if (!saveProfile()) {
            contacts_ = previousContacts; pendingRequests_ = previousPending;
            pendingRelayRequests_ = previousRequests; pendingRelayProfiles_ = previousProfiles;
            setError(tr("好友确认无法保存，未发出确认：%1").arg(vault_.error())); return false;
        }
        if (relay_.send(peerKey,QJsonObject {{"type","friend_accept"},{"name",profileName_},{"avatar",profileAvatar_}}).isEmpty()) {
            contacts_ = previousContacts; pendingRequests_ = previousPending;
            pendingRelayRequests_ = previousRequests; pendingRelayProfiles_ = previousProfiles;
            const bool restored = saveProfile();
            setError(restored ? tr("接受回复未能进入发送队列，申请仍待处理") : tr("接受回复未能进入发送队列，回滚保存也失败，请检查存储")); return false;
        }
        emit contactsChanged(); emit pendingRequestsChanged(); selectContact(selectedId);
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
        setError(tr("辅助连接身份未能创建：%1").arg(vault_.isReady()
            ? tr("端到端加密密钥生成或保存失败") : vault_.error()));
        return false;
    }
    if (enabled && !relay_.hasEndpoint()) {
        setError(tr("辅助中继尚未部署，暂时不能切换到辅助连接。"));
        return false;
    }
    // Relay transport uses its own persistent X25519 identity. Never gate it
    // on Jami registration. Disabling relay must also work without the engine.
    if (!accountId_.isEmpty()) daemon_.configurePrivateNetwork(accountId_, networkConfig_, false);
    assistedConnection_ = enabled;
    if (!saveProfile()) {
        assistedConnection_ = !enabled;
        setError(tr("连接模式无法保存到本机：%1").arg(profileReadable_ ? vault_.error()
            : tr("原账号配置无法读取，已阻止覆盖；请先恢复配置")));
        return false;
    }
    selfTestPassed_ = false;
    selfTestPacketId_.clear();
    peerProbePacketId_.clear();
    peerProbeStatus_.clear();
    emit peerProbeStatusChanged();
    relay_.setEnabled(enabled);
    networkStatus_ = enabled ? tr("辅助连接正在建立 — 通过你的加密中继")
                             : tr("纯直连 — 不使用公共引导或中继");
    emit assistedConnectionChanged();
    emit inviteCodeChanged();
    emit pairingCodeChanged();
    emit networkStatusChanged();
    setError({});
    return true;
}

bool MessengerController::customRelay() const { return relayEndpoint_ != QString::fromLatin1(P2P_MESSENGER_DEFAULT_RELAY_URL); }
bool MessengerController::restoreDefaultRelay() { return setRelayEndpoint(QString::fromLatin1(P2P_MESSENGER_DEFAULT_RELAY_URL)); }
bool MessengerController::setRelayEndpoint(const QString& endpoint)
{
    const auto input = endpoint.trimmed();
    const QUrl url(input, QUrl::StrictMode);
    const bool localWs = url.scheme() == QStringLiteral("ws") &&
                         (url.host() == QStringLiteral("localhost") ||
                          url.host() == QStringLiteral("127.0.0.1"));
    const bool secureRemote = url.scheme() == QStringLiteral("wss") && !url.host().isEmpty();
    if ((!input.isEmpty() && !(localWs || secureRemote)) || !url.userInfo().isEmpty() ||
        !url.query().isEmpty() || !url.fragment().isEmpty() ||
        (!url.path().isEmpty() && url.path() != QStringLiteral("/"))) {
        setError(tr("中继地址须为 wss://域名（本机测试可用 ws://localhost:端口），不要包含路径或账号。"));
        return false;
    }
    if (assistedConnection_ && input.isEmpty()) {
        setError(tr("请先切回纯直连，再清除中继地址。"));
        return false;
    }
    const auto previous = relayEndpoint_;
    const auto previousUid = uid_;
    uid_.clear();
    relayEndpoint_ = input.isEmpty() ? QString {} : url.toString(QUrl::FullyEncoded);
    if (!saveProfile()) {
        relayEndpoint_ = previous;
        uid_ = previousUid;
        setError(tr("中继地址无法保存到本机：%1").arg(profileReadable_ ? vault_.error()
            : tr("原账号配置无法读取，已阻止覆盖")));
        return false;
    }
    relay_.setEndpoint(QUrl(relayEndpoint_));
    emit uidChanged();
    peerProbePacketId_.clear();
    peerProbeStatus_.clear();
    emit peerProbeStatusChanged();
    emit relayEndpointChanged();
    if (assistedConnection_) {
        networkStatus_ = tr("辅助连接正在接入新中继");
        emit networkStatusChanged();
    }
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
    if (contactId != "welcome" && !message.value("outgoing").toBool())
        emit incomingNotice(contactName(contactId),message.value("body").toString().left(120));
    return true;
}

bool MessengerController::saveProfile()
{
    if (!profileReadable_) return false;
    return vault_.saveConversation(QStringLiteral("__profile"),
                            {QVariantMap {{QStringLiteral("accountId"), accountId_},
                                          {QStringLiteral("profileName"), profileName_},
                                          {QStringLiteral("avatar"), profileAvatar_},
                                          {QStringLiteral("stickers"), stickers_},
                                          {QStringLiteral("uid"), uid_},
                                          {QStringLiteral("directEndpoint"), directEndpoint_},
                                          {QStringLiteral("contacts"), contacts_},
                                          {QStringLiteral("leftGroups"),leftGroups_},
                                          {QStringLiteral("pendingRelayRequests"), pendingRelayRequests_},
                                          {QStringLiteral("pendingRelayProfiles"), pendingRelayProfiles_},
                                          {QStringLiteral("pendingRelayGroups"), pendingRelayGroups_},
                                          {QStringLiteral("network"), QVariantMap {
                                               {QStringLiteral("assistedConnection"), assistedConnection_},
                                               {QStringLiteral("relayEndpoint"), relayEndpoint_},
                                               {QStringLiteral("bootstrapNode"), networkConfig_.bootstrapNode},
                                               {QStringLiteral("turnHost"), networkConfig_.turnHost},
                                               {QStringLiteral("turnPort"), networkConfig_.turnPort},
                                               {QStringLiteral("turnUser"), networkConfig_.turnUser},
                                               {QStringLiteral("turnPassword"), networkConfig_.turnPassword}}}}});
}

void MessengerController::updateMessageDelivery(const QString& packetId,const QString& state) {
    if(state!="delivered" && state!="recipient_offline" && state!="forwarded")return;
    for(const auto& item:contacts_) {
        const auto contactId=item.toMap().value("id").toString();auto stored=contactId==activeContactId_?messages_:vault_.loadConversation(contactId);bool changed=false;
        for(auto& value:stored) {
            auto row=value.toMap();auto ids=row.value("packetIds").toStringList();if(ids.isEmpty()&&!row.value("packetId").toString().isEmpty())ids.append(row.value("packetId").toString());
            if(!ids.contains(packetId) || !row.value("outgoing").toBool())continue;
            auto delivered=row.value("deliveredPackets").toStringList();
            if(state=="delivered" && !delivered.contains(packetId))delivered.append(packetId);
            row.insert("deliveredPackets",delivered);
            row.insert("delivery",delivered.size()==ids.size()?tr("已送达"):state=="recipient_offline"?tr("等待对方上线"):delivered.isEmpty()?tr("发送中"):tr("发送中 %1/%2").arg(delivered.size()).arg(ids.size()));
            if(row!=value.toMap()){value=row;changed=true;}
        }
        if(changed) {
            if(!vault_.saveConversation(contactId,stored)){setError(tr("送达状态未能保存，请检查本机存储"));return;}
            if(contactId==activeContactId_){messages_=stored;emit messagesChanged();}return;
        }
    }
}

void MessengerController::setError(const QString& error)
{
    lastError_ = error;
    if (!customRelay()) {
        const auto host = QUrl(relayEndpoint_).host();
        if (!host.isEmpty()) lastError_.replace(host,tr("辅助服务"));
    }
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
