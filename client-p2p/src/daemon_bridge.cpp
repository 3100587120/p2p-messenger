#include "daemon_bridge.h"

#include <QDir>
#include <QStandardPaths>

#ifdef P2P_MESSENGER_WITH_DAEMON
#include <QMetaObject>
#include <jami/configurationmanager_interface.h>
#include <jami/conversation_interface.h>
#include <jami/datatransfer_interface.h>
#include <jami/jami.h>
#include <gnutls/gnutls.h>
#endif

bool DaemonBridge::start()
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (started_)
        return true;
    const auto overrideRoot = qEnvironmentVariable("P2P_MESSENGER_DATA_ROOT");
    const auto appData = overrideRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : overrideRoot;
    const auto dataHome = QDir(appData).filePath(QStringLiteral("engine-data"));
    const auto configHome = QDir(appData).filePath(QStringLiteral("engine-config"));
    if (!QDir().mkpath(dataHome) || !QDir().mkpath(configHome))
        return false;
    qputenv("JAMI_DATA_HOME", QDir::toNativeSeparators(dataHome).toUtf8());
    qputenv("JAMI_CONFIG_HOME", QDir::toNativeSeparators(configHome).toUtf8());
    if (gnutls_global_init() != GNUTLS_E_SUCCESS)
        return false;
    gnutlsReady_ = true;
    started_ = DRing::init(static_cast<DRing::InitFlag>(0));
    if (started_) {
        // Existing accounts may have been created by another Jami client. Clear
        // all inherited endpoints before the engine registers any account.
        for (const auto& id : DRing::getAccountList()) {
            auto details = DRing::getAccountDetails(id);
            if (details["Account.type"] != "RING") continue;
            details["Account.hostname"] = "";
            details["Account.bootstrapListUrl"] = "";
            details["Account.dhtProxyListUrl"] = "";
            details["Account.proxyEnabled"] = "false";
            details["Account.proxyServer"] = "";
            details["STUN.enable"] = "false";
            details["STUN.server"] = "";
            details["TURN.enable"] = "false";
            details["TURN.server"] = "";
            details["Account.upnpEnabled"] = "false";
            details["RingNS.uri"] = "";
            DRing::setAccountDetails(id, details);
        }
        DRing::registerSignalHandlers({DRing::exportable_callback<DRing::ConversationSignal::MessageReceived>(
            [this](const std::string& accountId, const std::string& conversationId,
                   std::map<std::string, std::string> message) {
                const auto author = message.find("author");
                const auto id = message.find("id");
                const auto account = QString::fromStdString(accountId);
                const auto sender = author == message.end() ? QString {} : QString::fromStdString(author->second);
                const auto interaction = id == message.end() ? QString {} : QString::fromStdString(id->second);
                const auto type = message.find("type");
                if (type != message.end() && type->second == "application/data-transfer+json") {
                    const auto fileId = message.find("fileId");
                    const auto name = message.find("displayName");
                    if (id == message.end() || fileId == message.end() || name == message.end()) return;
                    QMetaObject::invokeMethod(this,
                        [this, account, sender, conversation = QString::fromStdString(conversationId),
                         interaction,
                         file = QString::fromStdString(fileId->second),
                         display = QString::fromStdString(name->second)] {
                            if (sender != inviteCode(account))
                                emit incomingFile(conversation, interaction, file, display);
                        }, Qt::QueuedConnection);
                    return;
                }
                const auto body = message.find("body");
                if (body == message.end()) return;
                QMetaObject::invokeMethod(this,
                    [this, account, sender, interaction, conversation = QString::fromStdString(conversationId),
                     text = QString::fromStdString(body->second)] {
                        emit incomingMessage(conversation, text, interaction,
                                             sender == inviteCode(account));
                    },
                    Qt::QueuedConnection);
            }),
            DRing::exportable_callback<DRing::ConfigurationSignal::AccountDetailsChanged>(
                [this](const std::string& accountId,
                       const std::map<std::string, std::string>& details) {
                    const auto user = details.find("Account.username");
                    if (user == details.end() || user->second.empty()) return;
                    QMetaObject::invokeMethod(this,
                        [this, id = QString::fromStdString(accountId),
                         code = QString::fromStdString(user->second)] {
                            emit identityChanged(id, code);
                        }, Qt::QueuedConnection);
                }),
            DRing::exportable_callback<DRing::ConfigurationSignal::IncomingTrustRequest>(
                [this](const std::string& accountId, const std::string& from,
                       const std::string& conversationId, const std::vector<uint8_t>&, time_t) {
                    QMetaObject::invokeMethod(this,
                        [this, id = QString::fromStdString(accountId),
                         uri = QString::fromStdString(from),
                         conversation = QString::fromStdString(conversationId)] {
                            directRequestIds_.insert(conversation);
                            emit friendRequestReceived(id, uri);
                        }, Qt::QueuedConnection);
                }),
            DRing::exportable_callback<DRing::ConversationSignal::ConversationRequestReceived>(
                [this](const std::string& accountId, const std::string& conversationId,
                       std::map<std::string, std::string>) {
                    QMetaObject::invokeMethod(this,
                        [this, id = QString::fromStdString(accountId),
                         conversation = QString::fromStdString(conversationId)] {
                            if (!directRequestIds_.contains(conversation))
                                emit groupRequestReceived(id, conversation);
                        }, Qt::QueuedConnection);
                }),
            DRing::exportable_callback<DRing::ConfigurationSignal::ContactAdded>(
                [this](const std::string& accountId, const std::string& uri, bool confirmed) {
                    QMetaObject::invokeMethod(this,
                        [this, id = QString::fromStdString(accountId),
                         contact = QString::fromStdString(uri), confirmed] {
                            emit contactConfirmed(id, contact, confirmed);
                        }, Qt::QueuedConnection);
                }),
            DRing::exportable_callback<DRing::ConversationSignal::ConversationReady>(
                [this](const std::string& accountId, const std::string& conversationId) {
                    QMetaObject::invokeMethod(this,
                        [this, id = QString::fromStdString(accountId),
                         conversation = QString::fromStdString(conversationId)] {
                            emit conversationReady(id, conversation);
                        }, Qt::QueuedConnection);
                }),
            DRing::exportable_callback<DRing::DataTransferSignal::DataTransferEvent>(
                [this](const std::string& accountId, const std::string& conversationId,
                       const std::string&, const std::string& fileId, int eventCode) {
                    std::string path;
                    int64_t total = 0, progress = 0;
                    DRing::fileTransferInfo(accountId, conversationId, fileId, path, total, progress);
                    QMetaObject::invokeMethod(this,
                        [this, conversation = QString::fromStdString(conversationId),
                         file = QString::fromStdString(fileId), eventCode, progress, total] {
                            emit transferChanged(conversation, file, eventCode, progress, total);
                        }, Qt::QueuedConnection);
                })});
        started_ = DRing::start();
    }
    if (!started_) {
        DRing::unregisterSignalHandlers();
        DRing::fini();
        gnutls_global_deinit();
        gnutlsReady_ = false;
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
    if (gnutlsReady_)
        gnutls_global_deinit();
#endif
    started_ = false;
    gnutlsReady_ = false;
}

QString DaemonBridge::createLocalIdentity(const QString& displayName)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_)
        return {};
    for (const auto& id : DRing::getAccountList()) {
        const auto details = DRing::getAccountDetails(id);
        const auto type = details.find("Account.type");
        if (type != details.end() && type->second == "RING")
            return QString::fromStdString(id);
    }
    const auto id = DRing::addAccount({{"Account.type", "RING"},
                                       {"Account.alias", displayName.toStdString()},
                                       {"Account.hostname", ""},
                                       {"Account.bootstrapListUrl", ""},
                                       {"Account.dhtProxyListUrl", ""},
                                       {"Account.proxyEnabled", "false"},
                                       {"STUN.enable", "false"},
                                       {"TURN.enable", "false"},
                                       {"Account.upnpEnabled", "false"},
                                       {"RingNS.uri", ""}});
    return QString::fromStdString(id);
#else
    Q_UNUSED(displayName)
    return {};
#endif
}

QString DaemonBridge::inviteCode(const QString& accountId) const
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_ || accountId.isEmpty()) return {};
    const auto details = DRing::getAccountDetails(accountId.toStdString());
    const auto user = details.find("Account.username");
    return user == details.end() ? QString {} : QString::fromStdString(user->second);
#else
    Q_UNUSED(accountId)
    return {};
#endif
}

bool DaemonBridge::addVerifiedContact(const QString& accountId, const QString& contactUri)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_ || accountId.isEmpty() || contactUri.isEmpty())
        return false;
    DRing::sendTrustRequest(accountId.toStdString(), contactUri.toStdString());
    return !createConversation(accountId, contactUri).isEmpty();
#else
    Q_UNUSED(accountId)
    Q_UNUSED(contactUri)
    return false;
#endif
}

QString DaemonBridge::createConversation(const QString& accountId, const QString& contactUri)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_ || accountId.isEmpty() || contactUri.isEmpty()) return {};
    const auto account = accountId.toStdString();
    const auto contact = contactUri.toStdString();
    for (const auto& conversation : DRing::getConversations(account)) {
        const auto members = DRing::getConversationMembers(account, conversation);
        if (members.size() != 2) continue;
        for (const auto& member : members) {
            const auto uri = member.find("uri");
            if (uri != member.end() && uri->second == contact)
                return QString::fromStdString(conversation);
        }
    }
    return {};
#else
    Q_UNUSED(accountId)
    Q_UNUSED(contactUri)
    return {};
#endif
}

QStringList DaemonBridge::pendingFriendRequests(const QString& accountId) const
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    QStringList requests;
    if (!started_ || accountId.isEmpty()) return requests;
    for (const auto& request : DRing::getTrustRequests(accountId.toStdString())) {
        const auto from = request.find("from");
        if (from != request.end()) requests.append(QString::fromStdString(from->second));
    }
    return requests;
#else
    Q_UNUSED(accountId)
    return {};
#endif
}

QStringList DaemonBridge::pendingGroupRequests(const QString& accountId) const
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    QStringList requests;
    if (!started_ || accountId.isEmpty()) return requests;
    QSet<QString> directIds;
    for (const auto& request : DRing::getTrustRequests(accountId.toStdString())) {
        const auto id = request.find("conversationId");
        if (id != request.end()) directIds.insert(QString::fromStdString(id->second));
    }
    for (const auto& request : DRing::getConversationRequests(accountId.toStdString())) {
        const auto id = request.find("id");
        if (id == request.end()) continue;
        const auto conversation = QString::fromStdString(id->second);
        if (!directIds.contains(conversation)) requests.append(conversation);
    }
    return requests;
#else
    Q_UNUSED(accountId)
    return {};
#endif
}

QString DaemonBridge::groupRequestTitle(const QString& accountId, const QString& conversationId) const
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    for (const auto& request : DRing::getConversationRequests(accountId.toStdString())) {
        const auto id = request.find("id");
        if (id == request.end() || id->second != conversationId.toStdString()) continue;
        const auto title = request.find("title");
        if (title != request.end()) return QString::fromStdString(title->second);
    }
#else
    Q_UNUSED(accountId)
    Q_UNUSED(conversationId)
#endif
    return {};
}

bool DaemonBridge::acceptFriendRequest(const QString& accountId, const QString& contactUri)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    return started_ && DRing::acceptTrustRequest(accountId.toStdString(), contactUri.toStdString());
#else
    Q_UNUSED(accountId)
    Q_UNUSED(contactUri)
    return false;
#endif
}

bool DaemonBridge::acceptGroupRequest(const QString& accountId, const QString& conversationId)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_ || accountId.isEmpty() || conversationId.isEmpty()) return false;
    DRing::acceptConversationRequest(accountId.toStdString(), conversationId.toStdString());
    return true;
#else
    Q_UNUSED(accountId)
    Q_UNUSED(conversationId)
    return false;
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

void DaemonBridge::setConversationTitle(const QString& accountId, const QString& conversationId,
                                        const QString& title)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (started_ && !accountId.isEmpty() && !conversationId.isEmpty())
        DRing::updateConversationInfos(accountId.toStdString(), conversationId.toStdString(),
                                       {{"title", title.toStdString()}});
#else
    Q_UNUSED(accountId)
    Q_UNUSED(conversationId)
    Q_UNUSED(title)
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

bool DaemonBridge::downloadFile(const QString& accountId, const QString& conversationId,
                                const QString& interactionId, const QString& fileId, const QString& path)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    if (!started_ || accountId.isEmpty() || conversationId.isEmpty() ||
        interactionId.isEmpty() || fileId.isEmpty() || path.isEmpty()) return false;
    return DRing::downloadFile(accountId.toStdString(), conversationId.toStdString(),
                               interactionId.toStdString(), fileId.toStdString(), path.toStdString());
#else
    Q_UNUSED(accountId)
    Q_UNUSED(conversationId)
    Q_UNUSED(interactionId)
    Q_UNUSED(fileId)
    Q_UNUSED(path)
    return false;
#endif
}

bool DaemonBridge::configurePrivateNetwork(const QString& accountId, const PrivateNetworkConfig& config)
{
#ifdef P2P_MESSENGER_WITH_DAEMON
    QString reason;
    if (!started_ || !config.isValid(&reason) || accountId.isEmpty()) return false;
    auto details = DRing::getAccountDetails(accountId.toStdString());
    details["Account.hostname"] = config.bootstrapNode.toStdString();
    details["Account.bootstrapListUrl"] = "";
    details["Account.dhtProxyListUrl"] = "";
    details["Account.proxyEnabled"] = "false";
    details["Account.proxyServer"] = "";
    details["STUN.enable"] = "false";
    details["STUN.server"] = "";
    details["Account.upnpEnabled"] = "false";
    details["RingNS.uri"] = "";
    details["TURN.enable"] = config.turnHost.isEmpty() ? "false" : "true";
    details["TURN.server"] = config.turnHost.isEmpty()
        ? "" : QStringLiteral("%1:%2").arg(config.turnHost).arg(config.turnPort).toStdString();
    details["TURN.username"] = config.turnUser.toStdString();
    details["TURN.password"] = config.turnPassword.toStdString();
    DRing::setAccountDetails(accountId.toStdString(), details);
    return true;
#else
    Q_UNUSED(accountId); Q_UNUSED(config); return false;
#endif
}
