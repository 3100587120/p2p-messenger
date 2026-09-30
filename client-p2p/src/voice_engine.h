#pragma once
#include <QObject>
#include <QByteArray>
#include <QTimer>
class QAudioSource;
class QAudioSink;
class QIODevice;

// Audio hardware belongs to Qt. Only fixed-format PCM leaves this adapter;
// callers encrypt it and decide which authenticated peer may receive it.
class VoiceEngine final : public QObject {
    Q_OBJECT
public:
    explicit VoiceEngine(QObject* parent = nullptr);
    ~VoiceEngine() override;
    void capture(bool live);
    void stopCapture(bool keepRecording);
    bool play(const QByteArray& pcm, bool live = false);
    void stopPlayback();
    bool permissionPending() const { return permissionPending_; }
signals:
    void recordingChanged(bool recording);
    void pcmReady(const QByteArray& pcm);
    void recordingReady(const QByteArray& pcm);
    void errorOccurred(const QString& reason);
private:
    QAudioSource* input_ {nullptr};
    QAudioSink* output_ {nullptr};
    QIODevice* playbackDevice_ {nullptr};
    QByteArray recorded_, pendingPlayback_, frame_;
    QTimer pump_, recordDeadline_;
    bool live_ {false};
    bool playbackLive_ {false};
    bool permissionPending_ {false};
    quint64 captureGeneration_ {0};
};
