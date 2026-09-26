import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: 1160
    height: 760
    minimumWidth: 740
    minimumHeight: 560
    visible: true
    title: "P2P Messenger"
    color: "#0b1220"

    property color panel: "#182235"
    property color panelRaised: "#202d43"
    property color accent: "#68d7bb"
    property color subdued: "#91a1bb"

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
            Label { text: "输入对方给你的邀请码或扫描二维码后得到的内容。"; wrapMode: Text.Wrap; color: window.subdued; Layout.fillWidth: true }
            TextField { id: friendName; placeholderText: "备注名称"; Layout.fillWidth: true }
            TextArea { id: invite; placeholderText: "邀请码"; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.preferredHeight: 110 }
            Button {
                text: "验证并添加"
                enabled: friendName.text.trim().length > 0 && invite.text.trim().length > 0
                Layout.alignment: Qt.AlignRight
                onClicked: { if (messenger.addContact(friendName.text, invite.text)) addFriend.close() }
            }
        }
    }

    Dialog {
        id: networkSettings
        title: "自建网络设置"
        modal: true
        anchors.centerIn: parent
        width: Math.min(440, window.width - 40)
        standardButtons: Dialog.Cancel
        background: Rectangle { color: window.panel; radius: 16 }
        contentItem: ColumnLayout {
            spacing: 10
            Label { text: "填写你自己部署的 DHT 引导节点和 TURN；留空表示仅局域网发现。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            TextField { id: rendezvousUrl; placeholderText: "自建 DHT 节点，例如 chat.example.com:4222"; Layout.fillWidth: true }
            TextField { id: turnHost; placeholderText: "自建 TURN 主机"; Layout.fillWidth: true }
            RowLayout {
                TextField { id: turnPort; text: "3478"; Layout.fillWidth: true }
                TextField { id: turnUser; placeholderText: "TURN 用户名"; Layout.fillWidth: true }
            }
            TextField { id: turnPassword; placeholderText: "TURN 密码"; echoMode: TextInput.Password; Layout.fillWidth: true }
            Button { text: "保存自建配置"; Layout.alignment: Qt.AlignRight; onClicked: { if (messenger.configureNetwork(rendezvousUrl.text, turnHost.text, Number(turnPort.text), turnUser.text, turnPassword.text)) networkSettings.close() } }
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
        anchors.margins: 16
        spacing: 14

        Rectangle {
            Layout.preferredWidth: 310
            Layout.fillHeight: true
            color: Qt.rgba(0.095, 0.14, 0.22, 0.96)
            radius: 20
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 18
                RowLayout {
                    Layout.fillWidth: true
                    Rectangle { width: 34; height: 34; radius: 10; color: window.accent; Label { anchors.centerIn: parent; text: "✦"; color: "#102132"; font.pixelSize: 20; font.bold: true } }
                    Label { text: "P2P"; color: window.accent; font.bold: true; font.pixelSize: 24 }
                    Label { text: "MESSENGER"; color: "white"; font.bold: true; font.pixelSize: 18; Layout.fillWidth: true }
                    Rectangle { width: 10; height: 10; radius: 5; color: window.accent }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#30415d" }
                Label { text: messenger.networkStatus; color: window.subdued; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
                Label { text: messenger.inviteCode.length ? "我的邀请码" : "正在生成本机邀请码"; color: window.subdued; font.pixelSize: 12 }
                Label { text: messenger.inviteCode; color: "white"; font.pixelSize: 11; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; visible: messenger.inviteCode.length > 0 }
                Button { text: "复制我的邀请码"; enabled: messenger.inviteCode.length > 0; Layout.fillWidth: true; onClicked: messenger.copyInviteCode() }
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
                Button { text: "新建群聊"; Layout.fillWidth: true; onClicked: createGroup.open() }
                Button { text: "自建网络设置"; Layout.fillWidth: true; onClicked: networkSettings.open() }
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
                        onClicked: messenger.selectContact(modelData.id)
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
                    ColumnLayout {
                        spacing: 3
                        Label { text: messenger.activeContactName; color: "white"; font.pixelSize: 22; font.bold: true }
                        Label { text: "端到端加密  ·  本地记录"; color: window.subdued; font.pixelSize: 12 }
                    }
                    Label { text: "私有网络"; color: window.accent; font.bold: true }
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
