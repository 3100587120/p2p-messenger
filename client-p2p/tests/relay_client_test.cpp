#include "relay_client.h"
#include "local_vault.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
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
    bool offlineNotice = false, peerStartScheduled = false;
    QString sentId, fileSentId;
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
        if (sentId.isEmpty() && alice.isConnected() && (latePeer || bob.isConnected()))
            sentId = alice.send(RelayClient::publicKeyFromCode(bob.inviteCode()),
                QJsonObject {{QStringLiteral("type"), QStringLiteral("friend_request")}});
        if (fileSentId.isEmpty() && !sentId.isEmpty())
            fileSentId = alice.send(RelayClient::publicKeyFromCode(bob.inviteCode()),
                QJsonObject {{QStringLiteral("type"), QStringLiteral("file_chunk")},
                             {QStringLiteral("data"), QString::fromLatin1(QByteArray(12288, 'f').toBase64())}});
        if (received && delivered && fileReceived && fileDelivered) app.quit();
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
    if (!latePeer) bob.setEnabled(true);
    timer.start();
    app.exec();
    if (!received || !delivered || !fileReceived || !fileDelivered || (latePeer && !offlineNotice)) {
        std::cerr << "relay test failed: received=" << received << " delivered=" << delivered
                  << " file_received=" << fileReceived << " file_delivered=" << fileDelivered
                  << " alice_connected=" << alice.isConnected() << " bob_connected=" << bob.isConnected()
                  << " offline_notice=" << offlineNotice << '\n';
        return 4;
    }
    std::cout << "relay delivery, authenticated receipt, and file-sized frame passed\n";
    return 0;
}
