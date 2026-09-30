# Offline emoji font

`NotoColorEmoji_WindowsCompatible.ttf` is bundled so emoji do not depend on the
Android system font version or a font download service. The application loads
it before constructing the QML engine and uses Qt text rendering for emoji.

- Upstream: https://github.com/googlefonts/noto-emoji
- Pinned release: `v2.047`, `fonts/NotoColorEmoji_WindowsCompatible.ttf`
- Source: https://raw.githubusercontent.com/googlefonts/noto-emoji/v2.047/fonts/NotoColorEmoji_WindowsCompatible.ttf
- SHA-256: `A739BBA06F5F2EA260311FADCFD01EF389A192535323995CD30F6717303272F8`
- License: SIL Open Font License 1.1, included in `OFL.txt` and the application resources.

Implementation reference: https://doc.qt.io/qt-6.8/android-emojis.html
No QQ assets or proprietary emoji images are copied.
