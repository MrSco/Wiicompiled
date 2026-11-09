// Parsing of the [voice] section of Config.toml.

#include "voice_config.h"

#include <cstdio>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

VoiceConfig::Values Parse(const char* text) {
    std::istringstream input(text);
    return VoiceConfig::Parse(input, "test.toml");
}

void TestAllSettings() {
    const auto values = Parse(
        "[voice]\nenabled = true\ntalk_mode = \"voice_activation\"\npush_to_talk_key = 21\n"
        "activation_threshold_db = -35\ninput_device = \"USB Mic\"\ninput_volume = 1.5\n"
        "volume = 0.25\nmute_all = true\n");
    Check(values.enabled == std::optional<bool>{true}, "enabled");
    Check(values.pushToTalk == std::optional<bool>{false}, "voice activation mode");
    Check(values.pushToTalkKey == std::optional<int32_t>{21}, "push-to-talk key");
    Check(values.activationThresholdDb == std::optional<float>{-35.0f}, "integer threshold accepted as float");
    Check(values.inputDevice == std::optional<std::string>{"USB Mic"}, "input device");
    Check(values.inputVolume == std::optional<float>{1.5f}, "input volume");
    Check(values.volume == std::optional<float>{0.25f}, "volume");
    Check(values.muteAll == std::optional<bool>{true}, "mute all");
}

void TestRejectsBadValues() {
    const auto values =
        Parse("[voice]\ntalk_mode = \"always\"\nactivation_threshold_db = 6\ninput_volume = 3\nvolume = 2\n"
              "enabled = \"yes\"\n");
    Check(!values.pushToTalk, "unknown talk mode ignored");
    Check(!values.activationThresholdDb, "threshold above 0 dB ignored");
    Check(!values.inputVolume, "input volume above 200% ignored");
    Check(!values.volume, "volume above 100% ignored");
    Check(!values.enabled, "non-boolean enabled ignored");
}

void TestMissingSectionAndInvalidFile() {
    const auto empty = Parse("[video]\nwidescreen = true\n");
    Check(!empty.enabled && !empty.pushToTalk && !empty.volume, "no [voice] section: all defaults");
    const auto broken = Parse("[voice\nenabled = true\n");
    Check(!broken.enabled, "unparseable file: all defaults");
}

void TestMutedPlayers() {
    const auto values = Parse("[voice]\nmuted_players = [12345, 0, -4, 4294967295, 99]\n");
    Check(values.mutedPlayers == std::vector<uint32_t>({12345u, 4294967295u, 99u}),
          "muted players loaded; zero, negative and out-of-range ids dropped");
}

}  // namespace

int main() {
    TestAllSettings();
    TestRejectsBadValues();
    TestMissingSectionAndInvalidFile();
    TestMutedPlayers();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("voice config tests passed\n");
    return 0;
}
