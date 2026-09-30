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
    ~RelayClient() override;
    bool isReady() const;
    bool isConnected() const;
    bool hasPendingPacket(const QString& id) const {return outbox_.contains(id);}
    bool canSendBulk() const;
    bool registrationDeferred() const { return ownerRegistrationNotice_; }
    bool hasEndpoint() const;
    QString identityId() const;
    QString inviteCode() const;
    static QByteArray publicKeyFromCode(const QString& code);
    static QString idForPublicKey(const QByteArray& key);
    void setEndpoint(const QUrl& endpoint);
    void setEnabled(bool enabled);
    void refreshConnection();
    QString send(const QByteArray& recipientPublicKey, const QJsonObject& message);
    bool sendLive(const QByteArray& recipientPublicKey, const QJsonObject& message);
    void enableDirectory(bool enabled);
    bool lookupUid(const QString& uid);
    bool lookupPending() const {return !directoryQueries_.isEmpty();}
    bool cancelContactPackets(const QString& peerId,const QString& groupId);
    bool registerPasswordAccount(const QString& password, const QString& name);
    void setContactBackup(const QVariantMap& profile);
    bool loginPasswordAccount(const QString& uid, const QString& password);
    bool loginPending() const { return loginRequested_ || backupRestoring_; }
    QString loginUid() const {return loginUid_;}
    void cancelLogin();
    bool passwordConfigured() const { return passwordConfigured_; }
    bool registrationPending() const { return registrationPreparing_ || !registrationAuth_.isEmpty(); }
    QStringList sendBatch(const QByteArray& recipientPublicKey, const QList<QJsonObject>& messages);
    void rejectCurrentPacket();

signals:
    void loginStateChanged();
    void connectedChanged(bool connected);
    void uidAssigned(const QString& uid);
    void registrationDeferredChanged();
    void peerUnavailable(const QString& identity);
    void passwordAccountRestored(const QString& uid, const QString& name);
    void contactBackupRestored(const QVariantMap& profile);
    void friendRepairReceived(const QString& id,const QVariantMap& repair);
    void uidResolved(const QString& uid, const QString& code, const QString& name);
    void packetReceived(const QString& senderId, const QByteArray& senderPublicKey,
                        const QJsonObject& message);
    void deliveryState(const QString& packetId, const QString& state);
    void errorOccurred(const QString& message);

private:
    struct Outgoing {
        QString to;
        QByteArray envelope;
        QString scope;
        bool bulk{false};
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
    bool directoryEnabled_ {false};
    bool ownerRegistrationNotice_ {false};
    bool passwordConfigured_ {false};
    QJsonObject registrationAuth_;
    QString loginUid_, loginPassword_, loginRequest_;
    bool loginRequested_ {false};
    bool loginCryptoBusy_ {false};
    bool loginTokenReady_ {false};
    bool registrationPreparing_ {false};
    quint64 registrationGeneration_ {0};
    QString backupToken_, pendingBackupToken_, backupRequest_, backupWrite_;
    QString backupRestoreUid_,backupRestoreName_;
    QJsonObject backupSnapshot_;
    QByteArray backupPlain_;
    bool backupDirty_{false}, backupRestoring_{false};
    bool backupAllowed_{true};
    QTimer backupTimer_;
    void syncContactBackup();
    void finishContactRestore();
    QString directoryRegistration_;
    QString friendRepairQuery_;
    QMap<QString, QString> directoryQueries_;
    QTimer directoryDeadline_;
    QMap<QString, Outgoing> outbox_;
    QMap<QString, QString> liveRecipients_;
    QSet<QString> offlineRecipients_;
    QSet<QString> watchedRecipients_;
    QHash<QString,qint64> lastSent_;
    qint64 liveTrafficUntil_{0};
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
