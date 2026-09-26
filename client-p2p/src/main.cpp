#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFile>
#include <QTimer>

#include "messenger_controller.h"

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
    QGuiApplication application(argc, argv);
    traceStartup("qt-ready");
    QGuiApplication::setApplicationName(QStringLiteral("P2P Messenger"));
    QGuiApplication::setOrganizationName(QStringLiteral("P2P Messenger"));

    MessengerController messenger;
    traceStartup("controller-ready");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("messenger"), &messenger);
    engine.loadFromModule("P2PMessenger", "Main");
    traceStartup("qml-loaded");
    if (engine.rootObjects().isEmpty())
        return 1;
    bool validTimeout = false;
    const auto exitAfterMs = qEnvironmentVariableIntValue("P2P_MESSENGER_EXIT_AFTER_MS", &validTimeout);
    if (validTimeout && exitAfterMs > 0)
        QTimer::singleShot(exitAfterMs, &application, &QGuiApplication::quit);
    return application.exec();
}
