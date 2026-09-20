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
    color: "#101827"

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
                onClicked: { messenger.addContact(friendName.text, invite.text); addFriend.close() }
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
            Label { text: "仅填写你自己部署的服务；留空表示仅本地/直连。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            TextField { id: rendezvousUrl; placeholderText: "wss://你的域名/v1/rendezvous"; Layout.fillWidth: true }
            TextField { id: turnHost; placeholderText: "自建 TURN 主机"; Layout.fillWidth: true }
            RowLayout { TextField { id: turnPort; text: "3478"; Layout.fillWidth: true }; TextField { id: turnUser; placeholderText: "TURN 用户名"; Layout.fillWidth: true } }
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
                onClicked: { messenger.createGroup(groupName.text, groupMembers.text.split("\n")); createGroup.close() }
            }
        }
    }

    FileDialog {
        id: filePicker
        title: "选择要发送的文件"
        onAccepted: messenger.queueFile(selectedFile.toString().replace("file:///", ""))
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 14

        Rectangle {
            Layout.preferredWidth: 310
            Layout.fillHeight: true
            color: window.panel
            radius: 20
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 16
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "P2P"; color: window.accent; font.bold: true; font.pixelSize: 24 }
                    Label { text: "MESSENGER"; color: "white"; font.bold: true; font.pixelSize: 18; Layout.fillWidth: true }
                    Rectangle { width: 10; height: 10; radius: 5; color: window.accent }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#30415d" }
                Label { text: messenger.networkStatus; color: window.subdued; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
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
                            ColumnLayout { spacing: 2; Layout.fillWidth: true; Label { text: modelData.name; color: "white"; font.bold: true }; Label { text: modelData.status; color: window.subdued; font.pixelSize: 12 } }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: window.panel
            radius: 20
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 24
                spacing: 16
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout { spacing: 3; Label { text: messenger.activeContactName; color: "white"; font.pixelSize: 22; font.bold: true }; Label { text: "端到端加密 · 本地记录"; color: window.subdued; font.pixelSize: 12 } }
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
                            width: Math.min(parent.width * .76, messageText.implicitWidth + 34)
                            implicitHeight: messageText.implicitHeight + 26
                            Label { id: messageText; anchors.margins: 13; anchors.fill: parent; text: modelData.body; color: "white"; wrapMode: Text.Wrap; font.pixelSize: 15 }
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
                        onAccepted: { messenger.sendMessage(text); text = "" }
                    }
                    Button { text: "发送"; enabled: composer.text.trim().length > 0; onClicked: { messenger.sendMessage(composer.text); composer.text = "" } }
                }
            }
        }
    }
}
