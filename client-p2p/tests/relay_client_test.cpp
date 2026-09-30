#include "relay_client.h"
#include "local_vault.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonObject>
#include <QJsonDocument>
#include <QWebSocketServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QUuid>
#include <memory>

#include <iostream>

static int responsiveLoginRegression(QCoreApplication& app)
{
    QTemporaryDir root(QDir::tempPath()+"/async-login-test-XXXXXX");
    if(!root.isValid())return 7;
    LocalVault owner(QDir(root.path()).filePath("owner")); RelayCrypto identity;
    if(!identity.loadOrCreate(owner))return 7;
    const QString password="Regression-password-123";
    const auto record=identity.makeLoginRecord(password,"Login regression");
    if(record.isEmpty())return 7;
    for(int scenario=0;scenario<6;++scenario) {
        LocalVault vault(QDir(root.path()).filePath(QString::number(scenario)));
        auto client=std::make_unique<RelayClient>(vault);
        const auto original=client->inviteCode();
        QWebSocketServer server("isolated-login-test",QWebSocketServer::NonSecureMode);
        if(!server.listen(QHostAddress::LocalHost,0))return 7;
        QEventLoop loop;QTimer heartbeat;heartbeat.setInterval(5);
        int ticks=0;bool result=false,started=false,restored=false,tokenSeen=false,contactsRestored=false;
        QElapsedTimer elapsed;elapsed.start();qint64 last=0,maxGap=0;
        QObject::connect(&heartbeat,&QTimer::timeout,&loop,[&]{const auto now=elapsed.elapsed();maxGap=qMax(maxGap,now-last);last=now;++ticks;});
        QObject::connect(&server,&QWebSocketServer::newConnection,&loop,[&]{
            auto* socket=server.nextPendingConnection();
            QObject::connect(socket,&QWebSocket::disconnected,socket,&QObject::deleteLater);
            QObject::connect(socket,&QWebSocket::textMessageReceived,&loop,[&,socket](const QString& text){
                const auto frame=QJsonDocument::fromJson(text.toUtf8()).object();
                const auto op=frame.value("op").toString();
                if(op=="contacts_get"){
                    const auto backupId=QUuid::createUuid().toString(QUuid::WithoutBraces);
                    const auto encrypted=identity.seal(RelayCrypto::publicKeyFromCode(identity.inviteCode()),backupId,QJsonObject{{"type","contact_backup"},{"v",1},{"profile",QJsonObject{{"marker","friends-restored"}}}});
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op","contacts_get_result"},{"id",frame.value("id")},{"snapshot",QJsonObject{{"id",backupId},{"envelope",QString::fromLatin1(encrypted)}}}}).toJson(QJsonDocument::Compact)));return;}
                if(op=="register" && scenario==5){tokenSeen=true;loop.quit();return;}
                if(op=="register" && scenario==4) {
                    const auto auth=frame.value("auth").toObject();
                    if(auth.value("iterations")!=600000 || auth.value("backup").toString().isEmpty() || auth.value("code").toString()!=client->inviteCode()){loop.quit();return;}
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op","registered"},{"id",frame.value("id")},{"uid","11"}}).toJson(QJsonDocument::Compact)));return;
                }
                if(op=="login_info") {
                    QJsonObject info{{"op","login_info_result"},{"id",frame.value("id")},{"uid","11"},{"salt",record.value("salt")},{"iterations",600000}};
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
                    if(scenario==1 || scenario==2)QTimer::singleShot(20,&loop,[&]{
                        if(scenario==1)client->cancelLogin();else client.reset();
                        QTimer::singleShot(1800,&loop,[&]{result=!tokenSeen && (scenario==2 || (!client->loginPending() && client->inviteCode()==original));loop.quit();});
                    });
                } else if(op=="login") {
                    tokenSeen=true;
                    if(scenario!=0 && scenario!=3){result=false;loop.quit();return;}
                    if(frame.value("token")!=record.value("token")) {
                        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op","directory_error"},{"id",frame.value("id")},{"reason","invalid_password"}}).toJson(QJsonDocument::Compact)));return;
                    }
                    auto response=record;response.insert("op","login_result");response.insert("id",frame.value("id"));response.insert("uid","11");
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact)));
                }
            });
        });
        QObject::connect(client.get(),&RelayClient::passwordAccountRestored,&loop,[&](const QString& uid,const QString&){restored=uid=="11";});
        QObject::connect(client.get(),&RelayClient::contactBackupRestored,&loop,[&](const QVariantMap& value){contactsRestored=value.value("marker")=="friends-restored";});
        QObject::connect(client.get(),&RelayClient::uidAssigned,&loop,[&](const QString& uid){if(scenario==4){result=uid=="11" && client->passwordConfigured() && !client->registrationPending();loop.quit();}});
        QObject::connect(client.get(),&RelayClient::loginStateChanged,&loop,[&]{
            if(started && !client->loginPending() && (scenario==0 || scenario==3)) {
                result=scenario==0?restored && contactsRestored && client->passwordConfigured() && client->inviteCode()==identity.inviteCode():!restored && client->inviteCode()==original;
                loop.quit();
            }
        });
        QObject::connect(client.get(),&RelayClient::connectedChanged,&loop,[&](bool connected){
            if(connected && !started){started=true;if(scenario>=4){
                if(!client->registerPasswordAccount(password,"Async registration") || !client->registrationPending())loop.quit();
                if(scenario==5)QTimer::singleShot(20,&loop,[&]{client->setEnabled(false);QTimer::singleShot(1800,&loop,[&]{result=!tokenSeen && !client->registrationPending() && !vault.hasConversation("__account_registration") && !client->passwordConfigured();loop.quit();});});
            }else client->loginPasswordAccount("11",scenario==3?"Wrong-password-123":password);}
        });
        client->setEndpoint(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort())));client->setEnabled(true);
        QTimer timeout;timeout.setSingleShot(true);QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);timeout.start(15000);heartbeat.start();loop.exec();
        if(!result || ticks<10 || maxGap>250){std::cerr<<"LOGIN_SCENARIO="<<scenario<<" result="<<result<<" started="<<started<<" token="<<tokenSeen<<" ticks="<<ticks<<" gap="<<maxGap<<" pending="<<(client && client->registrationPending())<<" stored="<<vault.hasConversation("__account_registration")<<std::endl;return 7;}
        std::cout<<"LOGIN_SCENARIO="<<scenario<<" HEARTBEAT_TICKS="<<ticks<<" MAX_GUI_GAP_MS="<<maxGap<<" PASS"<<std::endl;
    }
    Q_UNUSED(app);
    std::cout<<"ASYNC_LOGIN_SUCCESS_CANCEL_DESTROY_WRONG_PASSWORD_REGISTRATION_AND_RETIRED_SESSION_NO_WRITES=PASS"<<std::endl;
    return 0;
}

static int untrustedReceiptRegression(QCoreApplication& app)
{
    QTemporaryDir root(QDir::tempPath() + "/relay-receipt-test-XXXXXX");
    if (!root.isValid()) return 7;
    qputenv("P2P_MESSENGER_DATA_ROOT", QDir(root.path()).filePath("alice").toUtf8());
    LocalVault aliceVault;
    RelayClient alice(aliceVault);
    qputenv("P2P_MESSENGER_DATA_ROOT", QDir(root.path()).filePath("bob").toUtf8());
    LocalVault bobVault;
    RelayCrypto bob;
    if (!alice.isReady() || !bob.loadOrCreate(bobVault)) return 7;
    QWebSocketServer server("receipt-test", QWebSocketServer::NonSecureMode);
    if (!server.listen(QHostAddress::LocalHost, 0)) return 7;
    QString sentId;
    bool forwarded = false, realAckSent = false, delivered = false, invalidSignal = false;
    QObject::connect(&alice, &RelayClient::deliveryState, &app, [&](const QString& id, const QString& state) {
        if (id == "unknown-packet") invalidSignal = true;
        if (id != sentId) return;
        if (state == "forwarded") forwarded = true;
        if (state == "delivered") {
            if (!realAckSent) invalidSignal = true;
            delivered = true;
            app.quit();
        }
    });
    QObject::connect(&server, &QWebSocketServer::newConnection, &app, [&] {
        auto* socket = server.nextPendingConnection();
        QObject::connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QWebSocket::textMessageReceived, &app, [&, socket](const QString& text) {
            const auto frame = QJsonDocument::fromJson(text.toUtf8()).object();
            if (frame.value("op") != "send") return;
            const auto id = frame.value("id").toString();
            for (const auto& state : {QStringLiteral("delivered"), QStringLiteral("forwarded")})
                socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {
                    {"op", "relay"}, {"id", id}, {"status", state}}).toJson(QJsonDocument::Compact)));
            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {
                {"op", "relay"}, {"id", "unknown-packet"}, {"status", "forwarded"}}).toJson(QJsonDocument::Compact)));
            QTimer::singleShot(200, &app, [&, socket, id] {
                if (invalidSignal || delivered || !forwarded) { app.exit(7); return; }
                const auto envelope = bob.seal(RelayClient::publicKeyFromCode(alice.inviteCode()),
                    "authenticated-ack", QJsonObject {{"type", "ack"}, {"id", id}});
                if (envelope.isEmpty()) { app.exit(7); return; }
                realAckSent = true;
                socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {
                    {"op", "packet"}, {"id", "authenticated-ack"}, {"from", bob.identityId()},
                    {"envelope", QString::fromLatin1(envelope)}}).toJson(QJsonDocument::Compact)));
            });
        });
    });
    QObject::connect(&alice, &RelayClient::connectedChanged, &app, [&](bool connected) {
        if (connected) sentId = alice.send(RelayClient::publicKeyFromCode(bob.inviteCode()),
                                           QJsonObject {{"type", "friend_request"}});
    });
    alice.setEndpoint(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort())));
    alice.setEnabled(true);
    QTimer::singleShot(5000, &app, &QCoreApplication::quit);
    app.exec();
    if (!forwarded || !realAckSent || !delivered || invalidSignal) return 7;
    std::cout << "RELAY_METADATA_CANNOT_FORGE_DELIVERY_AUTHENTICATED_ACK_REQUIRED=PASS" << std::endl;
    return 0;
}

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
    if (app.arguments().contains("--responsive-login-regression")) return responsiveLoginRegression(app);
    if (app.arguments().contains("--untrusted-receipt-regression")) return untrustedReceiptRegression(app);
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
