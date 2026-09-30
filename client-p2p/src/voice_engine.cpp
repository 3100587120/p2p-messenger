#include "voice_engine.h"
#include <QAudioSource>
#include <QAudioSink>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QCoreApplication>
#include <QPermissions>

namespace {
QAudioFormat pcmFormat() {
    QAudioFormat format;
    format.setSampleRate(16000); format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}
}
VoiceEngine::VoiceEngine(QObject* parent) : QObject(parent) {
    pump_.setInterval(20);
    connect(&pump_, &QTimer::timeout, this, [this] {
        if (!output_ || !playbackDevice_) return;
        const auto count = qMin<qsizetype>(pendingPlayback_.size(), output_->bytesFree());
        if (count <= 0) return;
        const auto written = playbackDevice_->write(pendingPlayback_.constData(), count);
        if (written > 0) pendingPlayback_.remove(0, written);
    });
    recordDeadline_.setSingleShot(true);
    connect(&recordDeadline_, &QTimer::timeout, this, [this] { stopCapture(true); });
}
VoiceEngine::~VoiceEngine() { stopCapture(false); stopPlayback(); }
void VoiceEngine::capture(bool live) {
    if (input_) { emit errorOccurred(tr("已有录音或通话正在使用麦克风")); return; }
    const auto generation = ++captureGeneration_;
    QMicrophonePermission permission;
    const auto status = qApp->checkPermission(permission);
    if (status == Qt::PermissionStatus::Undetermined) {
        qApp->requestPermission(permission, this, [this, live, generation](const QPermission&) {
            if (generation == captureGeneration_) capture(live);
        });
        return;
    }
    if (status == Qt::PermissionStatus::Denied) { emit errorOccurred(tr("麦克风权限被拒绝，请在系统应用权限中允许后重试")); return; }
    const auto device = QMediaDevices::defaultAudioInput();
    if (device.isNull() || !device.isFormatSupported(pcmFormat())) {
        emit errorOccurred(tr("没有可用的麦克风，或设备不支持 16 kHz 单声道录音")); return;
    }
    live_ = live; recorded_.clear(); frame_.clear();
    input_ = new QAudioSource(device, pcmFormat(), this);
    input_->setBufferSize(6400);
    auto* source = input_->start();
    if (!source || input_->error() != QtAudio::NoError) {
        stopCapture(false); emit errorOccurred(tr("麦克风启动失败，请检查设备和权限")); return;
    }
    connect(input_, &QAudioSource::stateChanged, this, [this](QtAudio::State state) {
        if (input_ && state == QtAudio::StoppedState && input_->error() != QtAudio::NoError) {
            stopCapture(false); emit errorOccurred(tr("录音设备发生错误，录音已停止"));
        }
    });
    connect(source, &QIODevice::readyRead, this, [this, source] {
        const auto bytes = source->readAll();
        if (live_) {
            frame_ += bytes;
            while (frame_.size() >= 3200) {
                const auto chunk = frame_.first(3200); frame_.remove(0, 3200);
                emit pcmReady(chunk);
                if (!input_) return;
            }
        } else {
            recorded_ += bytes;
            if (recorded_.size() >= 1920000) stopCapture(true);
        }
    });
    if (!live_) recordDeadline_.start(60000);
    emit recordingChanged(true);
}
void VoiceEngine::stopCapture(bool keepRecording) {
    ++captureGeneration_;
    recordDeadline_.stop();
    if (input_) { auto* old = input_; input_ = nullptr; old->disconnect(this); old->stop(); old->deleteLater(); }
    frame_.clear();
    const auto pcm = recorded_.first(1920000); recorded_.clear();
    emit recordingChanged(false);
    if (keepRecording && !live_ && !pcm.isEmpty()) emit recordingReady(pcm);
}
bool VoiceEngine::play(const QByteArray& pcm, bool live) {
    if (pcm.isEmpty() || pcm.size() % 2 || pcm.size() > 1920000) return false;
    if (!live) stopPlayback();
    if (!output_) {
        playbackLive_ = live;
        const auto device = QMediaDevices::defaultAudioOutput();
        if (device.isNull() || !device.isFormatSupported(pcmFormat())) {
            emit errorOccurred(tr("没有可用的扬声器，或设备不支持语音播放格式")); return false;
        }
        output_ = new QAudioSink(device, pcmFormat(), this);
        connect(output_, &QAudioSink::stateChanged, this, [this](QtAudio::State state) {
            if (output_ && state == QtAudio::IdleState && pendingPlayback_.isEmpty() && !playbackLive_) stopPlayback();
        });
        output_->setBufferSize(6400); playbackDevice_ = output_->start();
        if (!playbackDevice_ || output_->error() != QtAudio::NoError) {
            stopPlayback(); emit errorOccurred(tr("扬声器启动失败")); return false;
        }
        pump_.start();
    }
    pendingPlayback_ += pcm;
    if (live && pendingPlayback_.size() > 32000) pendingPlayback_ = pendingPlayback_.last(32000);
    return true;
}
void VoiceEngine::stopPlayback() {
    pump_.stop(); playbackDevice_ = nullptr; pendingPlayback_.clear();
    if (output_) { output_->stop(); output_->deleteLater(); output_ = nullptr; }
}
