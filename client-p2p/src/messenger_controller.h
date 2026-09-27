#pragma once

#include <QObject>
#include <QHash>
#include <QStringList>
#include <QVariantList>

#include "daemon_bridge.h"
#include "local_vault.h"
#include "private_network_config.h"

class MessengerController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList contacts READ contacts NOTIFY contactsChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(QString activeContactId READ activeContactId NOTIFY activeContactChanged)
    Q_PROPERTY(QString activeContactName READ activeContactName NOTIFY activeContactChanged)
    Q_PROPERTY(QString networkStatus READ networkStatus NOTIFY networkStatusChanged)
    Q_PROPERTY(QString inviteCode READ inviteCode NOTIFY inviteCodeChanged)
    Q_PROPERTY(QString accountId READ accountId NOTIFY accountIdChanged)
    Q_PROPERTY(QString profileName READ profileName NOTIFY profileNameChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QStringList pendingRequests READ pendingRequests NOTIFY pendingRequestsChanged)
    Q_PROPERTY(QStringList pendingGroupRequests READ pendingGroupRequests NOTIFY pendingGroupRequestsChanged)

public:
    explicit MessengerController(QObject* parent = nullptr);
    ~MessengerController() override;

    QVariantList contacts() const;
    QVariantList messages() const;
    QString activeContactId() const;
    QString activeContactName() const;
    QString networkStatus() const;
    QString inviteCode() const;
    QString accountId() const;
    QString profileName() const;
    QString lastError() const;
    QStringList pendingRequests() const;
    QStringList pendingGroupRequests() const;

    Q_INVOKABLE void selectContact(const QString& contactId);
    Q_INVOKABLE bool addContact(const QString& name, const QString& invite);
    Q_INVOKABLE bool createGroup(const QString& name, const QStringList& memberUris);
    Q_INVOKABLE bool sendMessage(const QString& body);
    Q_INVOKABLE bool queueFile(const QString& path);
    Q_INVOKABLE bool downloadFile(const QString& interactionId, const QString& fileId,
                                  const QString& destination);
    Q_INVOKABLE void copyInviteCode();
    Q_INVOKABLE bool setProfileName(const QString& name);
    Q_INVOKABLE bool retryIdentity();
    Q_INVOKABLE bool acceptFriendRequest(const QString& contactUri);
    Q_INVOKABLE bool acceptGroupRequest(const QString& conversationId);
    Q_INVOKABLE bool configureNetwork(const QString& rendezvous, const QString& turnHost,
                                       int turnPort, const QString& user, const QString& password);

signals:
    void contactsChanged();
    void messagesChanged();
    void activeContactChanged();
    void networkStatusChanged();
    void inviteCodeChanged();
    void accountIdChanged();
    void profileNameChanged();
    void lastErrorChanged();
    void pendingRequestsChanged();
    void pendingGroupRequestsChanged();

private:
    QVariantList contacts_;
    QVariantList messages_;
    QString activeContactId_;
    QString networkStatus_;
    QString accountId_;
    QString inviteCode_;
    QString profileName_;
    QString lastError_;
    QStringList pendingRequests_;
    QStringList pendingGroupRequests_;
    PrivateNetworkConfig networkConfig_;
    DaemonBridge daemon_;
    LocalVault vault_;
    QHash<QString, QString> androidDownloadDestinations_;
    QHash<QString, QString> androidDownloadPaths_;

    void appendMessage(const QString& body, bool outgoing, const QString& kind = QStringLiteral("text"));
    void appendMessageForContact(const QString& contactId, const QString& body, bool outgoing,
                                 const QString& kind = QStringLiteral("text"));
    void storeMessageForContact(const QString& contactId, const QVariantMap& message);
    void saveProfile();
    void setError(const QString& error);
    QString contactName(const QString& id) const;
};
