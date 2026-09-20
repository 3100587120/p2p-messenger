#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>

#include "daemon_bridge.h"

class MessengerController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList contacts READ contacts NOTIFY contactsChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(QString activeContactId READ activeContactId NOTIFY activeContactChanged)
    Q_PROPERTY(QString activeContactName READ activeContactName NOTIFY activeContactChanged)
    Q_PROPERTY(QString networkStatus READ networkStatus NOTIFY networkStatusChanged)

public:
    explicit MessengerController(QObject* parent = nullptr);

    QVariantList contacts() const;
    QVariantList messages() const;
    QString activeContactId() const;
    QString activeContactName() const;
    QString networkStatus() const;

    Q_INVOKABLE void selectContact(const QString& contactId);
    Q_INVOKABLE void addContact(const QString& name, const QString& invite);
    Q_INVOKABLE void createGroup(const QString& name, const QStringList& memberUris);
    Q_INVOKABLE void sendMessage(const QString& body);
    Q_INVOKABLE void queueFile(const QString& path);

signals:
    void contactsChanged();
    void messagesChanged();
    void activeContactChanged();
    void networkStatusChanged();

private:
    QVariantList contacts_;
    QVariantList messages_;
    QString activeContactId_;
    QString networkStatus_;
    QString accountId_;
    DaemonBridge daemon_;

    void appendMessage(const QString& body, bool outgoing, const QString& kind = QStringLiteral("text"));
    QString contactName(const QString& id) const;
};
