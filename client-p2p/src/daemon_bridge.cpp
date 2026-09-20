#include "daemon_bridge.h"

#ifdef P2P_MESSENGER_WITH_DAEMON
#include <QMetaObject>
#include <jami/configurationmanager_interface.h>
#include <jami/conversation_interface.h>
#include <jami/datatransfer_interface.h>
#include <jami/jami.h>
#endif

bool DaemonBridge::start()
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (started_)
        return true;
    started_ = DRing::init(static_cast<DRing::InitFlag>(0));
    if (started_) {
        DRing::registerSignalHandlers({DRing::exportable_callback<DRing::ConversationSignal::MessageReceived>(
            [this](const std::string&, const std::string& conversationId,
                   std::map<std::string, std::string> message) {
                const auto body = message.find("body");
                if (body == message.end()) return;
                QMetaObject::invokeMethod(this,
                    [this, conversation = QString::fromStdString(conversationId),
                     text = QString::fromStdString(body->second)] { emit incomingMessage(conversation, text); },
                    Qt::QueuedConnection);
            })});
        started_ = DRing::start();
    }
#endif
    return started_;
}

void DaemonBridge::stop()
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (started_) {
        DRing::unregisterSignalHandlers();
        DRing::fini();
    }
#endif
    started_ = false;
}

QString DaemonBridge::createLocalIdentity(const QString& displayName)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_)
        return {};
    const auto id = DRing::addAccount({{"Account.type", "RING"},
                                       {"Account.alias", displayName.toStdString()}});
    return QString::fromStdString(id);
#else
    Q_UNUSED(displayName)
    return {};
#endif
}

bool DaemonBridge::addVerifiedContact(const QString& accountId, const QString& contactUri)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_ || accountId.isEmpty() || contactUri.isEmpty())
        return false;
    DRing::addContact(accountId.toStdString(), contactUri.toStdString());
    return true;
#else
    Q_UNUSED(accountId)
    Q_UNUSED(contactUri)
    return false;
#endif
}

QString DaemonBridge::createConversation(const QString& accountId, const QString& contactUri)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    const auto conversation = createEmptyConversation(accountId);
    if (conversation.isEmpty())
        return {};
    DRing::addConversationMember(accountId.toStdString(), conversation.toStdString(), contactUri.toStdString());
    return conversation;
#else
    Q_UNUSED(accountId)
    Q_UNUSED(contactUri)
    return {};
#endif
}

QString DaemonBridge::createEmptyConversation(const QString& accountId)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_ || accountId.isEmpty())
        return {};
    const auto conversation = DRing::startConversation(accountId.toStdString());
    return QString::fromStdString(conversation);
#else
    Q_UNUSED(accountId)
    return {};
#endif
}

bool DaemonBridge::addGroupMember(const QString& accountId, const QString& conversationId, const QString& contactUri)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_)
        return false;
    DRing::addConversationMember(accountId.toStdString(), conversationId.toStdString(), contactUri.toStdString());
    return true;
#else
    Q_UNUSED(accountId)
    Q_UNUSED(conversationId)
    Q_UNUSED(contactUri)
    return false;
#endif
}

bool DaemonBridge::sendText(const QString& accountId, const QString& conversationId, const QString& text)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_ || text.isEmpty())
        return false;
    DRing::sendMessage(accountId.toStdString(), conversationId.toStdString(), text.toStdString(), {});
    return true;
#else
    Q_UNUSED(accountId)
    Q_UNUSED(conversationId)
    Q_UNUSED(text)
    return false;
#endif
}

bool DaemonBridge::sendFile(const QString& accountId, const QString& conversationId, const QString& path)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_ || path.isEmpty())
        return false;
    DRing::sendFile(accountId.toStdString(), conversationId.toStdString(), path.toStdString(), {}, {});
    return true;
#else
    Q_UNUSED(accountId)
    Q_UNUSED(conversationId)
    Q_UNUSED(path)
    return false;
#endif
}

bool DaemonBridge::configurePrivateNetwork(const QString& accountId, const PrivateNetworkConfig& config)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    QString reason;
    if (!started_ || !config.isValid(&reason) || accountId.isEmpty()) return false;
    DRing::setAccountDetails(accountId.toStdString(), {{"TURN.enable", config.turnHost.isEmpty() ? "false" : "true"},
                                                       {"TURN.server", config.turnHost.toStdString()},
                                                       {"TURN.username", config.turnUser.toStdString()},
                                                       {"TURN.password", config.turnPassword.toStdString()}});
    return true;
#else
    Q_UNUSED(accountId); Q_UNUSED(config); return false;
#endif
}
