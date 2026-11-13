#include "voice_chat.h"

#include "audio_backend.h"
#include "voice_config.h"
#include "runtime_log.h"
#include "voice/voice_codec.h"
#include "voice/voice_dsp.h"
#include "voice/voice_echo.h"
#include "voice/voice_jitter_buffer.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>

namespace VoiceChat {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kFrameBytes = Voice::kFrameSamples * static_cast<int>(sizeof(float));
// Voices are mixed to stereo so proximity chat can place them left or right.
constexpr int kOutputChannels = 2;
constexpr int kOutputFrameBytes = kFrameBytes * kOutputChannels;
// Output is topped up to this much queued audio. Two frames (40 ms) is enough
// to ride out worker scheduling hiccups without adding noticeable latency.
constexpr int kPlaybackQueueBytes = 2 * kOutputFrameBytes;
constexpr auto kWorkerTick = std::chrono::milliseconds(5);
// The mic test's own voice is mixed as a speaker with this id.
constexpr uint64_t kLoopbackSpeaker = ~uint64_t{0};

// Decoded audio above this counts as speech for the talking indicator.
constexpr float kSpeechFloorDb = -50.0f;
// How long a speaker still shows as talking after their last speech frame.
constexpr int kTalkingHoldFrames = 15;  // 300 ms

// "Lower game volume while talking" turns the game down to this (about -12 dB),
// ramping by kDuckStep per worker tick so it never clicks.
constexpr float kDuckGain = 0.25f;
constexpr float kDuckStep = 0.05f;

// The echo canceller's timeline: 48 kHz frames on the steady clock. Starts ten
// seconds in so positions slightly in the past are never negative.
int64_t TimelineNow() {
    static const Clock::time_point epoch = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - epoch).count();
    return (elapsed * Voice::kSampleRate) / 1000000 + int64_t{10} * Voice::kSampleRate;
}

// PlaybackReference sources.
constexpr int kGameSource = 0;
constexpr int kVoiceSource = 1;

void OnGamePlayback(const int16_t* samples, size_t frames, uint32_t channels, uint32_t sampleRate,
                    uint32_t queuedBytes, float gain);

struct Speaker {
    Voice::JitterBuffer jitter;
    Voice::Decoder decoder;
    bool decoderReady = false;
    // Targets set by SetSpeakerGain, and the gains at the end of the last mixed frame.
    float gainLeft = 1.0f;
    float gainRight = 1.0f;
    float appliedLeft = 1.0f;
    float appliedRight = 1.0f;
    int framesSinceSpeech = kTalkingHoldFrames;
};

class Engine {
public:
    static Engine& Instance() {
        static Engine engine;
        return engine;
    }

    Settings CurrentSettings() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_settings;
    }

    void Apply(const Settings& settings) {
        bool start = false;
        bool stop = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const bool deviceChanged = settings.inputDevice != m_settings.inputDevice;
            start = settings.enabled && !m_running;
            stop = !settings.enabled && m_running;
            m_settings = settings;
            m_referenceWanted = m_running && settings.echoCancellation;
            if (m_running && !stop && deviceChanged) {
                CloseCaptureLocked();
                OpenCaptureLocked();
                // A different microphone has a different echo path and noise.
                m_echo.Reset();
            }
        }
        if (start) {
            Start();
        } else if (stop) {
            Stop();
        }
    }

    void Start() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return;
        }
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SetUnavailableLocked(std::string("audio is unavailable: ") + SDL_GetError());
            return;
        }
        m_running = true;
        if (!m_encoder.Init()) {
            SetUnavailableLocked("the Opus encoder could not be created");
        }
        m_echoReady = m_echo.Init();
        if (!m_echoReady) {
            RT_LOGF(RT_TAG_AUDIO, "voice chat: echo cancellation could not be started\n");
        }
        {
            std::lock_guard<std::mutex> referenceLock(m_referenceMutex);
            m_reference.Reset();
        }
        m_captureCursor.Reset();
        m_referenceWanted = m_settings.echoCancellation;
        AudioBackend::Instance().SetPlaybackTap(&OnGamePlayback);
        OpenPlaybackLocked();
        OpenCaptureLocked();
        m_stop = false;
        m_worker = std::thread([this] { Run(); });
        RT_LOGF(RT_TAG_AUDIO, "voice chat: started\n");
    }

    void Stop() {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) {
                return;
            }
            m_stop = true;
            worker = std::move(m_worker);
        }
        if (worker.joinable()) {
            worker.join();
        }
        AudioBackend::Instance().SetPlaybackTap(nullptr);
        m_referenceWanted = false;
        m_duck = 1.0f;
        m_duckTarget = 1.0f;
        AudioBackend::Instance().SetDuckGain(1.0f);
        std::lock_guard<std::mutex> lock(m_mutex);
        CloseCaptureLocked();
        if (m_playback) {
            SDL_DestroyAudioStream(m_playback);
            m_playback = nullptr;
        }
        m_speakers.clear();
        m_pendingGains.clear();
        m_gate.Reset();
        m_transmitting = false;
        m_levelDb = Voice::kSilenceDb;
        m_micState = MicState::Off;
        m_micError.clear();
        m_running = false;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        RT_LOGF(RT_TAG_AUDIO, "voice chat: stopped\n");
    }

    void SetPushToTalkHeld(bool held) { m_pushToTalkHeld = held; }
    void SetFrameSink(FrameSink sink) { m_sink = sink; }

    void SetMicTest(bool enabled) {
        m_micTest = enabled;
        if (!enabled) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_speakers.erase(kLoopbackSpeaker);
        }
    }
    bool MicTest() const { return m_micTest; }

    MicState GetMicState() const { return m_micState; }
    std::string MicError() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_micError;
    }
    float InputLevelDb() const { return m_levelDb; }
    bool Transmitting() const { return m_transmitting; }

    void DeliverFrame(SpeakerId speaker, uint16_t seq, const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running && speaker != kLoopbackSpeaker) {
            DeliverLocked(speaker, seq, data, size);
        }
    }

    void RemoveSpeaker(SpeakerId speaker) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_speakers.erase(speaker);
    }

    void SetSpeakerGain(SpeakerId speaker, float left, float right) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            return;
        }
        left = std::clamp(left, 0.0f, 2.0f);
        right = std::clamp(right, 0.0f, 2.0f);
        // Only an existing speaker is updated: a gain alone must not create one.
        const auto it = m_speakers.find(speaker);
        if (it != m_speakers.end()) {
            it->second.gainLeft = left;
            it->second.gainRight = right;
        } else {
            m_pendingGains[speaker] = {left, right};
        }
    }

    bool SpeakerTalking(SpeakerId speaker) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_speakers.find(speaker);
        return it != m_speakers.end() && it->second.framesSinceSpeech < kTalkingHoldFrames;
    }

    // Game audio about to be queued for the speakers (AudioBackend's tap).
    void AddGamePlayback(const int16_t* samples, size_t frames, uint32_t channels, uint32_t sampleRate,
                         uint32_t queuedBytes, float gain) {
        if (!m_referenceWanted || channels == 0 || sampleRate == 0) {
            return;
        }
        const double queuedSeconds =
            static_cast<double>(queuedBytes) / (static_cast<double>(sampleRate) * channels * sizeof(int16_t));
        const int64_t start = TimelineNow() + std::llround(queuedSeconds * Voice::kSampleRate);
        std::lock_guard<std::mutex> lock(m_referenceMutex);
        if (m_gameResampler.Convert(samples, frames, channels, sampleRate, gain, m_gameConverted)) {
            m_reference.Add(kGameSource, start, m_gameConverted.data(),
                            m_gameConverted.size() / Voice::kEchoReferenceChannels);
        }
    }

private:
    Engine() = default;
    // Normally Shutdown() has already run. If an exit path skipped it, only the
    // worker is stopped here: SDL may already be torn down during static destruction.
    ~Engine() {
        m_stop = true;
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    static SDL_AudioSpec VoiceSpec(int channels) {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = channels;
        spec.freq = Voice::kSampleRate;
        return spec;
    }

    void SetUnavailableLocked(std::string error) {
        RT_LOGF(RT_TAG_AUDIO, "voice chat: %s\n", error.c_str());
        m_micState = MicState::Unavailable;
        m_micError = std::move(error);
    }

    SDL_AudioDeviceID FindInputDeviceLocked() const {
        if (m_settings.inputDevice.empty()) {
            return SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
        }
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
        SDL_AudioDeviceID found = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
        for (int i = 0; devices && i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(devices[i]);
            if (name && m_settings.inputDevice == name) {
                found = devices[i];
                break;
            }
        }
        SDL_free(devices);
        return found;
    }

#if defined(__ANDROID__)
    // Android asks the player before an app may record, but never from in here: its prompt is a
    // flat window, and one over the immersive game stops the OpenXR session, which came back
    // black. The launcher asks when Play is pressed (LauncherActivity.askForMicrophone). SDL's
    // own device open would ask, so check first and leave the microphone closed without it.
    static bool HasMicPermission() {
        auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
        if (!env || !activity) {
            return false;
        }
        bool granted = false;
        if (jclass type = env->GetObjectClass(activity)) {
            jmethodID check = env->GetMethodID(type, "checkSelfPermission", "(Ljava/lang/String;)I");
            jstring name = env->NewStringUTF("android.permission.RECORD_AUDIO");
            if (check && name) {
                granted = env->CallIntMethod(activity, check, name) == 0;  // PERMISSION_GRANTED
            }
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                granted = false;
            }
            if (name) env->DeleteLocalRef(name);
            env->DeleteLocalRef(type);
        }
        env->DeleteLocalRef(activity);
        return granted;
    }
#endif

    void OpenCaptureLocked() {
#if defined(__ANDROID__)
        if (!HasMicPermission()) {
            SetUnavailableLocked("voice chat needs the microphone. Quit to the WiiCompiled launcher and press Play "
                                 "to allow it, or allow it in the headset's Settings > Privacy > App permissions > "
                                 "Microphone, then turn voice chat off and on");
            return;
        }
#endif
        const SDL_AudioSpec spec = VoiceSpec(1);
        m_capture = SDL_OpenAudioDeviceStream(FindInputDeviceLocked(), &spec, nullptr, nullptr);
        if (!m_capture) {
            SetUnavailableLocked(std::string("the microphone could not be opened: ") + SDL_GetError());
            return;
        }
        if (!SDL_ResumeAudioStreamDevice(m_capture)) {
            SetUnavailableLocked(std::string("the microphone could not be started: ") + SDL_GetError());
            SDL_DestroyAudioStream(m_capture);
            m_capture = nullptr;
            return;
        }
        m_captured.clear();
        m_micState = MicState::Open;
        m_micError.clear();
    }

    void CloseCaptureLocked() {
        if (m_capture) {
            SDL_DestroyAudioStream(m_capture);
            m_capture = nullptr;
        }
        m_captured.clear();
    }

    void OpenPlaybackLocked() {
        const SDL_AudioSpec spec = VoiceSpec(kOutputChannels);
        m_playback = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!m_playback || !SDL_ResumeAudioStreamDevice(m_playback)) {
            RT_LOGF(RT_TAG_AUDIO, "voice chat: playback could not be opened: %s\n", SDL_GetError());
            if (m_playback) {
                SDL_DestroyAudioStream(m_playback);
                m_playback = nullptr;
            }
        }
    }

    void Run() {
        while (!m_stop) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ProcessCaptureLocked();
                ProcessPlaybackLocked();
            }
            UpdateDuck();
            std::this_thread::sleep_for(kWorkerTick);
        }
    }

    // Ramps the game's volume toward what "lower game volume while talking" wants.
    void UpdateDuck() {
        const float target = m_duckTarget;
        if (m_duck == target) {
            return;
        }
        m_duck = m_duck < target ? std::min(target, m_duck + kDuckStep) : std::max(target, m_duck - kDuckStep);
        AudioBackend::Instance().SetDuckGain(m_duck);
    }

    void ProcessCaptureLocked() {
        if (!m_capture) {
            m_levelDb = Voice::kSilenceDb;
            m_transmitting = false;
            m_duckTarget = 1.0f;
            return;
        }
        std::array<float, Voice::kFrameSamples> chunk{};
        for (;;) {
            const int got = SDL_GetAudioStreamData(m_capture, chunk.data(), kFrameBytes);
            if (got <= 0) {
                break;
            }
            m_captured.insert(m_captured.end(), chunk.begin(), chunk.begin() + got / sizeof(float));
        }
        // The newest captured sample arrived just now; older ones are counted back from it.
        const int64_t capturedEnd = TimelineNow();
        size_t offset = 0;
        while (m_captured.size() - offset >= static_cast<size_t>(Voice::kFrameSamples)) {
            const int64_t frameStart = capturedEnd - static_cast<int64_t>(m_captured.size() - offset);
            ProcessFrameLocked(m_captured.data() + offset, frameStart);
            offset += Voice::kFrameSamples;
        }
        m_captured.erase(m_captured.begin(), m_captured.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    // Removes echo and background noise from one captured frame, before the
    // level, the activation gate and the encoder see it.
    void CleanFrameLocked(float* frame, int64_t captureStart) {
        const bool cancelEcho = m_settings.echoCancellation;
        if (!m_echoReady || (!cancelEcho && !m_settings.noiseSuppression)) {
            return;
        }
        const int64_t start = m_captureCursor.Place(captureStart, Voice::kFrameSamples);
        std::array<float, Voice::kEchoFrameSamples * Voice::kEchoReferenceChannels> reference{};
        for (int part = 0; part < Voice::kFrameSamples / Voice::kEchoFrameSamples; ++part) {
            if (cancelEcho) {
                std::lock_guard<std::mutex> lock(m_referenceMutex);
                m_reference.Read(start + int64_t{part} * Voice::kEchoFrameSamples, reference.data(),
                                 Voice::kEchoFrameSamples);
            }
            m_echo.Process(frame + part * Voice::kEchoFrameSamples, reference.data(), cancelEcho,
                           m_settings.noiseSuppression);
        }
    }

    void ProcessFrameLocked(float* frame, int64_t captureStart) {
        CleanFrameLocked(frame, captureStart);
        Voice::ApplyGain(frame, Voice::kFrameSamples, m_settings.inputVolume);
        const float level = Voice::LevelDb(frame, Voice::kFrameSamples);
        m_levelDb = level;
        const bool gateOpen = m_gate.Update(level, m_settings.activationThresholdDb);
        const bool transmit = m_settings.pushToTalk ? m_pushToTalkHeld.load() : gateOpen;
        m_transmitting = transmit;
        m_duckTarget = transmit && m_settings.lowerGameVolume ? kDuckGain : 1.0f;
        if (!transmit) {
            return;
        }
        std::array<uint8_t, Voice::kMaxPacketBytes> packet{};
        const int size = m_encoder.Encode(frame, packet.data(), static_cast<int>(packet.size()));
        // Two bytes or fewer is a DTX frame: nothing worth sending.
        if (size <= 2) {
            return;
        }
        const uint16_t seq = m_sequence++;
        if (const FrameSink sink = m_sink.load()) {
            sink(seq, packet.data(), static_cast<size_t>(size));
        }
        if (m_micTest) {
            DeliverLocked(kLoopbackSpeaker, seq, packet.data(), static_cast<size_t>(size));
        }
    }

    void DeliverLocked(uint64_t speakerId, uint16_t seq, const uint8_t* data, size_t size) {
        const auto [it, inserted] = m_speakers.try_emplace(speakerId);
        Speaker& speaker = it->second;
        if (inserted) {
            // A gain set before the first frame (a muted player) applies from
            // the very first sample.
            if (const auto pending = m_pendingGains.find(speakerId); pending != m_pendingGains.end()) {
                speaker.gainLeft = speaker.appliedLeft = pending->second.first;
                speaker.gainRight = speaker.appliedRight = pending->second.second;
                m_pendingGains.erase(pending);
            }
        }
        if (!speaker.decoderReady) {
            speaker.decoderReady = speaker.decoder.Init();
        }
        speaker.jitter.Push(seq, data, size);
    }

    void ProcessPlaybackLocked() {
        if (!m_playback) {
            return;
        }
        while (SDL_GetAudioStreamQueued(m_playback) < kPlaybackQueueBytes) {
            std::array<float, Voice::kFrameSamples * kOutputChannels> mix{};
            MixFrameLocked(mix.data());
            if (m_referenceWanted) {
                // Other players' voices come out of the speakers too.
                const int queued = std::max(SDL_GetAudioStreamQueued(m_playback), 0);
                const int64_t start = TimelineNow() + queued / static_cast<int>(sizeof(float) * kOutputChannels);
                std::lock_guard<std::mutex> lock(m_referenceMutex);
                m_reference.Add(kVoiceSource, start, mix.data(), Voice::kFrameSamples);
            }
            if (!SDL_PutAudioStreamData(m_playback, mix.data(), kOutputFrameBytes)) {
                break;
            }
        }
    }

    void MixFrameLocked(float* mix) {
        std::array<float, Voice::kFrameSamples> voice{};
        const float volume = m_settings.muteAll ? 0.0f : m_settings.volume;
        for (auto& [id, speaker] : m_speakers) {
            // Buffers keep draining while muted so unmuting does not replay old audio.
            const Voice::JitterBuffer::Output out = speaker.jitter.Pop();
            if (!speaker.decoderReady || out.kind == Voice::JitterBuffer::Kind::Silent) {
                speaker.framesSinceSpeech = std::min(speaker.framesSinceSpeech + 1, kTalkingHoldFrames);
                speaker.appliedLeft = speaker.gainLeft;
                speaker.appliedRight = speaker.gainRight;
                continue;
            }
            if (out.kind == Voice::JitterBuffer::Kind::Frame) {
                speaker.decoder.Decode(out.packet->data(), out.packet->size(), voice.data());
            } else if (out.next) {
                speaker.decoder.DecodeFec(out.next->data(), out.next->size(), voice.data());
            } else {
                speaker.decoder.Conceal(voice.data());
            }
            if (Voice::LevelDb(voice.data(), voice.size()) >= kSpeechFloorDb) {
                speaker.framesSinceSpeech = 0;
            } else {
                speaker.framesSinceSpeech = std::min(speaker.framesSinceSpeech + 1, kTalkingHoldFrames);
            }
            // Ramp each channel from last frame's gain to the new one across this frame.
            const float fromLeft = volume * speaker.appliedLeft;
            const float fromRight = volume * speaker.appliedRight;
            const float stepLeft = (volume * speaker.gainLeft - fromLeft) / static_cast<float>(Voice::kFrameSamples);
            const float stepRight = (volume * speaker.gainRight - fromRight) / static_cast<float>(Voice::kFrameSamples);
            for (int i = 0; i < Voice::kFrameSamples; ++i) {
                const float t = static_cast<float>(i + 1);
                mix[2 * i] += voice[i] * (fromLeft + stepLeft * t);
                mix[2 * i + 1] += voice[i] * (fromRight + stepRight * t);
            }
            speaker.appliedLeft = speaker.gainLeft;
            speaker.appliedRight = speaker.gainRight;
        }
        for (int i = 0; i < Voice::kFrameSamples * kOutputChannels; ++i) {
            mix[i] = Voice::SoftLimit(mix[i]);
        }
    }

    std::mutex m_mutex;
    Settings m_settings;
    bool m_running = false;
    std::thread m_worker;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_pushToTalkHeld{false};
    std::atomic<bool> m_micTest{false};
    std::atomic<FrameSink> m_sink{nullptr};
    std::atomic<bool> m_transmitting{false};
    std::atomic<float> m_levelDb{Voice::kSilenceDb};
    std::atomic<MicState> m_micState{MicState::Off};
    std::string m_micError;

    SDL_AudioStream* m_capture = nullptr;
    SDL_AudioStream* m_playback = nullptr;
    std::vector<float> m_captured;
    Voice::Encoder m_encoder;
    Voice::ActivationGate m_gate;
    uint16_t m_sequence = 0;
    std::map<uint64_t, Speaker> m_speakers;
    std::map<uint64_t, std::pair<float, float>> m_pendingGains;

    // Echo cancellation. The reference is written from the game's audio thread
    // and the worker, so it has its own lock, always taken last.
    Voice::EchoCanceller m_echo;
    bool m_echoReady = false;
    Voice::TimelineCursor m_captureCursor;
    std::atomic<bool> m_referenceWanted{false};
    std::mutex m_referenceMutex;
    Voice::PlaybackReference m_reference;
    Voice::ReferenceResampler m_gameResampler;
    std::vector<float> m_gameConverted;

    // Lowering the game's volume while talking: the target is set per frame,
    // the worker ramps toward it.
    std::atomic<float> m_duckTarget{1.0f};
    float m_duck = 1.0f;
};

void OnGamePlayback(const int16_t* samples, size_t frames, uint32_t channels, uint32_t sampleRate,
                    uint32_t queuedBytes, float gain) {
    Engine::Instance().AddGamePlayback(samples, frames, channels, sampleRate, queuedBytes, gain);
}

Settings SettingsFromConfig() {
    Settings settings;
    settings.enabled = VoiceConfig::Enabled();
    settings.pushToTalk = VoiceConfig::PushToTalk();
    settings.activationThresholdDb = VoiceConfig::ActivationThresholdDb();
    settings.inputDevice = VoiceConfig::InputDevice();
    settings.inputVolume = VoiceConfig::InputVolume();
    settings.echoCancellation = VoiceConfig::EchoCancellation();
    settings.noiseSuppression = VoiceConfig::NoiseSuppression();
    settings.lowerGameVolume = VoiceConfig::LowerGameVolume();
    settings.volume = VoiceConfig::Volume();
    settings.muteAll = VoiceConfig::MuteAll();
    return settings;
}

}  // namespace

void Initialize() {
    Engine::Instance().Apply(SettingsFromConfig());
}

void Shutdown() {
    Engine::Instance().Stop();
}

Settings CurrentSettings() {
    return Engine::Instance().CurrentSettings();
}

void ApplySettings(const Settings& settings) {
    Engine::Instance().Apply(settings);
}

void SetPushToTalkHeld(bool held) {
    Engine::Instance().SetPushToTalkHeld(held);
}

void SetMicTest(bool enabled) {
    Engine::Instance().SetMicTest(enabled);
}

bool MicTestEnabled() {
    return Engine::Instance().MicTest();
}

MicState GetMicState() {
    return Engine::Instance().GetMicState();
}

std::string MicError() {
    return Engine::Instance().MicError();
}

float InputLevelDb() {
    return Engine::Instance().InputLevelDb();
}

bool Transmitting() {
    return Engine::Instance().Transmitting();
}

void SetFrameSink(FrameSink sink) {
    Engine::Instance().SetFrameSink(sink);
}

void DeliverFrame(SpeakerId speaker, uint16_t seq, const uint8_t* data, size_t size) {
    Engine::Instance().DeliverFrame(speaker, seq, data, size);
}

void RemoveSpeaker(SpeakerId speaker) {
    Engine::Instance().RemoveSpeaker(speaker);
}

void SetSpeakerGain(SpeakerId speaker, float left, float right) {
    Engine::Instance().SetSpeakerGain(speaker, left, right);
}

bool SpeakerTalking(SpeakerId speaker) {
    return Engine::Instance().SpeakerTalking(speaker);
}

std::vector<std::string> InputDeviceNames() {
    std::vector<std::string> names;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        return names;
    }
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
    for (int i = 0; devices && i < count; ++i) {
        if (const char* name = SDL_GetAudioDeviceName(devices[i])) {
            names.emplace_back(name);
        }
    }
    SDL_free(devices);
    return names;
}

}  // namespace VoiceChat
