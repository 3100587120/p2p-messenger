#pragma once

#include <QString>

// The only client-to-engine boundary.  Keeping the Jami API behind this small
// adapter prevents upstream UI code or network defaults from leaking into the
// product interface.
class DaemonBridge final
{
public:
    bool start();
    void stop();

    QString createLocalIdentity(const QString& displayName);
    bool addVerifiedContact(const QString& accountId, const QString& contactUri);
    QString createEmptyConversation(const QString& accountId);
    QString createConversation(const QString& accountId, const QString& contactUri);
    bool addGroupMember(const QString& accountId, const QString& conversationId, const QString& contactUri);
    bool sendText(const QString& accountId, const QString& conversationId, const QString& text);
    bool sendFile(const QString& accountId, const QString& conversationId, const QString& path);

private:
    bool started_ {false};
};
