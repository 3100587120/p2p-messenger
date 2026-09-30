#pragma once
#include <QObject>
#include <QVariantList>
class AccountManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY profilesChanged)
public:
    explicit AccountManager(QObject* parent = nullptr);
    QVariantList profiles() const;
    int activeIndex() const;
    QString activeRoot() const;
    QString baseRoot() const { return base_; }
    static bool resetForPasswordRelease(QString* error);
    void updateName(const QString& name);
    Q_INVOKABLE bool createAccount();
    Q_INVOKABLE bool beginLogin(const QString& uid, const QString& password);
    QStringList takeLogin();
    bool completeLogin(const QString& uid,const QString& inviteCode);
    Q_INVOKABLE void selectAccount(int index);
signals:
    void profilesChanged();
    void accountSelected(const QString& root);
    void errorOccurred(const QString& message);
private:
    QString base_, active_;
    QVariantList profiles_;
    QString loginUid_, loginPassword_,loginExpectedUid_;
    bool persist();
};
