#include "private_network_config.h"

#include <QRegularExpression>

bool PrivateNetworkConfig::isValid(QString* reason) const
{
    if (bootstrapNode.isEmpty() && turnHost.isEmpty())
        return true;
    if (!bootstrapNode.isEmpty()) {
        static const QRegularExpression endpoint(
            QStringLiteral(R"(^(?:\[[0-9A-Fa-f:]+\]|[A-Za-z0-9.-]+):([0-9]{1,5})$)"));
        for (const auto& node : bootstrapNode.split(QLatin1Char(';'))) {
            const auto match = endpoint.match(node);
            const auto port = match.hasMatch() ? match.captured(1).toInt() : 0;
            if (port < 1 || port > 65535) {
                if (reason) *reason = QStringLiteral("直连入口须填写 IP:端口");
                return false;
            }
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
