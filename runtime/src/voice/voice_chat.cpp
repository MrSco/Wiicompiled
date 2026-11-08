#include "voice_chat.h"

#include "voice_config.h"
#include "runtime_log.h"
#include "voice/voice_codec.h"
#include "voice/voice_dsp.h"
#include "voice/voice_jitter_buffer.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>

namespace VoiceChat {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kFrameBytes = Voice::kFrameSamples * static_cast<int>(sizeof(float));
// Output is topped up to this much queued audio. Two frames (40 ms) is enough
// to ride out worker scheduling hiccups without adding noticeable latency.
constexpr int kPlaybackQueueBytes = 2 * kFrameBytes;
constexpr auto kWorkerTick = std::chrono::milliseconds(5);
// The mic test's own voice is mixed as a speaker with this id.
constexpr uint64_t kLoopbackSpeaker = ~uint64_t{0};

struct Speaker {
    Voice::JitterBuffer jitter;
    Voice::Decoder decoder;
    bool decoderReady = false;
    float gain = 1.0f;
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
            if (m_running && !stop && deviceChanged) {
                CloseCaptureLocked();
                OpenCaptureLocked();
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
        std::lock_guard<std::mutex> lock(m_mutex);
        CloseCaptureLocked();
        if (m_playback) {
            SDL_DestroyAudioStream(m_playback);
            m_playback = nullptr;
        }
        m_speakers.clear();
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

    static SDL_AudioSpec VoiceSpec() {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = 1;
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

    void OpenCaptureLocked() {
        const SDL_AudioSpec spec = VoiceSpec();
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
        const SDL_AudioSpec spec = VoiceSpec();
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
            std::this_thread::sleep_for(kWorkerTick);
        }
    }

    void ProcessCaptureLocked() {
        if (!m_capture) {
            m_levelDb = Voice::kSilenceDb;
            m_transmitting = false;
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
        size_t offset = 0;
        while (m_captured.size() - offset >= static_cast<size_t>(Voice::kFrameSamples)) {
            ProcessFrameLocked(m_captured.data() + offset);
            offset += Voice::kFrameSamples;
        }
        m_captured.erase(m_captured.begin(), m_captured.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    void ProcessFrameLocked(float* frame) {
        Voice::ApplyGain(frame, Voice::kFrameSamples, m_settings.inputVolume);
        const float level = Voice::LevelDb(frame, Voice::kFrameSamples);
        m_levelDb = level;
        const bool gateOpen = m_gate.Update(level, m_settings.activationThresholdDb);
        const bool transmit = m_settings.pushToTalk ? m_pushToTalkHeld.load() : gateOpen;
        m_transmitting = transmit;
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
        if (m_micTest) {
            DeliverLocked(kLoopbackSpeaker, seq, packet.data(), static_cast<size_t>(size));
        }
    }

    void DeliverLocked(uint64_t speakerId, uint16_t seq, const uint8_t* data, size_t size) {
        Speaker& speaker = m_speakers[speakerId];
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
            std::array<float, Voice::kFrameSamples> mix{};
            MixFrameLocked(mix.data());
            if (!SDL_PutAudioStreamData(m_playback, mix.data(), kFrameBytes)) {
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
                continue;
            }
            if (out.kind == Voice::JitterBuffer::Kind::Frame) {
                speaker.decoder.Decode(out.packet->data(), out.packet->size(), voice.data());
            } else if (out.next) {
                speaker.decoder.DecodeFec(out.next->data(), out.next->size(), voice.data());
            } else {
                speaker.decoder.Conceal(voice.data());
            }
            const float gain = volume * speaker.gain;
            for (int i = 0; i < Voice::kFrameSamples; ++i) {
                mix[i] += voice[i] * gain;
            }
        }
        for (int i = 0; i < Voice::kFrameSamples; ++i) {
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
};

Settings SettingsFromConfig() {
    Settings settings;
    settings.enabled = VoiceConfig::Enabled();
    settings.pushToTalk = VoiceConfig::PushToTalk();
    settings.activationThresholdDb = VoiceConfig::ActivationThresholdDb();
    settings.inputDevice = VoiceConfig::InputDevice();
    settings.inputVolume = VoiceConfig::InputVolume();
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
