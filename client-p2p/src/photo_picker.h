#pragma once
#include <QObject>
#include <QString>
#include <functional>
bool systemPickerOpen();
bool openAvatarGallery(QObject* receiver, std::function<void(QString, QString)> callback, int mode = 0);
