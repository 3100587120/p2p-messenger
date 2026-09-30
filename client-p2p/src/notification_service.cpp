#include "notification_service.h"
#include <QGuiApplication>
#include <QWindow>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
namespace {
LRESULT CALLBACK notificationWindow(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_APP + 17 && (LOWORD(l) == NIN_BALLOONUSERCLICK || LOWORD(l) == WM_LBUTTONUP)) {
        for (auto* window : QGuiApplication::topLevelWindows()) if (window->isVisible()) { window->showNormal(); window->raise(); window->requestActivate(); break; }
        return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
}
#elif defined(Q_OS_ANDROID)
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#endif
NotificationService::NotificationService(QObject* parent) : QObject(parent) {
#ifdef Q_OS_WIN
    WNDCLASSW cls {}; cls.lpfnWndProc = notificationWindow; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"ShuangDianLiaoNotifications";
    RegisterClassW(&cls);
    window_ = CreateWindowExW(0,cls.lpszClassName,L"双点聊",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,nullptr);
    if (window_) {
        NOTIFYICONDATAW icon {}; icon.cbSize = sizeof(icon); icon.hWnd = static_cast<HWND>(window_); icon.uID=1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; icon.uCallbackMessage = WM_APP + 17;
        icon.hIcon = LoadIconW(nullptr,IDI_INFORMATION); wcscpy_s(icon.szTip,L"双点聊 · 消息提醒");
        Shell_NotifyIconW(NIM_ADD,&icon); icon.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION,&icon);
    }
#endif
}
NotificationService::~NotificationService() {
#ifdef Q_OS_WIN
    if (window_) { NOTIFYICONDATAW icon {}; icon.cbSize=sizeof(icon); icon.hWnd=static_cast<HWND>(window_); icon.uID=1; Shell_NotifyIconW(NIM_DELETE,&icon); DestroyWindow(static_cast<HWND>(window_)); }
#endif
}
void NotificationService::requestPermission() {
#ifdef Q_OS_ANDROID
    QJniObject::callStaticMethod<void>("io/p2pmessenger/app/MessageNotifications","requestPermission","(Landroid/content/Context;)V",QNativeInterface::QAndroidApplication::context().object<jobject>());
    QJniEnvironment().checkAndClearExceptions();
#endif
}
void NotificationService::show(const QString& title, const QString& body) {
    if (QGuiApplication::applicationState() == Qt::ApplicationActive) return; // Foreground uses the in-app banner.
#ifdef Q_OS_WIN
    if (!window_) return;
    NOTIFYICONDATAW icon {}; icon.cbSize=sizeof(icon); icon.hWnd=static_cast<HWND>(window_); icon.uID=1; icon.uFlags=NIF_INFO; icon.dwInfoFlags=NIIF_INFO;
    wcsncpy_s(icon.szInfoTitle,reinterpret_cast<const wchar_t*>(title.utf16()),_TRUNCATE);
    wcsncpy_s(icon.szInfo,reinterpret_cast<const wchar_t*>(body.utf16()),_TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY,&icon);
    for (auto* window : QGuiApplication::topLevelWindows()) if (window->isVisible()) { FLASHWINFO flash {sizeof(FLASHWINFO),reinterpret_cast<HWND>(window->winId()),FLASHW_TRAY,3,0}; FlashWindowEx(&flash); break; }
#elif defined(Q_OS_ANDROID)
    QJniObject::callStaticMethod<void>("io/p2pmessenger/app/MessageNotifications","show","(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)V",QNativeInterface::QAndroidApplication::context().object<jobject>(),QJniObject::fromString(title).object<jstring>(),QJniObject::fromString(body).object<jstring>());
    QJniEnvironment().checkAndClearExceptions();
#endif
}
