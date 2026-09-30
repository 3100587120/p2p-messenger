#include "background_session.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>

BackgroundSession::BackgroundSession(const QString& root):root_(root),lock_(QDir(root).filePath("session-owner.lock")) {
    QDir().mkpath(root); lock_.setStaleLockTime(0); timer_.setInterval(1000);
    connect(&timer_,&QTimer::timeout,this,[this] {
        if (!active_) return;
        lease(true);
        if (!owned_ && lock_.tryLock(0)) { owned_=true; if(resume_)resume_(); }
    });
}
BackgroundSession::~BackgroundSession() { lease(false); if(owned_)lock_.unlock(); }
void BackgroundSession::lease(bool active) {
    QSaveFile file(QDir(root_).filePath("foreground-lease.json"));
    if(file.open(QIODevice::WriteOnly)) { file.write(QJsonDocument(QJsonObject {{"active",active},{"until",QDateTime::currentMSecsSinceEpoch()+(active?12000:0)}}).toJson(QJsonDocument::Compact)); file.commit(); }
}
bool BackgroundSession::foregroundRequested(const QString& root) {
    QFile file(QDir(root).filePath("foreground-lease.json")); if(!file.open(QIODevice::ReadOnly))return false;
    const auto value=QJsonDocument::fromJson(file.readAll()).object();
    return value.value("active").toBool() && value.value("until").toInteger()>QDateTime::currentMSecsSinceEpoch();
}
bool BackgroundSession::claimInitial() { lease(true); owned_=lock_.tryLock(3000); timer_.start(); return owned_; }
void BackgroundSession::setCallbacks(std::function<void()> suspend,std::function<void()> resume) { suspend_=std::move(suspend); resume_=std::move(resume); }
void BackgroundSession::setActive(bool active) {
    active_=active;
    if(!active && owned_) { if(suspend_)suspend_(); owned_=false; lock_.unlock(); }
    lease(active);
}
#ifdef Q_OS_ANDROID
#include <QtCore/private/qandroidextras_p.h>
#include "account_manager.h"
#include "messenger_controller.h"
#include "notification_service.h"
#include <QStandardPaths>
#include <memory>
int runMessageService(int argc,char** argv) {
    qputenv("P2P_MESSENGER_DISABLE_DIRECT_ENGINE","1");
    qputenv("QT_BLOCK_EVENT_LOOPS_WHEN_SUSPENDED","0");
    QAndroidService app(argc,argv);
    QCoreApplication::setApplicationName("P2P Messenger"); QCoreApplication::setOrganizationName("P2P Messenger");
    const auto base=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(base); QLockFile lock(QDir(base).filePath("session-owner.lock")); lock.setStaleLockTime(0);
    std::unique_ptr<MessengerController> controller; QString currentRoot; bool owned=false;
    NotificationService notifications;
    QTimer timer; timer.setInterval(250);
    QObject::connect(&timer,&QTimer::timeout,&app,[&] {
        if(BackgroundSession::foregroundRequested(base)) {
            if(owned){controller.reset();currentRoot.clear();lock.unlock();owned=false;} return;
        }
        if(!owned){if(!lock.tryLock(0))return;owned=true;}
        qputenv("P2P_MESSENGER_DATA_ROOT",base.toUtf8());
        AccountManager accounts; const auto root=accounts.activeRoot();
        if(controller && currentRoot==root)return;
        controller.reset(); currentRoot=root; qputenv("P2P_MESSENGER_DATA_ROOT",root.toUtf8()); qputenv("P2P_MESSENGER_ACCOUNT_PROFILE","1");
        controller=std::make_unique<MessengerController>();
        QObject::connect(controller.get(),&MessengerController::incomingNotice,&notifications,&NotificationService::show);
        // Background mode never opens a microphone or attempts to answer a call.
        QObject::connect(controller.get(),&MessengerController::voiceChanged,&app,[&] {
            if(controller && controller->callState()=="ringing") { notifications.show("未接语音通话",controller->callPeerName()+"：请打开聊天后重新呼叫");controller->endCall(); }
        });
    });
    timer.start(); return app.exec();
}
#endif
