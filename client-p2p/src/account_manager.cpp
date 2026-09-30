#include "account_manager.h"
#include <QStandardPaths>
#include <QSettings>
#include <QDir>
#include <QUuid>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include "local_vault.h"
#include "relay_crypto.h"
bool AccountManager::resetForPasswordRelease(QString* error) {
    const auto base = qEnvironmentVariable("P2P_MESSENGER_DATA_ROOT").isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : qEnvironmentVariable("P2P_MESSENGER_DATA_ROOT");
    QSettings marker(QDir(base).filePath("account-reset.ini"),QSettings::IniFormat);
    const auto version = QStringLiteral("2026-09-30-password-accounts-1");
    if (marker.value("completed").toString()==version) return true;
    if (!QDir().mkpath(base)) { if(error)*error=QStringLiteral("无法创建本机账号目录"); return false; }
    QVariantList owner;
    if (QFileInfo::exists(QDir(base).filePath("vault"))) { LocalVault old(base); if (!old.isReady()) { if(error)*error=old.error(); return false; } owner=old.loadConversation("__owner_device"); }
    const auto backup = QDir(base).filePath("account-backups/"+version+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(backup)) { if(error)*error=QStringLiteral("无法创建账号备份目录，未清空账号"); return false; }
    QStringList moved;
    const auto rollback = [&] {
        const auto fresh=QDir(base).filePath("vault");
        if (moved.contains("vault") && QFileInfo::exists(fresh)) QDir().rename(fresh,QDir(backup).filePath("failed-new-vault"));
        for (const auto& name : moved) QDir().rename(QDir(backup).filePath(name),QDir(base).filePath(name));
    };
    for (const auto& name : {QStringLiteral("vault"),QStringLiteral("accounts"),QStringLiteral("accounts.ini")}) {
        const auto source=QDir(base).filePath(name); if (!QFileInfo::exists(source)) continue;
        if (QFileInfo(source).isSymLink() || !QDir().rename(source,QDir(backup).filePath(name))) { rollback(); if(error)*error=QStringLiteral("账号备份未完成，已停止重置；原资料保留"); return false; }
        moved.append(name);
    }
    if (!owner.isEmpty()) {
        LocalVault fresh(base);
        auto row=owner.first().toMap(); row.insert("enabled",true);
        if (!fresh.saveConversation("__owner_device",{row})) { rollback(); if(error)*error=QStringLiteral("主人资格恢复失败，未完成重置"); return false; }
    }
    marker.setValue("completed",version); marker.setValue("backup",backup); marker.sync();
    if (marker.status()!=QSettings::NoError) { rollback(); if(error)*error=QStringLiteral("重置标记无法保存，已保留原账号"); return false; }
    return true;
}
AccountManager::AccountManager(QObject* parent) : QObject(parent) {
    base_ = qEnvironmentVariable("P2P_MESSENGER_DATA_ROOT");
    if (base_.isEmpty()) base_ = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QSettings settings(QDir(base_).filePath("accounts.ini"), QSettings::IniFormat);
    active_ = settings.value("active", "default").toString();
    profiles_.append(QVariantMap {{"id","default"},{"name",tr("原有账号")}});
    const QRegularExpression uuid(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    for (const auto& value : settings.value("profiles").toList()) {
        auto row = value.toMap(); const auto id = row.value("id").toString();
        row.insert("name",row.value("name").toString().left(64));
        if (id == "default") profiles_[0] = row;
        else if (uuid.match(id).hasMatch() && profiles_.size() < 50) {
            bool duplicate = false; for (const auto& v : profiles_) duplicate |= v.toMap().value("id") == id;
            if (!duplicate) profiles_.append(row);
        }
    }
    if (activeIndex() < 0) active_ = "default";
}
QVariantList AccountManager::profiles() const { return profiles_; }
int AccountManager::activeIndex() const {
    for (int i = 0; i < profiles_.size(); ++i) if (profiles_[i].toMap().value("id") == active_) return i;
    return -1;
}
QString AccountManager::activeRoot() const { return active_ == "default" ? base_ : QDir(base_).filePath("accounts/" + active_); }
bool AccountManager::persist() {
    QSettings s(QDir(base_).filePath("accounts.ini"),QSettings::IniFormat);
    s.setValue("profiles",profiles_); s.setValue("active",active_); s.sync();
    if (s.status() != QSettings::NoError) { emit errorOccurred(tr("账号列表未能保存，请检查本机存储权限和空间")); return false; }
    return true;
}
void AccountManager::updateName(const QString& name) {
    const auto i = activeIndex(); if (i<0 || name.isEmpty()) return;
    auto row = profiles_[i].toMap(); if (row.value("name") == name) return;
    const auto previous = profiles_[i]; row.insert("name",name.left(64)); profiles_[i] = row;
    if (!persist()) { profiles_[i] = previous; return; } emit profilesChanged();
}
void AccountManager::selectAccount(int index) {
    if (index<0 || index>=profiles_.size()) return;
    const auto id = profiles_[index].toMap().value("id").toString(); if (id == active_) return;
    const auto previous = active_; active_ = id;
    if (!persist()) { active_ = previous; return; }
    emit profilesChanged(); emit accountSelected(activeRoot());
}
bool AccountManager::createAccount() {
    if (profiles_.size() >= 50) { emit errorOccurred(tr("本机账号数量已达 50 个")); return false; }
    const auto previous = profiles_; const auto previousActive = active_;
    active_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profiles_.append(QVariantMap {{"id",active_},{"name",tr("新账号")}});
    if (!persist()) { profiles_ = previous; active_ = previousActive; return false; }
    emit profilesChanged(); emit accountSelected(activeRoot());
    return true;
}
bool AccountManager::beginLogin(const QString& uid,const QString& password) {
    if (!QRegularExpression("^[1-9][0-9]{0,15}$").match(uid.trimmed()).hasMatch() || password.size()<8 || password.size()>128) { emit errorOccurred(tr("请输入数字 UID 和 8–128 位密码")); return false; }
    loginUid_=uid.trimmed(); loginPassword_=password; loginExpectedUid_=loginUid_;
    if (!createAccount()) { loginUid_.clear(); loginExpectedUid_.clear();loginPassword_.fill(QChar('\0')); loginPassword_.clear(); return false; }
    return true;
}
QStringList AccountManager::takeLogin() {
    if (loginUid_.isEmpty()) return {};
    const QStringList result {loginUid_,loginPassword_}; loginUid_.clear(); loginPassword_.fill(QChar('\0')); loginPassword_.clear(); return result;
}
bool AccountManager::completeLogin(const QString& uid,const QString& inviteCode) {
    if(loginExpectedUid_.isEmpty() || uid!=loginExpectedUid_)return false;
    loginExpectedUid_.clear();
    const auto scratchId=active_;
    // Only after server password authentication. UID alone is not sufficient:
    // match the restored cryptographic identity before reusing local records.
    for(int i=0;i<profiles_.size();++i) {
        const auto id=profiles_[i].toMap().value("id").toString();if(id==active_)continue;
        const auto root=id=="default"?base_:QDir(base_).filePath("accounts/"+id);
        LocalVault vault(root);const auto rows=vault.loadConversation("__profile");
        if(rows.isEmpty() || rows.first().toMap().value("uid").toString()!=uid || !vault.hasConversation("__relay_identity"))continue;
        RelayCrypto identity;if(!identity.loadOrCreate(vault) || identity.inviteCode()!=inviteCode)continue;
        if(!vault.saveConversation("__password_account",{QVariantMap{{"uid",uid}}}))return false;
        selectAccount(i);if(active_!=id)return false;
        // Keep the scratch directory as a recoverable authenticated backup,
        // but do not accumulate duplicate entries on every same-device login.
        const auto previous=profiles_;
        for(qsizetype j=profiles_.size();j>0;--j)if(profiles_[j-1].toMap().value("id")==scratchId)profiles_.removeAt(j-1);
        if(!persist())profiles_=previous;else emit profilesChanged();
        return true;
    }
    return false;
}
