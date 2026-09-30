#include "relay_crypto.h"
#include "local_vault.h"

#include <QCryptographicHash>
#include <QJsonDocument>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {
constexpr int keyBytes = 32;
constexpr int nonceBytes = 12;
constexpr int tagBytes = 16;
const auto identitySlot = QStringLiteral("__relay_identity");

QByteArray b64(const QByteArray& bytes)
{
    return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray unb64(const QString& text)
{
    return QByteArray::fromBase64(text.toLatin1(), QByteArray::Base64UrlEncoding);
}

QByteArray publicFromPrivate(const QByteArray& secret)
{
    if (secret.size() != keyBytes) return {};
    auto* pair = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
        reinterpret_cast<const unsigned char*>(secret.constData()), secret.size());
    if (!pair) return {};
    QByteArray result(keyBytes, Qt::Uninitialized);
    size_t size = result.size();
    const bool ok = EVP_PKEY_get_raw_public_key(pair,
        reinterpret_cast<unsigned char*>(result.data()), &size) == 1 && size == keyBytes;
    EVP_PKEY_free(pair);
    return ok ? result : QByteArray {};
}

QByteArray passwordKeys(const QString& password, const QString& salt, int iterations) {
    const auto rawSalt = unb64(salt);
    if (password.size() < 8 || password.size() > 128 || rawSalt.size() != 16 || iterations != 600000) return {};
    auto utf8 = password.toUtf8(); QByteArray keys(64,Qt::Uninitialized);
    const bool ok = PKCS5_PBKDF2_HMAC(utf8.constData(),utf8.size(),reinterpret_cast<const unsigned char*>(rawSalt.constData()),rawSalt.size(),iterations,EVP_sha256(),keys.size(),reinterpret_cast<unsigned char*>(keys.data())) == 1;
    utf8.fill('\0'); return ok ? keys : QByteArray {};
}

QByteArray sharedKey(const QByteArray& secret, const QByteArray& peer,
                     const QByteArray& senderPublic, const QByteArray& recipientPublic)
{
    if (secret.size() != keyBytes || peer.size() != keyBytes) return {};
    auto* own = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
        reinterpret_cast<const unsigned char*>(secret.constData()), secret.size());
    auto* other = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
        reinterpret_cast<const unsigned char*>(peer.constData()), peer.size());
    auto* context = own && other ? EVP_PKEY_CTX_new(own, nullptr) : nullptr;
    QByteArray shared(keyBytes, Qt::Uninitialized);
    size_t size = shared.size();
    const bool ok = context && EVP_PKEY_derive_init(context) == 1 &&
        EVP_PKEY_derive_set_peer(context, other) == 1 &&
        EVP_PKEY_derive(context, reinterpret_cast<unsigned char*>(shared.data()), &size) == 1 &&
        size == keyBytes;
    EVP_PKEY_CTX_free(context);
    EVP_PKEY_free(other);
    EVP_PKEY_free(own);
    if (!ok) return {};
    // Direction-separated key derivation; both sides agree on sender/recipient order.
    const auto result = QCryptographicHash::hash(
        QByteArrayLiteral("ShuangDianLiao relay v1\0") + shared + senderPublic + recipientPublic,
        QCryptographicHash::Sha256);
    shared.fill('\0');
    return result;
}

QByteArray aad(const QString& packetId, const QString& from, const QString& to)
{
    return QByteArrayLiteral("SD1|") + packetId.toLatin1() + '|' + from.toLatin1() + '|' + to.toLatin1();
}
}

bool RelayCrypto::loadOrCreate(LocalVault& vault)
{
    if (!vault.isReady()) return false;
    const auto entries = vault.loadConversation(identitySlot);
    // Never replace an unreadable identity: peers would see a new account and
    // locally queued ciphertext would become permanently undecryptable.
    if (entries.isEmpty() && vault.hasConversation(identitySlot)) return false;
    if (!entries.isEmpty()) {
        const auto entry = entries.first().toMap();
        privateKey_ = unb64(entry.value(QStringLiteral("private")).toString());
        publicKey_ = publicFromPrivate(privateKey_);
        return publicKey_.size() == keyBytes;
    }
    auto* context = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    EVP_PKEY* pair = nullptr;
    const bool generated = context && EVP_PKEY_keygen_init(context) == 1 &&
                           EVP_PKEY_keygen(context, &pair) == 1;
    EVP_PKEY_CTX_free(context);
    if (!generated || !pair) return false;
    privateKey_.resize(keyBytes);
    publicKey_.resize(keyBytes);
    size_t privateSize = keyBytes, publicSize = keyBytes;
    const bool exported = EVP_PKEY_get_raw_private_key(pair,
        reinterpret_cast<unsigned char*>(privateKey_.data()), &privateSize) == 1 &&
        EVP_PKEY_get_raw_public_key(pair,
        reinterpret_cast<unsigned char*>(publicKey_.data()), &publicSize) == 1 &&
        privateSize == keyBytes && publicSize == keyBytes;
    EVP_PKEY_free(pair);
    if (!exported) return false;
    if (!vault.saveConversation(identitySlot, QVariantList {
            QVariantMap {{QStringLiteral("private"), QString::fromLatin1(b64(privateKey_))}}})) {
        privateKey_.fill('\0');
        privateKey_.clear();
        publicKey_.clear();
        return false;
    }
    return true;
}

QString RelayCrypto::idForPublicKey(const QByteArray& key)
{
    if (key.size() != keyBytes) return {};
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
}

QString RelayCrypto::identityId() const { return idForPublicKey(publicKey_); }

QJsonObject RelayCrypto::makeLoginRecord(const QString& password, const QString& name) const {
    QByteArray salt(16,Qt::Uninitialized);
    if (privateKey_.size()!=32 || RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()),salt.size())!=1) return {};
    const auto encodedSalt = QString::fromLatin1(b64(salt));
    auto keys = passwordKeys(password,encodedSalt,600000); if (keys.size()!=64) return {};
    RelayCrypto protection; protection.privateKey_ = keys.last(32); protection.publicKey_ = publicFromPrivate(protection.privateKey_);
    const auto backup = protection.seal(protection.publicKey_,"account-"+identityId(),QJsonObject {{"private",QString::fromLatin1(b64(privateKey_))},{"name",name.left(64)}});
    const auto token = QString::fromLatin1(b64(keys.first(32))); keys.fill('\0'); protection.privateKey_.fill('\0');
    if (backup.isEmpty()) return {};
    return QJsonObject {{"v",1},{"salt",encodedSalt},{"iterations",600000},{"token",token},{"backup",QString::fromLatin1(backup)},{"name",name.left(64)},{"code",inviteCode()}};
}
QString RelayCrypto::loginToken(const QString& password, const QString& salt, int iterations) {
    auto keys = passwordKeys(password,salt,iterations); if (keys.size()!=64) return {};
    const auto token=QString::fromLatin1(b64(keys.first(32))); keys.fill('\0'); return token;
}
bool RelayCrypto::restoreLoginRecord(LocalVault& vault, const QString& password, const QJsonObject& record) {
    auto keys = passwordKeys(password,record.value("salt").toString(),record.value("iterations").toInt());
    const auto expected = publicKeyFromCode(record.value("code").toString());
    if (keys.size()!=64 || expected.size()!=32 || record.value("backup").toString().size()>8192) return false;
    RelayCrypto protection; protection.privateKey_=keys.last(32); protection.publicKey_=publicFromPrivate(protection.privateKey_); keys.fill('\0');
    bool ok=false; const auto plain=protection.open(protection.identityId(),"account-"+idForPublicKey(expected),record.value("backup").toString().toLatin1(),&ok);
    protection.privateKey_.fill('\0');
    auto secret=unb64(plain.value("private").toString());
    if (!ok || publicFromPrivate(secret)!=expected) { secret.fill('\0'); return false; }
    const bool saved=vault.saveConversation(identitySlot,{QVariantMap {{"private",QString::fromLatin1(b64(secret))}}}); secret.fill('\0');
    return saved && loadOrCreate(vault);
}

QString RelayCrypto::inviteCode() const
{
    return publicKey_.size() == keyBytes ? QStringLiteral("SD1-") + QString::fromLatin1(b64(publicKey_)) : QString {};
}

QByteArray RelayCrypto::publicKeyFromCode(const QString& code)
{
    const auto trimmed = code.trimmed();
    if (!trimmed.startsWith(QStringLiteral("SD1-"))) return {};
    const auto key = unb64(trimmed.mid(4));
    return key.size() == keyBytes ? key : QByteArray {};
}

QByteArray RelayCrypto::seal(const QByteArray& recipientPublicKey, const QString& packetId,
                              const QJsonObject& message) const
{
    if (publicKey_.size() != keyBytes || recipientPublicKey.size() != keyBytes || packetId.isEmpty()) return {};
    const auto key = sharedKey(privateKey_, recipientPublicKey, publicKey_, recipientPublicKey);
    if (key.size() != keyBytes) return {};
    QByteArray nonce(nonceBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), nonce.size()) != 1) return {};
    const auto plain = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray encrypted(plain.size() + tagBytes, Qt::Uninitialized);
    auto* context = EVP_CIPHER_CTX_new();
    int written = 0, total = 0;
    const auto associated = aad(packetId, identityId(), idForPublicKey(recipientPublicKey));
    const bool ok = context && EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonceBytes, nullptr) == 1 &&
        EVP_EncryptInit_ex(context, nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.constData()),
            reinterpret_cast<const unsigned char*>(nonce.constData())) == 1 &&
        EVP_EncryptUpdate(context, nullptr, &written,
            reinterpret_cast<const unsigned char*>(associated.constData()), associated.size()) == 1 &&
        EVP_EncryptUpdate(context, reinterpret_cast<unsigned char*>(encrypted.data()), &written,
            reinterpret_cast<const unsigned char*>(plain.constData()), plain.size()) == 1;
    total = written;
    const bool finished = ok && EVP_EncryptFinal_ex(context,
        reinterpret_cast<unsigned char*>(encrypted.data()) + total, &written) == 1;
    total += written;
    const bool tagged = finished && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, tagBytes,
        encrypted.data() + total) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!tagged) return {};
    encrypted.resize(total + tagBytes);
    return QJsonDocument(QJsonObject {
        {QStringLiteral("v"), 1},
        {QStringLiteral("pk"), QString::fromLatin1(b64(publicKey_))},
        {QStringLiteral("nonce"), QString::fromLatin1(b64(nonce))},
        {QStringLiteral("ct"), QString::fromLatin1(b64(encrypted))}
    }).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QJsonObject RelayCrypto::open(const QString& senderId, const QString& packetId,
                               const QByteArray& envelope, bool* ok) const
{
    if (ok) *ok = false;
    const auto document = QJsonDocument::fromJson(QByteArray::fromBase64(envelope, QByteArray::Base64UrlEncoding));
    if (!document.isObject()) return {};
    const auto object = document.object();
    if (object.value(QStringLiteral("v")).toInt() != 1) return {};
    const auto senderPublic = unb64(object.value(QStringLiteral("pk")).toString());
    const auto nonce = unb64(object.value(QStringLiteral("nonce")).toString());
    const auto encrypted = unb64(object.value(QStringLiteral("ct")).toString());
    if (senderPublic.size() != keyBytes || nonce.size() != nonceBytes ||
        encrypted.size() < tagBytes || idForPublicKey(senderPublic) != senderId) return {};
    const auto key = sharedKey(privateKey_, senderPublic, senderPublic, publicKey_);
    if (key.size() != keyBytes) return {};
    const auto associated = aad(packetId, senderId, identityId());
    QByteArray plain(encrypted.size() - tagBytes, Qt::Uninitialized);
    auto* context = EVP_CIPHER_CTX_new();
    int written = 0, total = 0;
    const bool initialized = context && EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonceBytes, nullptr) == 1 &&
        EVP_DecryptInit_ex(context, nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.constData()),
            reinterpret_cast<const unsigned char*>(nonce.constData())) == 1 &&
        EVP_DecryptUpdate(context, nullptr, &written,
            reinterpret_cast<const unsigned char*>(associated.constData()), associated.size()) == 1 &&
        EVP_DecryptUpdate(context, reinterpret_cast<unsigned char*>(plain.data()), &written,
            reinterpret_cast<const unsigned char*>(encrypted.constData()), plain.size()) == 1;
    total = written;
    const bool tagged = initialized && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, tagBytes,
        const_cast<char*>(encrypted.constData() + plain.size())) == 1;
    const bool authenticated = tagged && EVP_DecryptFinal_ex(context,
        reinterpret_cast<unsigned char*>(plain.data()) + total, &written) == 1;
    total += written;
    EVP_CIPHER_CTX_free(context);
    if (!authenticated) return {};
    plain.resize(total);
    const auto payload = QJsonDocument::fromJson(plain);
    if (!payload.isObject()) return {};
    if (ok) *ok = true;
    return payload.object();
}
