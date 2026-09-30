#pragma once

#if defined(Q_OS_ANDROID) && !defined(QT_NO_DEBUG)
class QGuiApplication;
class MessengerController;
class QString;
bool prepareAndroidAcceptance(QGuiApplication& app, QString* peer);
void startAndroidAcceptance(QGuiApplication& app, MessengerController& controller, const QString& peer);
#endif
