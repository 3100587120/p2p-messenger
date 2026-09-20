#include "local_vault.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <openssl/evp.h>
#include <openssl/rand.h>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace {
constexpr auto keySize = 32;
constexpr auto nonceSize = 12;
constexpr auto tagSize = 16;
const QByteArray magic("P2PV1");

#ifdef Q_OS_WIN
QByteArray protectForCurrentUser(const QByteArray& key)
{
    DATA_BLOB input {static_cast<DWORD>(key.size()), reinterpret_cast<BYTE*>(const_cast<char*>(key.constData()))};
    DATA_BLOB output {};
    if (!CryptProtectData(&input, L"P2P Messenger local vault", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        return {};
    QByteArray protectedKey(reinterpret_cast<const char*>(output.pbData), static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return protectedKey;
}

QByteArray unprotectForCurrentUser(const QByteArray& protectedKey)
{
    DATA_BLOB input {static_cast<DWORD>(protectedKey.size()), reinterpret_cast<BYTE*>(const_cast<char*>(protectedKey.constData()))};
    DATA_BLOB output {};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        return {};
    QByteArray key(reinterpret_cast<const char*>(output.pbData), static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return key;
}
#endif
}

LocalVault::LocalVault()
    : rootPath_(QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                    .filePath(QStringLiteral("vault")))
{
    initialise();
}

bool LocalVault::isReady() const { return masterKey_.size() == keySize; }
QString LocalVault::error() const { return error_; }

bool LocalVault::initialise()
{
    if (!QDir().mkpath(rootPath_)) {
        error_ = QStringLiteral("无法创建本地加密存储目录");
        return false;
    }
    const auto keyPath = QDir(rootPath_).filePath(QStringLiteral("master-key.protected"));
    QFile keyFile(keyPath);
    if (keyFile.exists()) {
        if (!keyFile.open(QIODevice::ReadOnly)) {
            error_ = QStringLiteral("无法读取本地密钥");
            return false;
        }
#ifdef Q_OS_WIN
        masterKey_ = unprotectForCurrentUser(keyFile.readAll());
#else
        error_ = QStringLiteral("此平台的安全密钥存储尚未接入");
        return false;
#endif
        if (masterKey_.size() != keySize) {
            error_ = QStringLiteral("本地密钥无法由当前设备解锁");
            masterKey_.clear();
            return false;
        }
        return true;
    }
    QByteArray key(keySize, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), key.size()) != 1) {
        error_ = QStringLiteral("无法生成本地加密密钥");
        return false;
    }
#ifdef Q_OS_WIN
    const auto protectedKey = protectForCurrentUser(key);
    if (protectedKey.isEmpty()) {
        error_ = QStringLiteral("Windows 未能保护本地密钥");
        return false;
    }
    QSaveFile output(keyPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(protectedKey) != protectedKey.size() || !output.commit()) {
        error_ = QStringLiteral("无法保存受保护的本地密钥");
        return false;
    }
    masterKey_ = key;
    return true;
#else
    Q_UNUSED(key)
    error_ = QStringLiteral("此平台的安全密钥存储尚未接入");
    return false;
#endif
}

QString LocalVault::conversationPath(const QString& conversationId) const
{
    const auto safeName = QCryptographicHash::hash(conversationId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(rootPath_).filePath(safeName + QStringLiteral(".p2pvault"));
}

QByteArray LocalVault::encrypt(const QByteArray& plain) const
{
    if (!isReady()) return {};
    QByteArray nonce(nonceSize, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), nonce.size()) != 1) return {};
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (!context) return {};
    QByteArray cipher(plain.size(), Qt::Uninitialized);
    QByteArray tag(tagSize, Qt::Uninitialized);
    int written = 0, finalWritten = 0;
    const bool ok = EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr,
                                       reinterpret_cast<const unsigned char*>(masterKey_.constData()),
                                       reinterpret_cast<const unsigned char*>(nonce.constData())) == 1
        && EVP_EncryptUpdate(context, reinterpret_cast<unsigned char*>(cipher.data()), &written,
                             reinterpret_cast<const unsigned char*>(plain.constData()), plain.size()) == 1
        && EVP_EncryptFinal_ex(context, reinterpret_cast<unsigned char*>(cipher.data()) + written, &finalWritten) == 1
        && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, tagSize, tag.data()) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!ok) return {};
    cipher.truncate(written + finalWritten);
    return magic + nonce + tag + cipher;
}

QByteArray LocalVault::decrypt(const QByteArray& encrypted) const
{
    if (!isReady() || !encrypted.startsWith(magic) || encrypted.size() < magic.size() + nonceSize + tagSize)
        return {};
    const auto nonce = encrypted.mid(magic.size(), nonceSize);
    const auto tag = encrypted.mid(magic.size() + nonceSize, tagSize);
    const auto cipher = encrypted.mid(magic.size() + nonceSize + tagSize);
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (!context) return {};
    QByteArray plain(cipher.size(), Qt::Uninitialized);
    int written = 0, finalWritten = 0;
    const bool ok = EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr,
                                       reinterpret_cast<const unsigned char*>(masterKey_.constData()),
                                       reinterpret_cast<const unsigned char*>(nonce.constData())) == 1
        && EVP_DecryptUpdate(context, reinterpret_cast<unsigned char*>(plain.data()), &written,
                             reinterpret_cast<const unsigned char*>(cipher.constData()), cipher.size()) == 1
        && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, tagSize, const_cast<char*>(tag.constData())) == 1
        && EVP_DecryptFinal_ex(context, reinterpret_cast<unsigned char*>(plain.data()) + written, &finalWritten) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!ok) return {};
    plain.truncate(written + finalWritten);
    return plain;
}

QVariantList LocalVault::loadConversation(const QString& conversationId) const
{
    QFile input(conversationPath(conversationId));
    if (!input.open(QIODevice::ReadOnly)) return {};
    const auto plain = decrypt(input.readAll());
    const auto document = QJsonDocument::fromJson(plain);
    return document.isArray() ? document.array().toVariantList() : QVariantList {};
}

bool LocalVault::saveConversation(const QString& conversationId, const QVariantList& messages)
{
    const auto plain = QJsonDocument(QJsonArray::fromVariantList(messages)).toJson(QJsonDocument::Compact);
    const auto encrypted = encrypt(plain);
    if (encrypted.isEmpty()) return false;
    QSaveFile output(conversationPath(conversationId));
    return output.open(QIODevice::WriteOnly)
        && output.write(encrypted) == encrypted.size()
        && output.commit();
}
