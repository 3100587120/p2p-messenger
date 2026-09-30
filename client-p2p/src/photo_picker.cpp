#include "photo_picker.h"
#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QPointer>
#include <QtCore/qnativeinterface.h>
namespace {
QPointer<QObject> receiver;
std::function<void(QString, QString)> resultCallback;
void result(JNIEnv* env, jclass, jstring path, jstring error) {
    const auto read = [env](jstring s) {
        if (!s) return QString();
        const auto* chars = env->GetStringChars(s, nullptr);
        if (!chars) return QString();
        const auto text = QString::fromUtf16(reinterpret_cast<const char16_t*>(chars),env->GetStringLength(s));
        env->ReleaseStringChars(s,chars); return text;
    };
    const auto file = read(path), message = read(error);
    QMetaObject::invokeMethod(QCoreApplication::instance(), [file,message] {
        const auto target = receiver; const auto callback = resultCallback;
        receiver.clear(); resultCallback = {};
        if (target && callback) callback(file,message);
    },Qt::QueuedConnection);
}
}
#endif
bool openAvatarGallery(QObject* target, std::function<void(QString, QString)> callback, int mode) {
#ifdef Q_OS_ANDROID
    if (receiver) return false;
    QJniEnvironment env;
    const JNINativeMethod methods[] {{const_cast<char*>("result"),const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)V"),reinterpret_cast<void*>(result)}};
    if (!env.registerNativeMethods("io/p2pmessenger/app/AvatarPhotoActivity",methods,1)) return false;
    receiver = target; resultCallback = std::move(callback);
    const auto context = QNativeInterface::QAndroidApplication::context();
    QJniObject::callStaticMethod<void>("io/p2pmessenger/app/AvatarPhotoActivity","open","(Landroid/content/Context;I)V",context.object<jobject>(),jint(mode));
    if (env.checkAndClearExceptions()) { receiver.clear(); resultCallback = {}; return false; }
    return true;
#else
    Q_UNUSED(target); Q_UNUSED(callback); Q_UNUSED(mode); return false;
#endif
}
