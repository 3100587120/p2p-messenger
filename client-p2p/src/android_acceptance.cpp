#include <QtGlobal>
#include "android_acceptance.h"

#if defined(Q_OS_ANDROID) && !defined(QT_NO_DEBUG)
#include "messenger_controller.h"
#include "local_vault.h"
#include <QCoreApplication>
#include <QJniObject>
#include <QStandardPaths>
#include <QSslSocket>
#include <QTimer>
#include <QUuid>
#include <QDir>
#include <QDebug>
#include <memory>
#include <QtCore/qnativeinterface.h>

bool androidAcceptanceHeadless()
{
    const QJniObject activity(QNativeInterface::QAndroidApplication::context().object<jobject>());
    const auto intent = activity.callObjectMethod("getIntent", "()Landroid/content/Intent;");
    return intent.callMethod<jboolean>("getBooleanExtra", "(Ljava/lang/String;Z)Z",
        QJniObject::fromString("p2p_test_headless").object<jstring>(), false);
}

bool prepareAndroidAcceptance(QCoreApplication&, QString* peer)
{
    const QJniObject activity(QNativeInterface::QAndroidApplication::context().object<jobject>());
    const auto intent = activity.callObjectMethod("getIntent", "()Landroid/content/Intent;");
    const auto extra = QJniObject::fromString(QStringLiteral("p2p_test_peer"));
    *peer = intent.callObjectMethod("getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;",
                                   extra.object<jstring>()).toString();
    if (peer->isEmpty()) return true;
    const auto root = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                          .filePath("acceptance-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    qputenv("P2P_MESSENGER_DATA_ROOT", root.toUtf8());
    qInfo().noquote() << "P2P_ANDROID_TLS=" << QSslSocket::supportsSsl()
                     << "backend=" << QSslSocket::activeBackend()
                     << "version=" << QSslSocket::sslLibraryVersionString();
    LocalVault first;
    const QVariantList sample {QVariantMap {{QStringLiteral("body"), QStringLiteral("vault-round-trip")}}};
    const bool saved = first.isReady() && first.saveConversation("__acceptance", sample);
    LocalVault reopened;
    const bool passed = saved && reopened.isReady() && reopened.loadConversation("__acceptance") == sample;
    qInfo().noquote() << "P2P_ANDROID_VAULT=" << (passed ? "PASS" : "FAIL");
    return passed && QSslSocket::supportsSsl();
}

void startAndroidAcceptance(QCoreApplication& app, MessengerController& controller, const QString& peer)
{
    if (peer.isEmpty()) return;
    struct State { bool queued = false; bool accepted = false; bool sent = false; bool delivered = false; bool received = false; bool done = false; };
    const auto state = std::make_shared<State>();
    const auto expectedPeer = RelayClient::idForPublicKey(RelayClient::publicKeyFromCode(peer));
    QObject::connect(&controller, &MessengerController::pendingRequestsChanged, &app,
        [&controller, expectedPeer] {
            if (!controller.pendingRequests().contains(expectedPeer)) return;
            QTimer::singleShot(0, &controller, [&controller, expectedPeer] {
                if (!controller.pendingRequests().contains(expectedPeer)) return;
                LocalVault saved;
                const auto profiles = saved.loadConversation("__profile");
                if (profiles.isEmpty() || !profiles.first().toMap().value("pendingRelayRequests")
                    .toMap().contains(expectedPeer)) {
                    qWarning().noquote() << "P2P_ANDROID_REQUEST_PERSISTENCE=FAIL";
                    return;
                }
                qInfo().noquote() << "P2P_ANDROID_INCOMING_REQUEST_ACCEPTED="
                                 << controller.acceptFriendRequest(expectedPeer);
            });
        });
    const auto finish = [&app, state] {
        if (state->accepted && state->sent && state->delivered && state->received && !state->done) {
            state->done = true;
            qInfo().noquote() << "P2P_ANDROID_FRIEND_FLOW=PASS";
            QTimer::singleShot(2500, &app, &QCoreApplication::quit);
        }
    };
    QObject::connect(&controller, &MessengerController::lastErrorChanged, &app, [&controller] {
        qWarning().noquote() << "P2P_ANDROID_ERROR=" << controller.lastError();
    });
    QObject::connect(&controller, &MessengerController::contactsChanged, &app, [&controller, state, finish] {
        for (const auto& row : controller.contacts()) {
            const auto contact = row.toMap();
            if (contact.value("transport") == "relay" && contact.value("ready").toBool()) state->accepted = true;
        }
        if (state->accepted && !state->sent) {
            // Avoid re-entering the incoming packet handler before it sends its receipt.
            QTimer::singleShot(0, &controller, [&controller, state, finish] {
                if (!state->sent) state->sent = controller.sendMessage("cloud-to-controller");
                finish();
            });
        }
    });
    QObject::connect(&controller, &MessengerController::messagesChanged, &app, [&controller, state, finish] {
        for (const auto& row : controller.messages()) {
            const auto message = row.toMap();
            if (message.value("body") == "controller-to-cloud" && !message.value("outgoing").toBool()) state->received = true;
            if (message.value("body") == "cloud-to-controller" && message.value("outgoing").toBool() &&
                message.value("delivery").toString() == QStringLiteral("已送达")) state->delivered = true;
        }
        finish();
    });
    auto* timer = new QTimer(&app);
    QObject::connect(timer, &QTimer::timeout, &app, [&controller, state, peer] {
        if (!controller.assistedConnection()) controller.setAssistedConnection(true);
        if (!state->queued && controller.networkStatus().contains(QStringLiteral("自检通过"))) {
            state->queued = controller.addContact("Acceptance peer", peer);
            qInfo().noquote() << "P2P_ANDROID_REQUEST_QUEUED=" << state->queued;
        }
    });
    timer->start(500);
    QTimer::singleShot(120000, &app, [&app, state] {
        if (!state->done) {
            qWarning().noquote() << "P2P_ANDROID_FRIEND_FLOW=FAIL queued=" << state->queued
                                << "accepted=" << state->accepted << "sent=" << state->sent
                                << "delivered=" << state->delivered << "received=" << state->received;
            app.exit(4);
        }
    });
}
#endif
