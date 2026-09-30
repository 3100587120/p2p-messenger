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
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <iostream>

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
            reopened.inviteCode() != invite) return 6;
    }
    if (identityBefore != readBytes(identityPath) || keyBefore != readBytes(keyPath) ||
        (!welcomeBefore.isEmpty() && welcomeBefore != readBytes(vaultFile(root, "welcome"))) ||
        QDir(QDir(root).filePath("vault")).entryList({QFileInfo(profilePath).fileName() + ".empty-*"}, QDir::Files).isEmpty()) return 6;
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

    int pendingChanges = 0, contactChanges = 0;
    QObject::connect(&controller, &MessengerController::pendingRequestsChanged,
                     &controller, [&] { ++pendingChanges; });
    QObject::connect(&controller, &MessengerController::contactsChanged,
                     &controller, [&] { ++contactChanges; });
    QLockFile lock(vaultFile(root, "__profile") + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return 8;
    relay->packetReceived(peerId, peerKey, QJsonObject {{"type", "friend_request"}});
    if (!controller.pendingRequests().isEmpty() || pendingChanges != 0 ||
        !controller.lastError().contains(QStringLiteral("原因")) ||
        !vault.loadConversation("__profile").first().toMap().value("pendingRelayRequests").toMap().isEmpty()) return 8;
    relay->packetReceived(peerId, peerKey, QJsonObject {
        {"type", "group_invite"}, {"groupId", "test-group"},
        {"members", QJsonArray {peerCode, relay->inviteCode()}}});
    if (!controller.pendingGroupRequests().isEmpty() ||
        !vault.loadConversation("__profile").first().toMap().value("pendingRelayGroups").toMap().isEmpty()) return 8;
    lock.unlock();
    relay->packetReceived(peerId, peerKey, QJsonObject {{"type", "friend_request"}});
    if (controller.pendingRequests() != QStringList {peerId} || pendingChanges != 1 ||
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
    std::cout << "FRIEND_PERSISTENCE_FAILURE_AND_DELIVERY_STATUS=PASS" << std::endl;
    return 0;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir root(QDir::tempPath() + "/friend-flow-XXXXXX");
    if (!root.isValid()) return 2;
    qputenv("P2P_MESSENGER_DATA_ROOT", root.path().toUtf8());
    if (app.arguments().contains("--qml-smoke")) {
        const auto pathIndex = app.arguments().indexOf("--qml-path");
        const auto path = app.arguments().value(pathIndex + 1);
        if (pathIndex < 0 || !QFileInfo::exists(path)) return 9;
        MessengerController controller;
        QQmlApplicationEngine engine;
        bool warnings = false;
        QObject::connect(&engine, &QQmlEngine::warnings, &app, [&](const QList<QQmlError>& errors) {
            warnings = true;
            for (const auto& error : errors) std::cerr << error.toString().toStdString() << std::endl;
        });
        engine.rootContext()->setContextProperty("messenger", &controller);
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
        window->setProperty("width", 390);
        window->setProperty("height", 780);
        QTimer::singleShot(100, &app, &QCoreApplication::quit);
        app.exec();
        if (warnings || !window->property("mobile").toBool()) {
            std::cerr << "QML portrait layout failed: width=" << window->property("width").toInt()
                      << " mobile=" << window->property("mobile").toBool() << " warnings=" << warnings << std::endl;
            return 9;
        }
        std::cout << "QML_DESKTOP_AND_PORTRAIT_LAYOUT_LOAD=PASS" << std::endl;
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
