#pragma once

#include <QString>

struct PrivateNetworkConfig
{
    QString rendezvousUrl;
    QString turnHost;
    quint16 turnPort {3478};
    QString turnUser;
    QString turnPassword;

    bool isEmpty() const { return rendezvousUrl.isEmpty() && turnHost.isEmpty(); }
    bool isValid(QString* reason = nullptr) const;
};
