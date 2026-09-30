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
    property int mobileTab: 0
    property bool voiceInput: false
    property bool expandedTools: false
    property string contactSearch: ""
    font.pixelSize: 14
    property color accent: "#1685ef"
    property color subdued: "#758398"
    property string accountError: ""
    Popup {
        id: notice; x: (window.width - width) / 2; y: 12; width: Math.min(380,window.width - 24); padding: 14
        property string heading: ""; property string message: ""
        background: Rectangle { color: "#e7f3ff"; border.color: "#1685ef"; radius: 12 }
        contentItem: ColumnLayout { Label { text: notice.heading; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight } Label { text: notice.message; wrapMode: Text.Wrap; Layout.fillWidth: true } }
        onOpened: noticeTimeout.restart()
    }
    Timer { id: noticeTimeout; interval: 5000; onTriggered: notice.close() }
    component Action: Button {
        id: actionButton
        property string iconName: ""
        implicitHeight: 42; implicitWidth: iconRow.implicitWidth+24; leftPadding: 8; rightPadding: 8
        background: Rectangle { color: parent.down ? "#d8eafe" : "#edf5ff"; radius: 9 }
        Accessible.name: text
        contentItem: Item {
            Row { id: iconRow; anchors.centerIn: parent; spacing: actionButton.iconName.length ? 6 : 0
                FeatureIcon { glyph: actionButton.iconName; tint: actionButton.enabled ? window.accent : "#9ca9bb"; visible: glyph.length > 0; width: 19; height: 19; anchors.verticalCenter: parent.verticalCenter }
                Text { text: actionButton.text; color: actionButton.enabled ? window.accent : "#9ca9bb"; font.pixelSize: 14; anchors.verticalCenter: parent.verticalCenter }
            }
        }
    }
    component Sheet: Dialog {
        id: sheet
        modal: true; anchors.centerIn: parent
        width: Math.min(430, window.width - 24); padding: 16
        background: Rectangle { color: "white"; radius: 16; border.color: "#dde6f2" }
        header: Label { text: sheet.title; font.pixelSize: 19; font.bold: true; color: "#22354e"; padding: 20; bottomPadding: 10 }
        footer: DialogButtonBox {
            standardButtons: sheet.standardButtons; padding: 12
            delegate: Action { text: sheet.standardButtons === Dialog.Cancel ? "取消" : DialogButtonBox.buttonRole === DialogButtonBox.AcceptRole ? "确定" : "关闭" }
            onAccepted: sheet.accept(); onRejected: sheet.reject()
        }
    }
    Component.onCompleted: Qt.callLater(function() { if (!messenger.profileName.length) account.open(); if (messenger.lastError.length) failure.open() })
    Connections {
        target: messenger
        function onLastErrorChanged() { if (messenger.lastError.length) { window.accountError = ""; failure.open() } }
        function onPendingRequestsChanged() { if (messenger.pendingRequests.length) requests.open() }
        function onVoiceChanged() { if (messenger.callState !== "idle") call.open(); else call.close() }
        function onAvatarPickerRequested() { avatarPicker.open() }
        function onAttachmentPickerRequested(sticker) { if (sticker) stickerPicker.open(); else filePicker.open() }
        function onPhotoPickerRequested() { photoPicker.open() }
        function onIncomingNotice(title, text) { notice.heading = title; notice.message = text; notice.open() }
        function onUidChanged() { if (messenger.userCode.length) { account.close(); notice.heading = "账号已就绪"; notice.message = "UID：" + messenger.userCode; notice.open() } }
    }
    Connections { target: typeof accountManager !== "undefined" ? accountManager : null; function onErrorOccurred(message) { window.accountError = message; failure.open() } }
    Connections { target: typeof notificationService !== "undefined" ? notificationService : null; function onBackgroundError(message) { window.accountError=message;failure.open() } }
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
            RowLayout { Layout.fillWidth: true
                Rectangle { Layout.preferredWidth: 64; Layout.preferredHeight: 64; radius: 18; color: "#eaf3ff"; clip: true
                    Image { anchors.fill: parent; source: messenger.avatarUrl; fillMode: Image.PreserveAspectCrop }
                    FeatureIcon { anchors.centerIn: parent; width: 30; height: 30; glyph: "account"; visible: !messenger.avatarUrl.length }
                }
                ColumnLayout { Layout.fillWidth: true; Layout.minimumWidth: 0
                    Label { text: messenger.profileName.length ? messenger.profileName : "欢迎加入双点聊"; font.bold: true; font.pixelSize: 17; Layout.fillWidth: true; elide: Text.ElideRight }
                    Label { text: messenger.userCode.length ? "UID " + messenger.userCode : "设置昵称和密码即可注册"; color: window.subdued; Layout.fillWidth: true; elide: Text.ElideRight }
                }
            }
            Action { iconName: "avatar"; text: "上传头像"; onClicked: messenger.chooseAvatar(); Layout.fillWidth: true }
            TextField { id: accountName; text: messenger.profileName; placeholderText: "给自己起个昵称"; maximumLength: 64; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            TextField { id: accountPassword; visible: !messenger.passwordConfigured; placeholderText: "设置登录密码（8–128 位）"; echoMode: TextInput.Password; maximumLength: 128; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            Label { text: messenger.registrationStatus; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: "昵称用于好友展示；UID 和密码用于登录。聊天记录仅保存在本机。"; font.pixelSize: 12; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Action { iconName: "account"; text: messenger.registrationPending ? "UID 注册等待连接中" : messenger.passwordConfigured ? "保存昵称" : "设置密码并注册 UID"; enabled: !messenger.registrationPending && accountName.text.trim().length > 0 && (messenger.passwordConfigured || accountPassword.text.length >= 8); Layout.fillWidth: true; onClicked: { const ok = messenger.passwordConfigured ? messenger.setProfileName(accountName.text) : messenger.registerAccount(accountName.text, accountPassword.text); if (ok) { accountPassword.text = ""; account.close() } } }
            Action { iconName: "copy"; text: "复制我的 UID"; enabled: messenger.userCode.length > 0; Layout.fillWidth: true; onClicked: messenger.copyUid() }
            ComboBox {
                visible: typeof accountManager !== "undefined"
                model: typeof accountManager !== "undefined" ? accountManager.profiles : []
                textRole: "name"; currentIndex: typeof accountManager !== "undefined" ? accountManager.activeIndex : -1
                Layout.fillWidth: true; Layout.minimumWidth: 0
                onActivated: accountManager.selectAccount(currentIndex)
            }
            Action { iconName: "friend"; text: "创建另一个账号"; visible: typeof accountManager !== "undefined"; Layout.fillWidth: true; onClicked: accountManager.createAccount() }
            Action { iconName: "account"; text: "登录已有 UID"; visible: typeof accountManager !== "undefined"; Layout.fillWidth: true; onClicked: login.open() }
            }
        }
    }
    Sheet {
        id: login; title: "UID 账号登录"; standardButtons: Dialog.Cancel
        contentItem: ColumnLayout {
            TextField { id: loginUid; placeholderText: "UID"; inputMethodHints: Qt.ImhDigitsOnly; maximumLength: 16; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            TextField { id: loginPassword; placeholderText: "登录密码"; echoMode: TextInput.Password; maximumLength: 128; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            Label { text: "登录到新的本机资料，不覆盖当前聊天记录。密码丢失后无法恢复身份。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: window.subdued }
            Action { iconName: "account"; text: "登录"; enabled: loginUid.text.length > 0 && loginPassword.text.length >= 8; Layout.fillWidth: true; onClicked: { if (accountManager.beginLogin(loginUid.text,loginPassword.text)) { loginPassword.text = ""; login.close() } } }
        }
    }
    Sheet {
        id: settings; title: "设置"; standardButtons: Dialog.Close
        contentItem: ColumnLayout {
            spacing: 12
            Switch { text: "辅助连接（推荐）"; checked: messenger.assistedConnection; Layout.fillWidth: true; onClicked: messenger.setAssistedConnection(checked) }
            Action { iconName: "requests"; text: "开启系统消息提醒"; Layout.fillWidth: true; onClicked: messenger.enableMessageReminders() }
            Switch { visible: Qt.platform.os === "android"; text: "后台收消息（常驻通知）"; checked: typeof notificationService !== "undefined" && notificationService.backgroundEnabled; Layout.fillWidth: true; onClicked: { if(typeof notificationService!=="undefined")notificationService.setBackgroundEnabled(checked) } }
            Action { visible: Qt.platform.os === "android"; iconName:"settings";text:"后台运行与电池设置";Layout.fillWidth:true;onClicked:notificationService.openBackgroundSettings() }
            Label { visible: Qt.platform.os === "android"; text: "关闭界面后由后台服务接收。请允许通知和后台运行；系统强行停止或限制后台会阻止接收。"; font.pixelSize: 12; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: messenger.networkStatus; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: "辅助模式通过中继转发加密内容；纯直连不使用中继，跨网成功取决于网络条件。"; wrapMode: Text.Wrap; Layout.fillWidth: true }
            CheckBox { id: customRelay; text: "使用自己的中继"; checked: messenger.customRelay }
            TextField { id: relayAddress; visible: customRelay.checked; placeholderText: "wss://你的服务地址"; Layout.fillWidth: true; Layout.minimumWidth: 0; inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText }
            Action { visible: customRelay.checked; text: "仅保存到本机"; Layout.fillWidth: true; onClicked: messenger.setRelayEndpoint(relayAddress.text) }
            Action { visible: messenger.customRelay; text: "恢复默认辅助服务"; Layout.fillWidth: true; onClicked: messenger.restoreDefaultRelay() }
            Label { visible: customRelay.checked; text: "不会显示内置地址。更换服务后会重新注册 UID，双方需使用同一服务。"; wrapMode: Text.Wrap; color: window.subdued; Layout.fillWidth: true }
            Action { iconName: "copy"; text: "复制完整配对码（备用）"; Layout.fillWidth: true; onClicked: messenger.copyLocalPairingCode() }
        }
    }
    Sheet {
        id: addFriend; title: "添加好友"; standardButtons: Dialog.Cancel
        contentItem: ColumnLayout {
            spacing: 12
            TextField { id: friendUid; placeholderText: "输入对方的数字 UID"; inputMethodHints: Qt.ImhDigitsOnly; maximumLength: 16; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            TextField { id: friendRemark; placeholderText: "备注名（选填）"; maximumLength: 64; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            Label { text: "通过同一辅助服务查找账号，发送后需对方同意。"; color: window.subdued; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Action { iconName: "friend"; text: "查找并发送好友申请"; enabled: friendUid.text.trim().length > 0; Layout.fillWidth: true; onClicked: { if (messenger.addFriendByUid(friendUid.text, friendRemark.text)) addFriend.close() } }
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
            Action { iconName: "group"; text: "创建群聊"; enabled: groupName.text.trim().length > 0 && group.selected.length > 0; Layout.fillWidth: true; onClicked: { if (messenger.createGroup(groupName.text, group.selected)) group.close() } }
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
        Action { iconName: "emoji"; text: "发送自定义表情图片"; Layout.fillWidth: true; onClicked: { emojis.close(); messenger.chooseAttachment(true) } }
    } }
    FileDialog { id: avatarPicker; title: "选择头像"; nameFilters: ["图片 (*.png *.jpg *.jpeg *.webp)"]; onAccepted: messenger.setAvatar(selectedFile.toString()) }
    FileDialog { id: stickerPicker; title: "选择表情图片"; nameFilters: ["图片 (*.png *.jpg *.jpeg *.webp *.gif)"]; onAccepted: messenger.sendSticker(selectedFile.toString()) }
    FileDialog { id: filePicker; title: "发送文件"; onAccepted: messenger.queueFile(selectedFile.toString()) }
    FileDialog { id: photoPicker; title: "发送照片"; nameFilters: ["照片 (*.png *.jpg *.jpeg *.webp)"]; onAccepted: messenger.sendPhoto(selectedFile.toString()) }
    FileDialog { id: saveFile; title: "保存文件"; fileMode: FileDialog.SaveFile; property string interactionId: ""; property string fileId: ""; onAccepted: messenger.downloadFile(interactionId, fileId, selectedFile.toString()) }
    Popup { id: photoPreview; modal: true; anchors.centerIn: parent; width: window.width-24; height: window.height-48; padding: 12
        property string photo: ""; background: Rectangle {color:"#17202f";radius:14}
        contentItem: ColumnLayout { Image { source: photoPreview.photo; Layout.fillWidth: true; Layout.fillHeight: true; fillMode: Image.PreserveAspectFit } Action {text:"关闭";Layout.alignment:Qt.AlignHCenter;onClicked:photoPreview.close()} }
    }
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
                    Label { text: window.mobile ? ["消息","联系人","我"][window.mobileTab] : "双点聊"; font.bold: true; font.pixelSize: 23; color: "#22354e"; Layout.fillWidth: true; Layout.minimumWidth: 0 }
                    Action { iconName: "plus"; text: ""; implicitWidth:42; onClicked: listMenu.open(); Menu {id:listMenu;MenuItem{text:"添加好友";onTriggered:addFriend.open()} MenuItem{text:"新建群聊";onTriggered:group.open()} MenuItem{text:"好友申请（"+messenger.pendingRequests.length+"）";onTriggered:requests.open()} MenuItem{text:"我的账号";onTriggered:account.open()} MenuItem{text:"设置";onTriggered:settings.open()} } }
                }
                Label { text: messenger.profileName.length ? messenger.profileName + (messenger.userCode.length ? " · UID " + messenger.userCode : " · 待注册") : "欢迎，先创建账号"; color: "#263b56"; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }
                Label { text: messenger.networkStatus; color: window.subdued; wrapMode: Text.Wrap; font.pixelSize: 12; Layout.fillWidth: true; Layout.minimumWidth: 0 }
                RowLayout {
                    visible: !window.mobile
                    Layout.fillWidth: true
                    Action { iconName: "account"; text: "账号"; Layout.fillWidth: true; onClicked: account.open() }
                    Action { iconName: "friend"; text: "加好友"; Layout.fillWidth: true; enabled: messenger.profileName.length > 0; onClicked: addFriend.open() }
                    Action { iconName: "requests"; text: "申请 " + messenger.pendingRequests.length; Layout.fillWidth: true; onClicked: requests.open() }
                }
                TextField { visible: !window.mobile || window.mobileTab!==2; placeholderText:"搜索好友或群聊"; Layout.fillWidth:true; Layout.minimumWidth:0; onTextChanged:window.contactSearch=text; font.pixelSize:14 }
                Action { visible: messenger.pendingRequests.length>0 && (!window.mobile || window.mobileTab!==2); iconName:"requests"; text:"新的好友申请 · "+messenger.pendingRequests.length; Layout.fillWidth:true; onClicked:requests.open() }
                ColumnLayout { visible: window.mobile && window.mobileTab===2; Layout.fillWidth:true; spacing:14
                    Action { iconName:"account";text:"账号与个人资料";Layout.fillWidth:true;onClicked:account.open() }
                    Action { iconName:"copy";text:"我的 UID · "+(messenger.userCode||"未注册");Layout.fillWidth:true;onClicked:messenger.copyUid() }
                    Action { iconName:"requests";text:"好友与群聊申请";Layout.fillWidth:true;onClicked:requests.open() }
                    Action { iconName:"settings";text:"设置与后台消息";Layout.fillWidth:true;onClicked:settings.open() }
                }
                ListView {
                    visible: !window.mobile || window.mobileTab!==2
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 1
                    model: messenger.contacts.filter(function(row){return (!window.mobile || row.id!=="welcome") && (!window.contactSearch.length || row.name.toLowerCase().indexOf(window.contactSearch.toLowerCase())>=0)})
                    delegate: ItemDelegate {
                        required property var modelData; width: ListView.view.width; height: 76
                        background: Rectangle { radius: 10; color: messenger.activeContactId === modelData.id ? "#eaf4ff" : "transparent" }
                        onClicked: { messenger.selectContact(modelData.id); window.showThread = true }
                        contentItem: RowLayout {
                            Rectangle {
                                Layout.preferredWidth: 48; Layout.preferredHeight: 48; radius: 15; color: "#dbeeff"
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
                    Label { anchors.centerIn:parent; visible:parent.count===0; text:"还没有会话\n点击右上角 ＋ 添加好友"; horizontalAlignment:Text.AlignHCenter; color:window.subdued; lineHeight:1.7 }
                }
                Item { visible:window.mobile && window.mobileTab===2;Layout.fillHeight:true;Layout.fillWidth:true }
                RowLayout { visible:window.mobile;Layout.fillWidth:true;spacing:0
                    Repeater {model:[{name:"消息",icon:"requests"},{name:"联系人",icon:"group"},{name:"我",icon:"account"}]
                        delegate:Button {required property var modelData;required property int index;Layout.fillWidth:true;implicitHeight:58;flat:true;onClicked:window.mobileTab=index
                            contentItem:Column {spacing:4;FeatureIcon{anchors.horizontalCenter:parent.horizontalCenter;glyph:modelData.icon;width:23;height:23;tint:window.mobileTab===index?window.accent:window.subdued} Text{anchors.horizontalCenter:parent.horizontalCenter;text:modelData.name;font.pixelSize:12;color:window.mobileTab===index?window.accent:window.subdued}}
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
                    Action { iconName:"phone";text:"";Layout.preferredWidth:36;Layout.minimumWidth:36;onClicked:messenger.startCall() }
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
                                Image { id:messageImage; visible: modelData.kind === "sticker" || modelData.kind === "photo"; source: visible ? "data:image/"+(modelData.kind==="photo"?"jpeg":"png")+";base64," + modelData.fileData : ""; Layout.fillWidth: true; Layout.preferredHeight: visible ? 180 : 0; fillMode: Image.PreserveAspectFit
                                    MouseArea {anchors.fill:parent;onClicked:{photoPreview.photo=messageImage.source;photoPreview.open()}}
                                }
                                Action { iconName: "play"; text: "播放语音"; visible: modelData.kind === "voice"; onClicked: messenger.playVoice(modelData.fileData) }
                                Action { iconName: "save"; text: "保存文件"; visible: modelData.kind === "file-offer"; onClicked: { saveFile.interactionId = modelData.interactionId || ""; saveFile.fileId = modelData.fileId || ""; saveFile.open() } }
                                Label { text: (modelData.time || "") + " " + (modelData.delivery || ""); color: window.subdued; font.pixelSize: 10; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; wrapMode: Text.Wrap }
                            }
                        }
                    }
                }
                Rectangle { visible:window.expandedTools;Layout.fillWidth:true;implicitHeight:158;color:"white";radius:12
                    GridLayout { anchors.fill:parent;anchors.margins:10;columns:3;columnSpacing:10;rowSpacing:8
                        Repeater {model: Qt.platform.os === "android" ? [{name:"照片",icon:"photo"},{name:"拍照",icon:"camera"},{name:"文件",icon:"file"},{name:"语音通话",icon:"phone"},{name:"表情",icon:"emoji"},{name:"群昵称",icon:"group"}] : [{name:"照片",icon:"photo"},{name:"文件",icon:"file"},{name:"语音通话",icon:"phone"},{name:"表情",icon:"emoji"},{name:"群昵称",icon:"group"}]
                            delegate:Button {required property var modelData;required property int index;Layout.fillWidth:true;Layout.fillHeight:true;flat:true
                                contentItem:Column{spacing:7;FeatureIcon{anchors.horizontalCenter:parent.horizontalCenter;glyph:modelData.icon;width:26;height:26}Text{anchors.horizontalCenter:parent.horizontalCenter;text:modelData.name;font.pixelSize:12;color:"#44566e"}}
                                onClicked:{window.expandedTools=false;if(modelData.icon==="photo")messenger.choosePhoto(false);else if(modelData.icon==="camera")messenger.choosePhoto(true);else if(modelData.icon==="file")messenger.chooseAttachment(false);else if(modelData.icon==="phone")messenger.startCall();else if(modelData.icon==="emoji")emojis.open();else nickname.open()}
                            }
                        }
                    }
                }
                Label { visible:messenger.recording; text:holdArea.cancelVoice?"松开取消发送":"正在录音 · 上滑取消";color:holdArea.cancelVoice?"#d64c65":window.accent;Layout.alignment:Qt.AlignHCenter }
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    Action {iconName:window.voiceInput?"keyboard":"mic";text:"";Layout.preferredWidth:36;Layout.minimumWidth:36;onClicked:{messenger.finishVoice(false);window.voiceInput=!window.voiceInput;window.expandedTools=false}}
                    TextField { id: composer; objectName: "chatInput"; visible:!window.voiceInput; placeholderText: "输入消息"; Layout.fillWidth: true; Layout.minimumWidth: 0; selectByMouse: true; font.pixelSize:15; onAccepted: { if (messenger.sendMessage(text)) text = "" } }
                    Rectangle {visible:window.voiceInput;Layout.fillWidth:true;Layout.minimumWidth:0;implicitHeight:44;radius:8;color:holdArea.pressed?"#e0eafe":"white";border.color:"#d7e0ec"
                        Label {anchors.centerIn:parent;text:holdArea.pressed?holdArea.cancelVoice?"松开取消":"松开发送":"按住说话";font.pixelSize:15;color:"#44566e"}
                        MouseArea {id:holdArea;anchors.fill:parent;property real startY:0;property bool cancelVoice:false
                            onPressed:function(mouse){startY=mouse.y;cancelVoice=false;messenger.recordVoice()}
                            onPositionChanged:function(mouse){cancelVoice=mouse.y<startY-55}
                            onReleased:messenger.finishVoice(!cancelVoice)
                            onCanceled:messenger.finishVoice(false)
                        }
                    }
                    Action {iconName:"emoji";text:"";Layout.preferredWidth:36;Layout.minimumWidth:36;onClicked:emojis.open()}
                    Action {objectName:"chatFile";iconName:"plus";text:"";Layout.preferredWidth:36;Layout.minimumWidth:36;onClicked:window.expandedTools=!window.expandedTools}
                    Action { objectName: "chatSend"; text: "发送"; Layout.preferredWidth: 58; Layout.minimumWidth: 58; enabled: composer.text.trim().length > 0; onClicked: { if (messenger.sendMessage(composer.text)) composer.text = "" } }
                }
            }
        }
    }
}
