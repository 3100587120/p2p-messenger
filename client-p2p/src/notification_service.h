#pragma once
#include <QObject>
class NotificationService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool backgroundEnabled READ backgroundEnabled NOTIFY backgroundEnabledChanged)
public:
    explicit NotificationService(QObject* parent = nullptr);
    ~NotificationService() override;
    void show(const QString& title, const QString& body);
    void requestPermission();
    bool backgroundEnabled() const;
    Q_INVOKABLE void setBackgroundEnabled(bool enabled);
    Q_INVOKABLE void openBackgroundSettings();
    void accountReady();
signals:
    void backgroundEnabledChanged();
    void backgroundError(const QString& message);
private:
    void* window_ {nullptr};
};
