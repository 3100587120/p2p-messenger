#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

class LocalVault;

// An identity is generated locally and stored only in LocalVault. The relay
// sees routing hashes and ciphertext, never the shared secret or plaintext.
class RelayCrypto final
{
public:
    bool loadOrCreate(LocalVault& vault);
    QString identityId() const;
    QString inviteCode() const;
    QJsonObject makeLoginRecord(const QString& password, const QString& name) const;
    static QString loginToken(const QString& password, const QString& salt, int iterations);
    bool restoreLoginRecord(LocalVault& vault, const QString& password, const QJsonObject& record);
    static QByteArray publicKeyFromCode(const QString& code);
    static QString idForPublicKey(const QByteArray& key);

    QByteArray seal(const QByteArray& recipientPublicKey, const QString& packetId,
                    const QJsonObject& message) const;
    QJsonObject open(const QString& senderId, const QString& packetId,
                     const QByteArray& envelope, bool* ok) const;

private:
    QByteArray privateKey_;
    QByteArray publicKey_;
};
