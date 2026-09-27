import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
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
    color: "#0c1420"
    Material.theme: Material.Dark
    Material.accent: accent

    property color panel: "#172435"
    property color panelRaised: "#26394d"
    property color accent: "#77e0c0"
    property color subdued: "#a9b9c9"
    property bool mobile: Qt.platform.os === "android" || width < 740
    property bool showThread: false
    property bool pairingCopied: false
    Timer { id: copiedReset; interval: 3000; onTriggered: window.pairingCopied = false }

    Component.onCompleted: {
        if (messenger.profileName.length === 0)
            Qt.callLater(function() { accountSettings.open() })
        if (messenger.pendingRequests.length > 0)
            Qt.callLater(function() { friendRequestDialog.open() })
    }

    Connections {
        target: messenger
        function onLastErrorChanged() {
            if (messenger.lastError.length > 0) failureDialog.open()
        }
        function onPendingRequestsChanged() {
            if (messenger.pendingRequests.length > 0 && !friendRequestDialog.visible)
                friendRequestDialog.open()
        }
    }

    Dialog {
        id: failureDialog
        title: "操作未完成"
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, window.width - 32)
        standardButtons: Dialog.Ok
        background: Rectangle { color: window.panel; radius: 18; border.color: "#b26165"; border.width: 1 }
        contentItem: Label {
            text: messenger.lastError
            color: "#f5d2d0"
            wrapMode: Text.Wrap
            padding: 18
            font.pixelSize: 15
        }
    }

    Dialog {
        id: friendRequestDialog
        title: "收到好友申请"
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, window.width - 32)
        standardButtons: Dialog.Close
        background: Rectangle { color: window.panel; radius: 18; border.color: window.accent; border.width: 1 }
        contentItem: ColumnLayout {
            spacing: 14
            Label {
                text: messenger.pendingRequests.length > 0
                      ? "设备 " + messenger.pendingRequests[0].slice(0, 12) + "… 想添加你为好友。"
                      : "尚未收到好友申请。请让双方都打开双点聊；跨网测试辅助连接时，两端都要开启辅助连接。"
                color: "white"; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Button {
                text: "接受并添加好友"
                enabled: messenger.pendingRequests.length > 0
                Layout.fillWidth: true
                onClicked: {
                    if (messenger.pendingRequests.length > 0 && messenger.acceptFriendRequest(messenger.pendingRequests[0])) {
                        if (messenger.pendingRequests.length === 0) friendRequestDialog.close()
                    }
                }
            }
        }
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
            implicitHeight: Math.min(accountContent.implicitHeight + 12, window.height - 130)
            contentWidth: availableWidth
            ColumnLayout {
            id: accountContent
            width: accountScroll.availableWidth
            spacing: 12
            Label { text: "账号只保存在这台设备，不依赖第三方注册服务。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: "账号名称"; color: "white"; font.bold: true }
            TextField { id: accountName; text: messenger.profileName; placeholderText: "给自己起个名字"; Layout.fillWidth: true; maximumLength: 64 }
            Button { text: "保存账号名称"; enabled: accountName.text.trim().length > 0; Layout.fillWidth: true; onClicked: { if (messenger.setProfileName(accountName.text)) accountSettings.close() } }
            Label { text: "我的设备码"; color: window.subdued; font.pixelSize: 12 }
            Label { text: messenger.inviteCode.length ? messenger.inviteCode : "正在生成，请稍后重试"; color: "white"; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                Button { text: "复制设备码"; enabled: messenger.inviteCode.length > 0; Layout.fillWidth: true; onClicked: messenger.copyInviteCode() }
                Button { text: "重试"; Layout.fillWidth: true; onClicked: messenger.retryIdentity() }
            }
            Button { text: "复制我的配对码"; Layout.fillWidth: true; onClicked: { if (messenger.copyLocalPairingCode()) directAddress.text = messenger.directEndpoint } }
            Label { text: "连接方式"; color: "white"; font.bold: true; Layout.fillWidth: true }
            Button {
                text: messenger.assistedConnection ? "✓ 辅助连接（点此切回纯直连）" : "✓ 纯直连（点此切换辅助连接）"
                Layout.fillWidth: true
                onClicked: messenger.setAssistedConnection(!messenger.assistedConnection)
            }
            Label {
                text: messenger.assistedConnection
                      ? "已允许使用 Jami 公共节点寻找对方；直连不通时可尝试 TURN 中继。消息和文件仍端到端加密，但公共服务会看到连接元数据；服务不可用时也可能连接失败。"
                      : "默认模式：不连接公共引导或中继。跨网连接取决于双方网络是否允许直连；失败时可自行切换辅助连接。"
                color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12
            }
            CheckBox { id: advancedNetwork; text: "高级网络设置"; Layout.fillWidth: true }
            Label { text: "本机账号 ID：" + messenger.accountId; color: window.subdued; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; font.pixelSize: 12; visible: advancedNetwork.checked }
            Label { text: "监听端口：" + (messenger.listeningPort > 0 ? messenger.listeningPort : "尚未就绪"); color: window.subdued; Layout.fillWidth: true; font.pixelSize: 12; visible: advancedNetwork.checked }
            TextField { id: directAddress; text: messenger.directEndpoint; placeholderText: "可达 IP:端口"; Layout.fillWidth: true; visible: advancedNetwork.checked }
            Button { text: "保存手动直连地址"; Layout.fillWidth: true; visible: advancedNetwork.checked; onClicked: messenger.setDirectEndpoint(directAddress.text) }
            Label { text: "纯直连模式下，跨网自动地址若不可达，可在这里填写公网 IPv6 或已映射的端口。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12; visible: advancedNetwork.checked }
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
        onOpened: messenger.refreshNearbyPeers()
        background: Rectangle { color: window.panel; radius: 16 }
        contentItem: ScrollView {
            id: addFriendScroll
            implicitHeight: Math.min(addFriendContent.implicitHeight + 12, window.height - 130)
            contentWidth: availableWidth
            ColumnLayout {
            id: addFriendContent
            width: addFriendScroll.availableWidth
            spacing: 14
            Label { text: "同一 Wi-Fi 可点选附近设备；不在一起时，请对方发来配对码。"; wrapMode: Text.Wrap; color: window.subdued; Layout.fillWidth: true }
            Label { text: "附近设备"; color: window.accent; font.bold: true; Layout.fillWidth: true }
            Repeater {
                model: messenger.nearbyPeers
                delegate: Button {
                    required property var modelData
                    text: "添加 " + modelData.name
                    Layout.fillWidth: true
                    onClicked: { if (messenger.addNearbyPeer(modelData.uri)) addFriend.close() }
                }
            }
            Label { text: "暂无附近设备，可粘贴配对码"; color: window.subdued; visible: messenger.nearbyPeers.length === 0; Layout.fillWidth: true }
            Button { text: "重新查找"; Layout.fillWidth: true; onClicked: messenger.refreshNearbyPeers() }
            TextField { id: friendName; placeholderText: "备注名称（可选）"; Layout.fillWidth: true }
            TextArea { id: invite; placeholderText: "粘贴对方的配对码"; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.preferredHeight: 90 }
            Button {
                text: "发送好友申请"
                enabled: invite.text.trim().length > 0
                Layout.alignment: Qt.AlignRight
                onClicked: { if (messenger.addContact(friendName.text, invite.text)) addFriend.close() }
            }
            }
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
                Button { text: window.pairingCopied ? "已复制，发给对方即可" : "分享我的配对码"; Layout.fillWidth: true; onClicked: { if (messenger.copyLocalPairingCode()) { window.pairingCopied = true; copiedReset.restart() } } }
                Label { text: messenger.inviteCode.length ? "我的邀请码" : "邀请码尚未生成"; color: window.subdued; font.pixelSize: 12; visible: !window.mobile }
                Label { text: messenger.inviteCode; color: "white"; font.pixelSize: 11; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; visible: !window.mobile && messenger.inviteCode.length > 0 }
                Button { text: "复制我的邀请码"; enabled: messenger.inviteCode.length > 0; Layout.fillWidth: true; visible: !window.mobile; onClicked: messenger.copyInviteCode() }
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
                Button { text: "查看好友申请"; Layout.fillWidth: true; onClicked: { messenger.refreshPendingRequests(); friendRequestDialog.open() } }
                Button { text: "新建群聊"; Layout.fillWidth: true; onClicked: createGroup.open() }
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
                    Button { text: "加好友"; visible: window.mobile; onClicked: addFriend.open() }
                    Button { text: "申请 " + messenger.pendingRequests.length; visible: window.mobile; onClicked: { messenger.refreshPendingRequests(); friendRequestDialog.open() } }
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
