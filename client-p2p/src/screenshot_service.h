#pragma once
#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QImage>
#include <QPointer>
#include <QWindow>

class ScreenshotService final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    Q_PROPERTY(QString shortcutText READ shortcutText CONSTANT)
public:
    explicit ScreenshotService(QObject* parent=nullptr);
    ~ScreenshotService() override;
    QString shortcutText() const {return shortcut_;}
    void start();
    void cancel();
    static QImage selectedRegion(const QImage& desktop,const QPoint& start,const QPoint& end);
    bool nativeEventFilter(const QByteArray&,void*,qintptr*) override;
signals:
    void requested();
    void selected(const QImage& image);
    void canceled();
    void error(const QString& reason);
private:
    QPointer<QWindow> overlay_;
    QString shortcut_;
    bool registered_=false;
};
