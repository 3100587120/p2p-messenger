#include <QGuiApplication>
#include <QQuickStyle>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFile>
#include <QTimer>
#include <QTemporaryDir>
#include <QDir>
#include <QFileInfo>
#include <QScopedValueRollback>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#endif

#include "messenger_controller.h"
#include "android_acceptance.h"
#include "local_vault.h"
#include <QStandardPaths>
#include "account_manager.h"
#include "notification_service.h"
#include "background_session.h"
#include "photo_picker.h"
#include "screenshot_service.h"
#include <cstring>
#include <memory>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>

namespace {
void traceStartup(const char* stage)
{
    const auto path = qEnvironmentVariable("P2P_MESSENGER_STARTUP_TRACE");
    if (path.isEmpty()) return;
    QFile trace(path);
    if (trace.open(QIODevice::WriteOnly | QIODevice::Append)) {
        trace.write(stage);
        trace.write("\n");
    }
}
}

int main(int argc, char* argv[])
{
    traceStartup("entry");
#ifdef Q_OS_WIN
    for(int i=1;i<argc;++i)if(std::strcmp(argv[i],"--connection-config-report")==0) {
        QCoreApplication app(argc,argv);
        QCoreApplication::setApplicationName(QStringLiteral("P2P Messenger"));
        QCoreApplication::setOrganizationName(QStringLiteral("P2P Messenger"));
        AccountManager accounts;
        if(!QFileInfo::exists(QDir(accounts.activeRoot()).filePath("vault/master-key.protected")))return 6;
        LocalVault vault(accounts.activeRoot());const auto rows=vault.loadConversation("__profile");
        if(!vault.isReady() || rows.isEmpty())return 6;
        const auto profile=rows.first().toMap(),network=profile.value("network").toMap();
        const QUrl saved(network.value("relayEndpoint").toString());
        std::cout<<QJsonDocument(QJsonObject{{"profileReadable",true},{"registered",!profile.value("uid").toString().isEmpty()},{"assisted",network.value("assistedConnection",true).toBool()},{"savedEndpointPresent",!saved.isEmpty()},{"savedEndpointSecure",saved.scheme()=="wss" && !saved.host().isEmpty()}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        return 0;
    }
    // Validate this exact deliverable, not a test binary with an injected URL.
    // Temporary identity only: no real accounts, credentials or history touched.
    for(int i=1;i<argc;++i)if(std::strcmp(argv[i],"--verify-default-relay")==0) {
        QCoreApplication app(argc,argv);
        QTemporaryDir temporary(QDir::tempPath()+"/packaged-relay-XXXXXX");
        if(!temporary.isValid())return 6;
        qunsetenv("P2P_MESSENGER_RELAY_URL");qunsetenv("P2P_MESSENGER_OWNER_DEVICE_ROOT");
        qputenv("P2P_MESSENGER_DATA_ROOT",temporary.path().toUtf8());
        qputenv("P2P_MESSENGER_DISABLE_DIRECT_ENGINE","1");
        MessengerController controller;
        if(controller.relayEndpoint().isEmpty()){std::cerr<<"PACKAGED_DEFAULT_RELAY_MISSING"<<std::endl;return 6;}
        bool passed=false;QTimer poll;poll.setInterval(100);
        QObject::connect(&poll,&QTimer::timeout,&app,[&]{
            if(controller.networkStatus().contains(QStringLiteral("自检通过"))){passed=true;app.quit();}
        });
        QTimer::singleShot(45000,&app,&QCoreApplication::quit);poll.start();app.exec();
        std::cout<<(passed?"PACKAGED_DEFAULT_RELAY_AUTHENTICATED_ROUNDTRIP=PASS":"PACKAGED_DEFAULT_RELAY_AUTHENTICATED_ROUNDTRIP=FAIL")<<std::endl;
        if(!passed)std::cerr<<controller.lastError().toStdString()<<std::endl;
        return passed?0:6;
    }
#endif
#ifdef Q_OS_ANDROID
    // A QtService context is not an Activity and has no getIntent(). Dispatch
    // BEFORE debug acceptance code or any Activity/QGuiApplication operations.
    for(int i=1;i<argc;++i) if(std::strcmp(argv[i],"--message-service")==0)return runMessageService(argc,argv);
#endif
#if defined(Q_OS_ANDROID) && !defined(QT_NO_DEBUG)
    // Optional CI-only path in this exact APK: isolate Keystore/TLS/controller
    // from the emulator's unsupported Qt GUI SIMD instruction translation.
    // Normal launches and UI acceptance do not take this path.
    if (androidAcceptanceHeadless()) {
        qputenv("P2P_MESSENGER_DISABLE_DIRECT_ENGINE", "1");
        QCoreApplication application(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("P2P Messenger"));
        QCoreApplication::setOrganizationName(QStringLiteral("P2P Messenger"));
        QString peer;
        if (!prepareAndroidAcceptance(application, &peer) || peer.isEmpty()) return 2;
        MessengerController messenger;
        startAndroidAcceptance(application, messenger, peer);
        return application.exec();
    }
#endif
#ifdef Q_OS_ANDROID
    qputenv("QT_BLOCK_EVENT_LOOPS_WHEN_SUSPENDED","0");
#endif
    QQuickStyle::setStyle(QStringLiteral("Material"));
    QGuiApplication application(argc, argv);
    traceStartup("qt-ready");
    QGuiApplication::setApplicationName(QStringLiteral("P2P Messenger"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("双点聊"));
    QGuiApplication::setOrganizationName(QStringLiteral("P2P Messenger"));
#ifdef Q_OS_WIN
    // Administrative provisioning only. File contents and keys never go to logs.
    if (application.arguments().contains("--public-identity")) {
        LocalVault device(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
        RelayCrypto identity;
        if (!device.hasConversation("__relay_identity") || !identity.loadOrCreate(device)) return 3;
        std::cout << QJsonDocument(QJsonObject {{"id",identity.identityId()},{"code",identity.inviteCode()}}).toJson(QJsonDocument::Compact).constData() << std::endl;
        return 0;
    }
    const auto provision = application.arguments().indexOf("--provision-owner-device");
    if (provision >= 0) {
        QFile file(application.arguments().value(provision + 1));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 128) return 3;
        const auto secret = QByteArray::fromBase64(file.readAll().trimmed());
        if (secret.size() != 32) return 3;
        LocalVault device(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
        return device.saveConversation("__owner_device", {QVariantMap {{"secret", QString::fromLatin1(secret.toBase64())}, {"enabled",application.arguments().contains("--enable-owner-binding")}}}) ? 0 : 3;
    }
    if (application.arguments().contains("--reset-local-accounts")) {
        QString error; return AccountManager::resetForPasswordRelease(&error) ? 0 : 4;
    }
#endif
#ifdef Q_OS_ANDROID
    const auto updateLanDiscovery = [](Qt::ApplicationState state) {
        const auto context = QNativeInterface::QAndroidApplication::context();
        QJniObject::callStaticMethod<void>(
            "io/p2pmessenger/app/LocalDiscovery", "setActive",
            "(Landroid/content/Context;Z)V", context.object<jobject>(),
            static_cast<jboolean>(state == Qt::ApplicationActive));
    };
    QObject::connect(&application, &QGuiApplication::applicationStateChanged,
                     &application, updateLanDiscovery);
    updateLanDiscovery(application.applicationState());
#endif

#if defined(Q_OS_ANDROID) && !defined(QT_NO_DEBUG)
    QString acceptancePeer;
    if (!prepareAndroidAcceptance(application, &acceptancePeer)) return 2;
#endif
    QString accountResetError;
    if (!AccountManager::resetForPasswordRelease(&accountResetError)) { std::cerr << accountResetError.toStdString() << std::endl; return 4; }
    AccountManager accounts;
#ifdef Q_OS_ANDROID
    BackgroundSession background(accounts.baseRoot());
    if(!background.claimInitial())return 5;
#endif
    qputenv("P2P_MESSENGER_DATA_ROOT", accounts.activeRoot().toUtf8());
    qputenv("P2P_MESSENGER_ACCOUNT_PROFILE", "1");
    qputenv("P2P_MESSENGER_OWNER_DEVICE_ROOT",accounts.baseRoot().toUtf8());
    auto messenger = std::make_unique<MessengerController>();
    traceStartup("controller-ready");
    NotificationService notifications;
    ScreenshotService screenCapture;
    QQmlApplicationEngine engine;
    notifications.setSessionRoot(accounts.baseRoot());
    engine.rootContext()->setContextProperty(QStringLiteral("notificationService"),&notifications);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCapture"),&screenCapture);
    const auto restoreScreenshotWindow=[&]{for(auto* object:engine.rootObjects())QMetaObject::invokeMethod(object,"finishScreenshotSelection");};
    QObject::connect(&screenCapture,&ScreenshotService::requested,&engine,[&]{for(auto* object:engine.rootObjects())QMetaObject::invokeMethod(object,"beginScreenshotSelection");});
    QObject::connect(&screenCapture,&ScreenshotService::selected,&engine,[&](const QImage& image){restoreScreenshotWindow();messenger->acceptScreenshot(image);});
    QObject::connect(&screenCapture,&ScreenshotService::canceled,&engine,restoreScreenshotWindow);
    QObject::connect(&screenCapture,&ScreenshotService::error,&engine,[&](const QString& reason){restoreScreenshotWindow();messenger->screenshotFailed(reason);});
    engine.rootContext()->setContextProperty(QStringLiteral("messenger"), messenger.get());
    engine.rootContext()->setContextProperty(QStringLiteral("accountManager"), &accounts);
    const auto bindProfile = [&] {
        QObject::connect(messenger.get(),&MessengerController::screenshotSelectionRequested,&screenCapture,&ScreenshotService::start);
        QObject::connect(messenger.get(), &MessengerController::incomingNotice, &notifications, &NotificationService::show);
        QObject::connect(messenger.get(), &MessengerController::notificationPermissionRequested, &notifications, &NotificationService::requestPermission);
        QObject::connect(messenger.get(),&MessengerController::uidChanged,&notifications,[&]{if(!messenger->userCode().isEmpty())notifications.accountReady();});
        QObject::connect(messenger.get(),&MessengerController::uidChanged,&accounts,[&]{accounts.completeLogin(messenger->userCode(),messenger->inviteCode());});
        accounts.updateName(messenger->profileName());
        QObject::connect(messenger.get(), &MessengerController::profileNameChanged, &accounts, [&] { accounts.updateName(messenger->profileName()); });
    };
    bindProfile();
    bool rebuildingProfile = false;
    const auto reloadProfile = [&](const QString& root) {
        QTimer::singleShot(0, &engine, [&, root] {
#ifdef Q_OS_ANDROID
            if(!background.ownsSession())return;
#endif
            // Replacing the QQuickWindow can emit Hidden/Active synchronously.
            // That is not a real handover to the Android background process.
            QScopedValueRollback<bool> rebuilding(rebuildingProfile, true);
            screenCapture.cancel();
            const auto previousContact=messenger->activeContactId();
            // Keep the native Android window/surface alive during every handover.
            messenger->suspendForBackground();
            auto oldMessenger=std::move(messenger);
            qputenv("P2P_MESSENGER_DATA_ROOT", root.toUtf8());
            messenger = std::make_unique<MessengerController>();
            const auto pendingLogin = accounts.takeLogin();
            engine.rootContext()->setContextProperty(QStringLiteral("messenger"), messenger.get());
            bindProfile();
            if (pendingLogin.size()==2) messenger->loginAccount(pendingLogin[0],pendingLogin[1]);
            else messenger->selectContact(previousContact);
            for(auto* object:engine.rootObjects())QMetaObject::invokeMethod(object,"resetSession");
            oldMessenger.reset();
            if (engine.rootObjects().isEmpty()) application.exit(1);
        });
    };
    QObject::connect(&accounts,&AccountManager::accountSelected,&engine,reloadProfile);
#ifdef Q_OS_ANDROID
    application.setQuitOnLastWindowClosed(false);
    background.setCallbacks([&]{messenger->suspendForBackground();},[&]{reloadProfile(accounts.activeRoot());});
    QObject::connect(&application,&QGuiApplication::applicationStateChanged,&engine,[&](Qt::ApplicationState state){
        if(rebuildingProfile)return;
        if((state==Qt::ApplicationHidden || state==Qt::ApplicationSuspended) && (systemPickerOpen() || messenger->voicePermissionPending()))return;
        if((state==Qt::ApplicationHidden || state==Qt::ApplicationSuspended) && !notifications.backgroundEnabled())return;
        background.setActive(state==Qt::ApplicationActive || state==Qt::ApplicationInactive);
        if(state==Qt::ApplicationActive && !messenger->userCode().isEmpty())notifications.accountReady();
    });
    if(!messenger->userCode().isEmpty())notifications.accountReady();
#endif
    engine.loadFromModule("P2PMessenger", "Main");
    traceStartup("qml-loaded");
    if (engine.rootObjects().isEmpty())
        return 1;
#if defined(Q_OS_ANDROID) && !defined(QT_NO_DEBUG)
    startAndroidAcceptance(application, *messenger, acceptancePeer);
#endif
    bool validTimeout = false;
    const auto exitAfterMs = qEnvironmentVariableIntValue("P2P_MESSENGER_EXIT_AFTER_MS", &validTimeout);
    if (validTimeout && exitAfterMs > 0)
        QTimer::singleShot(exitAfterMs, &application, &QGuiApplication::quit);
    return application.exec();
}
