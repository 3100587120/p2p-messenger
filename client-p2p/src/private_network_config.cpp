#include "private_network_config.h"

#include <QRegularExpression>

bool PrivateNetworkConfig::isValid(QString* reason) const
{
    if (bootstrapNode.isEmpty() && turnHost.isEmpty())
        return true;
    if (!bootstrapNode.isEmpty()) {
        static const QRegularExpression endpoint(
            QStringLiteral(R"(^(?:\[[0-9A-Fa-f:]+\]|[A-Za-z0-9.-]+):([0-9]{1,5})$)"));
        const auto match = endpoint.match(bootstrapNode);
        const auto port = match.hasMatch() ? match.captured(1).toInt() : 0;
        if (port < 1 || port > 65535) {
            if (reason) *reason = QStringLiteral("自建 DHT 引导节点须填写 主机:端口");
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
