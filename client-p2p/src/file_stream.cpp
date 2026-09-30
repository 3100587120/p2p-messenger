#include "file_stream.h"
#include "local_vault.h"
#include "relay_client.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QStorageInfo>
#include <QCryptographicHash>
#include <QUuid>
#include <QRegularExpression>

namespace {constexpr qint64 chunkBytes=16384;
QString slot(const QString& transfer,qint64 index){return transfer+"-"+QString::number(index);}
}
FileStream::FileStream(LocalVault& vault,RelayClient& relay,QObject* parent):QObject(parent),vault_(vault),relay_(relay){
    const auto rows=vault_.loadConversation("__stream_outgoing");if(!rows.isEmpty())outgoing_=rows.first().toMap();
    for(auto& value:outgoing_){auto row=value.toMap();auto pending=row.value("pending").toStringList();for(qsizetype i=pending.size();i>0;--i)if(!relay_.hasPendingPacket(pending[i-1]))pending.removeAt(i-1);row.insert("pending",pending);value=row;}
    if(!outgoing_.isEmpty())saveOutgoing();
    pump_.setInterval(30);connect(&pump_,&QTimer::timeout,this,&FileStream::pump);pump_.start();
    connect(&relay_,&RelayClient::deliveryState,this,[this](const QString& packet,const QString& state){
        if(state!="delivered")return;
        for(auto it=outgoing_.begin();it!=outgoing_.end();++it){auto row=it.value().toMap();auto pending=row.value("pending").toStringList();if(!pending.removeAll(packet))continue;
            row.insert("pending",pending);it.value()=row;saveOutgoing();return;}
    });
    exportTimer_.setInterval(0);connect(&exportTimer_,&QTimer::timeout,this,[this]{
        const auto rows=vault_.loadConversation(slot(exportTransfer_,exportIndex_));const auto bytes=rows.isEmpty()?QByteArray():QByteArray::fromBase64(rows.first().toByteArray());
        if(rows.isEmpty() || export_->write(bytes)!=bytes.size()){exportTimer_.stop();export_.reset();emit error(tr("文件保存失败，请检查权限和剩余空间"));return;}
        if(++exportIndex_==exportCount_){const auto path=export_->fileName();const bool ok=export_->commit();exportTimer_.stop();export_.reset();if(ok)emit fileSaved(path);else emit error(tr("文件提交失败，未覆盖目标文件"));}
    });
}
bool FileStream::saveOutgoing(){return vault_.saveConversation("__stream_outgoing",{outgoing_});}
bool FileStream::sendFile(const QString& path,const QString& contact,const QString& group,const QList<QByteArray>& recipients){
    const QFileInfo info(path);QFile input(path);if(!info.isFile() || !input.open(QIODevice::ReadOnly) || recipients.isEmpty())return false;
    const auto previous=outgoing_;const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    for(const auto& key:recipients){if(key.size()!=32){outgoing_=previous;return false;}const auto target=RelayClient::idForPublicKey(key);
        outgoing_.insert(id+target,QVariantMap{{"id",id},{"path",info.absoluteFilePath()},{"modified",info.lastModified().toMSecsSinceEpoch()},{"size",info.size()},{"name",info.fileName().left(180)},{"contact",contact},{"group",group},{"code","SD1-"+QString::fromLatin1(key.toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals))},{"next",0},{"pending",QStringList()}});
    }
    if(!saveOutgoing()){outgoing_=previous;return false;}
    emit fileReady(contact,QVariantMap{{"body",tr("文件：%1（分块传输中）").arg(info.fileName())},{"name",info.fileName()},{"fileId",id},{"messageId",id},{"sentAt",QDateTime::currentMSecsSinceEpoch()},{"filePath",info.absoluteFilePath()},{"outgoing",true},{"kind","file-offer"},{"time",QDateTime::currentDateTime().toString("HH:mm")}});return true;
}
void FileStream::pump(){
    if(!relay_.isConnected())return;
    for(auto it=outgoing_.begin();it!=outgoing_.end();){auto row=it.value().toMap();auto pending=row.value("pending").toStringList();const auto size=row.value("size").toLongLong(),count=qMax<qint64>(1,(size+chunkBytes-1)/chunkBytes),index=row.value("next").toLongLong();
        if(index>=count && pending.isEmpty()){it=outgoing_.erase(it);saveOutgoing();continue;}if(pending.size()>=4 || index>=count){++it;continue;}
        QFile input(row.value("path").toString());const QFileInfo info(input);
        if(!input.open(QIODevice::ReadOnly) || input.size()!=size || info.lastModified().toMSecsSinceEpoch()!=row.value("modified").toLongLong() || !input.seek(index*chunkBytes)){
            emit error(tr("待发送文件已改变或无法读取，已停止该传输"));it=outgoing_.erase(it);saveOutgoing();continue;}
        const auto bytes=input.read(chunkBytes);const auto expected=qMin(chunkBytes,size-index*chunkBytes);if(bytes.size()!=expected){emit error(tr("文件读取不完整"));it=outgoing_.erase(it);saveOutgoing();continue;}
        const auto packet=relay_.send(RelayClient::publicKeyFromCode(row.value("code").toString()),QJsonObject{{"type","stream_chunk"},{"fileId",row.value("id").toString()},{"name",row.value("name").toString()},{"groupId",row.value("group").toString()},{"size",size},{"count",count},{"index",index},{"data",QString::fromLatin1(bytes.toBase64())}});
        if(packet.isEmpty())return;pending.append(packet);row.insert("pending",pending);row.insert("next",index+1);it.value()=row;if(!saveOutgoing()){pump_.stop();emit error(tr("文件进度无法保存，请检查本机空间"));return;}++it;
    }
}
bool FileStream::receive(const QString& sender,const QString& contact,const QJsonObject& msg){
    const auto id=msg.value("fileId").toString();if(!QRegularExpression("^[0-9a-f-]{36}$").match(id).hasMatch())return false;
    const auto size=msg.value("size").toInteger(-1),count=msg.value("count").toInteger(-1),index=msg.value("index").toInteger(-1);
    if(size<0 || size>9007199254740991LL || count!=qMax<qint64>(1,(size+chunkBytes-1)/chunkBytes) || index<0 || index>=count || msg.value("data").toString().size()>32768)return false;
    const auto bytes=QByteArray::fromBase64(msg.value("data").toString().toLatin1());if(bytes.size()!=qMin(chunkBytes,size-index*chunkBytes))return false;
    const auto transfer="__stream_in_"+QString::fromLatin1(QCryptographicHash::hash((sender+contact+id).toUtf8(),QCryptographicHash::Sha256).toHex());
    const auto old=vault_.loadConversation(transfer);auto row=old.isEmpty()?QVariantMap():old.first().toMap();
    if(row.value("cancelled").toBool())return true;
    if(!row.isEmpty() && (row.value("size").toLongLong()!=size || row.value("count").toLongLong()!=count || row.value("name").toString()!=QFileInfo(msg.value("name").toString()).fileName()))return false;
    if(row.value("done").toBool()){
        emit fileReady(contact,QVariantMap{{"body",tr("文件：%1（可保存）").arg(row.value("name").toString())},{"name",row.value("name")},{"fileId",id},{"messageId",id},{"streamTransfer",transfer},{"senderId",sender},{"outgoing",false},{"kind","file-offer"},{"time",QDateTime::currentDateTime().toString("HH:mm")}});return true;
    }
    if(index<row.value("next",0).toLongLong())return true;
    auto holes=row.value("holes").toList();const auto next=row.value("next",0).toLongLong();if(index>next+16)return false;
    if(row.isEmpty()){
        const QStorageInfo disk(vault_.rootPath());if(disk.isValid() && disk.bytesAvailable()<size*2+1024*1024){emit error(tr("无法接收文件：本机空间不足，需要保存加密文件"));return false;}
        row=QVariantMap{{"size",size},{"count",count},{"name",QFileInfo(msg.value("name").toString()).fileName()},{"next",0}};
    }
    if(!vault_.saveConversation(slot(transfer,index),{bytes.toBase64()}))return false;
    if(!holes.contains(index))holes.append(index);auto contiguous=next;while(holes.removeAll(contiguous))++contiguous;
    row.insert("next",contiguous);row.insert("holes",holes);row.insert("done",contiguous==count);
    if(!vault_.saveConversation(transfer,{row}))return false;
    if(contiguous==count)emit fileReady(contact,QVariantMap{{"body",tr("文件：%1（可保存）").arg(row.value("name").toString())},{"name",row.value("name")},{"fileId",id},{"messageId",id},{"streamTransfer",transfer},{"senderId",sender},{"outgoing",false},{"kind","file-offer"},{"time",QDateTime::currentDateTime().toString("HH:mm")}});
    return true;
}
bool FileStream::exportFile(const QString& transfer,const QString& path){
    if(export_ || !transfer.startsWith("__stream_in_"))return false;const auto rows=vault_.loadConversation(transfer);if(rows.isEmpty() || !rows.first().toMap().value("done").toBool())return false;
    export_=std::make_unique<QSaveFile>(path);if(!export_->open(QIODevice::WriteOnly)){export_.reset();return false;}
    exportTransfer_=transfer;exportIndex_=0;exportCount_=rows.first().toMap().value("count").toLongLong();exportTimer_.start();return true;
}
void FileStream::cancelContact(const QString& contact){for(auto it=outgoing_.begin();it!=outgoing_.end();)if(it.value().toMap().value("contact")==contact)it=outgoing_.erase(it);else ++it;saveOutgoing();}
void FileStream::cancelFile(const QString& id){for(auto it=outgoing_.begin();it!=outgoing_.end();)if(it.value().toMap().value("id")==id)it=outgoing_.erase(it);else ++it;saveOutgoing();}
bool FileStream::cancelIncoming(const QString& sender,const QString& contact,const QString& id){
    const auto transfer="__stream_in_"+QString::fromLatin1(QCryptographicHash::hash((sender+contact+id).toUtf8(),QCryptographicHash::Sha256).toHex());
    const auto rows=vault_.loadConversation(transfer);auto row=rows.isEmpty()?QVariantMap():rows.first().toMap();row.insert("cancelled",true);return vault_.saveConversation(transfer,{row});
}
