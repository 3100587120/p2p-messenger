#include "relay_crypto.h"
#include "local_vault.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonObject>
#include <QTemporaryDir>

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
    std::cout << "relay crypto round-trip, tamper rejection, identity persistence passed\n";
    return 0;
}
