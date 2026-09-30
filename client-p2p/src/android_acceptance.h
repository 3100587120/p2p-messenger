#pragma once

#if defined(Q_OS_ANDROID) && !defined(QT_NO_DEBUG)
class QCoreApplication;
class MessengerController;
class QString;
bool androidAcceptanceHeadless();
bool prepareAndroidAcceptance(QCoreApplication& app, QString* peer);
void startAndroidAcceptance(QCoreApplication& app, MessengerController& controller, const QString& peer);
#endif
