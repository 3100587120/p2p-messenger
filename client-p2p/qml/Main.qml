import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: Qt.platform.os === "android" ? 390 : 1160
    height: Qt.platform.os === "android" ? 780 : 760
    minimumWidth: Qt.platform.os === "android" ? 320 : 740
    minimumHeight: Qt.platform.os === "android" ? 480 : 560
    visible: true
    title: "双点聊"
    color: "#0b1220"

    property color panel: "#182235"
    property color panelRaised: "#202d43"
    property color accent: "#68d7bb"
    property color subdued: "#91a1bb"
    property bool mobile: Qt.platform.os === "android" || width < 740
    property bool showThread: false
    property bool pairingCopied: false
    property bool searchedNearby: false
    Timer { id: copiedReset; interval: 3000; onTriggered: window.pairingCopied = false }

    Component.onCompleted: {
        if (messenger.profileName.length === 0)
            Qt.callLater(function() { accountSettings.open() })
    }

    Dialog {
        id: accountSettings
        title: "本机账号"
        modal: true
        anchors.centerIn: parent
        width: Math.min(440, window.width - 24)
        standardButtons: Dialog.Close
        background: Rectangle { color: window.panel; radius: 16 }
        contentItem: ScrollView {
            id: accountScroll
            implicitHeight: Math.min(610, window.height - 130)
            contentWidth: availableWidth
            ColumnLayout {
            width: accountScroll.availableWidth
            spacing: 12
            Label { text: "账号只保存在这台设备，不依赖第三方注册服务。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: "账号名称"; color: "white"; font.bold: true }
            TextField { id: accountName; text: messenger.profileName; placeholderText: "给自己起个名字"; Layout.fillWidth: true; maximumLength: 64 }
            Button { text: "保存账号名称"; enabled: accountName.text.trim().length > 0; Layout.fillWidth: true; onClicked: { if (messenger.setProfileName(accountName.text)) accountSettings.close() } }
            Label { text: "本机账号 ID"; color: window.subdued; font.pixelSize: 12 }
            Label { text: messenger.accountId.length ? messenger.accountId : "尚未创建"; color: "white"; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; font.pixelSize: 12 }
            Label { text: "好友邀请码"; color: window.subdued; font.pixelSize: 12 }
            Label { text: messenger.inviteCode.length ? messenger.inviteCode : "正在生成，请稍后重试"; color: "white"; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                Button { text: "复制邀请码"; enabled: messenger.inviteCode.length > 0; Layout.fillWidth: true; onClicked: messenger.copyInviteCode() }
                Button { text: "重试"; Layout.fillWidth: true; onClicked: messenger.retryIdentity() }
            }
            Label { text: "双机直连配对"; color: window.accent; font.bold: true }
            Label { text: "本机 DHT 监听端口：" + (messenger.listeningPort > 0 ? messenger.listeningPort : "尚未就绪"); color: window.subdued; Layout.fillWidth: true; font.pixelSize: 12 }
            TextField { id: directAddress; text: messenger.directEndpoint; placeholderText: "本机可达 IP:端口，如 192.168.1.2:4222"; Layout.fillWidth: true }
            Button { text: "使用本机局域网地址"; Layout.fillWidth: true; onClicked: { if (messenger.useLocalNetworkAddress()) directAddress.text = messenger.directEndpoint } }
            Button { text: "一键复制局域网配对码"; Layout.fillWidth: true; onClicked: { if (messenger.copyLocalPairingCode()) directAddress.text = messenger.directEndpoint } }
            Button { text: "保存本机直连地址"; Layout.fillWidth: true; onClicked: messenger.setDirectEndpoint(directAddress.text) }
            Label { text: "同一 Wi-Fi 可留空并分享邀请码；跨网必须填写对方能访问的公网 IPv6 或已映射端口。此地址只打包进配对码，不发送给服务端。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12 }
            Button { text: "复制双机配对码"; enabled: messenger.pairingCode.length > 0; Layout.fillWidth: true; onClicked: messenger.copyPairingCode() }
            Label { text: "仅直连，不使用 TURN 中继。两端均被运营商 NAT 阻挡时会连接失败；安卓后台运行仍需单独验证。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12 }
            Label { text: messenger.lastError; color: "#ff9c9c"; wrapMode: Text.Wrap; Layout.fillWidth: true; visible: messenger.lastError.length > 0 }
            }
        }
    }

    Dialog {
        id: addFriend
        title: "添加好友"
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, window.width - 40)
        standardButtons: Dialog.Cancel
        background: Rectangle { color: window.panel; radius: 16 }
        contentItem: ColumnLayout {
            spacing: 14
            Label { text: "粘贴对方的 40 位邀请码，或包含可达 IP 的完整双机配对码。"; wrapMode: Text.Wrap; color: window.subdued; Layout.fillWidth: true }
            TextField { id: friendName; placeholderText: "备注名称（可选）"; Layout.fillWidth: true }
            TextArea { id: invite; placeholderText: "邀请码 / p2pm://pair?..."; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.preferredHeight: 110 }
            Button {
                text: "验证并添加"
                enabled: invite.text.trim().length > 0
                Layout.alignment: Qt.AlignRight
                onClicked: { if (messenger.addContact(friendName.text, invite.text)) addFriend.close() }
            }
        }
    }

    Dialog {
        id: networkSettings
        title: "指定直连入口"
        modal: true
        anchors.centerIn: parent
        width: Math.min(440, window.width - 40)
        standardButtons: Dialog.Cancel
        background: Rectangle { color: window.panel; radius: 16 }
        contentItem: ColumnLayout {
            spacing: 10
            Label { text: "仅填写另一台设备可达的数字 IP:端口；留空则只尝试局域网发现。不会启用中继。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            TextField { id: rendezvousUrl; placeholderText: "例如 192.168.1.2:4222"; Layout.fillWidth: true }
            Button { text: "保存直连入口"; Layout.alignment: Qt.AlignRight; onClicked: { if (messenger.configureNetwork(rendezvousUrl.text)) networkSettings.close() } }
        }
    }

    Dialog {
        id: createGroup
        title: "新建群聊"
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, window.width - 40)
        standardButtons: Dialog.Cancel
        background: Rectangle { color: window.panel; radius: 16 }
        contentItem: ColumnLayout {
            spacing: 14
            Label { text: "输入群名称，并粘贴成员已验证的邀请码（每行一个）。"; wrapMode: Text.Wrap; color: window.subdued; Layout.fillWidth: true }
            TextField { id: groupName; placeholderText: "群名称"; Layout.fillWidth: true }
            TextArea { id: groupMembers; placeholderText: "成员邀请码"; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.preferredHeight: 130 }
            Button {
                text: "创建加密群聊"
                enabled: groupName.text.trim().length > 0
                Layout.alignment: Qt.AlignRight
                onClicked: { if (messenger.createGroup(groupName.text, groupMembers.text.split("\n"))) createGroup.close() }
            }
        }
    }

    FileDialog {
        id: filePicker
        title: "选择要发送的文件"
        onAccepted: messenger.queueFile(selectedFile.toString())
    }

    FileDialog {
        id: saveFileDialog
        title: "保存收到的文件"
        fileMode: FileDialog.SaveFile
        property string interactionId: ""
        property string fileId: ""
        onAccepted: messenger.downloadFile(interactionId, fileId, selectedFile.toString())
    }

    Rectangle {
        anchors.fill: parent
        z: -1
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#0b1220" }
            GradientStop { position: 0.55; color: "#111d31" }
            GradientStop { position: 1.0; color: "#0b1220" }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: window.mobile ? 8 : 16
        spacing: 14

        Rectangle {
            visible: !window.mobile || !window.showThread
            Layout.preferredWidth: window.mobile ? 0 : 310
            Layout.fillWidth: window.mobile
            Layout.fillHeight: true
            color: Qt.rgba(0.095, 0.14, 0.22, 0.96)
            radius: 20
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: window.mobile ? 14 : 20
                spacing: window.mobile ? 10 : 18
                RowLayout {
                    Layout.fillWidth: true
                    Rectangle { width: 34; height: 34; radius: 10; color: window.accent; Label { anchors.centerIn: parent; text: "✦"; color: "#102132"; font.pixelSize: 20; font.bold: true } }
                    Label { text: "双点聊"; color: window.accent; font.bold: true; font.pixelSize: 24; Layout.fillWidth: true }
                    Rectangle { width: 10; height: 10; radius: 5; color: window.accent }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#30415d" }
                Label { text: messenger.networkStatus; color: window.subdued; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
                Button { text: messenger.profileName.length ? "账号：" + messenger.profileName : "设置本机账号"; Layout.fillWidth: true; onClicked: accountSettings.open() }
                Button { text: window.pairingCopied ? "已复制，发给对方即可" : "一键复制我的配对码"; Layout.fillWidth: true; onClicked: { if (messenger.copyLocalPairingCode()) { window.pairingCopied = true; copiedReset.restart() } } }
                Label { text: messenger.inviteCode.length ? "我的邀请码" : "邀请码尚未生成"; color: window.subdued; font.pixelSize: 12; visible: !window.mobile }
                Label { text: messenger.inviteCode; color: "white"; font.pixelSize: 11; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; visible: !window.mobile && messenger.inviteCode.length > 0 }
                Button { text: "复制我的邀请码"; enabled: messenger.inviteCode.length > 0; Layout.fillWidth: true; visible: !window.mobile; onClicked: messenger.copyInviteCode() }
                Label { text: messenger.lastError; color: "#ff9c9c"; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true; visible: messenger.lastError.length > 0 }
                Label { text: "待处理好友申请"; color: window.accent; font.pixelSize: 12; visible: messenger.pendingRequests.length > 0 }
                Repeater {
                    model: messenger.pendingRequests
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label { text: modelData.slice(0, 12) + "…"; color: "white"; Layout.fillWidth: true }
                        Button { text: "接受"; onClicked: messenger.acceptFriendRequest(modelData) }
                    }
                }
                Label { text: "待处理群聊邀请"; color: window.accent; font.pixelSize: 12; visible: messenger.pendingGroupRequests.length > 0 }
                Repeater {
                    model: messenger.pendingGroupRequests
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label { text: "群聊 " + modelData.slice(0, 10) + "…"; color: "white"; Layout.fillWidth: true }
                        Button { text: "加入"; onClicked: messenger.acceptGroupRequest(modelData) }
                    }
                }
                Button { text: "+ 添加好友"; Layout.fillWidth: true; onClicked: addFriend.open() }
                Button { text: "扫描同一 Wi-Fi 的设备"; Layout.fillWidth: true; onClicked: { window.searchedNearby = true; messenger.refreshNearbyPeers() } }
                Label { text: "未发现设备？让对方也打开双点聊，并保持在同一 Wi-Fi。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12; visible: window.searchedNearby && messenger.nearbyPeers.length === 0 }
                Label { text: "附近设备 · 点击添加"; color: window.accent; font.pixelSize: 12; visible: messenger.nearbyPeers.length > 0 }
                Repeater {
                    model: messenger.nearbyPeers
                    delegate: Button {
                        required property var modelData
                        Layout.fillWidth: true
                        text: "添加 " + modelData.name
                        onClicked: messenger.addNearbyPeer(modelData.uri)
                    }
                }
                Button { text: "新建群聊"; Layout.fillWidth: true; onClicked: createGroup.open() }
                Button { text: "指定直连入口（可选）"; Layout.fillWidth: true; onClicked: networkSettings.open() }
                Label { text: "消息"; color: window.subdued; font.bold: true; font.pixelSize: 12 }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: messenger.contacts
                    spacing: 6
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        highlighted: messenger.activeContactId === modelData.id
                        onClicked: { messenger.selectContact(modelData.id); if (window.mobile) window.showThread = true }
                        contentItem: RowLayout {
                            spacing: 10
                            Rectangle { width: 38; height: 38; radius: 19; color: "#31516a"; Label { anchors.centerIn: parent; text: modelData.initial; color: "white"; font.bold: true } }
                            ColumnLayout {
                                spacing: 2; Layout.fillWidth: true
                                Label { text: modelData.name; color: "white"; font.bold: true }
                                Label { text: modelData.status; color: window.subdued; font.pixelSize: 12 }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            visible: !window.mobile || window.showThread
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Qt.rgba(0.095, 0.14, 0.22, 0.96)
            radius: 20
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 24
                spacing: 16
                RowLayout {
                    Layout.fillWidth: true
                    Button { text: "‹"; visible: window.mobile; onClicked: window.showThread = false }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        Label { text: messenger.activeContactName; color: "white"; font.pixelSize: window.mobile ? 18 : 22; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                        Label { text: "端到端加密  ·  本地记录"; color: window.subdued; font.pixelSize: 12 }
                    }
                    Label { text: "私有网络"; color: window.accent; font.bold: true; visible: !window.mobile }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#30415d" }
                ListView {
                    id: thread
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: messenger.messages
                    spacing: 10
                    delegate: Item {
                        required property var modelData
                        width: ListView.view.width
                        height: bubble.implicitHeight
                        Rectangle {
                            id: bubble
                            anchors.right: modelData.outgoing ? parent.right : undefined
                            color: modelData.outgoing ? "#296e66" : window.panelRaised
                            radius: 14
                            width: Math.min(parent.width * .76, Math.max(messageText.implicitWidth + 34, modelData.kind === "file-offer" ? 220 : 0))
                            implicitHeight: bubbleContents.implicitHeight + 26
                            ColumnLayout {
                                id: bubbleContents
                                anchors.fill: parent
                                anchors.margins: 13
                                spacing: 8
                                Label { id: messageText; Layout.fillWidth: true; text: modelData.body; color: "white"; wrapMode: Text.Wrap; font.pixelSize: 15 }
                                Button {
                                    text: "保存文件"
                                    visible: modelData.kind === "file-offer" && !modelData.body.includes("已完成")
                                    Layout.alignment: Qt.AlignLeft
                                    onClicked: {
                                        saveFileDialog.interactionId = modelData.interactionId
                                        saveFileDialog.fileId = modelData.fileId
                                        saveFileDialog.open()
                                    }
                                }
                            }
                        }
                    }
                    onCountChanged: positionViewAtEnd()
                }
                RowLayout {
                    Layout.fillWidth: true
                    Button { text: "文件"; onClicked: filePicker.open() }
                    TextField {
                        id: composer
                        Layout.fillWidth: true
                        placeholderText: "输入消息"
                        onAccepted: { if (messenger.sendMessage(text)) text = "" }
                    }
                    Button { text: "发送"; enabled: composer.text.trim().length > 0; onClicked: { if (messenger.sendMessage(composer.text)) composer.text = "" } }
                }
            }
        }
    }
}
