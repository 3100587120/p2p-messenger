#pragma once

#include "relay_crypto.h"

#include <QObject>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QWebSocket>

class LocalVault;

class RelayClient final : public QObject
{
    Q_OBJECT
public:
    explicit RelayClient(LocalVault& vault, QObject* parent = nullptr);
    bool isReady() const;
    bool isConnected() const;
    bool hasEndpoint() const;
    QString identityId() const;
    QString inviteCode() const;
    static QByteArray publicKeyFromCode(const QString& code);
    static QString idForPublicKey(const QByteArray& key);
    void setEndpoint(const QUrl& endpoint);
    void setEnabled(bool enabled);
    void refreshConnection();
    QString send(const QByteArray& recipientPublicKey, const QJsonObject& message);
    QStringList sendBatch(const QByteArray& recipientPublicKey, const QList<QJsonObject>& messages);
    void rejectCurrentPacket();

signals:
    void connectedChanged(bool connected);
    void packetReceived(const QString& senderId, const QByteArray& senderPublicKey,
                        const QJsonObject& message);
    void deliveryState(const QString& packetId, const QString& state);
    void errorOccurred(const QString& message);

private:
    struct Outgoing {
        QString to;
        QByteArray envelope;
    };
    LocalVault& vault_;
    RelayCrypto crypto_;
    QWebSocket socket_;
    QTimer reconnect_;
    QTimer retryOutbox_;
    QTimer failureNotice_;
    QTimer heartbeat_;
    QTimer pongDeadline_;
    QTimer connectDeadline_;
    QString pendingError_;
    bool failureReported_ {false};
    QUrl endpoint_;
    bool enabled_ {false};
    bool connected_ {false};
    QMap<QString, Outgoing> outbox_;
    QSet<QString> offlineRecipients_;
    QSet<QString> seen_;
    bool currentPacketAccepted_ {true};

    void connectNow();
    void sendFrame(const QString& id, const Outgoing& outgoing);
    void resendOutbox();
    void onFrame(const QString& text);
    bool persistOutbox();
    bool persistSeen();
    void sendAck(const QByteArray& recipientPublicKey, const QString& originalId);
};
