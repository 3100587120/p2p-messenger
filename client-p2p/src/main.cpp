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
    MessengerController messenger;
    traceStartup("controller-ready");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("messenger"), &messenger);
    engine.loadFromModule("P2PMessenger", "Main");
    traceStartup("qml-loaded");
    if (engine.rootObjects().isEmpty())
        return 1;
#if defined(Q_OS_ANDROID) && !defined(QT_NO_DEBUG)
    startAndroidAcceptance(application, messenger, acceptancePeer);
#endif
    bool validTimeout = false;
    const auto exitAfterMs = qEnvironmentVariableIntValue("P2P_MESSENGER_EXIT_AFTER_MS", &validTimeout);
    if (validTimeout && exitAfterMs > 0)
        QTimer::singleShot(exitAfterMs, &application, &QGuiApplication::quit);
    return application.exec();
}
