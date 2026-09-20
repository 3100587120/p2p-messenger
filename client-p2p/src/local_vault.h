#pragma once

#include <QString>
#include <QVariantList>

// Stores UI-visible conversation history in a per-user encrypted vault.  The
// encryption key never leaves the operating system's protected key store.
class LocalVault final
{
public:
    LocalVault();

    bool isReady() const;
    QString error() const;
    QVariantList loadConversation(const QString& conversationId) const;
    bool saveConversation(const QString& conversationId, const QVariantList& messages);

private:
    QString rootPath_;
    QByteArray masterKey_;
    QString error_;

    bool initialise();
    QByteArray encrypt(const QByteArray& plain) const;
    QByteArray decrypt(const QByteArray& encrypted) const;
    QString conversationPath(const QString& conversationId) const;
};
