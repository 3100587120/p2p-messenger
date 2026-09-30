# 双点聊 0.4.10 测试包（2026-09-30）

用户要求打包当前代码，并删除以前的构建产物。本轮没有部署新中继。

## 交付

- APK：`D:\p2p-messenger\dist\ShuangDianLiao-Android-0.4.10.apk`
  - SHA-256：`CD5CA97385435B07E47767958DF2F38705451E84A9FF87E00AD39EA51369F10B`
- Windows ZIP：`D:\p2p-messenger\dist\ShuangDianLiao-Windows-0.4.10.zip`
  - SHA-256：`CBF1F636D63CEEB7843F6213EE4C35C6FD7AE217CED117CC8C8B4AC3214426B2`
- 已解压 EXE：`D:\codex\双点聊-0.4.10\双点聊.exe`
  - SHA-256：`C4680947ADB2FEC53A906E2DF85110B31CF03C93BA7AF6BCD2678E527F755DCF`

使用说明随 Windows ZIP 一起交付，并在 dist 中保留。不要仅复制 EXE；运行库和 QML 目录必须一同保留。

## 验证与限制

- 包含 `9ca470e` 的好友持久化、加密回执、断连提示修复及之前的空配置安全恢复。
- Android versionCode 14 / versionName 0.4.10-test / arm64-v8a；应用标签“ 双点聊 ”，存在正常启动入口，保持竖屏设置。
- APK v1/v2 签名验证成功，新旧证书 SHA-256 相同：`27f1830b1030c3e9ca9021234014301760d8032e18db1e50ac49f91c430bbaea`。可覆盖升级 0.4.9，不需要卸载；仍为 debug 签名测试包。
- APK 包含 OpenSSL 3、Qt OpenSSL TLS 后端、Qt Android 平台插件和 Jami 内核。
- Windows 从实际解压目录使用一次性账号、禁用直连内核、离屏软件渲染启动，日志到达 qml-loaded，退出码 0。没有操作原有账号或实际用户窗口。
- Windows / Android 均使用既有 D 盘工具和增量缓存构建；Android 内核无须重编，Gradle 报告 BUILD SUCCESSFUL。
- 未进行真实手机安装或手机蜂窝网络验收。
- **国内免 VPN 异网连接仍未解决。** 保留原来配置的 Cloudflare Workers 中继；默认纯直连不使用公共中继，辅助模式仍可能因入口不可达而超时。本次版本号和构建成功不表示网络问题已经解决。

## 清理范围

- 删除 29 个已核对路径和 CMake 源目录的废弃构建/旧分发目录，及 36 个旧 APK/ZIP/EXE，约 2.87 GiB。
- 新包验证后再删除最后两份 0.4.9 回滚分发包和一份重复 APK，以及已经退出的 `D:\codex\双点聊-0.4.9` 旧安装目录。
- dist 仅保留 0.4.10 APK、Windows ZIP 和说明。未删除源码、SDK/JDK/Qt、当前增量构建缓存、用户密钥或聊天记录；可重新构建旧产物，但已删除的二进制不提供本地回收恢复。
- 当次收尾 C 盘约 4.4 GiB、D 盘约 3.5 GiB 可用。本轮未写入额外 C 盘构建缓存。
