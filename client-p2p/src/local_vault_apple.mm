#include "local_vault_apple.h"

#import <Security/Security.h>

QByteArray p2pAppleLoadOrCreateVaultKey(const QString& keyName)
{
    const auto service = QStringLiteral("org.p2pmessenger.local-vault").toUtf8();
    const auto account = keyName.toUtf8();
    NSDictionary* query = @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
                            (__bridge id)kSecAttrService: [NSData dataWithBytes: service.constData() length: service.size()],
                            (__bridge id)kSecAttrAccount: [NSData dataWithBytes: account.constData() length: account.size()],
                            (__bridge id)kSecReturnData: @YES};
    CFTypeRef value = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &value) == errSecSuccess) {
        NSData* data = (__bridge_transfer NSData*)value;
        if (data.length == 32)
            return QByteArray(static_cast<const char*>(data.bytes), data.length);
    }
    QByteArray key(32, Qt::Uninitialized);
    if (SecRandomCopyBytes(kSecRandomDefault, key.size(), reinterpret_cast<uint8_t*>(key.data())) != errSecSuccess)
        return {};
    NSDictionary* item = @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
                           (__bridge id)kSecAttrService: [NSData dataWithBytes: service.constData() length: service.size()],
                           (__bridge id)kSecAttrAccount: [NSData dataWithBytes: account.constData() length: account.size()],
                           (__bridge id)kSecValueData: [NSData dataWithBytes: key.constData() length: key.size()],
                           (__bridge id)kSecAttrAccessible: (__bridge id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly};
    return SecItemAdd((__bridge CFDictionaryRef)item, nullptr) == errSecSuccess ? key : QByteArray {};
}
