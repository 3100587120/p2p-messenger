#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>

#include <functional>

// Discovers a local UPnP IGD and maps the Jami UDP port without an internet service.
class GatewayMapper final : public QObject
{
    Q_OBJECT
public:
    explicit GatewayMapper(QObject* parent = nullptr);

    QString endpoint() const { return endpoint_; }
    bool busy() const { return busy_; }
    void start(quint16 port);
#ifdef P2P_MESSENGER_GATEWAY_TESTING
    void startWithGatewayForTest(quint16 port, const QString& localIp, const QUrl& location);
#endif

signals:
    void endpointChanged(const QString& endpoint);
    void statusChanged(const QString& status);

private:
    QNetworkAccessManager http_;
    QUdpSocket discovery_;
    QTimer discoveryTimeout_;
    QTimer renewal_;
    QUrl controlUrl_;
    QString serviceType_;
    QString localIp_;
    QString publicIp_;
    QString endpoint_;
    quint16 internalPort_ {0};
    quint16 externalPort_ {0};
    int mappingAttempt_ {0};
    int generation_ {0};
    bool busy_ {false};

    void readDiscovery();
    void readDescription(const QUrl& location);
    void requestPublicAddress();
    void requestMapping();
    void postSoap(const QString& action, const QString& body,
                  std::function<void(bool, const QByteArray&)> completion);
    void fail(const QString& reason);
    void setEndpoint(const QString& endpoint);
};
