#include "local_vault.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#include <bcrypt.h>
#elif !defined(Q_OS_ANDROID)
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#endif

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#endif

#ifdef Q_OS_IOS
#include "local_vault_apple.h"
#endif

namespace {
constexpr auto keySize = 32;
constexpr auto nonceSize = 12;
constexpr auto tagSize = 16;
const QByteArray magic("P2PV1");

#ifdef Q_OS_WIN
QByteArray cngCrypt(bool encrypting, const QByteArray& key, const QByteArray& nonce,
                    const QByteArray& input, QByteArray* tag)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return {};
    const wchar_t mode[] = BCRYPT_CHAIN_MODE_GCM;
    if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)), sizeof(mode), 0) != 0
        || BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0,
                                       reinterpret_cast<PUCHAR>(const_cast<char*>(key.constData())), key.size(), 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0); return {};
    }
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(nonce.constData())); info.cbNonce = nonce.size();
    QByteArray localTag(tagSize, Qt::Uninitialized); info.pbTag = reinterpret_cast<PUCHAR>(localTag.data()); info.cbTag = localTag.size();
    QByteArray output(input.size(), Qt::Uninitialized); ULONG written = 0;
    const auto status = encrypting
        ? BCryptEncrypt(handle, reinterpret_cast<PUCHAR>(const_cast<char*>(input.constData())), input.size(), &info, nullptr, 0,
                        reinterpret_cast<PUCHAR>(output.data()), output.size(), &written, 0)
        : BCryptDecrypt(handle, reinterpret_cast<PUCHAR>(const_cast<char*>(input.constData())), input.size(), &info, nullptr, 0,
                        reinterpret_cast<PUCHAR>(output.data()), output.size(), &written, 0);
    BCryptDestroyKey(handle); BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) return {};
    output.truncate(written); if (tag) *tag = localTag; return output;
}
#endif

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

#ifdef Q_OS_ANDROID
QByteArray loadAndroidKey()
{
    auto context = QNativeInterface::QAndroidApplication::context();
    QJniEnvironment environment;
    auto clazz = environment->FindClass("org/p2pmessenger/VaultKeyStore");
    auto method = environment->GetStaticMethodID(clazz, "loadOrCreate", "(Landroid/content/Context;Ljava/lang/String;)[B");
    auto name = QJniObject::fromString(QStringLiteral("master-key"));
    auto array = static_cast<jbyteArray>(environment->CallStaticObjectMethod(clazz, method, context.object<jobject>(), name.object<jstring>()));
    if (!array || environment.checkAndClearExceptions()) return {};
    const auto size = environment->GetArrayLength(array);
    QByteArray key(size, Qt::Uninitialized);
    environment->GetByteArrayRegion(array, 0, size, reinterpret_cast<jbyte*>(key.data()));
    return environment.checkAndClearExceptions() ? QByteArray {} : key;
}

QByteArray androidCrypt(const char* method, const QByteArray& key, const QByteArray& nonce,
                        const QByteArray& input)
{
    QJniEnvironment environment;
    auto makeArray = [&environment](const QByteArray& bytes) {
        auto array = environment->NewByteArray(bytes.size());
        environment->SetByteArrayRegion(array, 0, bytes.size(), reinterpret_cast<const jbyte*>(bytes.constData()));
        return array;
    };
    auto keyArray = makeArray(key), nonceArray = makeArray(nonce), inputArray = makeArray(input);
    auto clazz = environment->FindClass("org/p2pmessenger/VaultKeyStore");
    auto methodId = environment->GetStaticMethodID(clazz, method, "([B[B[B)[B");
    auto result = static_cast<jbyteArray>(environment->CallStaticObjectMethod(clazz, methodId, keyArray, nonceArray, inputArray));
    environment->DeleteLocalRef(keyArray); environment->DeleteLocalRef(nonceArray); environment->DeleteLocalRef(inputArray);
    if (!result || environment.checkAndClearExceptions()) return {};
    const auto array = result; const auto size = environment->GetArrayLength(array);
    QByteArray bytes(size, Qt::Uninitialized);
    environment->GetByteArrayRegion(array, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    return environment.checkAndClearExceptions() ? QByteArray {} : bytes;
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
#if defined(Q_OS_WIN)
        masterKey_ = unprotectForCurrentUser(keyFile.readAll());
#elif defined(Q_OS_ANDROID)
        masterKey_ = loadAndroidKey();
#elif defined(Q_OS_IOS)
        masterKey_ = p2pAppleLoadOrCreateVaultKey(QStringLiteral("master-key"));
#else
        error_ = QStringLiteral("此平台的安全密钥存储尚未接入"); return false;
#endif
        if (masterKey_.size() != keySize) {
            error_ = QStringLiteral("本地密钥无法由当前设备解锁");
            masterKey_.clear();
            return false;
        }
        return true;
    }
    QByteArray key(keySize, Qt::Uninitialized);
#ifdef Q_OS_WIN
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(key.data()), key.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        error_ = QStringLiteral("无法生成本地加密密钥"); return false;
    }
#elif !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), key.size()) != 1) {
        error_ = QStringLiteral("无法生成本地加密密钥");
        return false;
    }
#endif
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
#elif defined(Q_OS_ANDROID)
    masterKey_ = loadAndroidKey();
    if (masterKey_.size() == keySize) return true;
    error_ = QStringLiteral("Android Keystore 未能保护本地密钥");
    return false;
#elif defined(Q_OS_IOS)
    masterKey_ = p2pAppleLoadOrCreateVaultKey(QStringLiteral("master-key"));
    if (masterKey_.size() == keySize) return true;
    error_ = QStringLiteral("iOS Keychain 未能保护本地密钥");
    return false;
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
 #ifdef Q_OS_WIN
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(nonce.data()), nonce.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    QByteArray tag; const auto cipher = cngCrypt(true, masterKey_, nonce, plain, &tag);
    return cipher.isEmpty() ? QByteArray {} : magic + nonce + tag + cipher;
 #elif defined(Q_OS_ANDROID)
    for (auto index = 0; index < nonce.size(); ++index)
        nonce[index] = static_cast<char>(QRandomGenerator::system()->bounded(256));
    const auto combined = androidCrypt("encrypt", masterKey_, nonce, plain);
    if (combined.size() < tagSize) return {};
    return magic + nonce + combined.right(tagSize) + combined.left(combined.size() - tagSize);
 #else
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
 #endif
}

QByteArray LocalVault::decrypt(const QByteArray& encrypted) const
{
    if (!isReady() || !encrypted.startsWith(magic) || encrypted.size() < magic.size() + nonceSize + tagSize)
        return {};
    const auto nonce = encrypted.mid(magic.size(), nonceSize);
    const auto tag = encrypted.mid(magic.size() + nonceSize, tagSize);
    const auto cipher = encrypted.mid(magic.size() + nonceSize + tagSize);
 #ifdef Q_OS_WIN
    auto localTag = tag; const auto plain = cngCrypt(false, masterKey_, nonce, cipher, &localTag);
    return plain;
 #elif defined(Q_OS_ANDROID)
    return androidCrypt("decrypt", masterKey_, nonce, cipher + tag);
 #else
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
 #endif
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
