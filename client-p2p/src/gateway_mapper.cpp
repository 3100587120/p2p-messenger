#include "gateway_mapper.h"

#include <QHostAddress>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QXmlStreamReader>

#include <utility>

namespace {
bool localGatewayAddress(const QHostAddress& address)
{
    return address.protocol() == QAbstractSocket::IPv4Protocol &&
           (address.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8) ||
            address.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12) ||
            address.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16));
}

bool publicIPv4(const QHostAddress& address)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol ||
        address.isLoopback() || address.isMulticast() || localGatewayAddress(address)) return false;
    for (const auto& [subnet, bits] : {
             std::pair {"0.0.0.0", 8}, std::pair {"100.64.0.0", 10},
             std::pair {"169.254.0.0", 16}, std::pair {"192.0.0.0", 24},
             std::pair {"192.0.2.0", 24}, std::pair {"198.18.0.0", 15},
             std::pair {"198.51.100.0", 24}, std::pair {"203.0.113.0", 24},
             std::pair {"240.0.0.0", 4}}) {
        if (address.isInSubnet(QHostAddress(QString::fromLatin1(subnet)), bits)) return false;
    }
    return true;
}

QString xmlValue(const QByteArray& document, const QString& element)
{
    QXmlStreamReader xml(document);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == element)
            return xml.readElementText().trimmed();
    }
    return {};
}

bool serviceControl(const QByteArray& document, QString* serviceType, QString* controlPath)
{
    QXmlStreamReader xml(document);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("service")) continue;
        QString type;
        QString path;
        while (xml.readNextStartElement()) {
            if (xml.name() == QLatin1String("serviceType")) type = xml.readElementText().trimmed();
            else if (xml.name() == QLatin1String("controlURL")) path = xml.readElementText().trimmed();
            else xml.skipCurrentElement();
        }
        if (type == QLatin1String("urn:schemas-upnp-org:service:WANIPConnection:2") ||
            type == QLatin1String("urn:schemas-upnp-org:service:WANIPConnection:1") ||
            type == QLatin1String("urn:schemas-upnp-org:service:WANPPPConnection:1")) {
            *serviceType = type;
            *controlPath = path;
            return !path.isEmpty();
        }
    }
    return false;
}
}

GatewayMapper::GatewayMapper(QObject* parent) : QObject(parent)
{
    http_.setProxy(QNetworkProxy::NoProxy);
    discoveryTimeout_.setSingleShot(true);
    renewal_.setSingleShot(true);
    connect(&discovery_, &QUdpSocket::readyRead, this, &GatewayMapper::readDiscovery);
    connect(&discoveryTimeout_, &QTimer::timeout, this, [this] {
        if (busy_ && controlUrl_.isEmpty()) fail(tr("路由器没有响应本地直连配置请求。"));
    });
    connect(&renewal_, &QTimer::timeout, this, [this] {
        if (!controlUrl_.isEmpty() && internalPort_ > 0) {
            busy_ = true;
            mappingAttempt_ = 0;
            requestMapping();
        }
    });
}

void GatewayMapper::start(quint16 port)
{
    if (port == 0 || (busy_ && internalPort_ == port) ||
        (!endpoint_.isEmpty() && internalPort_ == port)) return;
    ++generation_;
    internalPort_ = port;
    externalPort_ = port;
    mappingAttempt_ = 0;
    publicIp_.clear();
    serviceType_.clear();
    controlUrl_.clear();
    setEndpoint({});
    renewal_.stop();
    discovery_.close();
    busy_ = true;
    emit statusChanged(tr("正在向本地路由器申请跨网直连…"));
    if (!discovery_.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress)) {
        fail(tr("无法启动本地路由器发现。"));
        return;
    }
    discovery_.setSocketOption(QAbstractSocket::MulticastTtlOption, 2);
    for (const auto& target : {"upnp:rootdevice",
                               "urn:schemas-upnp-org:device:InternetGatewayDevice:1"}) {
        const QByteArray request = "M-SEARCH * HTTP/1.1\r\n"
                                   "HOST: 239.255.255.250:1900\r\n"
                                   "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " +
                                   QByteArray(target) + "\r\n\r\n";
        discovery_.writeDatagram(request, QHostAddress(QStringLiteral("239.255.255.250")), 1900);
    }
    discoveryTimeout_.start(4000);
}

#ifdef P2P_MESSENGER_GATEWAY_TESTING
void GatewayMapper::startWithGatewayForTest(quint16 port, const QString& localIp,
                                            const QUrl& location)
{
    ++generation_;
    internalPort_ = port;
    externalPort_ = port;
    mappingAttempt_ = 0;
    localIp_ = localIp;
    publicIp_.clear();
    serviceType_.clear();
    controlUrl_.clear();
    setEndpoint({});
    busy_ = true;
    readDescription(location);
}
#endif

void GatewayMapper::readDiscovery()
{
    while (discovery_.hasPendingDatagrams()) {
        QByteArray response;
        response.resize(qMin<qint64>(discovery_.pendingDatagramSize(), 8192));
        QHostAddress sender;
        discovery_.readDatagram(response.data(), response.size(), &sender);
        if (!controlUrl_.isEmpty() || !localGatewayAddress(sender)) continue;
        QUrl location;
        for (const auto& line : response.split('\n')) {
            const auto trimmed = line.trimmed();
            if (trimmed.left(9).compare("location:", Qt::CaseInsensitive) == 0)
                location = QUrl(QString::fromUtf8(trimmed.mid(9)).trimmed(), QUrl::StrictMode);
        }
        QHostAddress urlHost;
        if (!location.isValid() || location.scheme() != QLatin1String("http") ||
            !urlHost.setAddress(location.host()) || urlHost != sender ||
            !location.userInfo().isEmpty()) continue;
        for (const auto& interface : QNetworkInterface::allInterfaces()) {
            if (!(interface.flags() & QNetworkInterface::IsUp)) continue;
            for (const auto& entry : interface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol &&
                    sender.isInSubnet(entry.ip(), entry.prefixLength())) {
                    localIp_ = entry.ip().toString();
                    break;
                }
            }
            if (!localIp_.isEmpty()) break;
        }
        if (localIp_.isEmpty()) continue;
        discoveryTimeout_.stop();
        discovery_.close();
        readDescription(location);
        break;
    }
}

void GatewayMapper::readDescription(const QUrl& location)
{
    const int generation = generation_;
    QNetworkRequest request(location);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(4000);
    auto* reply = http_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, location, generation] {
        const auto data = reply->readAll();
        const bool okay = reply->error() == QNetworkReply::NoError && data.size() <= 524288;
        reply->deleteLater();
        if (generation != generation_) return;
        QString path;
        if (!okay || !serviceControl(data, &serviceType_, &path)) {
            fail(tr("路由器未提供可用的端口映射功能。"));
            return;
        }
        controlUrl_ = location.resolved(QUrl(path));
        if (controlUrl_.scheme() != QLatin1String("http") ||
            controlUrl_.host() != location.host() || !controlUrl_.userInfo().isEmpty()) {
            fail(tr("路由器返回了不安全的控制地址，已拒绝连接。"));
            return;
        }
        requestPublicAddress();
    });
}

void GatewayMapper::postSoap(const QString& action, const QString& body,
                             std::function<void(bool, const QByteArray&)> completion)
{
    const int generation = generation_;
    QNetworkRequest request(controlUrl_);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=utf-8"));
    request.setRawHeader("SOAPAction", QStringLiteral("\"%1#%2\"").arg(serviceType_, action).toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(5000);
    const auto envelope = QStringLiteral(
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%1 xmlns:u=\"%2\">%3</u:%1></s:Body></s:Envelope>")
                              .arg(action, serviceType_, body).toUtf8();
    auto* reply = http_.post(request, envelope);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation, completion = std::move(completion)] {
                const auto data = reply->readAll();
                const bool okay = reply->error() == QNetworkReply::NoError && data.size() <= 65536;
                reply->deleteLater();
                if (generation == generation_) completion(okay, data);
            });
}

void GatewayMapper::requestPublicAddress()
{
    postSoap(QStringLiteral("GetExternalIPAddress"), {}, [this](bool okay, const QByteArray& data) {
        QHostAddress address;
        const auto ip = xmlValue(data, QStringLiteral("NewExternalIPAddress"));
        if (!okay || !address.setAddress(ip)) {
            fail(tr("路由器没有提供公网地址。"));
            return;
        }
        if (!publicIPv4(address)) {
            fail(tr("路由器位于运营商内网，无法自动获得可直连的公网 IPv4。"));
            return;
        }
        publicIp_ = address.toString();
        requestMapping();
    });
}

void GatewayMapper::requestMapping()
{
    if (controlUrl_.isEmpty() || localIp_.isEmpty() || publicIp_.isEmpty()) return;
    const auto body = QStringLiteral(
        "<NewRemoteHost></NewRemoteHost><NewExternalPort>%1</NewExternalPort>"
        "<NewProtocol>UDP</NewProtocol><NewInternalPort>%2</NewInternalPort>"
        "<NewInternalClient>%3</NewInternalClient><NewEnabled>1</NewEnabled>"
        "<NewPortMappingDescription>ShuangDianLiao</NewPortMappingDescription>"
        "<NewLeaseDuration>%4</NewLeaseDuration>")
                          .arg(externalPort_).arg(internalPort_).arg(localIp_).arg(3600);
    postSoap(QStringLiteral("AddPortMapping"), body,
             [this](bool okay, const QByteArray&) {
                 if (!okay) {
                     ++mappingAttempt_;
                     if (mappingAttempt_ == 1)
                         externalPort_ = QRandomGenerator::global()->bounded(40000, 60000);
                     if (mappingAttempt_ < 2) requestMapping();
                     else fail(tr("路由器拒绝自动开放直连端口。可检查路由器的 UPnP 设置。"));
                     return;
                 }
                 busy_ = false;
                 setEndpoint(QStringLiteral("%1:%2").arg(publicIp_).arg(externalPort_));
                 emit statusChanged(tr("已自动配置跨网直连；仅使用本地路由器，不经过中继。"));
                 renewal_.start(25 * 60 * 1000);
             });
}

void GatewayMapper::fail(const QString& reason)
{
    busy_ = false;
    discoveryTimeout_.stop();
    discovery_.close();
    renewal_.stop();
    setEndpoint({});
    emit statusChanged(reason);
}

void GatewayMapper::setEndpoint(const QString& endpoint)
{
    if (endpoint_ == endpoint) return;
    endpoint_ = endpoint;
    emit endpointChanged(endpoint_);
}
