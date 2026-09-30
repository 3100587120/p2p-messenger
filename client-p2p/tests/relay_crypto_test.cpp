#include "relay_crypto.h"
#include "local_vault.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>

#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root(QStringLiteral("D:/p2p-messenger/work/relay-crypto-test-XXXXXX"));
    if (!root.isValid()) return 2;
    qputenv("P2P_MESSENGER_DATA_ROOT", QDir(root.path()).filePath(QStringLiteral("alice")).toUtf8());
    LocalVault aliceVault;
    RelayCrypto alice;
    if (!alice.loadOrCreate(aliceVault)) return 3;
    qputenv("P2P_MESSENGER_DATA_ROOT", QDir(root.path()).filePath(QStringLiteral("bob")).toUtf8());
    LocalVault bobVault;
    RelayCrypto bob;
    if (!bob.loadOrCreate(bobVault)) return 4;
    const auto alicePublic = RelayCrypto::publicKeyFromCode(alice.inviteCode());
    const auto bobPublic = RelayCrypto::publicKeyFromCode(bob.inviteCode());
    if (alicePublic.size() != 32 || bobPublic.size() != 32 || alice.identityId() == bob.identityId()) return 5;
    const auto id = QStringLiteral("01234567-89ab-cdef");
    const auto sealed = alice.seal(bobPublic, id, QJsonObject {{QStringLiteral("type"), QStringLiteral("text")},
                                                               {QStringLiteral("body"), QStringLiteral("hello")}});
    if (sealed.isEmpty()) return 6;
    bool ok = false;
    const auto plain = bob.open(alice.identityId(), id, sealed, &ok);
    if (!ok || plain.value(QStringLiteral("body")).toString() != QStringLiteral("hello")) return 7;
    bob.open(alice.identityId(), QStringLiteral("different-id"), sealed, &ok);
    if (ok) return 8;
    bob.open(bob.identityId(), id, sealed, &ok);
    if (ok) return 9;
    RelayCrypto restored;
    if (!restored.loadOrCreate(aliceVault)) {
        std::cerr << "identity reload failed; stored entries="
                  << aliceVault.loadConversation(QStringLiteral("__relay_identity")).size() << '\n';
        return 10;
    }
    if (restored.identityId() != alice.identityId()) {
        std::cerr << "identity changed after reload: original=" << alice.identityId().toStdString()
                  << " restored=" << restored.identityId().toStdString()
                  << " stored_private_length=" << aliceVault.loadConversation(QStringLiteral("__relay_identity"))
                         .first().toMap().value(QStringLiteral("private")).toString().size() << '\n';
        return 11;
    }
    const QString password = QStringLiteral("disposable-password-test-only");
    const auto login = alice.makeLoginRecord(password, "Alice QA");
    if (login.isEmpty() || login.contains("password") ||
        RelayCrypto::loginToken(password,login.value("salt").toString(),600000) != login.value("token").toString()) return 14;
    LocalVault loginVault(QDir(root.path()).filePath("login"));
    RelayCrypto recovered;
    if (!recovered.loadOrCreate(loginVault)) return 14;
    const auto scratchCode = recovered.inviteCode();
    if (recovered.restoreLoginRecord(loginVault,"wrong-password-test",login) || recovered.inviteCode()!=scratchCode) return 14;
    auto tampered = login; tampered.insert("code",bob.inviteCode());
    if (recovered.restoreLoginRecord(loginVault,password,tampered) || recovered.inviteCode()!=scratchCode) return 14;
    if (!recovered.restoreLoginRecord(loginVault,password,login) || recovered.inviteCode()!=alice.inviteCode()) return 14;
    const auto recoveredPlain = recovered.open(bob.identityId(),id,bob.seal(alicePublic,id,QJsonObject {{"body","after login"}}),&ok);
    if (!ok || recoveredPlain.value("body")!="after login") return 14;
    std::cout << "password login restores original identity; wrong password/substituted identity rejected=PASS\n";
    const auto aliceRoot = QDir(root.path()).filePath(QStringLiteral("alice"));
    const auto profilePath = QDir(aliceRoot).filePath("vault/" + QString::fromLatin1(
        QCryptographicHash::hash("__profile", QCryptographicHash::Sha256).toHex()) + ".p2pvault");
    const QVariantList first {QVariantMap {{"profileName", "first"}}};
    const QVariantList second {QVariantMap {{"profileName", "second"}}};
    if (!aliceVault.saveConversation("__profile", first) || !aliceVault.saveConversation("__profile", second)) return 12;
    {
        QFile empty(profilePath);
        if (!empty.open(QIODevice::WriteOnly) || !empty.resize(0)) return 12;
    }
    if (!aliceVault.recoverEmptyConversation("__profile", second) || aliceVault.loadConversation("__profile") != first) return 12;
    {
        QFile corrupt(profilePath);
        if (!corrupt.open(QIODevice::WriteOnly) || corrupt.write("nonempty-corrupt") != 16) return 12;
    }
    if (aliceVault.recoverEmptyConversation("__profile", first) ||
        aliceVault.saveConversation("__profile", second) || !aliceVault.error().contains(QStringLiteral("认证失败"))) return 12;
    QFile intactBackup(profilePath + ".bak");
    if (!intactBackup.open(QIODevice::ReadOnly) || intactBackup.readAll().isEmpty()) return 12;
    // A blocked backup destination must fail before replacing the primary.
    if (!aliceVault.saveConversation("__write_failure", first)) return 13;
    const auto failurePath = QDir(aliceRoot).filePath("vault/" + QString::fromLatin1(
        QCryptographicHash::hash("__write_failure", QCryptographicHash::Sha256).toHex()) + ".p2pvault");
    if (!QDir().mkpath(failurePath + ".bak") ||
        aliceVault.saveConversation("__write_failure", second) ||
        !aliceVault.error().contains(QStringLiteral("备份")) ||
        aliceVault.loadConversation("__write_failure") != first) return 13;
    std::cout << "failed-write preserves original record passed\n";
    std::cout << "encrypted backup recovery and nonempty corruption protection passed\n";
    std::cout << "relay crypto round-trip, tamper rejection, identity persistence passed\n";
    return 0;
}
