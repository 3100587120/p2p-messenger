# 0.5.2：媒体崩溃修复、移动布局与后台消息

## 已实现

- 修复 `VoiceEngine::stopCapture()` 使用 `QByteArray::first(1920000)` 读取短录音/空录音的越界。改用会截断长度的 `left()`；挂断、取消、选择器切换和析构都覆盖同一停止路径。
- 通话状态先归零再停止音频，避免状态回调重入。对方离线或连接丢失自动结束通话。
- 安卓相册/文档选择使用独立系统选择器 Activity，后台有界解码、复制，回到 Qt 主线程处理结果；照片入口与系统拍照入口均已接入。拍照 FileProvider 仅额外共享 camera 缓存目录。
- 蓝白界面、底部消息/联系人/我、功能图标、加号菜单。聊天工具抽屉包括照片、文件、通话、表情和群昵称；安卓另外包含拍照。Windows 未实现摄像头拍照，不显示该按钮。
- 按住说话、松开发送、上滑取消。录音最长 60 秒，辅助连接文件最大 2 MB，超限弹窗而不是继续读取大文件。
- 安卓新增独立 `:messages` QtService 进程，默认开启常驻通知。界面隐藏后接管加密连接、保存消息并触发系统通知；重新打开界面时重新加载本地资料。
- 前后台共用会话写入锁与前台租约；停止旧连接并拒绝其缓冲帧后才释放锁。切换账号重建窗口期间不误触发后台交接。相册/文件选择器或麦克风授权期间保持前台会话，避免选择结果落入旧账号。
- 设置提供后台消息开关、通知授权、后台电池设置入口。已经注册的用户可以在重启后由启动广播恢复后台服务。

## 验证结果与边界

Windows/Android 编译成功。APK versionCode 17，versionName 0.5.2-test，同一调试签名，可覆盖旧版。

以下检查通过：

- 空、短、满长、超长模拟录音停止；空通话挂断；重复停止。
- 前台/后台独占写入锁交接。
- 头像、照片、表情、文件排队、群昵称、语音接收与存储、通话离线结束。
- 好友持久化失败不虚报送达、身份/历史恢复、密码加密备份、伪回执拒绝。
- QML 桌面和 320/390 宽竖屏按钮可见且不越界；截图位于 work/ui-0.5.2-fonts。
- **公网 WSS 无界面 QCoreApplication 接收**：文字、JPEG 照片、表情图片、模拟 PCM 语音、文件实际经过部署的中继并触发接收通知信号。此测试未注册任何 UID，也未清空现有账号。
- 9 项中继/UID 路由与目录单元测试。
- APK 签名、独立进程/服务参数和拍照 FileProvider 资源检查。
- 解压后的 Windows EXE 隔离资料启动，退出码 0，无 QML 警告。

**未验证**：没有连接安卓真机，不能宣称 OEM 后台保活、锁屏/划掉界面的通知、系统相册/拍照实际返回、实机麦克风录制播放/挂断都已通过。公网检查是在这台电脑的两个独立身份之间经过公网服务，不是两台真实异网设备实测。

后台接管目前仅支持辅助连接模式，不运行纯直连引擎。后台服务并不绕过系统强行停止、通知关闭、厂商省电限制和 Doze 网络限制；需要用户首次打开登录并允许通知和后台运行。后台收到语音来电目前显示未接通话提醒并结束该次呼叫，需要打开聊天后重新呼叫。没有集成付费推送或新的第三方推送服务。

现有本机/线上账号、主人设备资格、聊天记录均保留；没有再次执行账号重置。iOS 与中继部署不在本次改动范围。

## 文件

- APK：`dist/ShuangDianLiao-Android-0.5.2.apk`
- Windows ZIP：`dist/ShuangDianLiao-Windows-0.5.2.zip`
- 已解压 EXE：`D:/codex/双点聊-0.5.2/双点聊.exe`
- APK SHA256：`346489FFCDAFA26960AAA635368F15E0CCD3AED549A81FD10C867D516EEE4D7C`
- ZIP SHA256：`28331C3CED965ECA3425FA9926BB6FE85647647558DC2E970D7E3F7888E0AA37`
- EXE SHA256：`825973A7CC9B36E21940F01CBE7536796C1E7E97C3F07474893B8C152FBBF848`

## 复测命令

```powershell
$env:TEMP='D:/p2p-messenger/work/build-temp'; $env:TMP=$env:TEMP
$test='D:/p2p-messenger/work/client-msvc/Release/P2PMessengerFriendFlowWithoutDaemonTest.exe'
& $test --voice-stop-regression
& $test --background-session-regression
& $test --feature-regression
& $test --friend-persistence-regression
& $test --identity-regression
& $test --empty-profile-regression
& $test --qml-smoke --qml-path D:/p2p-messenger/client-p2p/qml/Main.qml
# 公网模式需将 P2P_MESSENGER_RELAY_URL 设置为运营者的 WSS 地址：
& $test --public-headless-media-e2e
```

## 参考

- [Qt Android Services](https://doc.qt.io/qt-6/android-services.html)：独立进程 QtService、同一原生库不同启动参数。
- [Android 前台服务类型](https://developer.android.com/develop/background-work/services/fgs/service-types)：remoteMessaging。
- [Android Doze 和 App Standby](https://developer.android.com/training/monitoring-device-state/doze-standby)：后台消息与省电边界。
