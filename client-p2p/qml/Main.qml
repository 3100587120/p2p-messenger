import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: Qt.platform.os === "android" ? 390 : 1100
    height: Qt.platform.os === "android" ? 780 : 740
    minimumWidth: 320; minimumHeight: 480
    visible: true; title: "双点聊"; color: "#f3f6fb"
    Material.theme: Material.Light; Material.accent: "#1685ef"
    property bool mobile: Qt.platform.os === "android" || width < 740
    property bool showThread: false
    property color accent: "#1685ef"
    property color subdued: "#758398"
    property string accountError: ""
    component Action: Button {
        implicitHeight: 38; leftPadding: 10; rightPadding: 10
        background: Rectangle { color: parent.down ? "#d8eafe" : "#edf5ff"; radius: 9 }
        contentItem: Text { text: parent.text; color: parent.enabled ? window.accent : "#9ca9bb"; font.pixelSize: 14; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
    }
    component Sheet: Dialog {
        modal: true; anchors.centerIn: parent
        width: Math.min(430, window.width - 24); padding: 16
        background: Rectangle { color: "white"; radius: 16; border.color: "#dde6f2" }
    }
    Component.onCompleted: Qt.callLater(function() { if (!messenger.profileName.length) account.open(); if (messenger.lastError.length) failure.open() })
    Connections {
        target: messenger
        function onLastErrorChanged() { if (messenger.lastError.length) { window.accountError = ""; failure.open() } }
        function onPendingRequestsChanged() { if (messenger.pendingRequests.length) requests.open() }
        function onVoiceChanged() { if (messenger.callState !== "idle") call.open(); else call.close() }
    }
    Connections { target: typeof accountManager !== "undefined" ? accountManager : null; function onErrorOccurred(message) { window.accountError = message; failure.open() } }
    Sheet { id: failure; title: "操作未完成"; standardButtons: Dialog.Ok; contentItem: Label { text: window.accountError.length ? window.accountError : messenger.lastError; wrapMode: Text.Wrap; color: "#b73545" } }
    Sheet {
        id: account; title: messenger.profileName.length ? "我的账号" : "创建账号"
        closePolicy: messenger.profileName.length ? Popup.CloseOnEscape : Popup.NoAutoClose
        standardButtons: messenger.profileName.length ? Dialog.Close : Dialog.NoButton
        contentItem: ScrollView {
            id: accountScroll; contentWidth: availableWidth
            implicitHeight: Math.min(accountForm.implicitHeight, window.height - 160)
            ColumnLayout {
            id: accountForm; width: accountScroll.availableWidth; spacing: 12
            Image { source: messenger.avatarUrl; Layout.preferredWidth: 72; Layout.preferredHeight: 72; fillMode: Image.PreserveAspectFit; visible: source.toString().length > 0 }
            Action { text: "上传头像"; onClicked: avatarPicker.open(); Layout.fillWidth: true }
            TextField { id: accountName; text: messenger.profileName; placeholderText: "给自己起个昵称"; maximumLength: 64; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            Label { text: messenger.userCode.length ? "UID：" + messenger.userCode : "UID 等待联网注册"; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: "无需手机号或邮箱。身份密钥和记录保存在本机，请勿清除应用数据。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Action { text: messenger.profileName.length ? "保存昵称" : "创建并注册"; enabled: accountName.text.trim().length > 0; Layout.fillWidth: true; onClicked: { if (messenger.setProfileName(accountName.text)) account.close() } }
            Action { text: "复制我的 UID"; enabled: messenger.userCode.length > 0; Layout.fillWidth: true; onClicked: messenger.copyUid() }
            ComboBox {
                visible: typeof accountManager !== "undefined"
                model: typeof accountManager !== "undefined" ? accountManager.profiles : []
                textRole: "name"; currentIndex: typeof accountManager !== "undefined" ? accountManager.activeIndex : -1
                Layout.fillWidth: true; Layout.minimumWidth: 0
                onActivated: accountManager.selectAccount(currentIndex)
            }
            Action { text: "创建另一个账号"; visible: typeof accountManager !== "undefined"; Layout.fillWidth: true; onClicked: accountManager.createAccount() }
            }
        }
    }
    Sheet {
        id: settings; title: "连接设置"; standardButtons: Dialog.Close
        contentItem: ColumnLayout {
            spacing: 12
            Switch { text: "辅助连接（推荐）"; checked: messenger.assistedConnection; Layout.fillWidth: true; onClicked: messenger.setAssistedConnection(checked) }
            Label { text: messenger.networkStatus; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: "辅助模式通过中继转发加密内容；纯直连不使用中继，跨网成功取决于网络条件。"; wrapMode: Text.Wrap; Layout.fillWidth: true }
            CheckBox { id: customRelay; text: "使用自己的中继"; checked: messenger.customRelay }
            TextField { id: relayAddress; visible: customRelay.checked; placeholderText: "wss://你的服务地址"; Layout.fillWidth: true; Layout.minimumWidth: 0; inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText }
            Action { visible: customRelay.checked; text: "仅保存到本机"; Layout.fillWidth: true; onClicked: messenger.setRelayEndpoint(relayAddress.text) }
            Action { visible: messenger.customRelay; text: "恢复默认辅助服务"; Layout.fillWidth: true; onClicked: messenger.restoreDefaultRelay() }
            Label { visible: customRelay.checked; text: "不会显示内置地址。更换服务后会重新注册 UID，双方需使用同一服务。"; wrapMode: Text.Wrap; color: window.subdued; Layout.fillWidth: true }
            Action { text: "复制完整配对码（备用）"; Layout.fillWidth: true; onClicked: messenger.copyLocalPairingCode() }
        }
    }
    Sheet {
        id: addFriend; title: "添加好友"; standardButtons: Dialog.Cancel
        contentItem: ColumnLayout {
            spacing: 12
            TextField { id: friendUid; placeholderText: "输入对方的数字 UID"; inputMethodHints: Qt.ImhDigitsOnly; maximumLength: 16; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            TextField { id: friendRemark; placeholderText: "备注名（选填）"; maximumLength: 64; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            Label { text: "通过同一辅助服务查找账号，发送后需对方同意。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Action { text: "查找并发送好友申请"; enabled: friendUid.text.trim().length > 0; Layout.fillWidth: true; onClicked: { if (messenger.addFriendByUid(friendUid.text, friendRemark.text)) addFriend.close() } }
            CheckBox { id: advancedAdd; text: "使用完整配对码" }
            TextArea { id: friendCode; visible: advancedAdd.checked; placeholderText: "粘贴配对码"; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 80 }
            Action { text: "使用配对码发送申请"; visible: advancedAdd.checked; Layout.fillWidth: true; onClicked: { if (messenger.addContact(friendRemark.text, friendCode.text)) addFriend.close() } }
        }
    }
    Sheet {
        id: requests; title: "好友与群聊申请"; standardButtons: Dialog.Close
        contentItem: ScrollView {
            id: requestScroll; implicitHeight: Math.min(400, window.height - 160); contentWidth: availableWidth
            ColumnLayout {
                width: requestScroll.availableWidth; spacing: 10
                Label { text: "没有待处理申请"; visible: messenger.friendRequests.length === 0 && messenger.pendingGroupRequests.length === 0; wrapMode: Text.Wrap; Layout.fillWidth: true }
                Repeater {
                    model: messenger.friendRequests
                    delegate: RowLayout {
                        required property var modelData; Layout.fillWidth: true
                        Label { text: modelData.name; elide: Text.ElideRight; Layout.fillWidth: true; Layout.minimumWidth: 0 }
                        Action { text: "同意"; onClicked: messenger.acceptFriendRequest(modelData.id) }
                    }
                }
                Repeater { model: messenger.pendingGroupRequests; delegate: Action { required property var modelData; text: "加入群聊 " + modelData.slice(0,8); Layout.fillWidth: true; onClicked: messenger.acceptGroupRequest(modelData) } }
            }
        }
    }
    Sheet {
        id: group; title: "创建群聊"; standardButtons: Dialog.Cancel
        property var selected: []; onOpened: selected = []
        contentItem: ColumnLayout {
            TextField { id: groupName; placeholderText: "群名称"; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            Label { text: "选择已添加的好友"; color: window.subdued }
            ScrollView {
                id: groupScroll; Layout.fillWidth: true; Layout.preferredHeight: 180; contentWidth: availableWidth
                Column {
                    width: groupScroll.availableWidth
                    Repeater {
                        model: messenger.contacts
                        delegate: CheckBox {
                            required property var modelData; width: parent.width; text: modelData.name
                            visible: !!modelData.ready && modelData.transport === "relay" && !modelData.group
                            onClicked: { let ids = group.selected.slice(); if (checked) ids.push(modelData.relayPublic); else ids = ids.filter(x => x !== modelData.relayPublic); group.selected = ids }
                        }
                    }
                }
            }
            Action { text: "创建群聊"; enabled: groupName.text.trim().length > 0 && group.selected.length > 0; Layout.fillWidth: true; onClicked: { if (messenger.createGroup(groupName.text, group.selected)) group.close() } }
        }
    }
    Sheet { id: nickname; title: "我的群昵称"; standardButtons: Dialog.Cancel; contentItem: ColumnLayout {
        TextField { id: groupNickname; placeholderText: "仅用于当前群聊"; maximumLength: 64; Layout.fillWidth: true; Layout.minimumWidth: 0 }
        Action { text: "保存"; Layout.fillWidth: true; onClicked: { if (messenger.setGroupNickname(groupNickname.text)) nickname.close() } }
    } }
    Sheet {
        id: call; title: "语音通话"; closePolicy: Popup.NoAutoClose
        contentItem: ColumnLayout {
            Label { text: messenger.callPeerName; font.pixelSize: 24; Layout.fillWidth: true; elide: Text.ElideRight }
            Label { text: messenger.callState === "ringing" ? "邀请你语音通话" : messenger.callState === "active" ? "通话中 · 端到端加密" : "正在接通…"; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Action { text: "接听"; visible: messenger.callState === "ringing"; Layout.fillWidth: true; onClicked: messenger.answerCall() }
            Action { text: messenger.callState === "ringing" ? "拒绝" : "挂断"; Layout.fillWidth: true; onClicked: messenger.endCall() }
        }
    }
    Sheet { id: emojis; title: "表情"; standardButtons: Dialog.Close; contentItem: ColumnLayout {
        GridLayout { columns: 4; Layout.fillWidth: true; Repeater { model: ["😀","😂","🥰","👍","❤️","🎉","😎","🙏"]; delegate: Action { required property var modelData; text: modelData; Layout.fillWidth: true; onClicked: { composer.text += modelData; emojis.close() } } } }
        ScrollView {
            id: stickerScroll; visible: messenger.stickerLibrary.length > 0; Layout.fillWidth: true; Layout.preferredHeight: 160; contentWidth: availableWidth
            GridLayout {
                width: stickerScroll.availableWidth; columns: 3
                Repeater {
                    model: messenger.stickerLibrary
                    delegate: ItemDelegate {
                        required property var modelData; required property int index
                        Layout.fillWidth: true; Layout.minimumWidth: 0; implicitHeight: 72
                        contentItem: Image { source: "data:image/png;base64," + modelData.data; fillMode: Image.PreserveAspectFit }
                        onClicked: { if (messenger.sendSavedSticker(index)) emojis.close() }
                        onPressAndHold: messenger.removeSticker(index)
                    }
                }
            }
        }
        Label { text: "点选发送，长按移除收藏"; visible: messenger.stickerLibrary.length > 0; color: window.subdued; font.pixelSize: 11 }
        Action { text: "发送自定义表情图片"; Layout.fillWidth: true; onClicked: { emojis.close(); stickerPicker.open() } }
    } }
    FileDialog { id: avatarPicker; title: "选择头像"; nameFilters: ["图片 (*.png *.jpg *.jpeg *.webp)"]; onAccepted: messenger.setAvatar(selectedFile.toString()) }
    FileDialog { id: stickerPicker; title: "选择表情图片"; nameFilters: ["图片 (*.png *.jpg *.jpeg *.webp *.gif)"]; onAccepted: messenger.sendSticker(selectedFile.toString()) }
    FileDialog { id: filePicker; title: "发送文件"; onAccepted: messenger.queueFile(selectedFile.toString()) }
    FileDialog { id: saveFile; title: "保存文件"; fileMode: FileDialog.SaveFile; property string interactionId: ""; property string fileId: ""; onAccepted: messenger.downloadFile(interactionId, fileId, selectedFile.toString()) }
    RowLayout {
        anchors.fill: parent; anchors.margins: window.mobile ? 0 : 12; spacing: window.mobile ? 0 : 12
        Rectangle {
            color: "white"; radius: window.mobile ? 0 : 14; visible: !window.mobile || !window.showThread
            Layout.preferredWidth: window.mobile ? 0 : 290; Layout.fillWidth: window.mobile; Layout.fillHeight: true; Layout.minimumWidth: 0
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 14; spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    Image { source: messenger.avatarUrl; Layout.preferredWidth: 42; Layout.preferredHeight: 42; fillMode: Image.PreserveAspectFit; visible: source.toString().length > 0 }
                    Label { text: "双点聊"; font.bold: true; font.pixelSize: 23; color: window.accent; Layout.fillWidth: true; Layout.minimumWidth: 0 }
                    Action { text: "设置"; onClicked: settings.open() }
                }
                Label { text: messenger.profileName.length ? messenger.profileName + (messenger.userCode.length ? " · UID " + messenger.userCode : " · 待注册") : "欢迎，先创建账号"; color: "#263b56"; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }
                Label { text: messenger.networkStatus; color: window.subdued; wrapMode: Text.Wrap; font.pixelSize: 12; Layout.fillWidth: true; Layout.minimumWidth: 0 }
                RowLayout {
                    Layout.fillWidth: true
                    Action { text: "账号"; Layout.fillWidth: true; onClicked: account.open() }
                    Action { text: "加好友"; Layout.fillWidth: true; enabled: messenger.profileName.length > 0; onClicked: addFriend.open() }
                    Action { text: "申请 " + messenger.pendingRequests.length; Layout.fillWidth: true; onClicked: requests.open() }
                }
                RowLayout { Layout.fillWidth: true; Label { text: "消息"; color: window.subdued; Layout.fillWidth: true } Action { text: "新建群聊"; enabled: messenger.profileName.length > 0; onClicked: group.open() } }
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 5; model: messenger.contacts
                    delegate: ItemDelegate {
                        required property var modelData; width: ListView.view.width; height: 68
                        background: Rectangle { radius: 10; color: messenger.activeContactId === modelData.id ? "#eaf4ff" : "transparent" }
                        onClicked: { messenger.selectContact(modelData.id); window.showThread = true }
                        contentItem: RowLayout {
                            Rectangle {
                                Layout.preferredWidth: 42; Layout.preferredHeight: 42; radius: 14; color: "#dbeeff"
                                Label { anchors.centerIn: parent; text: modelData.initial; color: window.accent; font.pixelSize: 18; visible: !modelData.avatar }
                                Image { anchors.fill: parent; source: modelData.avatar ? "data:image/png;base64," + modelData.avatar : ""; fillMode: Image.PreserveAspectFit }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 4
                                Label { text: modelData.name; color: "#243750"; font.bold: true; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }
                                Label { text: modelData.status; color: window.subdued; font.pixelSize: 12; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }
                            }
                        }
                    }
                }
            }
        }
        Rectangle {
            color: "#f5f8fc"; radius: window.mobile ? 0 : 14; visible: !window.mobile || window.showThread
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumWidth: 0
            ColumnLayout {
                anchors.fill: parent; anchors.margins: window.mobile ? 12 : 20; spacing: 10
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    Action { objectName: "chatBack"; text: "‹"; visible: window.mobile; Layout.preferredWidth: 36; Layout.minimumWidth: 36; onClicked: window.showThread = false }
                    ColumnLayout { Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 2
                        Label { text: messenger.activeContactName; font.bold: true; font.pixelSize: 19; color: "#22354f"; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }
                        Label { text: "端到端加密 · 本地记录"; color: window.subdued; font.pixelSize: 11; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }
                    }
                    Action { text: "⋯"; Layout.preferredWidth: 36; Layout.minimumWidth: 36; onClicked: threadMenu.open(); Menu { id: threadMenu; MenuItem { text: "添加好友"; onTriggered: addFriend.open() } MenuItem { text: "好友申请"; onTriggered: requests.open() } MenuItem { text: "我的群昵称"; onTriggered: nickname.open() } MenuItem { text: "连接设置"; onTriggered: settings.open() } } }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#e0e8f2" }
                ListView {
                    id: thread; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 12; model: messenger.messages
                    onCountChanged: positionViewAtEnd()
                    delegate: Item {
                        required property var modelData; width: ListView.view.width; height: bubble.implicitHeight
                        Rectangle {
                            id: bubble; width: Math.min(parent.width * .85, 380); implicitHeight: bubbleContents.implicitHeight + 24; radius: 12
                            anchors.right: modelData.outgoing ? parent.right : undefined; color: modelData.outgoing ? "#dceeff" : "white"
                            ColumnLayout {
                                id: bubbleContents; anchors.fill: parent; anchors.margins: 12; spacing: 6
                                Label { text: modelData.body; color: "#203b58"; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; Layout.minimumWidth: 0; font.pixelSize: 15 }
                                Image { visible: modelData.kind === "sticker"; source: visible ? "data:image/png;base64," + modelData.fileData : ""; Layout.fillWidth: true; Layout.preferredHeight: visible ? 150 : 0; fillMode: Image.PreserveAspectFit }
                                Action { text: "▶ 播放语音"; visible: modelData.kind === "voice"; onClicked: messenger.playVoice(modelData.fileData) }
                                Action { text: "保存文件"; visible: modelData.kind === "file-offer"; onClicked: { saveFile.interactionId = modelData.interactionId || ""; saveFile.fileId = modelData.fileId || ""; saveFile.open() } }
                                Label { text: (modelData.time || "") + " " + (modelData.delivery || ""); color: window.subdued; font.pixelSize: 10; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; wrapMode: Text.Wrap }
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 6
                    Action { objectName: "chatFile"; text: "文件"; Layout.fillWidth: true; Layout.minimumWidth: 0; onClicked: filePicker.open() }
                    Action { text: "表情"; Layout.fillWidth: true; Layout.minimumWidth: 0; onClicked: emojis.open() }
                    Action { text: "语音"; Layout.fillWidth: true; Layout.minimumWidth: 0; enabled: !messenger.recording; onClicked: messenger.recordVoice() }
                    Action { text: "通话"; Layout.fillWidth: true; Layout.minimumWidth: 0; onClicked: messenger.startCall() }
                }
                RowLayout {
                    visible: messenger.recording; Layout.fillWidth: true
                    Label { text: "录音中 · 最长 60 秒"; color: window.accent; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }
                    Action { text: "取消"; onClicked: messenger.finishVoice(false) }
                    Action { text: "发送"; onClicked: messenger.finishVoice(true) }
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    TextField { id: composer; objectName: "chatInput"; placeholderText: "输入消息"; Layout.fillWidth: true; Layout.minimumWidth: 0; selectByMouse: true; onAccepted: { if (messenger.sendMessage(text)) text = "" } }
                    Action { objectName: "chatSend"; text: "发送"; Layout.preferredWidth: 58; Layout.minimumWidth: 58; enabled: composer.text.trim().length > 0; onClicked: { if (messenger.sendMessage(composer.text)) composer.text = "" } }
                }
            }
        }
    }
}
