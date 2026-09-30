#include "voice_engine.h"
#include <QAudioSource>
#include <QAudioSink>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QCoreApplication>
#include <QPermissions>
#include <QPointer>
#include <QGuiApplication>
#include <QtEndian>
#ifdef P2P_MESSENGER_WITH_DAEMON
#include <speex/speex_echo.h>
#endif

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
        const auto count = qMin<qsizetype>(qMin<qsizetype>(pendingPlayback_.size(), output_->bytesFree()),playbackLive_?640:1920000);
        if (count <= 0) return;
        const auto written = playbackDevice_->write(pendingPlayback_.constData(), count);
        if (written > 0) {if(playbackLive_)referencePlayback(pendingPlayback_.left(written));pendingPlayback_.remove(0, written);}
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
        permissionPending_ = true;
        qApp->requestPermission(permission, this, [this, live, generation](const QPermission&) {
            permissionPending_ = false;
            QTimer::singleShot(250,this,[this,live,generation] {
                if (generation != captureGeneration_) return;
                if (QGuiApplication::applicationState()!=Qt::ApplicationActive) { emit errorOccurred(tr("麦克风授权完成，请返回应用后重试")); return; }
                capture(live);
            });
        });
        return;
    }
    if (status == Qt::PermissionStatus::Denied) { emit errorOccurred(tr("麦克风权限被拒绝，请在系统应用权限中允许后重试")); return; }
    const auto device = QMediaDevices::defaultAudioInput();
    if (device.isNull() || !device.isFormatSupported(pcmFormat())) {
        emit errorOccurred(tr("没有可用的麦克风，或设备不支持 16 kHz 单声道录音")); return;
    }
    live_ = live; recorded_.clear(); frame_.clear();
    if(live)startEcho();else stopEcho();
    input_ = new QAudioSource(device, pcmFormat(), this);
    input_->setBufferSize(6400);
    auto* source = input_->start();
    if (!source || input_->error() != QtAudio::NoError) {
        stopCapture(false); emit errorOccurred(tr("麦克风启动失败，请检查设备和权限")); return;
    }
    const QPointer<QAudioSource> input(input_);
    connect(input_, &QAudioSource::stateChanged, this, [this,input](QtAudio::State state) {
        if (state == QtAudio::StoppedState) QTimer::singleShot(0,this,[this,input] {
            if (input && input_==input && input_->error()!=QtAudio::NoError) {
                stopCapture(false); emit errorOccurred(tr("录音设备发生错误，录音已停止"));
            }
        });
    });
    connect(source, &QIODevice::readyRead, this, [this, source] {
        const auto raw = source->readAll();
        const auto bytes = live_ ? cancelEcho(raw) : raw;
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
    permissionPending_ = false;
    recordDeadline_.stop();
    stopEcho();
    if (input_) { auto* old = input_; input_ = nullptr; old->disconnect(this); old->stop(); old->deleteLater(); }
    frame_.clear();
    // first(n) requires n <= size(). Short/empty recordings used to perform
    // an out-of-bounds read here (send, hang-up and even opening a picker).
    const auto pcm = recorded_.left(1920000); recorded_.clear();
    emit recordingChanged(false);
    if (keepRecording && !live_ && !pcm.isEmpty()) emit recordingReady(pcm);
}
bool VoiceEngine::play(const QByteArray& pcm, bool live) {
    if (pcm.isEmpty() || pcm.size() % 2 || pcm.size() > 1920000) return false;
    if (!live) stopPlayback();
    pendingPlayback_ += pcm;
    if (live && pendingPlayback_.size() > 32000) pendingPlayback_ = pendingPlayback_.last(32000);
    if (!output_) {
        playbackLive_ = live;
        const auto device = QMediaDevices::defaultAudioOutput();
        if (device.isNull() || !device.isFormatSupported(pcmFormat())) {
            emit errorOccurred(tr("没有可用的扬声器，或设备不支持语音播放格式")); return false;
        }
        output_ = new QAudioSink(device, pcmFormat(), this);
        const QPointer<QAudioSink> sink(output_);
        connect(output_, &QAudioSink::stateChanged, this, [this,sink](QtAudio::State state) {
            // Never tear down a backend synchronously inside start()/write().
            if (state == QtAudio::IdleState || state == QtAudio::StoppedState)
                QTimer::singleShot(0,this,[this,sink,state] {
                    if (!sink || output_ != sink) return;
                    if (state == QtAudio::StoppedState && output_->error() != QtAudio::NoError) {
                        stopPlayback(); emit errorOccurred(tr("播放设备发生错误，语音已停止"));
                    } else if (state == QtAudio::IdleState && pendingPlayback_.isEmpty() && !playbackLive_) stopPlayback();
                });
        });
        output_->setBufferSize(6400); playbackDevice_ = output_->start();
        if (!playbackDevice_ || output_->error() != QtAudio::NoError) {
            stopPlayback(); emit errorOccurred(tr("扬声器启动失败")); return false;
        }
        pump_.start();
        emit playbackChanged();
    }
    return true;
}
void VoiceEngine::startEcho() {
    stopEcho();
#ifdef P2P_MESSENGER_WITH_DAEMON
    auto* state=speex_echo_state_init(320,3200); // 20 ms frames, 200 ms tail.
    if(state){int rate=16000;speex_echo_ctl(state,SPEEX_ECHO_SET_SAMPLING_RATE,&rate);echo_=state;}
#endif
}
void VoiceEngine::stopEcho() {
#ifdef P2P_MESSENGER_WITH_DAEMON
    if(echo_)speex_echo_state_destroy(static_cast<SpeexEchoState*>(echo_));
#endif
    echo_=nullptr;echoRecorded_.clear();echoPlayback_.clear();
}
void VoiceEngine::referencePlayback(const QByteArray& pcm) {
#ifdef P2P_MESSENGER_WITH_DAEMON
    if(!echo_)return;echoPlayback_+=pcm;
    while(echoPlayback_.size()>=640){spx_int16_t reference[320];for(int i=0;i<320;++i)reference[i]=qFromLittleEndian<qint16>(echoPlayback_.constData()+i*2);
        speex_echo_playback(static_cast<SpeexEchoState*>(echo_),reference);echoPlayback_.remove(0,640);
    }
#else
    Q_UNUSED(pcm);
#endif
}
QByteArray VoiceEngine::cancelEcho(const QByteArray& pcm) {
#ifdef P2P_MESSENGER_WITH_DAEMON
    if(!echo_)return pcm;echoRecorded_+=pcm;QByteArray result;
    while(echoRecorded_.size()>=640){spx_int16_t input[320],output[320];for(int i=0;i<320;++i)input[i]=qFromLittleEndian<qint16>(echoRecorded_.constData()+i*2);
        speex_echo_capture(static_cast<SpeexEchoState*>(echo_),input,output);const auto at=result.size();result.resize(at+640);for(int i=0;i<320;++i)qToLittleEndian<qint16>(output[i],result.data()+at+i*2);echoRecorded_.remove(0,640);
    }return result;
#else
    return pcm;
#endif
}
QByteArray VoiceEngine::mixFrames(const QList<QByteArray>& frames) {
    if(frames.isEmpty() || frames.size()>5)return {};
    for(const auto& frame:frames)if(frame.size()!=3200)return {};
    QByteArray result(3200,Qt::Uninitialized);
    for(int i=0;i<1600;++i){int sum=0;for(const auto& frame:frames)sum+=qFromLittleEndian<qint16>(frame.constData()+i*2);
        qToLittleEndian<qint16>(qBound(-32768,sum,32767),result.data()+i*2);
    }return result;
}
void VoiceEngine::stopPlayback() {
    pump_.stop(); playbackDevice_ = nullptr; pendingPlayback_.clear();
    if (output_) { output_->stop(); output_->deleteLater(); output_ = nullptr; }
    emit playbackChanged();
}
