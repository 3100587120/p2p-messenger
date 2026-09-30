#include <QGuiApplication>
#include <QQuickStyle>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFile>
#include <QTimer>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#endif

#include "messenger_controller.h"
#include "android_acceptance.h"
#include "local_vault.h"
#include <QStandardPaths>
#include "account_manager.h"
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
    AccountManager accounts;
    qputenv("P2P_MESSENGER_DATA_ROOT", accounts.activeRoot().toUtf8());
    qputenv("P2P_MESSENGER_ACCOUNT_PROFILE", "1");
    auto messenger = std::make_unique<MessengerController>();
    traceStartup("controller-ready");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("messenger"), messenger.get());
    engine.rootContext()->setContextProperty(QStringLiteral("accountManager"), &accounts);
    const auto bindProfile = [&] {
        accounts.updateName(messenger->profileName());
        QObject::connect(messenger.get(), &MessengerController::profileNameChanged, &accounts, [&] { accounts.updateName(messenger->profileName()); });
    };
    bindProfile();
    QObject::connect(&accounts, &AccountManager::accountSelected, &engine, [&](const QString& root) {
        QTimer::singleShot(0, &engine, [&, root] {
            // Replacing the only window must not enqueue application shutdown.
            const bool quitOnClose = application.quitOnLastWindowClosed();
            application.setQuitOnLastWindowClosed(false);
            for (auto* object : engine.rootObjects()) delete object;
            engine.rootContext()->setContextProperty(QStringLiteral("messenger"), static_cast<QObject*>(nullptr));
            messenger.reset();
            qputenv("P2P_MESSENGER_DATA_ROOT", root.toUtf8());
            messenger = std::make_unique<MessengerController>();
            engine.rootContext()->setContextProperty(QStringLiteral("messenger"), messenger.get());
            bindProfile(); engine.loadFromModule("P2PMessenger", "Main");
            application.setQuitOnLastWindowClosed(quitOnClose);
            if (engine.rootObjects().isEmpty()) application.exit(1);
        });
    });
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
