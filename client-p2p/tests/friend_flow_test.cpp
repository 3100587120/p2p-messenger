#include "messenger_controller.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QTimer>
#include <iostream>

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
    MessengerController reopened;
    if (reopened.profileName() != "New name" || reopened.contacts().size() != 2 ||
        vault.loadConversation("direct-history") != history ||
        !reopened.setAssistedConnection(true) || reopened.inviteCode() != code) return 6;
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
    if (app.arguments().contains("--identity-regression")) return identityRegression();
    MessengerController controller;
    bool invitationPrinted = false, accepted = false, receivedText = false, sentText = false;
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
