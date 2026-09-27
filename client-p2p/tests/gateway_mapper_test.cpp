#include "gateway_mapper.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>

namespace {
class MockGateway final : public QTcpServer
{
public:
    bool validMapping = false;
    QString externalAddress = QStringLiteral("8.8.8.8");

    MockGateway()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                auto* buffer = new QByteArray;
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    const auto headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    int length = 0;
                    for (const auto& line : buffer->left(headerEnd).split('\n')) {
                        if (line.trimmed().toLower().startsWith("content-length:"))
                            length = line.trimmed().mid(15).trimmed().toInt();
                    }
                    if (buffer->size() < headerEnd + 4 + length) return;
                    QByteArray response;
                    if (buffer->startsWith("GET /description")) {
                        response = R"(<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/control</controlURL></service></serviceList></device></root>)";
                    } else if (buffer->contains("#GetExternalIPAddress")) {
                        response = "<NewExternalIPAddress>" + externalAddress.toUtf8() +
                                   "</NewExternalIPAddress>";
                    } else if (buffer->contains("#AddPortMapping")) {
                        validMapping = buffer->contains("<NewExternalPort>4222</NewExternalPort>") &&
                                       buffer->contains("<NewInternalPort>4222</NewInternalPort>") &&
                                       buffer->contains("<NewInternalClient>192.168.2.17</NewInternalClient>") &&
                                       buffer->contains("<NewLeaseDuration>3600</NewLeaseDuration>");
                        response = validMapping ? "<ok/>" : "<invalid/>";
                    }
                    const QByteArray status = response.isEmpty() ? "404 Not Found" : "200 OK";
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: text/xml\r\nContent-Length: " +
                                  QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
                    socket->disconnectFromHost();
                    delete buffer;
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
    }
};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    MockGateway gateway;
    if (!gateway.listen(QHostAddress::LocalHost, 0)) return 1;

    GatewayMapper mapper;
    QString endpoint;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&mapper, &GatewayMapper::endpointChanged, &loop,
                     [&](const QString& value) { if (!value.isEmpty()) { endpoint = value; loop.quit(); } });

    const QUrl location(QStringLiteral("http://127.0.0.1:%1/description").arg(gateway.serverPort()));
    timeout.start(8000);
    mapper.startWithGatewayForTest(4222, QStringLiteral("192.168.2.17"), location);
    loop.exec();
    if (endpoint != QLatin1String("8.8.8.8:4222") || !gateway.validMapping) {
        std::fprintf(stderr, "UPnP mock mapping failed: %s\n", endpoint.toUtf8().constData());
        return 2;
    }

    // A CGNAT address must never be advertised as an internet-reachable endpoint.
    gateway.externalAddress = QStringLiteral("100.64.1.2");
    endpoint.clear();
    bool rejectedCgnat = false;
    QObject::connect(&mapper, &GatewayMapper::statusChanged, &loop,
                     [&](const QString& status) {
                         if (status.contains(QStringLiteral("运营商内网"))) {
                             rejectedCgnat = true;
                             loop.quit();
                         }
                     });
    timeout.start(8000);
    mapper.startWithGatewayForTest(4222, QStringLiteral("192.168.2.17"), location);
    loop.exec();
    if (!rejectedCgnat || !mapper.endpoint().isEmpty()) {
        std::fprintf(stderr, "CGNAT address was not rejected\n");
        return 3;
    }
    std::puts("Local UPnP mapping and CGNAT rejection passed");
    return 0;
}
