#pragma once

#include <QObject>
#include <QHash>
#include <QStringList>
#include <QVariantList>
#include <QTimer>
#include <QImage>

#include "daemon_bridge.h"
#include "gateway_mapper.h"
#include "local_vault.h"
#include "private_network_config.h"
#include "relay_client.h"
#include "voice_engine.h"
#include "file_stream.h"

class QTimer;

class MessengerController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList contacts READ contacts NOTIFY contactsChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(QString activeContactId READ activeContactId NOTIFY activeContactChanged)
    Q_PROPERTY(QString activeContactName READ activeContactName NOTIFY activeContactChanged)
    Q_PROPERTY(QVariantMap activeContactDetails READ activeContactDetails NOTIFY activeContactChanged)
    Q_PROPERTY(bool activeIsGroup READ activeIsGroup NOTIFY activeContactChanged)
    Q_PROPERTY(bool playingVoice READ playingVoice NOTIFY voiceChanged)
    Q_PROPERTY(QString playingVoiceData READ playingVoiceData NOTIFY voiceChanged)
    Q_PROPERTY(QString screenshotPreview READ screenshotPreview NOTIFY screenshotChanged)
    Q_PROPERTY(QString networkStatus READ networkStatus NOTIFY networkStatusChanged)
    Q_PROPERTY(QString inviteCode READ inviteCode NOTIFY inviteCodeChanged)
    Q_PROPERTY(QString accountId READ accountId NOTIFY accountIdChanged)
    Q_PROPERTY(QString profileName READ profileName NOTIFY profileNameChanged)
    Q_PROPERTY(QString avatarUrl READ avatarUrl NOTIFY profileNameChanged)
    Q_PROPERTY(QString userCode READ userCode NOTIFY uidChanged)
    Q_PROPERTY(QString registrationStatus READ registrationStatus NOTIFY registrationStatusChanged)
    Q_PROPERTY(bool passwordConfigured READ passwordConfigured NOTIFY uidChanged)
    Q_PROPERTY(bool registrationPending READ registrationPending NOTIFY registrationStatusChanged)
    Q_PROPERTY(bool loginPending READ loginPending NOTIFY registrationStatusChanged)
    Q_PROPERTY(QString loginUid READ loginUid NOTIFY registrationStatusChanged)
    Q_PROPERTY(bool friendLookupPending READ friendLookupPending NOTIFY registrationStatusChanged)
    Q_PROPERTY(QVariantList friendRequests READ friendRequests NOTIFY pendingRequestsChanged)
    Q_PROPERTY(QString directEndpoint READ directEndpoint NOTIFY directEndpointChanged)
    Q_PROPERTY(QString pairingCode READ pairingCode NOTIFY pairingCodeChanged)
    Q_PROPERTY(int listeningPort READ listeningPort NOTIFY listeningPortChanged)
    Q_PROPERTY(QVariantList nearbyPeers READ nearbyPeers NOTIFY nearbyPeersChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QStringList pendingRequests READ pendingRequests NOTIFY pendingRequestsChanged)
    Q_PROPERTY(QStringList pendingGroupRequests READ pendingGroupRequests NOTIFY pendingGroupRequestsChanged)
    Q_PROPERTY(bool assistedConnection READ assistedConnection NOTIFY assistedConnectionChanged)
    Q_PROPERTY(QString relayEndpoint READ relayEndpoint NOTIFY relayEndpointChanged)
    Q_PROPERTY(bool customRelay READ customRelay NOTIFY relayEndpointChanged)
    Q_PROPERTY(QVariantList stickerLibrary READ stickerLibrary NOTIFY stickersChanged)
    Q_PROPERTY(QString peerProbeStatus READ peerProbeStatus NOTIFY peerProbeStatusChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY voiceChanged)
    Q_PROPERTY(QString callState READ callState NOTIFY voiceChanged)
    Q_PROPERTY(QString callPeerName READ callPeerName NOTIFY voiceChanged)

public:
    explicit MessengerController(QObject* parent = nullptr);
    ~MessengerController() override;

    QVariantList contacts() const;
    QVariantList messages() const;
    QString activeContactId() const;
    QString activeContactName() const;
    QVariantMap activeContactDetails() const;
    bool activeIsGroup() const {const auto row=activeEntry();return row.value("group").toBool() && (!row.value("groupId").toString().isEmpty() || !row.value("conversationId").toString().isEmpty());}
    bool playingVoice() const {return voice_.playing();}
    QString playingVoiceData() const {return playingVoice()?playingVoiceData_:QString();}
    QString screenshotPreview() const {return screenshotPreview_;}
    Q_INVOKABLE void captureScreenshot();
    void acceptScreenshot(const QImage& image);
    void screenshotFailed(const QString& reason){setError(reason);}
    void backgroundCallFailed(){endCall();setError(tr("后台通话服务未能启动，请开启麦克风权限并允许后台运行"));}
    Q_INVOKABLE bool sendScreenshot();
    Q_INVOKABLE void discardScreenshot(){screenshotPreview_.clear();emit screenshotChanged();}
    Q_INVOKABLE void stopVoicePlayback(){voice_.stopPlayback();}
    Q_INVOKABLE bool copyMessage(int index);
    Q_INVOKABLE QVariantMap messageSenderDetails(int index) const;
    Q_INVOKABLE bool addMessageSender(int index);
    Q_INVOKABLE bool deleteLocalMessage(int index);
    Q_INVOKABLE bool withdrawMessage(int index);
    Q_INVOKABLE bool forwardMessage(int index,const QString& targetId);
    Q_INVOKABLE bool removeActiveContact();
    Q_INVOKABLE bool setContactMuted(bool muted);
    Q_INVOKABLE bool setContactRemark(const QString& remark);
    Q_INVOKABLE bool setContactPinned(bool pinned);
    Q_INVOKABLE bool clearActiveHistory();
    Q_INVOKABLE void setThreadVisible(bool visible);
    Q_INVOKABLE bool setGroupAnnouncement(const QString& text);
    Q_INVOKABLE bool setGroupAdministrator(const QString& memberId, bool enabled);
    Q_INVOKABLE QVariantList groupMembers() const;
    Q_INVOKABLE bool canManageGroup() const;
    Q_INVOKABLE bool ownsGroup() const;
    Q_INVOKABLE bool inviteGroupMembers(const QStringList& codes);
    Q_INVOKABLE bool removeGroupMember(const QString& memberId);
    Q_INVOKABLE QVariantList searchHistory(const QString& query, bool allChats = false) const;
    Q_INVOKABLE QVariantMap droppedFileDetails(const QString& url) const;
    Q_INVOKABLE bool renameGroup(const QString& name);
    Q_INVOKABLE void chooseGroupAvatar();
    Q_INVOKABLE bool setGroupAvatar(const QString& path);
    QString networkStatus() const;
    QString inviteCode() const;
    QString accountId() const;
    QString profileName() const;
    QString avatarUrl() const;
    QString userCode() const;
    QString registrationStatus() const;
    bool passwordConfigured() const { return relay_.passwordConfigured(); }
    bool registrationPending() const { return relay_.registrationPending(); }
    bool loginPending() const {return relay_.loginPending();}
    QString loginUid() const {return relay_.loginUid();}
    bool friendLookupPending() const {return relay_.lookupPending();}
    Q_INVOKABLE void cancelLogin() {relay_.cancelLogin();}
    Q_INVOKABLE bool registerAccount(const QString& name, const QString& password);
    Q_INVOKABLE bool loginAccount(const QString& uid, const QString& password);
    QVariantList friendRequests() const;
    Q_INVOKABLE bool setAvatar(const QString& path);
    Q_INVOKABLE void chooseAvatar();
    Q_INVOKABLE void chooseAttachment(bool sticker);
    Q_INVOKABLE void choosePhoto(bool camera = false);
    Q_INVOKABLE bool sendPhoto(const QString& path);
    void suspendForBackground();
    Q_INVOKABLE void enableMessageReminders() { emit notificationPermissionRequested(); }
    Q_INVOKABLE bool setGroupNickname(const QString& name);
    Q_INVOKABLE bool addFriendByUid(const QString& uid, const QString& remark);
    Q_INVOKABLE void copyUid();
    bool recording() const { return recording_; }
    bool voicePermissionPending() const { return voice_.permissionPending(); }
    QString callState() const { return callState_; }
    QString callPeerName() const { return callPeerName_; }
    Q_PROPERTY(int callDuration READ callDuration NOTIFY voiceChanged)
    Q_PROPERTY(int callParticipants READ callParticipants NOTIFY voiceChanged)
    int callDuration() const;
    int callParticipants() const;
    Q_INVOKABLE bool sendSticker(const QString& path);
    QVariantList stickerLibrary() const { return stickers_; }
    Q_INVOKABLE bool sendSavedSticker(int index);
    Q_INVOKABLE void removeSticker(int index);
    bool customRelay() const;
    Q_INVOKABLE bool restoreDefaultRelay();
    Q_INVOKABLE void recordVoice();
    Q_INVOKABLE void finishVoice(bool send);
    Q_INVOKABLE void playVoice(const QString& data);
    Q_INVOKABLE void startCall();
    Q_INVOKABLE void answerCall();
    Q_INVOKABLE void endCall();
    QString directEndpoint() const;
    QString pairingCode() const;
    int listeningPort() const;
    QVariantList nearbyPeers() const;
    QString lastError() const;
    QStringList pendingRequests() const;
    QStringList pendingGroupRequests() const;
    bool assistedConnection() const;
    QString relayEndpoint() const;
    QString peerProbeStatus() const;

    Q_INVOKABLE void selectContact(const QString& contactId);
    Q_INVOKABLE bool addContact(const QString& name, const QString& invite);
    Q_INVOKABLE bool createGroup(const QString& name, const QStringList& memberUris);
    Q_INVOKABLE bool sendMessage(const QString& body);
    Q_INVOKABLE bool queueFile(const QString& path);
    Q_INVOKABLE bool downloadFile(const QString& interactionId, const QString& fileId,
                                  const QString& destination);
    Q_INVOKABLE void copyInviteCode();
    Q_INVOKABLE void copyPairingCode();
    Q_INVOKABLE bool copyLocalPairingCode();
    Q_INVOKABLE void refreshNearbyPeers();
    Q_INVOKABLE bool addNearbyPeer(const QString& peerUri);
    Q_INVOKABLE bool setDirectEndpoint(const QString& endpoint);
    Q_INVOKABLE bool useLocalNetworkAddress();
    Q_INVOKABLE bool setProfileName(const QString& name);
    Q_INVOKABLE bool retryIdentity();
    Q_INVOKABLE bool acceptFriendRequest(const QString& contactUri);
    Q_INVOKABLE void refreshPendingRequests();
    Q_INVOKABLE bool acceptGroupRequest(const QString& conversationId);
    Q_INVOKABLE bool configureNetwork(const QString& rendezvous);
    Q_INVOKABLE bool setAssistedConnection(bool enabled);
    Q_INVOKABLE bool setRelayEndpoint(const QString& endpoint);
    Q_INVOKABLE bool testPeerConnection(const QString& invite);

signals:
    void screenshotSelectionRequested();
    void operationCompleted(const QString& title,const QString& message);
    void screenshotChanged();
    void groupAvatarPickerRequested();
    void photoPickerRequested();
    void notificationPermissionRequested();
    void incomingNotice(const QString& title, const QString& text);
    void avatarPickerRequested();
    void attachmentPickerRequested(bool sticker);
    void registrationStatusChanged();
    void uidChanged();
    void voiceChanged();
    void stickersChanged();
    void contactsChanged();
    void messagesChanged();
    void activeContactChanged();
    void networkStatusChanged();
    void inviteCodeChanged();
    void accountIdChanged();
    void profileNameChanged();
    void directEndpointChanged();
    void pairingCodeChanged();
    void listeningPortChanged();
    void nearbyPeersChanged();
    void lastErrorChanged();
    void pendingRequestsChanged();
    void pendingGroupRequestsChanged();
    void assistedConnectionChanged();
    void relayEndpointChanged();
    void peerProbeStatusChanged();

private:
    friend class CallProtocolRegression;
    QVariantList contacts_;
    QVariantList stickers_;
    QVariantList messages_;
    QString activeContactId_;
    QString networkStatus_;
    QString accountId_;
    QString inviteCode_;
    QString profileName_;
    QString profileAvatar_;
    QString uid_, uidRemark_;
    QString directEndpoint_;
    QString mappedEndpoint_;
    int listeningPort_ {0};
    QTimer* identityRefresh_ {nullptr};
    QTimer* pendingRefresh_ {nullptr};
    QVariantList nearbyPeers_;
    QString lastError_;
    QStringList pendingRequests_;
    QStringList pendingGroupRequests_;
    PrivateNetworkConfig networkConfig_;
    bool assistedConnection_ {true};
    QString relayEndpoint_;
    QString peerProbeStatus_;
    QString peerProbePacketId_;
    QString selfTestPacketId_;
    bool selfTestPassed_ {false};
    bool profileReadable_ {true};
    QVariantMap pendingRelayRequests_;
    QVariantMap pendingRelayProfiles_;
    QVariantMap pendingRelayGroups_;
    QVariantMap incomingRelayFiles_;
    QStringList leftGroups_;
    QStringList appliedFriendRepairs_;
    QVariantMap removedGroupRevisions_;
    DaemonBridge daemon_;
    GatewayMapper gatewayMapper_;
    LocalVault vault_;
    RelayClient relay_;
    FileStream files_;
    VoiceEngine voice_;
    QString playingVoiceData_;
    QString screenshotPreview_;
    QTimer callDeadline_;
    QTimer callHeartbeat_;
    QString callPing_;
    bool recording_ {false};
    bool threadVisible_ {false};
    qint64 callStartedAt_ {0};
    QString callContactId_;
    QString callGroupId_;
    struct CallParticipant {QByteArray key;bool accepted{false};qint64 lastSeen{0};int sequence{-1};QList<QByteArray> frames;};
    QMap<QString,CallParticipant> callParticipants_;
    QTimer callMix_;
    QString recordingContact_, callState_ {QStringLiteral("idle")}, callPeerName_, callId_, callPeerId_;
    QByteArray callKey_;
    int callSequence_ {0}, receivedSequence_ {-1};
    QVariantMap activeEntry() const;
    bool sendMedia(const QByteArray& data, const QString& kind, const QString& contactId,const QString& fileName = {});
    void receiveCall(const QString& sender, const QByteArray& key, const QJsonObject& message);
    bool sendCallEvent(QJsonObject event,bool acceptedOnly=false);
    void broadcastProfile();
    void broadcastGroupProfile();
    void broadcastGroupMemberProfile(const QVariantMap& group);
    void broadcastGroupEvent(const QVariantMap& group, QJsonObject event);
    bool changeGroupMembers(const QString& groupId,const QStringList& additions,const QString& removal);
    QHash<QString, QString> androidDownloadDestinations_;
    QHash<QString, QString> androidDownloadPaths_;

    void appendMessage(const QString& body, bool outgoing, const QString& kind = QStringLiteral("text"));
    void appendMessageForContact(const QString& contactId, const QString& body, bool outgoing,
                                 const QString& kind = QStringLiteral("text"));
    bool storeMessageForContact(const QString& contactId, const QVariantMap& message);
    void updateMessageDelivery(const QString& packetId,const QString& state);
    bool saveProfile();
    void setError(const QString& error);
    QString contactName(const QString& id) const;
};
