#include "messenger_controller.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
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

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir root(QDir::tempPath() + "/friend-flow-XXXXXX");
    if (!root.isValid()) return 2;
    qputenv("P2P_MESSENGER_DATA_ROOT", root.path().toUtf8());
    if (app.arguments().contains("--empty-profile-regression")) {
        const auto index = app.arguments().indexOf("--fixture-root");
        return emptyProfileRegression(root.path(), index >= 0 ? app.arguments().value(index + 1) : QString {});
    }
    if (app.arguments().contains("--identity-regression")) return identityRegression();
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
