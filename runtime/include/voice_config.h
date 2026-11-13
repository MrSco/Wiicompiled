#pragma once

// The [voice] section of Config.toml. Kept apart from runtime_config.h so voice
// chat touches as little shared code as possible; it reuses that file's TOML
// helpers and WriteSetting, which edits one key in place without disturbing
// the rest of the file.
//
//   [voice]
//   enabled = false                  # the microphone is only opened while true
//   talk_mode = "push_to_talk"       # or "voice_activation" (the default on Android)
//   push_to_talk_key = 25            # SDL scancode (25 = V)
//   activation_threshold_db = -40.0  # -80 to 0
//   input_device = ""                # device name; empty = system default
//   input_volume = 1.0               # 0 to 2
//   echo_cancellation = true         # removes game sound and voices the mic picks up from speakers
//   noise_suppression = true         # removes steady background noise (fans, hum)
//   lower_game_volume = false        # turns the game down while you talk
//   volume = 1.0                     # 0 to 1
//   mute_all = false
//   muted_players = []               # profile ids, remembered across sessions
//   # Proximity chat during races: opponents fade with distance and are placed
//   # left/right. Friends, teammates and your own console are always full volume.
//   proximity = true
//   proximity_range_m = 120.0        # metres until opponents fade out (40-400)
//   proximity_min_volume = 0.0       # volume kept beyond the range (0-0.5)
//   proximity_stereo = true

#include "runtime_config.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace VoiceConfig {

struct Values {
    std::optional<bool> enabled;
    std::optional<bool> pushToTalk;  // false = voice activation
    std::optional<int32_t> pushToTalkKey;
    std::optional<float> activationThresholdDb;
    std::optional<std::string> inputDevice;
    std::optional<float> inputVolume;
    std::optional<bool> echoCancellation;
    std::optional<bool> noiseSuppression;
    std::optional<bool> lowerGameVolume;
    std::optional<float> volume;
    std::optional<bool> muteAll;
    std::vector<uint32_t> mutedPlayers;
    std::optional<bool> proximity;
    std::optional<float> proximityRangeMeters;
    std::optional<float> proximityMinimumVolume;
    std::optional<bool> proximityStereo;
};

inline constexpr const char* kSection = "voice";

// Out-of-range or mistyped values are dropped so their defaults apply.
inline Values ParseDocument(const toml::value& document) {
    using RuntimeConfigFile::FindConfigFloat;
    using RuntimeConfigFile::FindConfigInt;
    using RuntimeConfigFile::FindConfigValue;
    auto inRange = [](std::optional<float> value, float low, float high) {
        return value && *value >= low && *value <= high ? value : std::nullopt;
    };
    Values values;
    values.enabled = FindConfigValue<bool>(document, kSection, "enabled");
    if (const auto mode = FindConfigValue<std::string>(document, kSection, "talk_mode")) {
        if (*mode == "push_to_talk" || *mode == "voice_activation") {
            values.pushToTalk = *mode == "push_to_talk";
        }
    }
    values.pushToTalkKey = FindConfigInt(document, kSection, "push_to_talk_key");
    values.activationThresholdDb = inRange(FindConfigFloat(document, kSection, "activation_threshold_db"), -80.0f, 0.0f);
    values.inputDevice = FindConfigValue<std::string>(document, kSection, "input_device");
    values.inputVolume = inRange(FindConfigFloat(document, kSection, "input_volume"), 0.0f, 2.0f);
    values.echoCancellation = FindConfigValue<bool>(document, kSection, "echo_cancellation");
    values.noiseSuppression = FindConfigValue<bool>(document, kSection, "noise_suppression");
    values.lowerGameVolume = FindConfigValue<bool>(document, kSection, "lower_game_volume");
    values.volume = inRange(FindConfigFloat(document, kSection, "volume"), 0.0f, 1.0f);
    values.muteAll = FindConfigValue<bool>(document, kSection, "mute_all");
    values.proximity = FindConfigValue<bool>(document, kSection, "proximity");
    values.proximityRangeMeters = inRange(FindConfigFloat(document, kSection, "proximity_range_m"), 40.0f, 400.0f);
    values.proximityMinimumVolume =
        inRange(FindConfigFloat(document, kSection, "proximity_min_volume"), 0.0f, 0.5f);
    values.proximityStereo = FindConfigValue<bool>(document, kSection, "proximity_stereo");
    if (const auto muted = FindConfigValue<std::vector<int64_t>>(document, kSection, "muted_players")) {
        for (const int64_t id : *muted) {
            if (id > 0 && id <= static_cast<int64_t>(UINT32_MAX)) {
                values.mutedPlayers.push_back(static_cast<uint32_t>(id));
            }
        }
    }
    return values;
}

inline Values Parse(std::istream& input, std::string sourceName = "Config.toml") {
    try {
        return ParseDocument(toml::parse(input, std::move(sourceName)));
    } catch (const std::exception&) {
        return {};  // runtime_config.h already reports an invalid file
    }
}

inline Values& Mutable() {
    static Values values = [] {
        RuntimeConfigFile::Get();  // creates Config.toml on first run
        std::ifstream file(RuntimeConfigFile::ResolveConfigPath(), std::ios::binary);
        return file ? Parse(file) : Values{};
    }();
    return values;
}

inline const Values& Get() { return Mutable(); }

inline std::string FormatFloat(float value) {
    std::ostringstream formatted;
    formatted << value;
    return formatted.str();
}

inline bool Enabled() { return Get().enabled.value_or(false); }
#if defined(__ANDROID__)
// A headset has no keyboard, so it starts voice activated.
inline constexpr bool kDefaultPushToTalk = false;
#else
inline constexpr bool kDefaultPushToTalk = true;
#endif
inline bool PushToTalk() { return Get().pushToTalk.value_or(kDefaultPushToTalk); }
// 25 is SDL_SCANCODE_V; spelled out so this header does not need SDL.
inline constexpr int32_t kDefaultPushToTalkKey = 25;
inline int32_t PushToTalkKey() { return Get().pushToTalkKey.value_or(kDefaultPushToTalkKey); }
inline float ActivationThresholdDb() { return Get().activationThresholdDb.value_or(-40.0f); }
inline std::string InputDevice() { return Get().inputDevice.value_or(std::string{}); }
inline float InputVolume() { return Get().inputVolume.value_or(1.0f); }
inline bool EchoCancellation() { return Get().echoCancellation.value_or(true); }
inline bool NoiseSuppression() { return Get().noiseSuppression.value_or(true); }
inline bool LowerGameVolume() { return Get().lowerGameVolume.value_or(false); }
inline float Volume() { return Get().volume.value_or(1.0f); }
inline bool MuteAll() { return Get().muteAll.value_or(false); }
inline const std::vector<uint32_t>& MutedPlayers() { return Get().mutedPlayers; }
inline bool Proximity() { return Get().proximity.value_or(true); }
inline float ProximityRangeMeters() { return Get().proximityRangeMeters.value_or(120.0f); }
inline float ProximityMinimumVolume() { return Get().proximityMinimumVolume.value_or(0.0f); }
inline bool ProximityStereo() { return Get().proximityStereo.value_or(true); }

inline bool SetEnabled(bool value) {
    Mutable().enabled = value;
    return RuntimeConfigFile::WriteSetting(kSection, "enabled", value ? "true" : "false");
}

inline bool SetPushToTalk(bool value) {
    Mutable().pushToTalk = value;
    return RuntimeConfigFile::WriteSetting(kSection, "talk_mode",
                                           value ? "\"push_to_talk\"" : "\"voice_activation\"");
}

inline bool SetPushToTalkKey(int32_t value) {
    Mutable().pushToTalkKey = value;
    return RuntimeConfigFile::WriteSetting(kSection, "push_to_talk_key", std::to_string(value));
}

inline bool SetActivationThresholdDb(float value) {
    value = std::clamp(value, -80.0f, 0.0f);
    Mutable().activationThresholdDb = value;
    return RuntimeConfigFile::WriteSetting(kSection, "activation_threshold_db", FormatFloat(value));
}

inline bool SetInputDevice(const std::string& value) {
    Mutable().inputDevice = value;
    return RuntimeConfigFile::WriteSetting(kSection, "input_device", RuntimeConfigFile::FormatString(value));
}

inline bool SetInputVolume(float value) {
    value = std::clamp(value, 0.0f, 2.0f);
    Mutable().inputVolume = value;
    return RuntimeConfigFile::WriteSetting(kSection, "input_volume", FormatFloat(value));
}

inline bool SetEchoCancellation(bool value) {
    Mutable().echoCancellation = value;
    return RuntimeConfigFile::WriteSetting(kSection, "echo_cancellation", value ? "true" : "false");
}

inline bool SetNoiseSuppression(bool value) {
    Mutable().noiseSuppression = value;
    return RuntimeConfigFile::WriteSetting(kSection, "noise_suppression", value ? "true" : "false");
}

inline bool SetLowerGameVolume(bool value) {
    Mutable().lowerGameVolume = value;
    return RuntimeConfigFile::WriteSetting(kSection, "lower_game_volume", value ? "true" : "false");
}

inline bool SetVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().volume = value;
    return RuntimeConfigFile::WriteSetting(kSection, "volume", FormatFloat(value));
}

inline bool SetMuteAll(bool value) {
    Mutable().muteAll = value;
    return RuntimeConfigFile::WriteSetting(kSection, "mute_all", value ? "true" : "false");
}

inline bool SetMutedPlayers(const std::vector<uint32_t>& profileIds) {
    Mutable().mutedPlayers = profileIds;
    std::string list = "[";
    for (size_t i = 0; i < profileIds.size(); ++i) {
        list += (i ? ", " : "") + std::to_string(profileIds[i]);
    }
    return RuntimeConfigFile::WriteSetting(kSection, "muted_players", list + "]");
}

inline bool SetProximity(bool value) {
    Mutable().proximity = value;
    return RuntimeConfigFile::WriteSetting(kSection, "proximity", value ? "true" : "false");
}

inline bool SetProximityRangeMeters(float value) {
    value = std::clamp(value, 40.0f, 400.0f);
    Mutable().proximityRangeMeters = value;
    return RuntimeConfigFile::WriteSetting(kSection, "proximity_range_m", FormatFloat(value));
}

inline bool SetProximityMinimumVolume(float value) {
    value = std::clamp(value, 0.0f, 0.5f);
    Mutable().proximityMinimumVolume = value;
    return RuntimeConfigFile::WriteSetting(kSection, "proximity_min_volume", FormatFloat(value));
}

inline bool SetProximityStereo(bool value) {
    Mutable().proximityStereo = value;
    return RuntimeConfigFile::WriteSetting(kSection, "proximity_stereo", value ? "true" : "false");
}

}  // namespace VoiceConfig
