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
#include <QThreadPool>
#include <QPointer>
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
QString displayedGroupNumber(const QString& id){if(id.isEmpty())return {};bool ok=false;const auto n=QCryptographicHash::hash(id.toUtf8(),QCryptographicHash::Sha256).toHex().first(16).toULongLong(&ok,16);return ok?QString::number(1000000000000000ULL+n%9000000000000000ULL):QString();}
QString numericUid(const QJsonObject& message){const auto uid=message.value("uid").toString();return QRegularExpression("^[1-9][0-9]{0,15}$").match(uid).hasMatch()?uid:QString();}
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
    , files_(vault_,relay_,this)
{
    connect(&files_,&FileStream::error,this,&MessengerController::setError);
    connect(&relay_,&RelayClient::friendRepairReceived,this,[this](const QString& ticket,const QVariantMap& repair){
        if(appliedFriendRepairs_.contains(ticket) || repair.value("uid").toString()!=uid_ || !QStringList{"1","11","12","13"}.contains(uid_))return;
        const auto peers=repair.value("peers").toList();if(peers.size()!=(uid_=="1"?3:1))return;
        QStringList seen;for(const auto& value:peers){const auto peer=value.toMap();const auto uid=peer.value("uid").toString();if(!QStringList{"1","11","12","13"}.contains(uid) || uid==uid_ || (uid_!="1" && uid!="1") || seen.contains(uid) || RelayClient::publicKeyFromCode(peer.value("code").toString()).isEmpty())return;seen.append(uid);}
        const auto previous=contacts_;const auto previousApplied=appliedFriendRepairs_;const auto oldRequests=pendingRequests_;const auto oldKeys=pendingRelayRequests_;const auto oldProfiles=pendingRelayProfiles_;
        for(const auto& value:peers){const auto peer=value.toMap();const auto code=peer.value("code").toString(),id=RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code));bool found=false;
            // A peer-provided UID is display metadata, not proof of identity. Do not
            // merge another identity's history merely because it claims this UID.
            for(auto& contactValue:contacts_){auto row=contactValue.toMap();if(row.value("group").toBool() || (row.value("uri")!=id && row.value("relayPublic")!=code))continue;
                row.insert("uri",id);row.insert("relayPublic",code);row.insert("peerUid",peer.value("uid"));row.insert("peerName",peer.value("name"));row.insert("ready",true);row.insert("transport","relay");row.insert("status",tr("已验证"));row.remove("requestPacketId");if(!row.value("hasRemark").toBool())row.insert("name",peer.value("name"));contactValue=row;found=true;break;
            }
            if(!found){auto row=contact(QUuid::createUuid().toString(QUuid::WithoutBraces),peer.value("name").toString(),tr("已验证"));row.insert("uri",id);row.insert("relayPublic",code);row.insert("peerUid",peer.value("uid"));row.insert("peerName",peer.value("name"));row.insert("ready",true);row.insert("transport","relay");contacts_.append(row);}
            pendingRequests_.removeAll(id);pendingRelayRequests_.remove(id);pendingRelayProfiles_.remove(id);
        }
        appliedFriendRepairs_.append(ticket);if(!saveProfile()){contacts_=previous;appliedFriendRepairs_=previousApplied;pendingRequests_=oldRequests;pendingRelayRequests_=oldKeys;pendingRelayProfiles_=oldProfiles;setError(tr("好友关系修复未能保存，请检查本机空间"));return;}
        emit contactsChanged();emit activeContactChanged();emit pendingRequestsChanged();broadcastProfile();emit operationCompleted(tr("好友关系已修复"),tr("UID 1 与 11–13 的已授权好友关系已恢复，聊天记录未清空"));
    });
    connect(&files_,&FileStream::fileReady,this,[this](const QString& contactId,const QVariantMap& message){
        const auto stored=contactId==activeContactId_?messages_:vault_.loadConversation(contactId);
        for(const auto& value:stored)if(value.toMap().value("messageId")==message.value("messageId") && value.toMap().value("senderId")==message.value("senderId"))return;
        if(!storeMessageForContact(contactId,message)){relay_.rejectCurrentPacket();setError(tr("文件记录无法保存，请检查本机空间"));}
    });
    connect(&files_,&FileStream::fileSaved,this,[this](const QString& path){
#ifdef Q_OS_ANDROID
        if(androidDownloadDestinations_.contains(path)){
            const auto destination=androidDownloadDestinations_.take(path);const auto context=QNativeInterface::QAndroidApplication::context();
            QPointer<MessengerController> guard(this);QThreadPool::globalInstance()->start([guard,path,destination,context]{
                const bool ok=QJniObject::callStaticMethod<jboolean>("io/p2pmessenger/app/ContentFiles","copyFromCache","(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z",context.object<jobject>(),QJniObject::fromString(path).object<jstring>(),QJniObject::fromString(destination).object<jstring>());
                QFile::remove(path);QMetaObject::invokeMethod(QCoreApplication::instance(),[guard,ok]{if(!guard)return;if(ok)emit guard->operationCompleted(tr("文件已保存"),tr("已写入你选择的位置"));else guard->setError(tr("无法保存到选定位置，请检查存储权限和空间"));},Qt::QueuedConnection);
            });return;
        }
#endif
        emit operationCompleted(tr("文件已保存"),tr("已写入你选择的位置"));
    });
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
    connect(&relay_,&RelayClient::contactBackupRestored,this,[this](const QVariantMap& restored){
        if(!contacts_.isEmpty())return; // Never replace a populated local profile.
        const auto rows=restored.value("contacts").toList();if(rows.size()>200)return;
        QVariantList validated;
        for(const auto& value:rows){const auto row=value.toMap();const auto key=RelayClient::publicKeyFromCode(row.value("relayPublic").toString());
            if(row.value("transport")!="relay" || row.value("id").toString().isEmpty())return;
            if(!row.value("group").toBool() && (key.isEmpty() || row.value("uri").toString()!=RelayClient::idForPublicKey(key)))return;
            if(row.value("group").toBool()){if(row.value("groupId").toString().isEmpty() || row.value("members").toList().size()>20)return;for(const auto& code:row.value("members").toList())if(RelayClient::publicKeyFromCode(code.toString()).isEmpty())return;}
            validated.append(row);
        }
        contacts_=validated;leftGroups_=restored.value("leftGroups").toStringList();appliedFriendRepairs_=restored.value("appliedFriendRepairs").toStringList();removedGroupRevisions_=restored.value("removedGroupRevisions").toMap();profileAvatar_=safeAvatar(restored.value("avatar").toString());
        if(!saveProfile()){contacts_.clear();setError(tr("好友备份已解密，但本机无法保存，请检查空间"));return;}
        emit contactsChanged();emit activeContactChanged();
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
        if(type=="stream_chunk"){
            const auto group=message.value("groupId").toString();QString contactId;
            for(const auto& value:contacts_){const auto row=value.toMap();if(row.value("transport")!="relay" || !row.value("ready").toBool())continue;
                if(group.isEmpty() && !row.value("group").toBool() && row.value("uri")==senderId){contactId=row.value("id").toString();break;}
                if(!group.isEmpty() && row.value("groupId")==group){for(const auto& code:row.value("members").toList())if(RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()))==senderId)contactId=row.value("id").toString();if(!contactId.isEmpty())break;}
            }
            if(contactId.isEmpty() || !files_.receive(senderId,contactId,message))relay_.rejectCurrentPacket();return;
        }
        if (type.startsWith(QStringLiteral("call_"))) { receiveCall(senderId, senderPublic, message); return; }
        // The relay client sends an authenticated receipt for this probe.
        // It must not create a contact or appear in chat history.
        if (type == QStringLiteral("probe")) return;
        if(type=="withdraw"){
            const auto id=message.value("messageId").toString(),group=message.value("groupId").toString();if(id.size()!=36)return;
            for(const auto& value:contacts_){const auto row=value.toMap();if(row.value("transport")!="relay" || !row.value("ready").toBool())continue;
                bool allowed=group.isEmpty()?(!row.value("group").toBool() && row.value("uri")==senderId):row.value("groupId").toString()==group;
                if(allowed && !group.isEmpty()){allowed=false;for(const auto& code:row.value("members").toList())allowed|=RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()))==senderId;}
                if(!allowed)continue;const auto contactId=row.value("id").toString();auto stored=contactId==activeContactId_?messages_:vault_.loadConversation(contactId);bool changed=false;
                for(auto& item:stored){auto msg=item.toMap();if(msg.value("messageId")==id && !msg.value("outgoing").toBool() && msg.value("senderId")==senderId){msg.insert("body",tr("对方撤回了一条消息"));msg.insert("kind","withdrawn");msg.remove("fileData");msg.remove("filePath");msg.remove("fileId");item=msg;changed=true;}}
                if(message.value("file").toBool()){
                    if(!files_.cancelIncoming(senderId,contactId,id)){relay_.rejectCurrentPacket();return;}
                    if(!changed){if(!storeMessageForContact(contactId,QVariantMap{{"body",tr("对方撤回了一条消息")},{"kind","withdrawn"},{"outgoing",false},{"messageId",id},{"senderId",senderId},{"time",QDateTime::currentDateTime().toString("HH:mm")}}))relay_.rejectCurrentPacket();return;}
                }
                if(!changed){relay_.rejectCurrentPacket();return;}
                if(!vault_.saveConversation(contactId,stored)){relay_.rejectCurrentPacket();return;}if(contactId==activeContactId_){messages_=stored;emit messagesChanged();}return;
            }relay_.rejectCurrentPacket();return;
        }
        if (type == QStringLiteral("friend_request")) {
            for(const auto& value:contacts_){const auto row=value.toMap();if(row.value("uri")==senderId && row.value("transport")=="relay" && !row.value("group").toBool() && row.value("ready").toBool())return;}
            const auto previousRequests = pendingRelayRequests_;
            const auto previousPending = pendingRequests_;
            const auto previousProfiles = pendingRelayProfiles_;
            const auto avatar = safeAvatar(message.value(QStringLiteral("avatar")).toString());
            pendingRelayProfiles_.insert(senderId, QVariantMap {
                {QStringLiteral("name"), message.value(QStringLiteral("name")).toString().left(64)},
                {QStringLiteral("peerUid"),numericUid(message)},
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
                if(!numericUid(message).isEmpty())entry.insert("peerUid",numericUid(message));
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
                if(!numericUid(message).isEmpty())entry.insert("peerUid",numericUid(message));
                if (!entry.value("hasRemark").toBool() && !entry.value("peerName").toString().trimmed().isEmpty()) entry.insert("name",entry.value("peerName"));
                item = entry;
                if (!saveProfile()) { item = previous; relay_.rejectCurrentPacket(); return; }
                emit contactsChanged(); emit activeContactChanged(); return;
            }
            relay_.rejectCurrentPacket(); return;
        }
        if (type == QStringLiteral("group_invite")) {
            const auto groupId = message.value(QStringLiteral("groupId")).toString();
            if(removedGroupRevisions_.contains(groupId) && message.value("membersRevision").toInteger()<=removedGroupRevisions_.value(groupId).toLongLong())return;
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
            const auto owner=RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(members.first().toString()));
            if(owner!=senderId || (!message.value("ownerId").toString().isEmpty() && message.value("ownerId").toString()!=owner)){relay_.rejectCurrentPacket();return;}
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
            if(!previousGroups.contains(groupId))emit incomingNotice(tr("新的群邀请"),message.value("name").toString().left(64));
            return;
        }
        if(type=="group_invite_request" || type=="group_remove_request"){
            const auto groupId=message.value("groupId").toString();
            for(const auto& value:contacts_){const auto group=value.toMap();if(!group.value("group").toBool() || group.value("groupId")!=groupId || group.value("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(group.value("members").toList().value(0).toString()))).toString()!=relay_.identityId())continue;
                bool member=false;for(const auto& code:group.value("members").toList())member|=RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()))==senderId;
                if(!member){relay_.rejectCurrentPacket();return;}
                if(type=="group_invite_request"){QStringList add;for(const auto& code:message.value("codes").toArray())add.append(code.toString());if(!changeGroupMembers(groupId,add,{}))relay_.rejectCurrentPacket();}
                else {const auto target=message.value("memberId").toString();if(!group.value("admins").toStringList().contains(senderId) || group.value("admins").toStringList().contains(target)){relay_.rejectCurrentPacket();return;}if(!changeGroupMembers(groupId,{},target))relay_.rejectCurrentPacket();}
                return;
            }relay_.rejectCurrentPacket();return;
        }
        if(type=="group_members"){
            const auto groupId=message.value("groupId").toString();
            for(qsizetype i=0;i<contacts_.size();++i){auto row=contacts_[i].toMap();if(!row.value("group").toBool() || row.value("groupId")!=groupId)continue;
                const auto owner=row.value("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(row.value("members").toList().value(0).toString()))).toString();
                if(senderId!=owner || message.value("ownerId").toString()!=owner){relay_.rejectCurrentPacket();return;}
                const auto revision=message.value("revision").toInteger();if(revision<=row.value("membersRevision").toLongLong())return;
                const auto incoming=message.value("members").toArray();if(incoming.isEmpty() || incoming.size()>20 || RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(incoming.first().toString()))!=owner){relay_.rejectCurrentPacket();return;}
                QStringList codes,ids;for(const auto& code:incoming){const auto id=RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()));if(id.isEmpty() || ids.contains(id)){relay_.rejectCurrentPacket();return;}codes.append(code.toString());ids.append(id);}
                const auto previous=contacts_;const auto previousLeft=leftGroups_;const auto previousRemoved=removedGroupRevisions_;const auto localId=row.value("id").toString();
                if(!ids.contains(relay_.identityId())){contacts_.removeAt(i);removedGroupRevisions_.insert(groupId,revision);if(!leftGroups_.contains(groupId))leftGroups_.append(groupId);}
                else {row.insert("members",codes);row.insert("membersRevision",revision);auto admins=row.value("admins").toStringList();if(message.contains("admins") && message.value("adminsRevision").toInteger()>=row.value("adminsRevision").toLongLong()){admins.clear();for(const auto& admin:message.value("admins").toArray())if(ids.contains(admin.toString()) && !admins.contains(admin.toString()))admins.append(admin.toString());row.insert("adminsRevision",message.value("adminsRevision").toInteger());}for(qsizetype j=admins.size();j>0;--j)if(!ids.contains(admins[j-1]))admins.removeAt(j-1);row.insert("admins",admins);contacts_[i]=row;}
                if(!saveProfile()){contacts_=previous;leftGroups_=previousLeft;removedGroupRevisions_=previousRemoved;relay_.rejectCurrentPacket();return;}
                if(!ids.contains(relay_.identityId())){files_.cancelContact(localId);relay_.cancelContactPackets({},groupId);if(activeContactId_==localId){endCall();activeContactId_.clear();messages_.clear();emit messagesChanged();}emit incomingNotice(tr("群聊成员变更"),tr("你已被移出 %1").arg(row.value("name").toString()));}
                emit contactsChanged();emit activeContactChanged();return;
            }return;
        }
        if(type=="group_announcement" || type=="group_admins") {
            for(auto& value:contacts_) {
                auto row=value.toMap();if(!row.value("group").toBool() || row.value("groupId").toString()!=message.value("groupId").toString())continue;
                const auto owner=row.value("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(row.value("members").toList().value(0).toString()))).toString();
                if(senderId!=owner && (type=="group_admins" || !row.value("admins").toStringList().contains(senderId))){relay_.rejectCurrentPacket();return;}
                const auto revision=message.value("revision").toInteger();
                const auto slot=type=="group_admins"?"adminsRevision":"announcementRevision";
                if(revision<=row.value(slot).toLongLong() || revision>QDateTime::currentMSecsSinceEpoch()+60000)return;
                if(type=="group_admins") {
                    QStringList admins;
                    for(const auto& admin:message.value("admins").toArray()) {
                        bool member=false;for(const auto& code:row.value("members").toList())member|=RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()))==admin.toString();
                        if(!member || admins.contains(admin.toString())){relay_.rejectCurrentPacket();return;}admins.append(admin.toString());
                    }row.insert("admins",admins);
                }else{const auto text=message.value("text").toString();if(text.size()>2000){relay_.rejectCurrentPacket();return;}row.insert("announcement",text);}
                const auto previous=value;row.insert(slot,revision);value=row;
                if(!saveProfile()){value=previous;relay_.rejectCurrentPacket();return;}
                emit contactsChanged();emit activeContactChanged();
                if(type=="group_announcement")appendMessageForContact(row.value("id").toString(),tr("群公告：%1").arg(message.value("text").toString()),false,"system");
                return;
            }relay_.rejectCurrentPacket();return;
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
                profiles.insert(senderId,QVariantMap{{"name",message.value("name").toString().trimmed().left(64)},{"avatar",safeAvatar(message.value("avatar").toString())},{"nickname",message.value("nickname").toString().trimmed().left(64)},{"peerUid",numericUid(message)}});
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
                const auto owner=row.value("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(row.value("members").toList().value(0).toString()))).toString();
                if(senderId!=owner && !row.value("admins").toStringList().contains(senderId)){relay_.rejectCurrentPacket();return;}
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
                        {"messageId",message.value("messageId").toString()},{"sentAt",message.value("sentAt").toInteger()},
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
                {"messageId",fileId},
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
                        {"senderId",senderId},{"messageId",message.value("messageId").toString()},{"sentAt",message.value("sentAt").toInteger()},
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
            broadcastProfile();
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
    auto savedProfile = vault_.loadConversation(QStringLiteral("__profile"));
    if (!savedProfile.isEmpty())
        assistedConnection_ = savedProfile.first().toMap().value("network").toMap().value("assistedConnection",true).toBool();
    // Password login and assisted chat do not need Jami. Reinitializing its
    // native engine on every account/background handover blocked the GUI.
    if (!assistedConnection_ && daemon_.start()) {
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
            appliedFriendRepairs_=profile.value("appliedFriendRepairs").toStringList();
            removedGroupRevisions_=profile.value("removedGroupRevisions").toMap();
            for(auto& value:contacts_){auto row=value.toMap();if(row.value("transport")=="relay"){row.insert("group",!row.value("groupId").toString().isEmpty());if(row.value("group").toBool() && row.value("ownerId").toString().isEmpty())row.insert("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(row.value("members").toList().value(0).toString())));value=row;}}
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
        setError(tr("此安装包没有可用的内置连接服务，请更新安装包；这不是好友拒绝或账号丢失。"));
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
    if (!activeContactId_.isEmpty()) {
        messages_ = vault_.loadConversation(activeContactId_);
        if(!messages_.isEmpty())for(auto& value:contacts_){auto row=value.toMap();if(row.value("id")!=activeContactId_ || row.contains("lastMessage"))continue;const auto latest=messages_.last().toMap();row.insert("lastMessage",latest.value("body").toString().left(120));row.insert("lastMessageTime",latest.value("time"));value=row;}
    }
    if (expiredQuickTunnel) saveProfile();
    relay_.enableDirectory(!profileName_.isEmpty());
    callDeadline_.setSingleShot(true);
    connect(&callDeadline_, &QTimer::timeout, this, [this] { endCall(); setError(tr("对方未接听或通话连接已中断")); });
    callHeartbeat_.setInterval(3000);
    connect(&callHeartbeat_, &QTimer::timeout, this, [this] {
        if (callState_ == "idle") return;
        if(!callGroupId_.isEmpty()){
            const auto now=QDateTime::currentMSecsSinceEpoch();
            for(auto it=callParticipants_.begin();it!=callParticipants_.end();++it)if(it->accepted && now-it->lastSeen>15000){it->accepted=false;it->frames.clear();emit voiceChanged();}
            if(callState_=="active" && callParticipants()==1){endCall();return;}
            sendCallEvent(QJsonObject{{"type","call_ping"},{"nonce",QUuid::createUuid().toString(QUuid::WithoutBraces)}});return;
        }
        callPing_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!relay_.sendLive(callKey_, QJsonObject {{"type","call_ping"},{"callId",callId_},{"nonce",callPing_}})) {
            endCall(); setError(tr("通话连接已断开，已自动挂断"));
        }
    });
    connect(&relay_, &RelayClient::peerUnavailable, this, [this](const QString& peer) {
        if(!callGroupId_.isEmpty() && callParticipants_.contains(peer)){auto& participant=callParticipants_[peer];participant.accepted=false;participant.frames.clear();emit voiceChanged();if(callState_=="active" && callParticipants()==1)endCall();return;}
        if (callState_ != "idle" && peer == callPeerId_) { endCall(); setError(tr("对方已离线，语音通话已自动挂断")); }
    });
    connect(&relay_, &RelayClient::connectedChanged, this, [this](bool online) { if (!online && callState_ != "idle") endCall(); });
    if (auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) connect(gui, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if ((state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden) && !voice_.permissionPending() && callState_!="active") { finishVoice(false); if(callState_!="ringing")endCall(); }
    });
    connect(&voice_, &VoiceEngine::recordingChanged, this, [this](bool active) {
        recording_ = active && callState_ == "idle";
        if (active && callState_ == "connecting") {callState_ = "active";callStartedAt_=QDateTime::currentMSecsSinceEpoch();}
        emit voiceChanged();
    });
    connect(&voice_, &VoiceEngine::recordingReady, this, [this](const QByteArray& pcm) { sendMedia(pcm, "voice", recordingContact_); });
    connect(&voice_, &VoiceEngine::playbackChanged, this, &MessengerController::voiceChanged);
    connect(&voice_, &VoiceEngine::errorOccurred, this, [this](const QString& reason) { finishVoice(false); endCall(); setError(reason); });
    connect(&voice_, &VoiceEngine::pcmReady, this, [this](const QByteArray& pcm) {
        if (callState_ == "active") sendCallEvent(QJsonObject {{"type", "call_audio"}, {"seq", callSequence_++}, {"data", QString::fromLatin1(pcm.toBase64())}},true);
    });
    callMix_.setInterval(100);
    connect(&callMix_,&QTimer::timeout,this,[this]{
        if(callState_!="active" || callGroupId_.isEmpty())return;
        QList<QByteArray> frames;for(auto& peer:callParticipants_)if(peer.accepted && !peer.frames.isEmpty())frames.append(peer.frames.takeFirst());
        const auto pcm=VoiceEngine::mixFrames(frames);if(!pcm.isEmpty())voice_.play(pcm,true);
    });
    callMix_.start();
    const auto incoming=vault_.loadConversation("__incoming_call");
    if(!incoming.isEmpty()){
        const auto row=incoming.first().toMap();const auto key=RelayClient::publicKeyFromCode(row.value("code").toString());
        receiveCall(RelayClient::idForPublicKey(key),key,QJsonObject::fromVariantMap(row.value("offer").toMap()));
    }
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
    const auto sentAt=QDateTime::currentMSecsSinceEpoch();
    QList<QJsonObject> packets;
    const auto count = qMax(1,int((data.size() + 12287) / 12288));
    const auto hash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    for (int i = 0; i < count; ++i) packets.append(QJsonObject {{"type", "file_chunk"}, {"mediaKind", kind}, {"fileId", id}, {"name", kind == "voice" ? "voice.pcm" : kind=="photo" ? "photo.jpg" : "sticker.png"}, {"size", int(data.size())}, {"count", count}, {"index", i}, {"sha256", hash}, {"data", QString::fromLatin1(data.mid(i * 12288, 12288).toBase64(QByteArray::Base64UrlEncoding))}});
    for(auto& packet:packets)packet.insert("sentAt",sentAt);
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
    const bool saved=storeMessageForContact(contactId, QVariantMap {{"body", isFile?tr("文件：%1").arg(QFileInfo(fileName).fileName()):kind == "voice" ? tr("语音 %1 秒").arg(qMax(1, data.size()/32000)) : kind=="photo" ? tr("[照片]") : tr("[表情图片]")}, {"kind", isFile?"file-offer":kind}, {"outgoing", true}, {"fileData", QString::fromLatin1(data.toBase64())},{"fileId",id},{"messageId",id},{"sentAt",sentAt},{"name",QFileInfo(fileName).fileName()},{"packetIds",packetIds},{"delivery",tr("已排队")}, {"time", QDateTime::currentDateTime().toString("HH:mm")}});
    if(!saved)setError(tr("媒体已排队，但本机聊天记录未能保存，请勿重复发送"));return saved;
}
bool MessengerController::sendPhoto(const QString& path) {
    const auto local=QUrl(path).isLocalFile()?QUrl(path).toLocalFile():path;
    QImageReader reader(local); reader.setAutoTransform(true); const auto dims=reader.size();
    if (!dims.isValid() || dims.width()>30000 || dims.height()>30000) { setError(tr("照片格式无法读取或像素尺寸过大，请用文件方式发送原图")); return false; }
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
    // Transfer ringing through the encrypted vault, without rejecting it when
    // the user taps a notification and the service releases its writer lock.
    if(callState_=="ringing"){callDeadline_.stop();callHeartbeat_.stop();callState_="idle";callId_.clear();callKey_.clear();callPeerId_.clear();}
    else endCall();
    finishVoice(false);
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
    emit screenshotSelectionRequested();
#else
    setError(tr("此设备不支持桌面截图"));
#endif
}
void MessengerController::acceptScreenshot(const QImage& image) {
    if(image.isNull())return;
    QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
    if(!image.scaled(1600,1600,Qt::KeepAspectRatio,Qt::SmoothTransformation).save(&buffer,"JPEG",85)){setError(tr("截图转换失败"));return;}
    screenshotPreview_="data:image/jpeg;base64,"+QString::fromLatin1(bytes.toBase64());emit screenshotChanged();
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
    if (!assistedConnection_ || !relay_.isConnected() || !entry.value("ready").toBool() || entry.value("transport") != "relay") { setError(tr("语音通话需要连接已接通，并选择已确认好友或群聊")); return; }
    callGroupId_=entry.value("groupId").toString();callParticipants_.clear();
    if(entry.value("group").toBool()){
        const auto members=entry.value("members").toList();if(callGroupId_.isEmpty() || members.size()>6){callGroupId_.clear();setError(tr("当前多人语音支持最多 6 人的辅助连接群聊"));return;}
        for(const auto& code:members){const auto key=RelayClient::publicKeyFromCode(code.toString());const auto id=RelayClient::idForPublicKey(key);if(id!=relay_.identityId() && !id.isEmpty())callParticipants_.insert(id,CallParticipant{key});}
        if(callParticipants_.isEmpty()){callGroupId_.clear();setError(tr("群聊没有其他成员"));return;}
    }
    callKey_ = RelayClient::publicKeyFromCode(entry.value("relayPublic").toString()); callPeerId_ = RelayClient::idForPublicKey(callKey_);
    callId_ = QUuid::createUuid().toString(QUuid::WithoutBraces); callPeerName_ = entry.value("name").toString(); callState_ = "dialing";
    callContactId_=activeContactId_;callStartedAt_=0;
    callSequence_ = 0; receivedSequence_ = -1;
    if (!sendCallEvent(QJsonObject {{"type","call_offer"},{"expires",QDateTime::currentSecsSinceEpoch()+45}})) { endCall(); setError(tr("通话请求未能发出")); return; }
    callDeadline_.start(45000); callHeartbeat_.start(); emit voiceChanged();
}
void MessengerController::answerCall() {
    if (callState_ != "ringing") return;
    if(!callGroupId_.isEmpty() && callParticipants_.contains(callPeerId_)){auto& peer=callParticipants_[callPeerId_];peer.accepted=true;peer.lastSeen=QDateTime::currentMSecsSinceEpoch();}
    if (!sendCallEvent(QJsonObject {{"type","call_accept"}})) { endCall(); setError(tr("无法接通通话")); return; }
    callState_ = "connecting"; callDeadline_.start(15000); emit voiceChanged(); voice_.capture(true);
}
void MessengerController::endCall() {
    if (callState_ == "idle") return;
    sendCallEvent(QJsonObject {{"type","call_end"}});
    callDeadline_.stop(); callHeartbeat_.stop(); callPing_.clear();
    const auto seconds=callDuration();
    if(!callContactId_.isEmpty())appendMessageForContact(callContactId_,callStartedAt_>0?tr("语音通话 · %1:%2").arg(seconds/60).arg(seconds%60,2,10,QChar('0')):tr("语音通话 · 未接通"),true,"call");
    vault_.saveConversation("__incoming_call",{});
    callState_ = "idle"; callId_.clear(); callKey_.clear(); callPeerId_.clear();callStartedAt_=0;callContactId_.clear();
    callGroupId_.clear();callParticipants_.clear();
    voice_.stopCapture(false); voice_.stopPlayback(); emit voiceChanged();
}
int MessengerController::callDuration() const {return callStartedAt_>0?int(qMax<qint64>(0,QDateTime::currentMSecsSinceEpoch()-callStartedAt_)/1000):0;}
int MessengerController::callParticipants() const {int count=callState_=="active"?1:0;for(const auto& peer:callParticipants_)count+=peer.accepted;return callGroupId_.isEmpty()?(callState_=="active"?2:0):count;}
bool MessengerController::sendCallEvent(QJsonObject event,bool acceptedOnly) {
    event.insert("callId",callId_);if(callGroupId_.isEmpty())return relay_.sendLive(callKey_,event);
    event.insert("groupId",callGroupId_);bool sent=false;
    for(auto it=callParticipants_.begin();it!=callParticipants_.end();++it)if(!acceptedOnly || it->accepted)sent=relay_.sendLive(it->key,event) || sent;
    return sent;
}
void MessengerController::receiveCall(const QString& sender, const QByteArray& key, const QJsonObject& msg) {
    const auto type = msg.value("type").toString(), id = msg.value("callId").toString();
    if (id.size()!=36) return;
    if (type == "call_offer") {
        const auto expires = msg.value("expires").toInteger(), now = QDateTime::currentSecsSinceEpoch();
        if (expires <= now || expires > now+60 || callState_ != "idle" || recording_) return;
        for (const auto& item : contacts_) {
            const auto entry = item.toMap();
            const auto groupId=msg.value("groupId").toString();
            if(entry.value("transport")!="relay" || !entry.value("ready").toBool())continue;
            if(!groupId.isEmpty()){
                const auto members=entry.value("members").toList();if(!entry.value("group").toBool() || entry.value("groupId").toString()!=groupId || members.size()>6)continue;
                bool member=false;for(const auto& code:members)member|=RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()))==sender;if(!member)continue;
                callParticipants_.clear();for(const auto& code:members){const auto peerKey=RelayClient::publicKeyFromCode(code.toString());const auto peerId=RelayClient::idForPublicKey(peerKey);if(peerId!=relay_.identityId() && !peerId.isEmpty())callParticipants_.insert(peerId,CallParticipant{peerKey});}
                callGroupId_=groupId;
            }else{if(entry.value("uri")!=sender || entry.value("group").toBool())continue;callGroupId_.clear();callParticipants_.clear();}
            callId_ = id; callKey_ = key; callPeerId_ = sender; callPeerName_ = entry.value("name").toString(); callState_ = "ringing"; callSequence_ = 0; receivedSequence_ = -1;
            callContactId_=entry.value("id").toString();callStartedAt_=0;
            if(!vault_.saveConversation("__incoming_call",{QVariantMap{{"code","SD1-"+QString::fromLatin1(key.toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals))},{"offer",msg.toVariantMap()}}})) {callState_="idle";setError(tr("来电无法保存，请检查本机空间"));return;}
            emit incomingNotice(tr("语音来电"),callPeerName_+tr(" 邀请你通话，点击通知打开应用接听"));
            callDeadline_.start(int(qMin<qint64>(45,expires-now)*1000)); callHeartbeat_.start(); emit voiceChanged(); return;
        }
        return;
    }
    if(!callGroupId_.isEmpty()){
        if(id!=callId_ || callState_=="idle" || msg.value("groupId").toString()!=callGroupId_ || !callParticipants_.contains(sender))return;
        auto& peer=callParticipants_[sender];
        if(type=="call_accept") {const bool newlyJoined=!peer.accepted;peer.accepted=true;peer.lastSeen=QDateTime::currentMSecsSinceEpoch();emit voiceChanged();if(newlyJoined && callState_=="active")relay_.sendLive(key,QJsonObject{{"type","call_accept"},{"callId",id},{"groupId",callGroupId_}});if(callState_=="dialing"){callState_="connecting";callDeadline_.start(15000);voice_.capture(true);}return;}
        if(type=="call_end"){peer.accepted=false;peer.frames.clear();emit voiceChanged();if(callState_=="active" && callParticipants()==1)endCall();else if(callState_=="ringing" && sender==callPeerId_)endCall();return;}
        if(!peer.accepted)return;
        peer.lastSeen=QDateTime::currentMSecsSinceEpoch();
        if(type=="call_ping"){relay_.sendLive(key,QJsonObject{{"type","call_pong"},{"callId",id},{"groupId",callGroupId_},{"nonce",msg.value("nonce")}});return;}
        if(type=="call_pong"){if(callState_=="active")callDeadline_.start(15000);return;}
        if(type=="call_audio" && callState_=="active"){
            const auto sequence=msg.value("seq").toInt(-1);const auto pcm=QByteArray::fromBase64(msg.value("data").toString().toLatin1());
            if(sequence<=peer.sequence || pcm.size()!=3200)return;peer.sequence=sequence;peer.frames.append(pcm);while(peer.frames.size()>3)peer.frames.removeFirst();callDeadline_.start(15000);return;
        }return;
    }
    if (sender != callPeerId_ || id != callId_ || callState_ == "idle" || !msg.value("groupId").toString().isEmpty()) return;
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
QVariantMap MessengerController::activeContactDetails() const {auto row=activeEntry();if(activeIsGroup())row.insert("groupNumber",displayedGroupNumber(row.value("groupId",row.value("conversationId")).toString()));return row;}
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
            relay_.send(RelayClient::publicKeyFromCode(row.value("relayPublic").toString()), QJsonObject {{"type","profile_update"},{"name",profileName_},{"avatar",profileAvatar_},{"uid",uid_}});
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
    if (activeContactId_ == contactId) {
        setThreadVisible(threadVisible_);
        return;
    }
    activeContactId_ = contactId;
    messages_ = vault_.loadConversation(activeContactId_);
    emit activeContactChanged();
    emit messagesChanged();
    setThreadVisible(threadVisible_);
}
void MessengerController::setThreadVisible(bool visible) {
    threadVisible_=visible;
    if(!visible || !qobject_cast<QGuiApplication*>(QCoreApplication::instance()) || QGuiApplication::applicationState()!=Qt::ApplicationActive)return;
    for(auto& value:contacts_){auto row=value.toMap();if(row.value("id")==activeContactId_ && row.value("unread").toInt()>0){row.insert("unread",0);value=row;saveProfile();emit contactsChanged();break;}}
}
bool MessengerController::withdrawMessage(int index) {
    if(index<0 || index>=messages_.size())return false;auto msg=messages_[index].toMap();const auto row=activeEntry();
    const auto id=msg.value("messageId",msg.value("fileId")).toString();const auto sentAt=msg.value("sentAt").toLongLong();
    if(!msg.value("outgoing").toBool() || msg.value("kind")=="withdrawn" || id.size()!=36 || row.value("transport")!="relay" || !assistedConnection_){setError(tr("只能撤回本版本发送的辅助连接消息"));return false;}
    if(sentAt<=0 || QDateTime::currentMSecsSinceEpoch()-sentAt>120000){setError(tr("消息发送超过两分钟，不能撤回"));return false;}
    const auto event=QJsonObject{{"type","withdraw"},{"messageId",id},{"file",msg.value("kind")=="file-offer"},{"groupId",row.value("groupId").toString()}};
    if(row.value("group").toBool()){for(const auto& code:row.value("members").toList()){const auto key=RelayClient::publicKeyFromCode(code.toString());if(RelayClient::idForPublicKey(key)!=relay_.identityId() && relay_.send(key,event).isEmpty()){setError(tr("撤回指令无法排队，请重试"));return false;}}}
    else if(relay_.send(RelayClient::publicKeyFromCode(row.value("relayPublic").toString()),event).isEmpty()){setError(tr("撤回指令无法排队，请重试"));return false;}
    files_.cancelFile(id);const auto previous=messages_;msg.insert("body",tr("你撤回了一条消息"));msg.insert("kind","withdrawn");msg.remove("fileData");msg.remove("filePath");msg.remove("fileId");messages_[index]=msg;
    if(!vault_.saveConversation(activeContactId_,messages_)){messages_=previous;setError(tr("撤回已排队，但本机显示无法保存"));return false;}emit messagesChanged();return true;
}
bool MessengerController::setContactRemark(const QString& remark){
    if(activeIsGroup() || remark.size()>64){setError(tr("好友备注最多 64 字"));return false;}
    for(auto& value:contacts_){auto row=value.toMap();if(row.value("id")!=activeContactId_)continue;const auto previous=value;const auto name=remark.trimmed();row.insert("hasRemark",!name.isEmpty());row.insert("name",name.isEmpty()?row.value("peerName",row.value("name")).toString():name);value=row;if(!saveProfile()){value=previous;setError(tr("备注无法保存"));return false;}emit contactsChanged();emit activeContactChanged();return true;}return false;
}
bool MessengerController::setContactPinned(bool pinned){
    for(auto& value:contacts_){auto row=value.toMap();if(row.value("id")!=activeContactId_)continue;const auto previous=value;row.insert("pinned",pinned);value=row;if(!saveProfile()){value=previous;setError(tr("置顶设置无法保存"));return false;}emit contactsChanged();emit activeContactChanged();return true;}return false;
}
bool MessengerController::clearActiveHistory(){
    if(activeContactId_.isEmpty() || !vault_.saveConversation(activeContactId_,{})){setError(tr("本机记录无法清空"));return false;}
    messages_.clear();for(auto& value:contacts_){auto row=value.toMap();if(row.value("id")==activeContactId_){row.remove("lastMessage");row.remove("lastMessageTime");row.insert("unread",0);value=row;}}
    saveProfile();emit messagesChanged();emit contactsChanged();return true;
}
bool MessengerController::setContactMuted(bool muted) {
    for(auto& value:contacts_){auto row=value.toMap();if(row.value("id")!=activeContactId_)continue;const auto previous=value;row.insert("muted",muted);value=row;if(!saveProfile()){value=previous;setError(tr("免打扰设置无法保存"));return false;}emit contactsChanged();emit activeContactChanged();return true;}return false;
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
    files_.cancelContact(id);endCall();finishVoice(false);activeContactId_.clear();messages_.clear();emit contactsChanged();emit activeContactChanged();emit messagesChanged();return true;
}
bool MessengerController::renameGroup(const QString& name) {
    if(!canManageGroup()){setError(tr("只有群主或管理员可以修改群名称"));return false;}
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
        relay_.send(key,QJsonObject{{"type","group_member_profile"},{"groupId",group.value("groupId").toString()},{"name",profileName_},{"avatar",profileAvatar_},{"nickname",group.value("myNickname",profileName_).toString()},{"uid",uid_}});
    }
}
bool MessengerController::ownsGroup() const {
    const auto group=activeEntry();const auto owner=group.value("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(group.value("members").toList().value(0).toString()))).toString();
    return activeIsGroup() && !owner.isEmpty() && owner==relay_.identityId();
}
bool MessengerController::canManageGroup() const {return ownsGroup() || (activeIsGroup() && activeEntry().value("admins").toStringList().contains(relay_.identityId()));}
bool MessengerController::inviteGroupMembers(const QStringList& codes){
    if(!activeIsGroup() || activeEntry().value("transport")!="relay" || codes.isEmpty()){setError(tr("请选择要邀请的好友"));return false;}
    for(const auto& code:codes){bool friendReady=false;for(const auto& value:contacts_){const auto peer=value.toMap();friendReady|=!peer.value("group").toBool() && peer.value("ready").toBool() && peer.value("relayPublic")==code;}if(!friendReady){setError(tr("只能邀请已经添加的好友"));return false;}}
    const auto group=activeEntry();if(ownsGroup())return changeGroupMembers(group.value("groupId").toString(),codes,{});
    const auto owner=group.value("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(group.value("members").toList().value(0).toString()))).toString();
    for(const auto& code:group.value("members").toList())if(RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()))==owner){
        if(relay_.send(RelayClient::publicKeyFromCode(code.toString()),QJsonObject{{"type","group_invite_request"},{"groupId",group.value("groupId").toString()},{"codes",QJsonArray::fromStringList(codes)}}).isEmpty())return false;
        emit operationCompleted(tr("邀请已排队"),tr("群主设备上线后同步成员并发送邀请，对方需要接受邀请"));return true;
    }setError(tr("群主资料不完整，请等待群资料同步"));return false;
}
bool MessengerController::removeGroupMember(const QString& memberId){
    if(!canManageGroup()){setError(tr("只有群主或管理员可以移除成员"));return false;}
    const auto group=activeEntry();const auto owner=group.value("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(group.value("members").toList().value(0).toString()))).toString();
    if(memberId==owner || (!ownsGroup() && group.value("admins").toStringList().contains(memberId))){setError(tr("不能移除群主；管理员不能移除其他管理员"));return false;}
    if(ownsGroup())return changeGroupMembers(group.value("groupId").toString(),{},memberId);
    for(const auto& code:group.value("members").toList())if(RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()))==owner){
        if(relay_.send(RelayClient::publicKeyFromCode(code.toString()),QJsonObject{{"type","group_remove_request"},{"groupId",group.value("groupId").toString()},{"memberId",memberId}}).isEmpty())return false;
        emit operationCompleted(tr("移除请求已排队"),tr("群主设备上线后同步移除结果"));return true;
    }return false;
}
bool MessengerController::changeGroupMembers(const QString& groupId,const QStringList& additions,const QString& removal){
    for(auto& value:contacts_){auto group=value.toMap();if(!group.value("group").toBool() || group.value("groupId")!=groupId)continue;
        const auto owner=group.value("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(group.value("members").toList().value(0).toString()))).toString();if(owner!=relay_.identityId())return false;
        const auto oldCodes=group.value("members").toList();QStringList codes;for(const auto& code:oldCodes)codes.append(code.toString());
        if(!removal.isEmpty()){if(removal==owner)return false;bool found=false;for(qsizetype j=codes.size();j>0;--j)if(RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(codes[j-1]))==removal){codes.removeAt(j-1);found=true;}if(!found)return true;}
        for(const auto& code:additions){if(RelayClient::publicKeyFromCode(code).isEmpty())return false;if(!codes.contains(code))codes.append(code);}
        if(codes.size()>20){setError(tr("当前群聊最多 20 人"));return false;}
        auto admins=group.value("admins").toStringList();admins.removeAll(removal);group.insert("admins",admins);group.insert("members",codes);group.insert("ownerId",owner);
        const auto revision=group.value("membersRevision").toLongLong()+1;group.insert("membersRevision",revision);
        const auto previous=value;value=group;if(!saveProfile()){value=previous;setError(tr("成员变更无法保存"));return false;}
        const auto invite=QJsonObject{{"type","group_invite"},{"groupId",groupId},{"name",group.value("name").toString()},{"avatar",group.value("avatar").toString()},{"members",QJsonArray::fromStringList(codes)},{"ownerId",owner},{"membersRevision",revision},{"admins",QJsonArray::fromStringList(admins)},{"adminsRevision",group.value("adminsRevision").toLongLong()},{"announcement",group.value("announcement").toString()},{"announcementRevision",group.value("announcementRevision").toLongLong()}};
        bool queued=true;for(const auto& code:codes)if(RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code))!=owner)queued&=!relay_.send(RelayClient::publicKeyFromCode(code),invite).isEmpty();
        auto recipients=group;QStringList all=codes;for(const auto& code:oldCodes)if(!all.contains(code.toString()))all.append(code.toString());recipients.insert("members",all);
        broadcastGroupEvent(recipients,QJsonObject{{"type","group_members"},{"members",QJsonArray::fromStringList(codes)},{"ownerId",owner},{"revision",revision},{"admins",QJsonArray::fromStringList(admins)},{"adminsRevision",group.value("adminsRevision").toLongLong()}});
        emit contactsChanged();emit activeContactChanged();if(!queued){setError(tr("成员已保存，但部分邀请未能排队，请重试"));return false;}emit operationCompleted(tr("成员变更已保存"),tr("正在同步到群成员设备"));return true;
    }return false;
}
QVariantList MessengerController::searchHistory(const QString& query,bool allChats) const{
    const auto needle=query.trimmed();QVariantList matches;if(needle.isEmpty() || needle.size()>256)return matches;
    for(const auto& value:contacts_){const auto peer=value.toMap();const auto id=peer.value("id").toString();if(!allChats && id!=activeContactId_)continue;
        const auto history=id==activeContactId_?messages_:vault_.loadConversation(id);
        for(qsizetype i=history.size();i>0;--i){auto message=history[i-1].toMap();if(message.value("kind")=="withdrawn")continue;
            if(!message.value("body").toString().contains(needle,Qt::CaseInsensitive) && !message.value("name").toString().contains(needle,Qt::CaseInsensitive))continue;
            matches.append(QVariantMap{{"contactId",id},{"contactName",peer.value("name")},{"index",i-1},{"body",message.value("body")},{"time",message.value("time")}});if(matches.size()>=200)return matches;
        }
    }return matches;
}
QVariantMap MessengerController::droppedFileDetails(const QString& url) const{
    const QUrl parsed(url);if(!parsed.isLocalFile())return {};const QFileInfo file(parsed.toLocalFile());if(!file.isFile() || !file.isReadable())return {};
    return {{"url",QUrl::fromLocalFile(file.absoluteFilePath()).toString()},{"name",file.fileName()},{"size",file.size()}};
}
QVariantList MessengerController::groupMembers() const {
    const auto group=activeEntry();QVariantList result;const auto profiles=group.value("memberProfiles").toMap();
    const auto owner=group.value("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(group.value("members").toList().value(0).toString()))).toString();
    for(const auto& code:group.value("members").toList()) {
        const auto id=RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()));if(id.isEmpty())continue;
        auto profile=profiles.value(id).toMap();QString name=profile.value("nickname",profile.value("name")).toString();
        if(id==relay_.identityId())name=group.value("myNickname",profileName_).toString();
        if(name.isEmpty())for(const auto& value:contacts_){const auto peer=value.toMap();if(peer.value("uri")==id){name=peer.value("name").toString();break;}}
        if(name.isEmpty())name=tr("群成员 %1").arg(id.left(6));
        result.append(QVariantMap{{"id",id},{"name",name},{"owner",id==owner},{"admin",group.value("admins").toStringList().contains(id)}});
    }return result;
}
void MessengerController::broadcastGroupEvent(const QVariantMap& group,QJsonObject event) {
    event.insert("groupId",group.value("groupId").toString());
    for(const auto& code:group.value("members").toList()){const auto key=RelayClient::publicKeyFromCode(code.toString());if(RelayClient::idForPublicKey(key)==relay_.identityId())continue;if(relay_.send(key,event).isEmpty())setError(tr("群设置已保存，但同步排队失败，请检查本机空间"));}
}
bool MessengerController::setGroupAnnouncement(const QString& text) {
    if(!canManageGroup()){setError(tr("只有群主或管理员可以修改公告"));return false;}if(text.size()>2000){setError(tr("公告最多 2000 字"));return false;}
    for(auto& value:contacts_){auto group=value.toMap();if(group.value("id")!=activeContactId_)continue;
        const auto revision=qMax(QDateTime::currentMSecsSinceEpoch(),group.value("announcementRevision").toLongLong()+1);
        const auto previous=value;group.insert("announcement",text);group.insert("announcementRevision",revision);value=group;
        if(!saveProfile()){value=previous;return false;}broadcastGroupEvent(group,QJsonObject{{"type","group_announcement"},{"text",text},{"revision",revision}});emit contactsChanged();emit activeContactChanged();return true;
    }return false;
}
bool MessengerController::setGroupAdministrator(const QString& memberId,bool enabled) {
    if(!ownsGroup()){setError(tr("只有群主可以设置管理员"));return false;}bool found=false;for(const auto& value:groupMembers())if(value.toMap().value("id")==memberId && !value.toMap().value("owner").toBool())found=true;if(!found)return false;
    for(auto& value:contacts_){auto group=value.toMap();if(group.value("id")!=activeContactId_)continue;
        auto admins=group.value("admins").toStringList();admins.removeAll(memberId);if(enabled)admins.append(memberId);
        const auto revision=qMax(QDateTime::currentMSecsSinceEpoch(),group.value("adminsRevision").toLongLong()+1);
        const auto previous=value;group.insert("admins",admins);group.insert("adminsRevision",revision);value=group;
        if(!saveProfile()){value=previous;return false;}broadcastGroupEvent(group,QJsonObject{{"type","group_admins"},{"admins",QJsonArray::fromStringList(admins)},{"revision",revision}});emit contactsChanged();emit activeContactChanged();return true;
    }return false;
}
void MessengerController::chooseGroupAvatar() {
    if(!canManageGroup()){setError(tr("只有群主或管理员可以修改群头像"));return;}
#ifdef Q_OS_ANDROID
    const auto id=activeContactId_;
    if(!openAvatarGallery(this,[this,id](QString path,QString error){if(!error.isEmpty())setError(error);else if(!path.isEmpty()&&activeContactId_==id)setGroupAvatar(path);}))setError(tr("相册正在使用或无法打开，请检查系统相册权限"));
#else
    emit groupAvatarPickerRequested();
#endif
}
bool MessengerController::setGroupAvatar(const QString& path) {
    if(!canManageGroup()){setError(tr("只有群主或管理员可以修改群头像"));return false;}
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
            {QStringLiteral("uid"),uid_},
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
        entry.insert("ownerId",relay_.identityId());
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
    const auto messageId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto sentAt=QDateTime::currentMSecsSinceEpoch();
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
                        {"messageId",messageId},{"sentAt",sentAt},
                        {QStringLiteral("groupId"), current.value(QStringLiteral("groupId")).toString()},
                        {QStringLiteral("nickname"), current.value(QStringLiteral("myNickname"), profileName_).toString()},
                        {QStringLiteral("body"), text}
                    }).isEmpty()) ++queued;
                }
                if (queued == 0) { setError(tr("群消息未能保存到发送队列")); return false; }
                storeMessageForContact(activeContactId_,QVariantMap{{"body",text},{"outgoing",true},{"kind","text"},{"messageId",messageId},{"sentAt",sentAt},{"time",QDateTime::currentDateTime().toString("HH:mm")}});
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
                {"messageId",messageId},{"sentAt",sentAt},
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
                {"messageId",messageId},{"sentAt",sentAt},
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
            if(QFileInfo(localPath).size()>2*1024*1024){
                if(!current.value("ready").toBool()){setError(tr("请先确认好友关系"));return false;}
                QList<QByteArray> recipients;if(current.value("group").toBool()){for(const auto& code:current.value("members").toList()){const auto key=RelayClient::publicKeyFromCode(code.toString());if(RelayClient::idForPublicKey(key)!=relay_.identityId())recipients.append(key);}}
                else recipients.append(RelayClient::publicKeyFromCode(current.value("relayPublic").toString()));
                if(!files_.sendFile(localPath,activeContactId_,current.value("groupId").toString(),recipients)){setError(tr("文件无法读取或发送进度无法保存，请检查本机空间"));return false;}return true;
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
                if(!message.value("streamTransfer").toString().isEmpty()){
                    if(!files_.exportFile(message.value("streamTransfer").toString(),localPath)){setError(tr("文件正在保存或加密文件无法读取，请稍后重试"));return false;}
#ifdef Q_OS_ANDROID
                    if(androidDocument)androidDownloadDestinations_.insert(localPath,destination);
#endif
                    return true;
                }
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
        entry.insert("peerUid",peerProfile.value("peerUid"));entry.insert("peerName",peerProfile.value("name"));
        entry.insert(QStringLiteral("ready"), true);
        contacts_.append(entry);
        }
        if (!saveProfile()) {
            contacts_ = previousContacts; pendingRequests_ = previousPending;
            pendingRelayRequests_ = previousRequests; pendingRelayProfiles_ = previousProfiles;
            setError(tr("好友确认无法保存，未发出确认：%1").arg(vault_.error())); return false;
        }
        if (relay_.send(peerKey,QJsonObject {{"type","friend_accept"},{"name",profileName_},{"avatar",profileAvatar_},{"uid",uid_}}).isEmpty()) {
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
        const auto oldRequests = pendingGroupRequests_;
        const auto oldLeft = leftGroups_;
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
        entry.insert("ownerId",RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(invite.value("members").toList().value(0).toString())));
        entry.insert("membersRevision",invite.value("membersRevision",0));entry.insert("avatar",safeAvatar(invite.value("avatar").toString()));entry.insert("announcement",invite.value("announcement").toString().left(2000));
        entry.insert("adminsRevision",invite.value("adminsRevision",0));entry.insert("announcementRevision",invite.value("announcementRevision",0));
        QStringList validAdmins;for(const auto& admin:invite.value("admins").toList())for(const auto& code:entry.value("members").toList())if(RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(code.toString()))==admin.toString() && !validAdmins.contains(admin.toString()))validAdmins.append(admin.toString());entry.insert("admins",validAdmins);
        leftGroups_.removeAll(conversationId);
        entry.insert(QStringLiteral("ready"), true);
        contacts_.append(entry);
        if(!saveProfile()){
            contacts_.removeLast();pendingRelayGroups_.insert(conversationId,invite);
            pendingGroupRequests_=oldRequests;leftGroups_=oldLeft;
            setError(tr("加入群聊无法保存，请检查本机空间后重试"));return false;
        }
        emit pendingGroupRequestsChanged(); emit contactsChanged(); selectContact(id);
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
    if (!enabled && accountId_.isEmpty()) retryIdentity();
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
                             : accountId_.isEmpty()?tr("直连内核未就绪，模式已保存，不会使用中继"):tr("纯直连 — 不使用公共引导或中继");
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
    const bool incoming=!message.value("outgoing").toBool();
    bool muted=false;
    const auto kind=message.value("kind").toString();
    const auto preview=kind=="sticker"?tr("[表情]"):kind=="photo"?tr("[照片]"):kind=="voice"?tr("[语音]"):message.value("body").toString().left(120);
    const bool reading=threadVisible_ && contactId==activeContactId_ && qobject_cast<QGuiApplication*>(QCoreApplication::instance()) && QGuiApplication::applicationState()==Qt::ApplicationActive;
    for(auto& value:contacts_){auto row=value.toMap();if(row.value("id")!=contactId)continue;row.insert("lastMessage",preview);row.insert("lastMessageTime",message.value("time"));
        if(incoming && !reading)row.insert("unread",qMin(9999,row.value("unread").toInt()+1));
        muted=row.value("muted").toBool();value=row;break;
    }
    saveProfile();emit contactsChanged();
    if (contactId != "welcome" && incoming && !muted)
        emit incomingNotice(contactName(contactId),preview);
    return true;
}

bool MessengerController::saveProfile()
{
    if (!profileReadable_) return false;
    const bool saved=vault_.saveConversation(QStringLiteral("__profile"),
                            {QVariantMap {{QStringLiteral("accountId"), accountId_},
                                          {QStringLiteral("profileName"), profileName_},
                                          {QStringLiteral("avatar"), profileAvatar_},
                                          {QStringLiteral("stickers"), stickers_},
                                          {QStringLiteral("uid"), uid_},
                                          {QStringLiteral("directEndpoint"), directEndpoint_},
                                          {QStringLiteral("contacts"), contacts_},
                                          {QStringLiteral("leftGroups"),leftGroups_},
                                          {QStringLiteral("appliedFriendRepairs"),appliedFriendRepairs_},
                                          {QStringLiteral("removedGroupRevisions"),removedGroupRevisions_},
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
    if(saved && !uid_.isEmpty()){
        QVariantList portable;
        for(const auto& value:contacts_){auto row=value.toMap();if(row.value("transport")!="relay")continue;
            for(const auto* field:{"lastMessage","lastMessageTime","unread","avatar","memberProfiles","requestPacketId"})row.remove(QString::fromLatin1(field));
            portable.append(row);
        }
        relay_.setContactBackup(QVariantMap{{"contacts",portable},{"leftGroups",leftGroups_},{"removedGroupRevisions",removedGroupRevisions_},{"appliedFriendRepairs",appliedFriendRepairs_},{"avatar",profileAvatar_}});
    }
    return saved;
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
