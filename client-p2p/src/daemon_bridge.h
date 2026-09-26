#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include "private_network_config.h"

// The only client-to-engine boundary.  Keeping the Jami API behind this small
// adapter prevents upstream UI code or network defaults from leaking into the
// product interface.
class DaemonBridge final : public QObject
{
    Q_OBJECT
public:
    explicit DaemonBridge(QObject* parent = nullptr) : QObject(parent) {}
    bool start();
    void stop();

    QString createLocalIdentity(const QString& displayName);
    QString inviteCode(const QString& accountId) const;
    bool addVerifiedContact(const QString& accountId, const QString& contactUri);
    QString createEmptyConversation(const QString& accountId);
    QString createConversation(const QString& accountId, const QString& contactUri);
    QStringList pendingFriendRequests(const QString& accountId) const;
    QStringList pendingGroupRequests(const QString& accountId) const;
    QString groupRequestTitle(const QString& accountId, const QString& conversationId) const;
    bool acceptFriendRequest(const QString& accountId, const QString& contactUri);
    bool acceptGroupRequest(const QString& accountId, const QString& conversationId);
    bool addGroupMember(const QString& accountId, const QString& conversationId, const QString& contactUri);
    void setConversationTitle(const QString& accountId, const QString& conversationId,
                              const QString& title);
    bool sendText(const QString& accountId, const QString& conversationId, const QString& text);
    bool sendFile(const QString& accountId, const QString& conversationId, const QString& path);
    bool downloadFile(const QString& accountId, const QString& conversationId,
                      const QString& interactionId, const QString& fileId, const QString& path);
    bool configurePrivateNetwork(const QString& accountId, const PrivateNetworkConfig& config);

signals:
    void incomingMessage(const QString& conversationId, const QString& body,
                         const QString& interactionId, bool outgoing);
    void identityChanged(const QString& accountId, const QString& inviteCode);
    void friendRequestReceived(const QString& accountId, const QString& contactUri);
    void groupRequestReceived(const QString& accountId, const QString& conversationId);
    void contactConfirmed(const QString& accountId, const QString& contactUri, bool confirmed);
    void conversationReady(const QString& accountId, const QString& conversationId);
    void incomingFile(const QString& conversationId, const QString& interactionId,
                      const QString& fileId, const QString& name);
    void transferChanged(const QString& conversationId, const QString& fileId,
                         int eventCode, qint64 progress, qint64 total);

private:
    bool started_ {false};
    bool gnutlsReady_ {false};
    QSet<QString> directRequestIds_;
};
