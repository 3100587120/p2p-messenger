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
    if (daemon_.start()) {
        accountId_ = daemon_.createLocalIdentity(tr("我的设备"));
        if (!accountId_.isEmpty())
            networkStatus_ = tr("私有通信内核已启动 — 等待自建服务配置");
    }
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
    const auto verified = accountId_.isEmpty()
        || daemon_.addVerifiedContact(accountId_, invite.trimmed());
    const auto conversationId = verified && !accountId_.isEmpty()
        ? daemon_.createConversation(accountId_, invite.trimmed()) : QString {};
    auto entry = contact(id, trimmedName, verified ? tr("等待验证") : tr("需要通信内核"));
    entry.insert(QStringLiteral("uri"), invite.trimmed());
    entry.insert(QStringLiteral("conversationId"), conversationId);
    contacts_.append(entry);
    emit contactsChanged();
    selectContact(id);
}

void MessengerController::createGroup(const QString& name, const QStringList& memberUris)
{
    const auto groupName = name.trimmed();
    if (groupName.isEmpty())
        return;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto conversationId = !accountId_.isEmpty() ? daemon_.createEmptyConversation(accountId_) : QString {};
    if (!accountId_.isEmpty() && conversationId.isEmpty())
        return;
    for (const auto& uri : memberUris) {
        const auto member = uri.trimmed();
        if (!member.isEmpty() && !accountId_.isEmpty())
            daemon_.addGroupMember(accountId_, conversationId, member);
    }
    auto entry = contact(id, groupName, tr("群聊"));
    entry.insert(QStringLiteral("initial"), tr("群"));
    entry.insert(QStringLiteral("conversationId"), conversationId);
    entry.insert(QStringLiteral("group"), true);
    contacts_.append(entry);
    emit contactsChanged();
    selectContact(id);
}

void MessengerController::sendMessage(const QString& body)
{
    const auto text = body.trimmed();
    if (text.isEmpty())
        return;
    for (const auto& item : contacts_) {
        const auto current = item.toMap();
        if (current.value(QStringLiteral("id")).toString() != activeContactId_)
            continue;
        const auto conversationId = current.value(QStringLiteral("conversationId")).toString();
        if (!accountId_.isEmpty() && !conversationId.isEmpty()
            && !daemon_.sendText(accountId_, conversationId, text))
            return;
        appendMessage(text, true);
        return;
    }
}

void MessengerController::queueFile(const QString& path)
{
    if (path.isEmpty())
        return;
    for (const auto& item : contacts_) {
        const auto current = item.toMap();
        if (current.value(QStringLiteral("id")).toString() != activeContactId_)
            continue;
        const auto conversationId = current.value(QStringLiteral("conversationId")).toString();
        if (!accountId_.isEmpty() && !conversationId.isEmpty()
            && !daemon_.sendFile(accountId_, conversationId, path))
            return;
        appendMessage(tr("文件：%1（等待端到端传输）").arg(QFileInfo(path).fileName()), true,
                      QStringLiteral("file"));
        return;
    }
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
