#pragma once
#include <QObject>
#include <QLockFile>
#include <QTimer>
#include <functional>

// One session writer across the GUI and the Android headless service.
class BackgroundSession : public QObject {
public:
    explicit BackgroundSession(const QString& root);
    ~BackgroundSession();
    bool claimInitial();
    bool ownsSession() const { return owned_; }
    void setCallbacks(std::function<void()> suspend,std::function<void()> resume);
    void setActive(bool active);
    static bool foregroundRequested(const QString& root);
private:
    void lease(bool active);
    QString root_;
    QLockFile lock_;
    QTimer timer_;
    bool active_{true},owned_{false};
    std::function<void()> suspend_,resume_;
};
#ifdef Q_OS_ANDROID
int runMessageService(int argc,char** argv);
#endif
