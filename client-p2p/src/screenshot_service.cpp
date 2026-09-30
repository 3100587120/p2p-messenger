#include "screenshot_service.h"
#include <QGuiApplication>
#include <QScreen>
#include <QBackingStore>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QClipboard>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QRect selectionBounds(const QPoint& first,const QPoint& second) {
    return QRect(QPoint(qMin(first.x(),second.x()),qMin(first.y(),second.y())),
                 QPoint(qMax(first.x(),second.x()),qMax(first.y(),second.y())));
}
class RegionWindow final : public QWindow {
public:
    RegionWindow(const QImage& image,const QRect& geometry,std::function<void(QImage)> done)
        : desktop_(image),store_(this),done_(std::move(done)) {
        setSurfaceType(QSurface::RasterSurface);
        setFlags(Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint);
        setGeometry(geometry);setCursor(Qt::CrossCursor);setTitle(QStringLiteral("双点聊 · 自由截图"));
    }
    void finish(const QImage& result={}) {
        if(finished_)return;finished_=true;hide();done_(result);deleteLater();
    }
protected:
    bool event(QEvent* event) override {
        if(event->type()==QEvent::UpdateRequest){paint();return true;}
        if(event->type()==QEvent::Close && !finished_){finish();return true;}
        return QWindow::event(event);
    }
    void exposeEvent(QExposeEvent*) override {paint();}
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()==Qt::RightButton){finish();return;}
        if(event->button()==Qt::LeftButton){start_=end_=event->position().toPoint();dragging_=true;requestUpdate();}
    }
    void mouseMoveEvent(QMouseEvent* event) override {if(dragging_){end_=event->position().toPoint();requestUpdate();}}
    void mouseReleaseEvent(QMouseEvent* event) override {
        if(event->button()!=Qt::LeftButton || !dragging_)return;
        end_=event->position().toPoint();dragging_=false;
        const auto image=ScreenshotService::selectedRegion(desktop_,start_,end_);
        if(image.width()>=3 && image.height()>=3)finish(image);else requestUpdate();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if(event->key()==Qt::Key_Escape)finish();
        else if(event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter){
            const auto image=ScreenshotService::selectedRegion(desktop_,start_,end_);
            if(image.width()>=3 && image.height()>=3)finish(image);
        }
    }
private:
    void paint() {
        if(!isExposed() || finished_)return;
        store_.resize(size());const QRegion dirty(QRect(QPoint(),size()));store_.beginPaint(dirty);
        QPainter painter(store_.paintDevice());painter.drawImage(QPoint(),desktop_);
        painter.fillRect(QRect(QPoint(),size()),QColor(0,0,0,140));
        const auto selected=selectionBounds(start_,end_).intersected(desktop_.rect());
        if(selected.width()>1 && selected.height()>1){painter.drawImage(selected,desktop_,selected);painter.setPen(QPen(QColor("#1685ef"),2));painter.drawRect(selected.adjusted(0,0,-1,-1));}
        painter.fillRect(QRect(width()/2-230,20,460,40),QColor(20,30,45,220));painter.setPen(Qt::white);
        painter.drawText(QRect(width()/2-230,20,460,40),Qt::AlignCenter,QStringLiteral("拖动框选 · 松开完成 · Esc / 右键取消"));
        painter.end();store_.endPaint();store_.flush(dirty);
    }
    QImage desktop_;QBackingStore store_;std::function<void(QImage)> done_;
    QPoint start_,end_;bool dragging_=false,finished_=false;
};
constexpr int hotkeyId=0x53d1;
}

ScreenshotService::ScreenshotService(QObject* parent):QObject(parent) {
#ifdef Q_OS_WIN
    registered_=RegisterHotKey(nullptr,hotkeyId,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'A');
    if(registered_)shortcut_="Ctrl+Alt+A";
    else {registered_=RegisterHotKey(nullptr,hotkeyId,MOD_CONTROL|MOD_SHIFT|MOD_NOREPEAT,'A');if(registered_)shortcut_="Ctrl+Shift+A";}
    if(registered_)QCoreApplication::instance()->installNativeEventFilter(this);
#endif
}
ScreenshotService::~ScreenshotService(){cancel();
#ifdef Q_OS_WIN
    if(registered_){QCoreApplication::instance()->removeNativeEventFilter(this);UnregisterHotKey(nullptr,hotkeyId);}
#endif
}
bool ScreenshotService::nativeEventFilter(const QByteArray&,void* message,qintptr* result) {
#ifdef Q_OS_WIN
    const auto* msg=static_cast<MSG*>(message);
    if(msg->message==WM_HOTKEY && msg->wParam==hotkeyId){if(result)*result=0;emit requested();return true;}
#else
    Q_UNUSED(message);Q_UNUSED(result);
#endif
    return false;
}
QImage ScreenshotService::selectedRegion(const QImage& desktop,const QPoint& start,const QPoint& end) {
    const auto region=selectionBounds(start,end).intersected(desktop.rect());
    return region.width()<3 || region.height()<3?QImage():desktop.copy(region);
}
void ScreenshotService::cancel(){if(auto* window=static_cast<RegionWindow*>(overlay_.data()))window->finish();}
void ScreenshotService::start() {
#ifdef Q_OS_WIN
    if(overlay_)return;
    QRect bounds;for(auto* screen:QGuiApplication::screens())bounds=bounds.united(screen->geometry());
    if(bounds.isEmpty() || qint64(bounds.width())*bounds.height()>64000000){emit error(tr("显示器截图范围不可用或过大"));return;}
    QImage desktop(bounds.size(),QImage::Format_RGB32);
    if(desktop.isNull()){emit error(tr("截图内存不足，请关闭部分程序后重试"));return;}
    desktop.fill(Qt::black);QPainter painter(&desktop);
    for(auto* screen:QGuiApplication::screens()) {
        auto image=screen->grabWindow(0).toImage();
        if(image.isNull()){painter.end();emit error(tr("屏幕截图失败，请检查显示权限或远程桌面"));return;}
        image.setDevicePixelRatio(1);painter.drawImage(screen->geometry().translated(-bounds.topLeft()),image);
    }
    painter.end();
    auto* window=new RegionWindow(desktop,bounds,[this](const QImage& image){
        overlay_.clear();if(image.isNull())emit canceled();else {QGuiApplication::clipboard()->setImage(image);emit selected(image);}
    });
    overlay_=window;window->show();window->requestActivate();
#else
    emit error(tr("此设备不支持桌面截图"));
#endif
}
