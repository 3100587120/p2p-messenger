#pragma once

#include <QString>
#include <QVariantList>

// Stores UI-visible conversation history in a per-user encrypted vault.  The
// encryption key never leaves the operating system's protected key store.
class LocalVault final
{
public:
    explicit LocalVault(const QString& dataRoot = {});

    bool isReady() const;
    QString error() const;
    QString rootPath() const {return rootPath_;}
    QVariantList loadConversation(const QString& conversationId) const;
    bool hasConversation(const QString& conversationId) const;
    bool saveConversation(const QString& conversationId, const QVariantList& messages);
    // Only an exactly empty file can be rebuilt. Nonempty unreadable ciphertext
    // is never discarded. A readable encrypted backup takes priority.
    bool recoverEmptyConversation(const QString& conversationId, const QVariantList& fallback);

private:
    QString rootPath_;
    QByteArray masterKey_;
    mutable QString error_;

    bool initialise();
    QByteArray encrypt(const QByteArray& plain) const;
    QByteArray decrypt(const QByteArray& encrypted) const;
    QString conversationPath(const QString& conversationId) const;
    QVariantList readConversationFile(const QString& path) const;
};
