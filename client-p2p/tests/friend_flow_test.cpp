#include "messenger_controller.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QLockFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <iostream>
#include <QImage>
#include <QBuffer>
#include <QUuid>
#include <QDateTime>
#include <QRandomGenerator>
#include <memory>
#include "account_manager.h"
#include "voice_engine.h"
#include "background_session.h"

class VoiceEngineRegression {
public:
    static int run() {
        VoiceEngine voice; QByteArray received; int ready=0;
        QObject::connect(&voice,&VoiceEngine::recordingReady,&voice,[&](const QByteArray& data){received=data;++ready;});
        for(int size : {0,320,16000,1920000,1920320}) {
            voice.recorded_=QByteArray(size,'x');voice.live_=false;received.clear();const int before=ready;
            voice.stopCapture(true);
            if(received.size()!=qMin(size,1920000) || ready-before!=(size>0?1:0) || !voice.recorded_.isEmpty())return 17;
        }
        voice.live_=true;voice.recorded_.clear();voice.stopCapture(false);voice.stopPlayback();
        voice.stopCapture(false); // Picker/background/destruction can stop an already empty recorder.
        std::cout<<"EMPTY_SHORT_FULL_RECORDING_STOP_AND_HANGUP_NO_OUT_OF_BOUNDS=PASS"<<std::endl;return 0;
    }
};
static int backgroundSessionRegression(QGuiApplication& app,const QString& root) {
    BackgroundSession ui(root);int suspended=0,resumed=0;ui.setCallbacks([&]{++suspended;},[&]{++resumed;});
    if(!ui.claimInitial() || !BackgroundSession::foregroundRequested(root))return 18;
    QLockFile service(QDir(root).filePath("session-owner.lock"));service.setStaleLockTime(0);
    if(service.tryLock(0))return 18;
    ui.setActive(false);if(!ui.ownsSession() || suspended || !BackgroundSession::foregroundRequested(root))return 18;
    ui.setActive(true);
    QFile heartbeat(QDir(root).filePath("background-status.json"));if(!heartbeat.open(QIODevice::WriteOnly))return 18;heartbeat.write(QJsonDocument(QJsonObject{{"at",QDateTime::currentMSecsSinceEpoch()}}).toJson());heartbeat.close();
    ui.setActive(false);if(suspended!=1 || ui.ownsSession() || BackgroundSession::foregroundRequested(root) || !service.tryLock(0))return 18;
    ui.setActive(true);if(!BackgroundSession::foregroundRequested(root) || ui.ownsSession())return 18;
    service.unlock();QTimer::singleShot(1250,&app,&QCoreApplication::quit);app.exec();
    if(!ui.ownsSession() || resumed!=1 || service.tryLock(0))return 18;
    std::cout<<"GUI_SERVICE_SINGLE_WRITER_HANDOVER_AND_NO_OFFLINE_IF_SERVICE_MISSING=PASS"<<std::endl;return 0;
}

static QString vaultFile(const QString& root, const QString& slot)
{
    return QDir(root).filePath("vault/" + QString::fromLatin1(
        QCryptographicHash::hash(slot.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".p2pvault");
}

static QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
}

static int emptyProfileRegression(const QString& root, const QString& sourceRoot)
{
    // A user's fixture is copied before opening any production controller.
    // Only the isolated temporary destination may be modified by this test.
    if (!sourceRoot.isEmpty()) {
        if (!QDir().mkpath(QDir(root).filePath("vault"))) return 6;
        const QDir source(QDir(sourceRoot).filePath("vault"));
        for (const auto& name : source.entryList(QDir::Files))
            if (!QFile::copy(source.filePath(name), QDir(root).filePath("vault/" + name))) return 6;
    }
    LocalVault vault;
    RelayCrypto identity;
    if (!identity.loadOrCreate(vault)) return 6;
    const auto invite = identity.inviteCode();
    const auto identityPath = vaultFile(root, "__relay_identity");
    const auto keyPath = QDir(root).filePath("vault/master-key.protected");
    const auto identityBefore = readBytes(identityPath), keyBefore = readBytes(keyPath);
    const auto profilePath = vaultFile(root, "__profile");
    if (sourceRoot.isEmpty()) {
        QFile empty(profilePath);
        if (!empty.open(QIODevice::WriteOnly) || !empty.resize(0)) return 6;
    }
    const auto welcomeBefore = readBytes(vaultFile(root, "welcome"));
    {
        MessengerController controller;
        if (!controller.lastError().contains(QStringLiteral("0 字节")) ||
            !controller.setProfileName("Recovered name") || !controller.setAssistedConnection(true) ||
            controller.inviteCode() != invite || !controller.setAssistedConnection(false)) {
            std::cerr << "EMPTY_PROFILE_RECOVERY=FAIL " << controller.lastError().toStdString() << std::endl;
            return 6;
        }
    }
    {
        MessengerController reopened;
        if (reopened.profileName() != "Recovered name" || !reopened.setAssistedConnection(true) ||
            reopened.inviteCode() != invite) { std::cerr << "EMPTY_PROFILE_REOPEN=FAIL " << reopened.lastError().toStdString() << std::endl; return 6; }
    }
    if (identityBefore != readBytes(identityPath) || keyBefore != readBytes(keyPath) ||
        (!welcomeBefore.isEmpty() && welcomeBefore != readBytes(vaultFile(root, "welcome"))) ||
        QDir(QDir(root).filePath("vault")).entryList({QFileInfo(profilePath).fileName() + ".empty-*"}, QDir::Files).isEmpty()) {
        std::cerr << "EMPTY_PROFILE_PRESERVATION=FAIL identity=" << (identityBefore == readBytes(identityPath))
                  << " key=" << (keyBefore == readBytes(keyPath)) << " welcome=" << (welcomeBefore.isEmpty() || welcomeBefore == readBytes(vaultFile(root,"welcome"))) << std::endl;
        return 6;
    }
    std::cout << "EMPTY_PROFILE_RECOVERY_IDENTITY_AND_HISTORY_PRESERVED=PASS" << std::endl;
    return 0;
}

static int identityRegression()
{
    LocalVault vault;
    RelayCrypto identity;
    if (!identity.loadOrCreate(vault)) return 6;
    const auto code = identity.inviteCode();
    const QVariantList contacts {
        QVariantMap {{"id", "relay-history"}, {"transport", "relay"}, {"name", "Saved friend"}, {"ready", true}},
        QVariantMap {{"id", "direct-history"}, {"name", "Old direct friend"}, {"ready", true}, {"conversationId", "old-session"}}
    };
    if (!vault.saveConversation("__profile", {QVariantMap {
        {"accountId", "previous-direct-engine"}, {"profileName", "Saved name"}, {"contacts", contacts},
        {"network", QVariantMap {{"assistedConnection", false}, {"relayEndpoint", "ws://localhost:1"}}}}})) return 6;
    const QVariantList history {QVariantMap {{"body", "preserved local record"}}};
    if (!vault.saveConversation("direct-history", history)) return 6;
    {
        MessengerController controller;
        if (!controller.accountId().isEmpty() || controller.profileName() != "Saved name" ||
            controller.contacts().size() != 2 || !controller.contacts().first().toMap().value("ready").toBool() ||
            controller.contacts().last().toMap().value("ready").toBool() ||
            !controller.setAssistedConnection(true) || controller.inviteCode() != code ||
            !controller.setProfileName("New name") ||
            controller.addContact("Peer", code) ||
            !controller.lastError().contains(QStringLiteral("自检尚未通过")) ||
            !controller.setAssistedConnection(false)) {
            std::cerr << "ENGINE_INDEPENDENCE=FAIL " << controller.lastError().toStdString() << std::endl;
            return 6;
        }
    }
    {
        MessengerController reopened;
        if (reopened.profileName() != "New name" || reopened.contacts().size() != 2 ||
            vault.loadConversation("direct-history") != history ||
            !reopened.setAssistedConnection(true) || reopened.inviteCode() != code) return 6;
    }
    // A present but unreadable/invalid profile must never be silently replaced.
    if (!vault.saveConversation("__profile", {})) return 6;
    MessengerController invalidProfile;
    if (!invalidProfile.lastError().contains(QStringLiteral("账号资料无法解密")) ||
        invalidProfile.setProfileName("Must not overwrite") ||
        !vault.hasConversation("__profile") || !vault.loadConversation("__profile").isEmpty() ||
        vault.loadConversation("direct-history") != history) return 6;
    std::cout << "ENGINE_INDEPENDENCE_AND_PROFILE_PERSISTENCE=PASS" << std::endl;
    return 0;
}

static int friendPersistenceRegression(const QString& root)
{
    // Inject already-authenticated packets at the controller boundary. This
    // exercises persistence/UI state, not network reachability or encryption.
    const auto peerRoot = QDir(root).filePath("peer");
    qputenv("P2P_MESSENGER_DATA_ROOT", peerRoot.toUtf8());
    LocalVault peerVault;
    RelayCrypto peer;
    if (!peer.loadOrCreate(peerVault)) return 8;
    const auto peerCode = peer.inviteCode();
    const auto peerKey = RelayClient::publicKeyFromCode(peerCode);
    const auto peerId = peer.identityId();
    qputenv("P2P_MESSENGER_DATA_ROOT", root.toUtf8());
    LocalVault vault;
    const QVariantList contacts {QVariantMap {
        {"id", "pending-friend"}, {"name", "Pending friend"}, {"uri", peerId},
        {"transport", "relay"}, {"relayPublic", peerCode}, {"ready", false},
        {"requestPacketId", "request-1"}, {"status", "waiting"}}};
    if (!vault.saveConversation("__profile", {QVariantMap {
            {"contacts", contacts}, {"network", QVariantMap {
                {"assistedConnection", false}, {"relayEndpoint", "ws://localhost:1"}}}}})) return 8;
    MessengerController controller;
    auto* relay = controller.findChild<RelayClient*>();
    if (!relay || !controller.setAssistedConnection(true)) return 8;
    relay->connectedChanged(false);
    if (!controller.contacts().first().toMap().value("status").toString().contains(QStringLiteral("尚未确认送达"))) return 8;
    relay->deliveryState("request-1", "delivered");
    const auto deliveredStatus = controller.contacts().first().toMap().value("status").toString();
    relay->connectedChanged(false);
    if (!deliveredStatus.contains(QStringLiteral("对方已收到")) ||
        controller.contacts().first().toMap().value("status").toString() != deliveredStatus) return 8;

    int pendingChanges = 0, contactChanges = 0, notices = 0;
    QObject::connect(&controller,&MessengerController::incomingNotice,&controller,[&](const QString&,const QString&){ ++notices; });
    QObject::connect(&controller, &MessengerController::pendingRequestsChanged,
                     &controller, [&] { ++pendingChanges; });
    QObject::connect(&controller, &MessengerController::contactsChanged,
                     &controller, [&] { ++contactChanges; });
    QLockFile lock(vaultFile(root, "__profile") + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return 8;
    relay->packetReceived(peerId, peerKey, QJsonObject {{"type", "friend_request"}});
    if (!controller.pendingRequests().isEmpty() || pendingChanges != 0 || notices != 0 ||
        !controller.lastError().contains(QStringLiteral("原因")) ||
        !vault.loadConversation("__profile").first().toMap().value("pendingRelayRequests").toMap().isEmpty()) return 8;
    relay->packetReceived(peerId, peerKey, QJsonObject {
        {"type", "group_invite"}, {"groupId", "test-group"},
        {"members", QJsonArray {peerCode, relay->inviteCode()}}});
    if (!controller.pendingGroupRequests().isEmpty() ||
        !vault.loadConversation("__profile").first().toMap().value("pendingRelayGroups").toMap().isEmpty()) return 8;
    lock.unlock();
    relay->packetReceived(peerId, peerKey, QJsonObject {{"type", "friend_request"}});
    if (controller.pendingRequests() != QStringList {peerId} || pendingChanges != 1 || notices != 1 ||
        !vault.loadConversation("__profile").first().toMap().value("pendingRelayRequests").toMap().contains(peerId)) return 8;

    const auto beforeContacts = controller.contacts();
    const auto beforeMessages = controller.messages();
    relay->packetReceived(peerId, peerKey, QJsonObject {{"type", "text"}, {"body", "not an acceptance"}});
    if (controller.contacts() != beforeContacts || controller.messages() != beforeMessages || contactChanges != 0) return 8;
    if (!lock.tryLock(0)) return 8;
    relay->packetReceived(peerId, peerKey, QJsonObject {{"type", "friend_accept"}});
    if (controller.contacts() != beforeContacts || contactChanges != 0 ||
        vault.loadConversation("__profile").first().toMap().value("contacts").toList().first().toMap().value("ready").toBool()) return 8;
    lock.unlock();
    relay->packetReceived(peerId, peerKey, QJsonObject {{"type", "friend_accept"}});
    if (!controller.contacts().first().toMap().value("ready").toBool() || contactChanges != 1 ||
        !vault.loadConversation("__profile").first().toMap().value("contacts").toList().first().toMap().value("ready").toBool()) return 8;
    controller.selectContact("pending-friend");
    QLockFile messageLock(vaultFile(root,"pending-friend")+".lock"); messageLock.setStaleLockTime(0);
    if (!messageLock.tryLock(0)) return 8;
    relay->packetReceived(peerId,peerKey,QJsonObject {{"type","text"},{"body","persisted incoming message"}});
    if (notices!=1 || !controller.messages().isEmpty()) return 8;
    messageLock.unlock();
    relay->packetReceived(peerId,peerKey,QJsonObject {{"type","text"},{"body","persisted incoming message"}});
    if (notices!=2 || controller.messages().size()!=1 || vault.loadConversation("pending-friend").size()!=1) return 8;
    std::cout << "INCOMING_MESSAGE_AND_FRIEND_NOTICE_ONLY_AFTER_SUCCESSFUL_SAVE=PASS" << std::endl;
    std::cout << "FRIEND_PERSISTENCE_FAILURE_AND_DELIVERY_STATUS=PASS" << std::endl;
    return 0;
}

static int resetRegression(const QString& root) {
    LocalVault old(root); RelayCrypto original; if (!original.loadOrCreate(old)) return 15;
    const auto code=original.inviteCode();
    if (!old.saveConversation("__owner_device",{QVariantMap {{"secret","isolated-test-secret"},{"enabled",false}}}) ||
        !old.saveConversation("__profile",{QVariantMap {{"profileName","Old account"}}})) return 15;
    AccountManager before; if (!before.createAccount()) return 15;
    QString error; if (!AccountManager::resetForPasswordRelease(&error)) { std::cerr<<error.toStdString(); return 15; }
    const QDir backups(QDir(root).filePath("account-backups"));
    const auto paths=backups.entryList(QDir::Dirs|QDir::NoDotAndDotDot); if (paths.size()!=1) return 15;
    LocalVault saved(backups.filePath(paths.first())); RelayCrypto preserved;
    if (!preserved.loadOrCreate(saved) || preserved.inviteCode()!=code || saved.loadConversation("__profile").isEmpty()) return 15;
    LocalVault fresh(root); RelayCrypto next;
    if (fresh.hasConversation("__profile") || !fresh.loadConversation("__owner_device").first().toMap().value("enabled").toBool() ||
        !next.loadOrCreate(fresh) || next.inviteCode()==code) return 15;
    AccountManager after; if (after.profiles().size()!=1 || !AccountManager::resetForPasswordRelease(&error)) return 15;
    RelayCrypto unchanged; if (!unchanged.loadOrCreate(fresh) || unchanged.inviteCode()!=next.inviteCode()) return 15;
    std::cout << "ACCOUNT_RESET_BACKUP_OWNER_PRESERVATION_AND_IDEMPOTENCE=PASS" << std::endl; return 0;
}

static int featureRegression(const QString& root) {
    const auto peerRoot = QDir(root).filePath("peer");
    LocalVault peerVault(peerRoot); RelayCrypto peer; if (!peer.loadOrCreate(peerVault)) return 10;
    LocalVault vault; RelayCrypto self; if (!self.loadOrCreate(vault)) return 10;
    const auto peerCode = peer.inviteCode(), peerId = peer.identityId();
    const auto peerKey = RelayClient::publicKeyFromCode(peerCode);
    const QVariantList rows {QVariantMap {{"id","private"},{"name","Peer"},{"uri",peerId},{"transport","relay"},{"ready",true},{"relayPublic",peerCode}},
        QVariantMap {{"id","group"},{"name","Group"},{"transport","relay"},{"ready",true},{"group",true},{"groupId","group-test"},{"members",QStringList {self.inviteCode(),peerCode}}}};
    if (!vault.saveConversation("__password_account",{QVariantMap {{"uid","11"}}}) || !vault.saveConversation("__profile",{QVariantMap {{"uid","11"},{"contacts",rows},{"network",QVariantMap {{"relayEndpoint","ws://localhost:1"},{"assistedConnection",true}}}}})) return 10;
    QString avatar;
    {
        MessengerController c; c.selectContact("group");
        if (!c.setProfileName("Account") || !c.setGroupNickname("群内名字")) return 10;
        QImage image(64,64,QImage::Format_ARGB32); image.fill(Qt::blue);
        const auto file = QDir(root).filePath("avatar.png"); if (!image.save(file) || !c.setAvatar(file)) return 10;
        avatar = c.avatarUrl(); if (!avatar.startsWith("data:image/png;base64,")) return 10;
        QImage noise(128,128,QImage::Format_ARGB32);
        for (int y=0;y<128;y++) for (int x=0;x<128;x++) noise.setPixel(x,y,QRandomGenerator::global()->generate());
        const auto noisy = QDir(root).filePath("noisy-avatar.png");
        if (!noise.save(noisy) || !c.setAvatar(noisy) || c.avatarUrl().mid(22).size()>32768) return 10;
        avatar = c.avatarUrl();
        if (!c.sendSticker(file)) return 10;
        if (!c.sendPhoto(file) || c.messages().last().toMap().value("kind")!="photo")return 10;
        c.selectContact("private");if(!c.queueFile(noisy))return 10;c.selectContact("group");
        auto* relay = c.findChild<RelayClient*>(); if (!relay) return 10;
        relay->packetReceived(peerId,peerKey,QJsonObject {{"type","group_text"},{"groupId","group-test"},{"body","hello"},{"nickname","对方群昵称"}});
        if (!c.messages().last().toMap().value("body").toString().startsWith("对方群昵称：")) return 10;
        const QByteArray pcm(32000,'\0'); const auto hash = QString::fromLatin1(QCryptographicHash::hash(pcm,QCryptographicHash::Sha256).toHex());
        for (int i=0;i<3;i++) relay->packetReceived(peerId,peerKey,QJsonObject {{"type","file_chunk"},{"fileId","voice-test"},{"mediaKind","voice"},{"groupId","group-test"},{"name","voice.pcm"},{"size",pcm.size()},{"count",3},{"index",i},{"sha256",hash},{"data",QString::fromLatin1(pcm.mid(i*12288,12288).toBase64(QByteArray::Base64UrlEncoding))}});
        if (c.messages().last().toMap().value("kind") != "voice" || QByteArray::fromBase64(c.messages().last().toMap().value("fileData").toString().toLatin1()) != pcm) return 10;
        c.selectContact("private");
        const auto callId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        relay->packetReceived(peerId,peerKey,QJsonObject {{"type","call_offer"},{"callId",callId},{"expires",QDateTime::currentSecsSinceEpoch()+30}});
        if (c.callState() != "ringing" || c.recording()) return 10; // Never answer/start a microphone in automated tests.
        relay->peerUnavailable(peerId);
        if (c.callState() != "idle") return 10;
    }
    MessengerController reopened;
    if (reopened.avatarUrl() != avatar || reopened.profileName() != "Account" || reopened.contacts()[1].toMap().value("myNickname") != "群内名字" || reopened.stickerLibrary().size() != 1) return 10;
    AccountManager accounts; accounts.createAccount(); const auto secondRoot = accounts.activeRoot();
    if (secondRoot == root || accounts.profiles().size()!=2 || accounts.activeIndex()!=1) return 10;
    accounts.updateName("Second"); accounts.selectAccount(0);
    AccountManager restored; restored.selectAccount(1);
    if (restored.activeRoot()!=secondRoot || restored.profiles()[1].toMap().value("name")!="Second") return 10;
    LocalVault secondVault(secondRoot); RelayCrypto secondKey;
    if (!secondKey.loadOrCreate(secondVault) || secondKey.inviteCode()==self.inviteCode()) return 10;
    std::cout << "AVATAR_GROUP_NICKNAME_STICKER_VOICE_STORAGE_CALL_CONSENT=PASS" << std::endl;
    return 0;
}

static int cloudUidRegression(QGuiApplication& app, const QString& root)
{
    // Two disposable controllers; every packet goes through the public WSS
    // service. This is not a physical two-network/Android audio test.
    qunsetenv("P2P_MESSENGER_ACCOUNT_PROFILE");
    qputenv("P2P_MESSENGER_DATA_ROOT", QDir(root).filePath("alice").toUtf8());
    MessengerController alice;
    qputenv("P2P_MESSENGER_DATA_ROOT", QDir(root).filePath("bob").toUtf8());
    MessengerController bob;
    bool requested = false, accepted = false, aliceSent = false, bobSent = false;
    bool aliceReceived = false, bobReceived = false;
    if (!alice.registerAccount("Disposable Alice QA","temporary-qa-password-alice") || !bob.registerAccount("Disposable Bob QA","temporary-qa-password-bob")) return 11;
    QObject::connect(&bob, &MessengerController::pendingRequestsChanged, &app, [&] {
        if (accepted || bob.pendingRequests().isEmpty()) return;
        QTimer::singleShot(0, &app, [&] {
            if (!accepted && !bob.pendingRequests().isEmpty())
                accepted = bob.acceptFriendRequest(bob.pendingRequests().first());
        });
    });
    const auto sendOnce = [](MessengerController& c, bool& sent, const QString& body) {
        if (sent) return;
        for (const auto& item : c.contacts()) {
            const auto row = item.toMap();
            if (row.value("transport") == "relay" && row.value("ready").toBool()) {
                c.selectContact(row.value("id").toString());
                sent = c.sendMessage(body);
                return;
            }
        }
    };
    const auto received = [](const MessengerController& c, const QString& body) {
        for (const auto& item : c.messages()) {
            const auto row = item.toMap();
            if (!row.value("outgoing").toBool() && row.value("body") == body) return true;
        }
        return false;
    };
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (!requested && !alice.userCode().isEmpty() && !bob.userCode().isEmpty() &&
            alice.networkStatus().contains(QStringLiteral("自检通过")) &&
            bob.networkStatus().contains(QStringLiteral("自检通过"))) {
            requested = alice.addFriendByUid(bob.userCode(), "Bob QA");
        }
        if (accepted) {
            sendOnce(alice, aliceSent, "alice-public-relay-test");
            sendOnce(bob, bobSent, "bob-public-relay-test");
            aliceReceived = received(alice, "bob-public-relay-test");
            bobReceived = received(bob, "alice-public-relay-test");
        }
        if (aliceReceived && bobReceived) app.quit();
    });
    timer.start(200);
    QTimer::singleShot(60000, &app, &QCoreApplication::quit);
    app.exec();
    const bool pass = requested && accepted && aliceSent && bobSent && aliceReceived && bobReceived;
    std::cout << "PUBLIC_WSS_UID_FRIEND_ACCEPT_BIDIRECTIONAL_MESSAGES=" << (pass ? "PASS" : "FAIL")
              << " alice_uid=" << alice.userCode().toStdString() << " bob_uid=" << bob.userCode().toStdString() << std::endl;
    if (!pass) std::cerr << "alice=" << alice.lastError().toStdString() << " bob=" << bob.lastError().toStdString() << std::endl;
    return pass ? 0 : 11;
}

static int cloudLoginRegression(QGuiApplication& app,const QString& root) {
    qunsetenv("P2P_MESSENGER_OWNER_DEVICE_ROOT");
    qputenv("P2P_MESSENGER_DATA_ROOT",QDir(root).filePath("register").toUtf8());
    const auto ownerFile=qEnvironmentVariable("P2P_MESSENGER_QA_OWNER_KEY_FILE");
    if (!ownerFile.isEmpty()) {
        QFile key(ownerFile); if (!key.open(QIODevice::ReadOnly)) return 16;
        const auto secret=QByteArray::fromBase64(key.readAll().trimmed()); if (secret.size()!=32) return 16;
        LocalVault owner; if (!owner.saveConversation("__owner_device",{QVariantMap {{"secret",QString::fromLatin1(secret.toBase64())},{"enabled",true}}})) return 16;
        qputenv("P2P_MESSENGER_OWNER_DEVICE_ROOT",qgetenv("P2P_MESSENGER_DATA_ROOT"));
    }
    auto controller=std::make_unique<MessengerController>();
    const QString password="cloud-disposable-password-qa";
    QString uid, code; int stage=0; bool pass=false;
    if (!controller->registerAccount("Login QA",password)) return 16;
    QTimer deadline; deadline.setSingleShot(true); QObject::connect(&deadline,&QTimer::timeout,&app,&QCoreApplication::quit); deadline.start(60000);
    QTimer tick; QObject::connect(&tick,&QTimer::timeout,&app,[&] {
        if (stage==0 && !controller->userCode().isEmpty()) {
            uid=controller->userCode(); code=controller->inviteCode();
            if (!ownerFile.isEmpty() && uid.toInt()>10) { app.quit(); return; }
            controller.reset(); qunsetenv("P2P_MESSENGER_OWNER_DEVICE_ROOT");
            qputenv("P2P_MESSENGER_DATA_ROOT",QDir(root).filePath("wrong-password").toUtf8());
            controller=std::make_unique<MessengerController>();
            if (!controller->loginAccount(uid,"incorrect-password-qa")) { app.quit(); return; } stage=1;
        } else if (stage==1 && controller->lastError().contains(QStringLiteral("账号或密码错误"))) {
            if (!controller->userCode().isEmpty() || controller->inviteCode()==code) { app.quit(); return; }
            controller.reset(); qputenv("P2P_MESSENGER_DATA_ROOT",QDir(root).filePath("correct-password").toUtf8());
            controller=std::make_unique<MessengerController>();
            if (!controller->loginAccount(uid,password)) { app.quit(); return; } stage=2;
        } else if (stage==2 && controller->userCode()==uid && controller->networkStatus().contains(QStringLiteral("自检通过"))) {
            pass=controller->inviteCode()==code && controller->profileName()=="Login QA" && controller->passwordConfigured(); app.quit();
        }
    }); tick.start(200); app.exec();
    std::cout<<"PUBLIC_WSS_PASSWORD_LOGIN_ORIGINAL_IDENTITY_WRONG_PASSWORD_REJECTED="<<(pass?"PASS":"FAIL")<<" uid="<<uid.toStdString()<<std::endl;
    if (!pass) std::cerr<<controller->lastError().toStdString()<<std::endl;
    return pass?0:16;
}

static int welcomeRemovalRegression(const QString& root) {
    LocalVault vault;
    const QVariantList history {QVariantMap {{"body","keep my real history"},{"outgoing",false}}};
    const QVariantList oldWelcome {QVariantMap {{"body","legacy onboarding"}}};
    if(!vault.saveConversation("real-friend",history)||!vault.saveConversation("welcome",oldWelcome)||
       !vault.saveConversation("__profile",{QVariantMap {{"profileName","Existing account"},{"contacts",QVariantList {
           QVariantMap {{"id","welcome"},{"name","开始使用"}},QVariantMap {{"id","real-friend"},{"name","Real friend"},{"transport","relay"},{"ready",true}}}}}}))return 19;
    const auto legacy=readBytes(vaultFile(root,"welcome"));
    {
        MessengerController controller;
        if(controller.contacts().size()!=1 || controller.activeContactId()!="real-friend" || controller.messages()!=history)return 19;
        controller.selectContact("welcome");if(controller.activeContactId()!="real-friend")return 19;
        if(!controller.setProfileName("Updated name"))return 19;
    }
    if(vault.loadConversation("real-friend")!=history || readBytes(vaultFile(root,"welcome"))!=legacy ||
        vault.loadConversation("__profile").first().toMap().value("contacts").toList().size()!=1)return 19;
    qputenv("P2P_MESSENGER_DATA_ROOT",QDir(root).filePath("new-empty").toUtf8());
    MessengerController empty;if(!empty.contacts().isEmpty()||!empty.activeContactId().isEmpty()||!empty.messages().isEmpty())return 19;
    LocalVault fresh;if(fresh.hasConversation("welcome"))return 19;
    std::cout<<"WELCOME_REMOVED_NEW_AND_EXISTING_PROFILES_REAL_HISTORY_PRESERVED=PASS"<<std::endl;return 0;
}

static int localLoginHistoryRegression(const QString& root) {
    AccountManager accounts;LocalVault vault(accounts.activeRoot());RelayCrypto identity;
    const QVariantList history{QVariantMap{{"body","preserve after login"}}};
    if(!identity.loadOrCreate(vault)||!vault.saveConversation("friend",history)||!vault.saveConversation("__profile",{QVariantMap{{"uid","11"},{"profileName","Original"},{"contacts",QVariantList{QVariantMap{{"id","friend"},{"name","Friend"},{"ready",true}}}}}}))return 20;
    const auto original=accounts.activeRoot();
    if(!accounts.beginLogin("11","test-password")||accounts.takeLogin().size()!=2)return 20;
    if(accounts.completeLogin("11","wrong-identity")||accounts.activeRoot()==original)return 20;
    if(!accounts.beginLogin("11","test-password")||accounts.takeLogin().size()!=2 || !accounts.completeLogin("11",identity.inviteCode()) || accounts.activeRoot()!=original)return 20;
    qputenv("P2P_MESSENGER_DATA_ROOT",original.toUtf8());MessengerController controller;
    controller.selectContact("friend");if(controller.contacts().size()!=1 || controller.messages()!=history)return 20;
    qputenv("P2P_MESSENGER_DATA_ROOT",QDir(root).filePath("anonymous").toUtf8());MessengerController anonymous;
    if(anonymous.setAvatar("nonexistent.png") || !anonymous.lastError().contains(QStringLiteral("登录")))return 20;
    std::cout<<"AUTHENTICATED_IDENTITY_REUSES_LOCAL_FRIENDS_HISTORY_ANONYMOUS_AVATAR_BLOCKED=PASS"<<std::endl;return 0;
}

static int publicHeadlessMediaRegression(QCoreApplication& app,const QString& root) {
    const auto endpoint=qEnvironmentVariable("P2P_MESSENGER_RELAY_URL");
    if(!endpoint.startsWith("wss://"))return 18;
    const auto aliceRoot=QDir(root).filePath("alice"),bobRoot=QDir(root).filePath("bob");
    LocalVault aliceVault(aliceRoot),bobVault(bobRoot);RelayCrypto aliceKey,bobKey;
    if(!aliceKey.loadOrCreate(aliceVault)||!bobKey.loadOrCreate(bobVault))return 18;
    const auto profile=[&](LocalVault& vault,const RelayCrypto& peer,const QString& name) {
        return vault.saveConversation("__profile",{QVariantMap {{"profileName",name},{"contacts",QVariantList {QVariantMap {{"id","peer"},{"name","Peer"},{"uri",peer.identityId()},{"transport","relay"},{"ready",true},{"relayPublic",peer.inviteCode()}},QVariantMap{{"id","public-group"},{"name","QA group"},{"transport","relay"},{"ready",true},{"group",true},{"groupId","qa-headless-group"},{"members",QStringList{aliceKey.inviteCode(),bobKey.inviteCode()}}}}},{"network",QVariantMap {{"relayEndpoint",endpoint},{"assistedConnection",true}}}}});
    };
    if(!profile(aliceVault,bobKey,"Sender")||!profile(bobVault,aliceKey,"Background receiver"))return 18;
    qputenv("P2P_MESSENGER_DISABLE_DIRECT_ENGINE","1");
    qputenv("P2P_MESSENGER_DATA_ROOT",aliceRoot.toUtf8());MessengerController alice;
    qputenv("P2P_MESSENGER_DATA_ROOT",bobRoot.toUtf8());MessengerController bob;
    alice.selectContact("peer");bob.selectContact("peer");
    const auto photo=QDir(root).filePath("photo.png"),file=QDir(root).filePath("file.txt");
    QImage image(480,320,QImage::Format_RGB32);image.fill(Qt::blue);if(!image.save(photo))return 18;
    QFile fixture(file);if(!fixture.open(QIODevice::WriteOnly))return 18;fixture.write("public-relay-file-fixture");fixture.close();
    bool sent=false,pass=false;int notifications=0;
    QObject::connect(&bob,&MessengerController::incomingNotice,&app,[&](const QString&,const QString&){++notifications;});
    const QByteArray pcm(32000,'\0');
    QTimer timer;
    QObject::connect(&timer,&QTimer::timeout,&app,[&]{
        if(!sent && alice.networkStatus().contains(QStringLiteral("自检通过")) && bob.networkStatus().contains(QStringLiteral("自检通过"))) {
            sent=true;
            if(!alice.sendMessage("public-headless-test") || !alice.sendPhoto(photo) || !alice.sendSticker(photo) || !alice.queueFile(file)){app.quit();return;}
            alice.selectContact("public-group");if(!alice.sendMessage("public-group-text") || !alice.sendPhoto(photo) || !alice.queueFile(file)){app.quit();return;}alice.selectContact("peer");
            auto* relay=alice.findChild<RelayClient*>();if(!relay){app.quit();return;}
            QList<QJsonObject> packets;
            const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto hash=QString::fromLatin1(QCryptographicHash::hash(pcm,QCryptographicHash::Sha256).toHex());
            for(int i=0;i<3;++i)packets.append(QJsonObject {{"type","file_chunk"},{"mediaKind","voice"},{"fileId",id},{"name","voice.pcm"},{"size",pcm.size()},{"count",3},{"index",i},{"sha256",hash},{"data",QString::fromLatin1(pcm.mid(i*12288,12288).toBase64(QByteArray::Base64UrlEncoding))}});
            if(relay->sendBatch(RelayClient::publicKeyFromCode(bobKey.inviteCode()),packets).size()!=3){app.quit();return;}
        }
        bool text=false,photoReceived=false,sticker=false,voice=false,fileReceived=false;
        for(const auto& value:bob.messages()) {
            const auto row=value.toMap();if(row.value("outgoing").toBool())continue;
            const auto bytes=QByteArray::fromBase64(row.value("fileData").toString().toLatin1());
            text|=row.value("body")=="public-headless-test";
            photoReceived|=row.value("kind")=="photo" && !QImage::fromData(bytes).isNull();
            sticker|=row.value("kind")=="sticker" && !QImage::fromData(bytes).isNull();
            voice|=row.value("kind")=="voice" && bytes==pcm;
            fileReceived|=row.value("kind")=="file-offer" && bytes=="public-relay-file-fixture";
        }
        bool groupText=false,groupPhoto=false,groupFile=false;
        for(const auto& value:bobVault.loadConversation("public-group")){const auto row=value.toMap();groupText|=row.value("body").toString().contains("public-group-text");groupPhoto|=row.value("kind")=="photo";groupFile|=row.value("kind")=="file-offer";}
        if(text&&photoReceived&&sticker&&voice&&fileReceived&&groupText&&groupPhoto&&groupFile&&notifications>=8){bob.selectContact("public-group");const auto sender=bob.messageSenderDetails(0);pass=sender.value("uri")==aliceKey.identityId() && sender.value("name")=="Sender";app.quit();}
    });
    timer.start(250);QTimer::singleShot(60000,&app,&QCoreApplication::quit);app.exec();
    std::cout<<"PUBLIC_WSS_QCORE_NO_WINDOW_PRIVATE_GROUP_MEMBER_PROFILE_MEDIA_FILES_AND_NOTICES="<<(pass?"PASS":"FAIL")<<std::endl;
    if(!pass)std::cerr<<alice.lastError().toStdString()<<" / "<<bob.lastError().toStdString()<<std::endl;
    return pass?0:18;
}

static int interactionRegression(const QString& root) {
    LocalVault vault(root),peerVault(QDir(root).filePath("peer"));RelayCrypto self,peer;
    if(!self.loadOrCreate(vault)||!peer.loadOrCreate(peerVault))return 21;
    const auto key=RelayClient::publicKeyFromCode(peer.inviteCode());
    if(!vault.saveConversation("__password_account",{QVariantMap{{"uid","11"}}}) || !vault.saveConversation("__profile",{QVariantMap{{"uid","11"},{"contacts",QVariantList{
        QVariantMap{{"id","private"},{"name","Peer"},{"uri",peer.identityId()},{"relayPublic",peer.inviteCode()},{"transport","relay"},{"ready",true},{"group",true}},
        QVariantMap{{"id","group"},{"name","Group"},{"group",true},{"groupId","qa-group"},{"members",QStringList{self.inviteCode(),peer.inviteCode()}},{"transport","relay"},{"ready",true}}}},{"network",QVariantMap{{"assistedConnection",true},{"relayEndpoint","ws://localhost:1"}}}}}))return 21;
    MessengerController c;auto* relay=c.findChild<RelayClient*>();if(!relay)return 21;
    c.selectContact("private");if(c.activeIsGroup())return 21;
    relay->packetReceived(peer.identityId(),key,QJsonObject{{"type","friend_request"}});if(!c.pendingRequests().isEmpty())return 21;
    c.selectContact("group");if(!c.activeIsGroup() || !c.renameGroup("New group"))return 21;
    QImage image(64,64,QImage::Format_RGB32);image.fill(Qt::blue);const auto avatarFile=QDir(root).filePath("group.png");if(!image.save(avatarFile)||!c.setGroupAvatar(avatarFile))return 21;
    QByteArray raw;QBuffer imageBuffer(&raw);imageBuffer.open(QIODevice::WriteOnly);image.save(&imageBuffer,"PNG");
    relay->packetReceived(peer.identityId(),key,QJsonObject{{"type","group_member_profile"},{"groupId","qa-group"},{"name","Member profile"},{"avatar",QString::fromLatin1(raw.toBase64())}});
    relay->packetReceived(peer.identityId(),key,QJsonObject{{"type","group_text"},{"groupId","qa-group"},{"nickname","Nick"},{"body","member message"}});
    auto details=c.messageSenderDetails(c.messages().size()-1);if(details.value("name")!="Member profile" || details.value("avatar").toString().isEmpty() || details.value("uri")!=peer.identityId())return 21;
    const auto file=QDir(root).filePath("payload.txt");QFile data(file);if(!data.open(QIODevice::WriteOnly))return 21;data.write("payload");data.close();
    if(!c.queueFile(file))return 21;auto sent=c.messages().last().toMap();if(sent.value("kind")!="file-offer" || sent.value("packetIds").toStringList().isEmpty())return 21;
    for(const auto& id:sent.value("packetIds").toStringList())relay->deliveryState(id,"delivered");
    if(c.messages().last().toMap().value("delivery")!=QStringLiteral("已送达") || !c.forwardMessage(c.messages().size()-1,"private"))return 21;
    const auto history=c.messages();if(!c.copyMessage(0) || !c.deleteLocalMessage(0) || c.messages().size()!=history.size()-1)return 21;
    if(!c.removeActiveContact())return 21;
    for(const auto& v:c.contacts())if(v.toMap().value("id")=="group")return 21;
    if(vault.loadConversation("group").isEmpty())return 21;
    c.selectContact("private");if(!c.removeActiveContact() || !c.contacts().isEmpty())return 21;
    for(const auto& v:vault.loadConversation("__relay_outbox")){const auto row=v.toMap();if(row.value("to")==peer.identityId() && row.value("scope").toString().isEmpty())return 21;}
    if(!vault.loadConversation("__profile").first().toMap().value("leftGroups").toStringList().contains("qa-group"))return 21;
    std::cout<<"PRIVATE_GROUP_CLASSIFICATION_MEMBER_AVATAR_DETAILS_FILES_FORWARD_RECEIPTS_DELETE_CANCEL=PASS"<<std::endl;return 0;
}

int main(int argc, char** argv)
{
    for(int i=1;i<argc;++i)if(QString::fromLocal8Bit(argv[i])=="--public-headless-media-e2e") {
        QCoreApplication app(argc,argv);QTemporaryDir root(QDir::tempPath()+"/headless-media-XXXXXX");
        return root.isValid()?publicHeadlessMediaRegression(app,root.path()):18;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir root(QDir::tempPath() + "/friend-flow-XXXXXX");
    if (!root.isValid()) return 2;
    qputenv("P2P_MESSENGER_DATA_ROOT", root.path().toUtf8());
    if(app.arguments().contains("--welcome-removal-regression"))return welcomeRemovalRegression(root.path());
    if(app.arguments().contains("--local-login-history-regression"))return localLoginHistoryRegression(root.path());
    if(app.arguments().contains("--interaction-regression"))return interactionRegression(root.path());
    if(app.arguments().contains("--voice-stop-regression"))return VoiceEngineRegression::run();
    if(app.arguments().contains("--background-session-regression"))return backgroundSessionRegression(app,root.path());
    if (app.arguments().contains("--reset-regression")) return resetRegression(root.path());
    if (app.arguments().contains("--cloud-login-e2e")) return cloudLoginRegression(app,root.path());
    if (app.arguments().contains("--cloud-uid-e2e")) return cloudUidRegression(app, root.path());
    if (app.arguments().contains("--feature-regression")) return featureRegression(root.path());
    if (app.arguments().contains("--qml-smoke")) {
        const auto pathIndex = app.arguments().indexOf("--qml-path");
        const auto path = app.arguments().value(pathIndex + 1);
        if (pathIndex < 0 || !QFileInfo::exists(path)) return 9;
        LocalVault fixture;
        if(!fixture.saveConversation("__profile",{QVariantMap {{"profileName","界面预览"},{"contacts",QVariantList {QVariantMap {{"id","preview-friend"},{"name","测试好友"},{"initial","测"},{"status","已连接"},{"transport","relay"},{"ready",true}}}}}}))return 9;
        MessengerController controller;
        controller.setProfileName("界面预览");
        AccountManager uiAccounts;
        QQmlApplicationEngine engine;
        bool warnings = false;
        QObject::connect(&engine, &QQmlEngine::warnings, &app, [&](const QList<QQmlError>& errors) {
            warnings = true;
            for (const auto& error : errors) std::cerr << error.toString().toStdString() << std::endl;
        });
        engine.rootContext()->setContextProperty("messenger", &controller);
        engine.rootContext()->setContextProperty("accountManager",&uiAccounts);
        engine.load(QUrl::fromLocalFile(path));
        if (engine.rootObjects().size() != 1) {
            std::cerr << "QML load failed: root_count=" << engine.rootObjects().size() << std::endl;
            return 9;
        }
        auto* window = engine.rootObjects().first();
        if (window->property("mobile").toBool()) {
            std::cerr << "QML desktop layout unexpectedly mobile" << std::endl;
            return 9;
        }
        window->setProperty("minimumWidth", 320);
        window->setProperty("height", 780);
        window->setProperty("showThread", true);
        for (int width : {320, 390}) {
            window->setProperty("width", width);
            QTimer::singleShot(150, &app, &QCoreApplication::quit);
            app.exec();
            for (const auto* name : {"chatBack", "chatFile", "chatInput", "chatSend"}) {
                const auto* item = window->findChild<QQuickItem*>(QString::fromLatin1(name));
                if (!item || !item->isVisible() || item->width() <= 0) return 9;
                const auto topLeft = item->mapToScene(QPointF(0, 0));
                if (topLeft.x() < 0 || topLeft.x() + item->width() > width + 1 ||
                    topLeft.y() < 0 || topLeft.y() + item->height() > 781) {
                    std::cerr << "QML overflow: " << name << " width=" << width << std::endl;
                    return 9;
                }
            }
        }
        if (warnings || !window->property("mobile").toBool()) {
            std::cerr << "QML portrait layout failed: width=" << window->property("width").toInt()
                      << " mobile=" << window->property("mobile").toBool() << " warnings=" << warnings << std::endl;
            return 9;
        }
        if(!QMetaObject::invokeMethod(window,"openUidLogin"))return 9;
        QTimer::singleShot(100,&app,&QCoreApplication::quit);app.exec();
        auto* loginDialog=window->findChild<QObject*>("uidLoginDialog");auto* loginField=window->findChild<QQuickItem*>("uidLoginField");
        if(!loginDialog || !loginDialog->property("visible").toBool() || !loginField || loginField->width()<80 || loginField->height()<20)return 9;
        qputenv("P2P_MESSENGER_DATA_ROOT",QDir(root.path()).filePath("login-scratch").toUtf8());auto loginController=std::make_unique<MessengerController>();
        engine.rootContext()->setContextProperty("messenger",loginController.get());
        if(!loginController->loginAccount("11","test-password") || !QMetaObject::invokeMethod(window,"resetSession"))return 9;
        if(engine.rootObjects().first()!=window || !loginController->loginPending())return 9;
        loginController->cancelLogin();QTimer::singleShot(100,&app,&QCoreApplication::quit);app.exec();
        if(!loginDialog->property("visible").toBool())return 9;
        engine.rootContext()->setContextProperty("messenger",&controller);QMetaObject::invokeMethod(window,"resetSession");loginController.reset();
        qputenv("P2P_MESSENGER_DATA_ROOT",root.path().toUtf8());
        if(warnings || engine.rootObjects().first()!=window)return 9;
        std::cout<<"UID_LOGIN_FIELDS_VISIBLE_CANCEL_RETRY_PROFILE_SWAP_RETAINS_NATIVE_WINDOW=PASS"<<std::endl;
        const auto screenshotIndex=app.arguments().indexOf("--screenshot-dir");
        if(screenshotIndex>=0) {
            const auto directory=app.arguments().value(screenshotIndex+1); QDir().mkpath(directory);
            auto* quickWindow=qobject_cast<QQuickWindow*>(window); if(!quickWindow)return 9;
            const auto capture=[&](const QString& name) {
                QTimer::singleShot(200,&app,&QCoreApplication::quit);app.exec();
                return quickWindow->grabWindow().save(QDir(directory).filePath(name));
            };
            controller.selectContact("preview-friend");window->setProperty("showThread",true);
            if(!capture("chat-390.png"))return 9;
            window->setProperty("expandedTools",true);if(!capture("tools-390.png"))return 9;
            window->setProperty("showThread",false);if(!capture("messages-390.png"))return 9;
            window->setProperty("mobileTab",2);if(!capture("account-tab-390.png"))return 9;
        }
        std::cout << "QML_DESKTOP_AND_320_390_PORTRAIT_CONTROLS_FIT=PASS" << std::endl;
        return 0;
    }
    if (app.arguments().contains("--empty-profile-regression")) {
        const auto index = app.arguments().indexOf("--fixture-root");
        return emptyProfileRegression(root.path(), index >= 0 ? app.arguments().value(index + 1) : QString {});
    }
    if (app.arguments().contains("--identity-regression")) return identityRegression();
    if (app.arguments().contains("--friend-persistence-regression")) return friendPersistenceRegression(root.path());
    MessengerController controller;
    bool invitationPrinted = false, accepted = false, receivedText = false, sentText = false;
    const bool reciprocal = app.arguments().contains("--reciprocal");
    bool reciprocalRequested = false;
    if (reciprocal) {
        QObject::connect(&controller, &MessengerController::contactsChanged, &app, [&] {
            if (accepted) return;
            for (const auto& value : controller.contacts()) {
                const auto row = value.toMap();
                if (row.value("transport") == "relay" && row.value("ready").toBool()) {
                    accepted = true;
                    std::cout << "RECIPROCAL_ACCEPTED=1" << std::endl;
                    QTimer::singleShot(0, &app, [&] { sentText = controller.sendMessage("controller-to-cloud"); });
                    break;
                }
            }
        });
    }
    QObject::connect(&controller, &MessengerController::lastErrorChanged, &app, [&] {
        std::cerr << "controller_error=" << controller.lastError().toStdString() << std::endl;
    });
    QObject::connect(&controller, &MessengerController::pendingRequestsChanged, &app, [&] {
        if (accepted || controller.pendingRequests().isEmpty()) return;
        // Run after the packet handler has finished persistence and acknowledgement.
        QTimer::singleShot(0, &app, [&] {
            if (accepted || controller.pendingRequests().isEmpty()) return;
            LocalVault saved;
            const auto profiles = saved.loadConversation("__profile");
            if (profiles.isEmpty() || profiles.first().toMap().value("pendingRelayRequests").toMap().isEmpty()) {
                std::cerr << "request was not persisted" << std::endl; app.exit(7); return;
            }
            if (reciprocal) {
                if (reciprocalRequested) return;
                const auto peerId = controller.pendingRequests().first();
                const auto peerCode = QStringLiteral("SD1-") + profiles.first().toMap()
                    .value("pendingRelayRequests").toMap().value(peerId).toString();
                reciprocalRequested = controller.addContact("Android receiver", peerCode);
                std::cout << "RECIPROCAL_REQUEST_QUEUED=" << reciprocalRequested << std::endl;
                return;
            }
            accepted = controller.acceptFriendRequest(controller.pendingRequests().first());
            std::cout << "CONTROLLER_ACCEPTED=" << accepted << std::endl;
            if (accepted) sentText = controller.sendMessage("controller-to-cloud");
        });
    });
    QObject::connect(&controller, &MessengerController::messagesChanged, &app, [&] {
        for (const auto& item : controller.messages()) {
            const auto row = item.toMap();
            if (row.value("body").toString() == "cloud-to-controller" && !row.value("outgoing").toBool())
                receivedText = true;
        }
        if (accepted && sentText && receivedText) QTimer::singleShot(2000, &app, &QCoreApplication::quit);
    });
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (!controller.assistedConnection()) controller.setAssistedConnection(true);
        if (!invitationPrinted && controller.networkStatus().contains(QStringLiteral("自检通过"))) {
            invitationPrinted = true;
            std::cout << "CONTROLLER_INVITE=" << controller.inviteCode().toStdString() << std::endl;
        }
    });
    timer.start(500);
    const int configuredTimeout = qEnvironmentVariableIntValue("P2P_MESSENGER_TEST_TIMEOUT_MS");
    QTimer::singleShot(configuredTimeout > 0 ? qMin(configuredTimeout, 900000) : 180000,
                       &app, &QCoreApplication::quit);
    app.exec();
    const bool passed = accepted && sentText && receivedText;
    std::cout << "CONTROLLER_FRIEND_FLOW=" << (passed ? "PASS" : "FAIL") << std::endl;
    return passed ? 0 : 4;
}
