#include "account_manager.h"
#include <QStandardPaths>
#include <QSettings>
#include <QDir>
#include <QUuid>
#include <QRegularExpression>
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
void AccountManager::createAccount() {
    if (profiles_.size() >= 50) { emit errorOccurred(tr("本机账号数量已达 50 个")); return; }
    const auto previous = profiles_; const auto previousActive = active_;
    active_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profiles_.append(QVariantMap {{"id",active_},{"name",tr("新账号")}});
    if (!persist()) { profiles_ = previous; active_ = previousActive; return; }
    emit profilesChanged(); emit accountSelected(activeRoot());
}
