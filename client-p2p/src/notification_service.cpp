#include "notification_service.h"
#include <QGuiApplication>
#include <QWindow>
#include <QDesktopServices>
#include <QUrl>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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
    healthTimer_.setInterval(3000);
    connect(&healthTimer_,&QTimer::timeout,this,&NotificationService::refreshBackgroundStatus);
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
bool NotificationService::backgroundEnabled() const {
#ifdef Q_OS_ANDROID
    const auto result=QJniObject::callStaticMethod<jboolean>("io/p2pmessenger/app/MessageService","enabled","(Landroid/content/Context;)Z",QNativeInterface::QAndroidApplication::context().object<jobject>());
    QJniEnvironment().checkAndClearExceptions(); return result;
#else
    return false;
#endif
}
void NotificationService::setBackgroundEnabled(bool enabled) {
#ifdef Q_OS_ANDROID
    const auto ok=QJniObject::callStaticMethod<jboolean>("io/p2pmessenger/app/MessageService","configure","(Landroid/content/Context;Z)Z",QNativeInterface::QAndroidApplication::context().object<jobject>(),jboolean(enabled));
    if(QJniEnvironment().checkAndClearExceptions() || !ok) emit backgroundError(tr("后台连接服务启动失败，请在系统设置允许后台运行"));
#else
    Q_UNUSED(enabled);
#endif
    emit backgroundEnabledChanged();
    if(enabled) {serviceStartedAt_=QDateTime::currentMSecsSinceEpoch();missingServiceReported_=false;}
    refreshBackgroundStatus();
}
void NotificationService::accountReady() {
#ifdef Q_OS_ANDROID
    const auto ok=QJniObject::callStaticMethod<jboolean>("io/p2pmessenger/app/MessageService","accountReady","(Landroid/content/Context;)Z",QNativeInterface::QAndroidApplication::context().object<jobject>());
    if(QJniEnvironment().checkAndClearExceptions() || !ok) emit backgroundError(tr("后台收消息服务未能启动，请允许后台运行后重试"));
    if(!serviceStartedAt_)serviceStartedAt_=QDateTime::currentMSecsSinceEpoch();
    refreshBackgroundStatus();
#endif
}
void NotificationService::setSessionRoot(const QString& root) {
    root_=root;
#ifdef Q_OS_ANDROID
    healthTimer_.start();refreshBackgroundStatus();
#endif
}
void NotificationService::refreshBackgroundStatus() {
#ifdef Q_OS_ANDROID
    QString status;
    QFile file(QDir(root_).filePath("background-status.json"));QJsonObject health;
    if(file.open(QIODevice::ReadOnly))health=QJsonDocument::fromJson(file.readAll()).object();
    const auto now=QDateTime::currentMSecsSinceEpoch();
    const auto heartbeat=health.value("at").toInteger();
    const bool alive=heartbeat>0 && now>=heartbeat && now-heartbeat<15000;
    if(!backgroundEnabled())status=tr("后台收消息已关闭");
    else if(!notificationsAllowed())status=tr("通知权限未开启，无法显示新消息提醒");
    else if(alive)status=health.value("ownsSession").toBool()?(health.value("connected").toBool()?tr("后台消息连接正常"):tr("后台正在连接，请检查网络")):tr("后台服务已就绪");
    else if(!serviceStartedAt_)status=tr("登录后启动后台收消息");
    else if(now-serviceStartedAt_<20000)status=tr("后台服务正在启动");
    else {
        status=tr("后台服务没有运行，请允许后台运行并重新打开应用");
        if(!missingServiceReported_ && qobject_cast<QGuiApplication*>(QCoreApplication::instance()) && QGuiApplication::applicationState()==Qt::ApplicationActive){missingServiceReported_=true;emit backgroundError(status);}
    }
    if(alive)missingServiceReported_=false;
    if(status!=backgroundStatus_){backgroundStatus_=status;emit backgroundStatusChanged();}
#endif
}
bool NotificationService::notificationsAllowed() const {
#ifdef Q_OS_ANDROID
    const auto result=QJniObject::callStaticMethod<jboolean>("io/p2pmessenger/app/MessageNotifications","allowed","(Landroid/content/Context;)Z",QNativeInterface::QAndroidApplication::context().object<jobject>());
    return !QJniEnvironment().checkAndClearExceptions() && result;
#else
    return true;
#endif
}
void NotificationService::openBackgroundSettings() {
#ifdef Q_OS_ANDROID
    const auto ok=QJniObject::callStaticMethod<jboolean>("io/p2pmessenger/app/MessageService","openBackgroundSettings","(Landroid/content/Context;)Z",QNativeInterface::QAndroidApplication::context().object<jobject>());
    if(QJniEnvironment().checkAndClearExceptions() || !ok)emit backgroundError(tr("无法打开电池设置，请在系统设置中允许双点聊后台运行"));
#endif
}
void NotificationService::requestPermission() {
#ifdef Q_OS_ANDROID
    QJniObject::callStaticMethod<void>("io/p2pmessenger/app/MessageNotifications","requestPermission","(Landroid/content/Context;)V",QNativeInterface::QAndroidApplication::context().object<jobject>());
    QJniEnvironment().checkAndClearExceptions();
#endif
}
void NotificationService::openPermissionSettings() {
#ifdef Q_OS_ANDROID
    const auto ok=QJniObject::callStaticMethod<jboolean>("io/p2pmessenger/app/MessageService","openAppSettings","(Landroid/content/Context;)Z",QNativeInterface::QAndroidApplication::context().object<jobject>());
    if(QJniEnvironment().checkAndClearExceptions() || !ok)emit backgroundError(tr("系统应用设置无法打开，请手动打开双点聊的权限设置"));
#elif defined(Q_OS_WIN)
    QDesktopServices::openUrl(QUrl("ms-settings:privacy-microphone"));
#endif
}
void NotificationService::moveToBackground() {
#ifdef Q_OS_ANDROID
    QJniObject::callStaticMethod<void>("io/p2pmessenger/app/MessageService","moveToBackground","(Landroid/content/Context;)V",QNativeInterface::QAndroidApplication::context().object<jobject>());QJniEnvironment().checkAndClearExceptions();
#endif
}
void NotificationService::show(const QString& title, const QString& body) {
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) && QGuiApplication::applicationState() == Qt::ApplicationActive) return;
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
