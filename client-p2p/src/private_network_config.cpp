#include "private_network_config.h"

#include <QUrl>

bool PrivateNetworkConfig::isValid(QString* reason) const
{
    if (rendezvousUrl.isEmpty() && turnHost.isEmpty())
        return true;
    const QUrl rendezvous(rendezvousUrl);
    if (!rendezvousUrl.isEmpty()) {
        if (!rendezvous.isValid() || rendezvous.scheme() != QStringLiteral("wss")) {
            if (reason) *reason = QStringLiteral("信令地址必须使用自建 wss:// 地址");
            return false;
        }
    }
    if (!turnHost.isEmpty() && turnPort == 0) {
        if (reason) *reason = QStringLiteral("TURN 端口无效");
        return false;
    }
    if (!turnHost.isEmpty() && (turnUser.isEmpty() || turnPassword.isEmpty())) {
        if (reason) *reason = QStringLiteral("自建 TURN 必须同时配置用户名和密码");
        return false;
    }
    return true;
}
