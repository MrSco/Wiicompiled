#pragma once

// Voice chat engine: microphone capture, push-to-talk / voice activation,
// Opus encoding, per-speaker jitter buffering and a playback mix on its own
// SDL output stream, separate from the game's audio.
//
// Everything runs on one worker thread that only exists while voice chat is
// enabled, so a player who never turns it on never has the microphone opened.
// Public functions are safe to call from any thread.

#include <string>
#include <vector>

namespace VoiceChat {

struct Settings {
    bool enabled = false;
    bool pushToTalk = true;
    float activationThresholdDb = -40.0f;
    std::string inputDevice;  // empty = system default
    float inputVolume = 1.0f;
    float volume = 1.0f;
    bool muteAll = false;
};

enum class MicState {
    Off,          // voice chat disabled
    Open,         // capturing
    Unavailable,  // enabled, but the microphone could not be opened (see MicError)
};

// Reads [voice] from Config.toml and starts the engine if it is enabled.
void Initialize();
void Shutdown();

Settings CurrentSettings();
// Starts, stops or reconfigures the engine to match. Persisting is the caller's job.
void ApplySettings(const Settings& settings);

void SetPushToTalkHeld(bool held);
// Plays your own encoded voice back to you, exactly as others would hear it.
void SetMicTest(bool enabled);
bool MicTestEnabled();

MicState GetMicState();
std::string MicError();
// Level of the most recent microphone frame after the mic volume, in dBFS.
float InputLevelDb();
// True while your voice is being sent (push-to-talk held or voice activation open).
bool Transmitting();

std::vector<std::string> InputDeviceNames();

}  // namespace VoiceChat
