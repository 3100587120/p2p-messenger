#include "messenger_controller.h"

#include <QDateTime>
#include <QFileInfo>
#include <QUuid>

namespace {
QVariantMap contact(const QString& id, const QString& name, const QString& status)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("initial"), name.left(1).toUpper()},
            {QStringLiteral("status"), status}};
}
}

MessengerController::MessengerController(QObject* parent)
    : QObject(parent)
    , networkStatus_(tr("仅本地模式 — 尚未配置自建服务"))
{
    contacts_.append(contact(QStringLiteral("welcome"), tr("开始使用"), tr("本设备")));
    activeContactId_ = QStringLiteral("welcome");
    appendMessage(tr("欢迎使用 P2P Messenger。创建或扫描好友邀请码后，即可建立端到端加密连接。"), false);
}

QVariantList MessengerController::contacts() const { return contacts_; }
QVariantList MessengerController::messages() const { return messages_; }
QString MessengerController::activeContactId() const { return activeContactId_; }
QString MessengerController::activeContactName() const { return contactName(activeContactId_); }
QString MessengerController::networkStatus() const { return networkStatus_; }

void MessengerController::selectContact(const QString& contactId)
{
    if (activeContactId_ == contactId)
        return;
    activeContactId_ = contactId;
    messages_.clear();
    appendMessage(tr("这是与 %1 的本地加密会话视图。通信内核接入后，消息仅发送给已验证设备。")
                      .arg(contactName(contactId)), false);
    emit activeContactChanged();
    emit messagesChanged();
}

void MessengerController::addContact(const QString& name, const QString& invite)
{
    const auto trimmedName = name.trimmed();
    if (trimmedName.isEmpty() || invite.trimmed().isEmpty())
        return;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    contacts_.append(contact(id, trimmedName, tr("等待验证")));
    emit contactsChanged();
    selectContact(id);
}

void MessengerController::sendMessage(const QString& body)
{
    if (!body.trimmed().isEmpty())
        appendMessage(body.trimmed(), true);
}

void MessengerController::queueFile(const QString& path)
{
    if (!path.isEmpty())
        appendMessage(tr("文件：%1（等待端到端传输）").arg(QFileInfo(path).fileName()), true,
                      QStringLiteral("file"));
}

void MessengerController::appendMessage(const QString& body, bool outgoing, const QString& kind)
{
    const QVariantMap message {{QStringLiteral("body"), body},
                               {QStringLiteral("outgoing"), outgoing},
                               {QStringLiteral("kind"), kind},
                               {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))}};
    messages_.append(message);
    emit messagesChanged();
}

QString MessengerController::contactName(const QString& id) const
{
    for (const auto& item : contacts_) {
        const auto map = item.toMap();
        if (map.value(QStringLiteral("id")).toString() == id)
            return map.value(QStringLiteral("name")).toString();
    }
    return {};
}
