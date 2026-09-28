#include "relay_client.h"
#include "local_vault.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>

static int inspectProfile(QCoreApplication& app, const QString& profileRoot)
{
    qputenv("P2P_MESSENGER_DATA_ROOT", profileRoot.toUtf8());
    LocalVault original;
    const auto profiles = original.loadConversation(QStringLiteral("__profile"));
    if (!original.isReady() || profiles.isEmpty()) return 6;
    const auto profile = profiles.first().toMap();
    const auto network = profile.value(QStringLiteral("network")).toMap();
    const auto contacts = profile.value(QStringLiteral("contacts")).toList();
    std::cout << "assisted=" << network.value(QStringLiteral("assistedConnection")).toBool()
              << " endpoint=" << network.value(QStringLiteral("relayEndpoint")).toString().toStdString()
              << " contacts=" << contacts.size()
              << " pending=" << profile.value(QStringLiteral("pendingRelayRequests")).toMap().size()
              << " outbox=" << original.loadConversation(QStringLiteral("__relay_outbox")).size() << std::endl;
    QTemporaryDir temporary(QDir::tempPath() + QStringLiteral("/relay-profile-probe-XXXXXX"));
    if (!temporary.isValid()) return 2;
    qputenv("P2P_MESSENGER_DATA_ROOT", temporary.path().toUtf8());
    LocalVault scratch;
    RelayClient probe(scratch);
    QMap<QString, QString> targets;
    bool started = false;
    QObject::connect(&probe, &RelayClient::connectedChanged, &app, [&](bool connected) {
        std::cout << "probe_connected=" << connected << std::endl;
        if (!connected || started) return;
        started = true;
        for (const auto& value : contacts) {
            const auto contact = value.toMap();
            if (contact.value(QStringLiteral("transport")).toString() != QStringLiteral("relay")) continue;
            const auto key = RelayClient::publicKeyFromCode(contact.value(QStringLiteral("relayPublic")).toString());
            const auto fingerprint = RelayClient::idForPublicKey(key).left(12);
            const auto id = probe.send(key, QJsonObject {{QStringLiteral("type"), QStringLiteral("probe")}});
            targets.insert(id, fingerprint);
            std::cout << "peer=" << fingerprint.toStdString() << " ready="
                      << contact.value(QStringLiteral("ready")).toBool() << std::endl;
        }
    });
    QObject::connect(&probe, &RelayClient::deliveryState, &app, [&](const QString& id, const QString& state) {
        if (targets.contains(id))
            std::cout << "peer=" << targets.value(id).toStdString() << " state=" << state.toStdString() << std::endl;
    });
    QObject::connect(&probe, &RelayClient::errorOccurred, &app,
        [](const QString& error) { std::cerr << error.toStdString() << std::endl; });
    probe.setEndpoint(QUrl(network.value(QStringLiteral("relayEndpoint")).toString()));
    probe.setEnabled(true);
    QTimer::singleShot(25000, &app, &QCoreApplication::quit);
    app.exec();
    return 0;
}

static int runRemoteReceiver(QCoreApplication& app)
{
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/relay-remote-test-XXXXXX"));
    if (!root.isValid()) return 2;
    qputenv("P2P_MESSENGER_DATA_ROOT", root.path().toUtf8());
    LocalVault vault;
    RelayClient receiver(vault);
    if (!receiver.isReady()) return 3;
    bool received = false;
    QObject::connect(&receiver, &RelayClient::packetReceived, &app,
        [&](const QString&, const QByteArray&, const QJsonObject& packet) {
            if (packet.value(QStringLiteral("type")) == QStringLiteral("friend_request") &&
                packet.value(QStringLiteral("probe")) == QStringLiteral("cross_wan")) {
                received = true;
                QTimer::singleShot(3000, &app, &QCoreApplication::quit);
            }
        });
    QObject::connect(&receiver, &RelayClient::errorOccurred, &app,
        [](const QString& error) { std::cerr << error.toStdString() << '\n'; });
    const QUrl endpoint(qEnvironmentVariable("P2P_MESSENGER_RELAY_TEST_URL"));
    receiver.setEndpoint(endpoint);
    receiver.setEnabled(true);
    std::cout << "REMOTE_RECEIVER_INVITE=" << receiver.inviteCode().toStdString() << std::endl;
    QTimer::singleShot(90000, &app, &QCoreApplication::quit);
    app.exec();
    std::cout << "REMOTE_RECEIVER_RESULT=" << (received ? "PASS" : "FAIL") << std::endl;
    return received ? 0 : 4;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (!qEnvironmentVariable("P2P_MESSENGER_RELAY_PROFILE_ROOT").isEmpty())
        return inspectProfile(app, qEnvironmentVariable("P2P_MESSENGER_RELAY_PROFILE_ROOT"));
    if (qEnvironmentVariableIsSet("P2P_MESSENGER_RELAY_TEST_REMOTE_RECEIVER"))
        return runRemoteReceiver(app);
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/relay-client-test-XXXXXX"));
    if (!root.isValid()) return 2;
    qputenv("P2P_MESSENGER_DATA_ROOT", QDir(root.path()).filePath(QStringLiteral("alice")).toUtf8());
    LocalVault aliceVault;
    RelayClient alice(aliceVault);
    qputenv("P2P_MESSENGER_DATA_ROOT", QDir(root.path()).filePath(QStringLiteral("bob")).toUtf8());
    LocalVault bobVault;
    RelayClient bob(bobVault);
    if (!alice.isReady() || !bob.isReady()) return 3;
    const bool latePeer = qEnvironmentVariableIsSet("P2P_MESSENGER_RELAY_TEST_LATE_PEER");
    bool received = false, delivered = false, fileReceived = false, fileDelivered = false;
    bool selfReceived = false, selfDelivered = false;
    bool offlineNotice = false, peerStartScheduled = false;
    QString sentId, fileSentId, selfId;
    QObject::connect(&alice, &RelayClient::packetReceived, &app,
        [&](const QString& sender, const QByteArray&, const QJsonObject& packet) {
            if (sender == alice.identityId() && packet.value(QStringLiteral("type")) == QStringLiteral("probe"))
                selfReceived = true;
        });
    QObject::connect(&bob, &RelayClient::packetReceived, &app,
        [&](const QString& sender, const QByteArray&, const QJsonObject& packet) {
            if (sender == alice.identityId() && packet.value(QStringLiteral("type")) == QStringLiteral("friend_request"))
                received = true;
            if (sender == alice.identityId() && packet.value(QStringLiteral("type")) == QStringLiteral("file_chunk"))
                fileReceived = QByteArray::fromBase64(packet.value(QStringLiteral("data")).toString().toLatin1())
                    == QByteArray(12288, 'f');
        });
    QObject::connect(&alice, &RelayClient::deliveryState, &app,
        [&](const QString& id, const QString& state) {
            if (id == selfId && state == QStringLiteral("delivered")) selfDelivered = true;
            if (latePeer && id == sentId && state == QStringLiteral("recipient_offline")) {
                offlineNotice = true;
                if (!peerStartScheduled) {
                    peerStartScheduled = true;
                    QTimer::singleShot(1000, &bob, [&bob] { bob.setEnabled(true); });
                }
            }
            if (id == sentId && state == QStringLiteral("delivered")) delivered = true;
            if (id == fileSentId && state == QStringLiteral("delivered")) fileDelivered = true;
        });
    QTimer timer;
    timer.setInterval(100);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (selfId.isEmpty() && alice.isConnected())
            selfId = alice.send(RelayClient::publicKeyFromCode(alice.inviteCode()),
                QJsonObject {{QStringLiteral("type"), QStringLiteral("probe")}});
        if (sentId.isEmpty() && alice.isConnected() && (latePeer || bob.isConnected()))
            sentId = alice.send(RelayClient::publicKeyFromCode(bob.inviteCode()),
                QJsonObject {{QStringLiteral("type"), QStringLiteral("friend_request")}});
        if (fileSentId.isEmpty() && !sentId.isEmpty())
            fileSentId = alice.send(RelayClient::publicKeyFromCode(bob.inviteCode()),
                QJsonObject {{QStringLiteral("type"), QStringLiteral("file_chunk")},
                             {QStringLiteral("data"), QString::fromLatin1(QByteArray(12288, 'f').toBase64())}});
        if (received && delivered && fileReceived && fileDelivered && selfReceived && selfDelivered) app.quit();
    });
    QTimer::singleShot(latePeer ? 45000 : 30000, &app, &QCoreApplication::quit);
    const auto configuredEndpoint = qEnvironmentVariable("P2P_MESSENGER_RELAY_TEST_URL");
    const QUrl endpoint(configuredEndpoint.isEmpty() ? QStringLiteral("ws://127.0.0.1:8787")
                                                   : configuredEndpoint);
    alice.setEndpoint(endpoint);
    bob.setEndpoint(endpoint);
    QEventLoop disabledMode;
    QTimer::singleShot(300, &disabledMode, &QEventLoop::quit);
    disabledMode.exec();
    if (alice.isConnected() || bob.isConnected()) {
        std::cerr << "relay connected before Assisted Connection was enabled\n";
        return 5;
    }
    alice.setEnabled(true);
    if (latePeer) QTimer::singleShot(800, &alice, &RelayClient::refreshConnection);
    if (!latePeer) bob.setEnabled(true);
    timer.start();
    app.exec();
    if (!received || !delivered || !fileReceived || !fileDelivered ||
        !selfReceived || !selfDelivered || (latePeer && !offlineNotice)) {
        std::cerr << "relay test failed: received=" << received << " delivered=" << delivered
                  << " file_received=" << fileReceived << " file_delivered=" << fileDelivered
                  << " alice_connected=" << alice.isConnected() << " bob_connected=" << bob.isConnected()
                  << " offline_notice=" << offlineNotice << " self_received=" << selfReceived
                  << " self_delivered=" << selfDelivered << '\n';
        return 4;
    }
    std::cout << "relay delivery, authenticated receipt, and file-sized frame passed\n";
    return 0;
}
