#pragma once
#include <QObject>
class NotificationService final : public QObject {
public:
    explicit NotificationService(QObject* parent = nullptr);
    ~NotificationService() override;
    void show(const QString& title, const QString& body);
    void requestPermission();
private:
    void* window_ {nullptr};
};
