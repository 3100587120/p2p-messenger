#pragma once
#include <QObject>
#include <QTimer>
class NotificationService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool backgroundEnabled READ backgroundEnabled NOTIFY backgroundEnabledChanged)
    Q_PROPERTY(QString backgroundStatus READ backgroundStatus NOTIFY backgroundStatusChanged)
public:
    explicit NotificationService(QObject* parent = nullptr);
    ~NotificationService() override;
    void show(const QString& title, const QString& body);
    void requestPermission();
    bool backgroundEnabled() const;
    Q_INVOKABLE void setBackgroundEnabled(bool enabled);
    Q_INVOKABLE void openBackgroundSettings();
    Q_INVOKABLE void openPermissionSettings();
    Q_INVOKABLE void moveToBackground();
    Q_INVOKABLE bool notificationsAllowed() const;
    void accountReady();
    void setSessionRoot(const QString& root);
    QString backgroundStatus() const {return backgroundStatus_;}
signals:
    void backgroundEnabledChanged();
    void backgroundError(const QString& message);
    void backgroundStatusChanged();
private:
    void* window_ {nullptr};
    QString root_,backgroundStatus_;
    QTimer healthTimer_;
    qint64 serviceStartedAt_{0};
    bool missingServiceReported_{false};
    void refreshBackgroundStatus();
};
