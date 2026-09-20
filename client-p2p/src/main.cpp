#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "messenger_controller.h"

int main(int argc, char* argv[])
{
    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("P2P Messenger"));
    QGuiApplication::setOrganizationName(QStringLiteral("P2P Messenger"));

    MessengerController messenger;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("messenger"), &messenger);
    engine.loadFromModule("P2PMessenger", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;
    return application.exec();
}
