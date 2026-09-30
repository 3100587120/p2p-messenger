#pragma once
#include <QObject>
#include <QVariantMap>
#include <QJsonObject>
#include <QTimer>
#include <QSaveFile>
#include <memory>
class LocalVault;
class RelayClient;

// Bounded-memory, authenticated file chunks; only encrypted chunks live in the vault.
class FileStream final:public QObject {
    Q_OBJECT
public:
    FileStream(LocalVault& vault,RelayClient& relay,QObject* parent=nullptr);
    bool sendFile(const QString& path,const QString& contact,const QString& group,const QList<QByteArray>& recipients);
    bool receive(const QString& sender,const QString& contact,const QJsonObject& message);
    bool exportFile(const QString& transfer,const QString& path);
    void cancelContact(const QString& contact);
    void cancelFile(const QString& id);
    bool cancelIncoming(const QString& sender,const QString& contact,const QString& id);
signals:
    void fileReady(const QString& contact,const QVariantMap& message);
    void fileSaved(const QString& path);
    void error(const QString& reason);
private:
    LocalVault& vault_;RelayClient& relay_;QVariantMap outgoing_;
    QTimer pump_,exportTimer_;std::unique_ptr<QSaveFile> export_;
    QString exportTransfer_;qint64 exportIndex_{0},exportCount_{0};
    bool saveOutgoing();void pump();
    bool pumpQueued_{false};
};
