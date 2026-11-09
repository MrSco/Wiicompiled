#include "voice_settings_ui.h"

#include "voice_chat.h"
#include "voice_config.h"
#include "voice_game.h"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace VoiceSettingsUi {
namespace {

bool g_pushToTalkDown = false;
bool g_capturingKey = false;
// ImGui frame on which the menu was last open. A key capture only counts while
// the menu is actually showing, so hiding the bar mid-capture abandons it.
int g_menuOpenFrame = -1;

bool CaptureActive() {
    if (g_capturingKey && g_menuOpenFrame < ImGui::GetFrameCount() - 1) {
        g_capturingKey = false;
    }
    return g_capturingKey;
}

const ImVec4 kGreen(0.40f, 0.90f, 0.50f, 1.0f);
const ImVec4 kRed(1.0f, 0.45f, 0.35f, 1.0f);

// Maps a dBFS level onto the meter's 0..1 range. -60 dB and below reads empty.
float MeterFraction(float db) {
    constexpr float kFloorDb = -60.0f;
    return std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f);
}

void SetPushToTalkHeld(bool held) {
    if (held != g_pushToTalkDown) {
        g_pushToTalkDown = held;
        VoiceChat::SetPushToTalkHeld(held);
    }
}

void DrawPushToTalkKey() {
    const int32_t key = VoiceConfig::PushToTalkKey();
    std::string caption;
    if (g_capturingKey) {
        caption = "Press a key...";
    } else if (key > 0 && key < SDL_SCANCODE_COUNT) {
        caption = SDL_GetScancodeName(static_cast<SDL_Scancode>(key));
    }
    if (caption.empty()) {
        caption = "None";
    }
    if (ImGui::Button((caption + "##pushToTalkKey").c_str(), ImVec2(120.0f, 0.0f))) {
        g_capturingKey = !g_capturingKey;
        SetPushToTalkHeld(false);
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted("Push-to-talk key");
    if (g_capturingKey) {
        ImGui::TextDisabled("Escape cancels, Backspace clears. F10 is reserved.");
    }
}

void DrawLevelMeter(const VoiceChat::Settings& settings) {
    const VoiceChat::MicState micState = VoiceChat::GetMicState();
    const float level = micState == VoiceChat::MicState::Open ? VoiceChat::InputLevelDb() : -96.0f;
    const bool transmitting = VoiceChat::Transmitting();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, transmitting ? ImVec4(0.30f, 0.80f, 0.40f, 1.0f)
                                                                : ImVec4(0.55f, 0.55f, 0.60f, 1.0f));
    ImGui::ProgressBar(MeterFraction(level), ImVec2(220.0f, 0.0f), "");
    ImGui::PopStyleColor();
    if (!settings.pushToTalk) {
        // Mark the activation level on the meter so the slider has a visible target.
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float x = min.x + (max.x - min.x) * MeterFraction(settings.activationThresholdDb);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), IM_COL32(255, 210, 80, 255), 2.0f);
    }
    ImGui::SameLine();
    if (!settings.enabled) {
        ImGui::TextDisabled("Off");
    } else if (micState == VoiceChat::MicState::Unavailable) {
        ImGui::TextColored(kRed, "Microphone unavailable");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", VoiceChat::MicError().c_str());
        }
    } else if (transmitting) {
        ImGui::TextColored(kGreen, "Sending");
    } else {
        ImGui::TextDisabled("Not sending");
    }
}

ImVec4 TeamColor(int8_t team) {
    // Team VS/battle: 0 = red, 1 = blue, matching the game's own colours.
    return team == 0 ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(0.45f, 0.65f, 1.0f, 1.0f);
}

void DrawPlayers() {
    ImGui::SeparatorText("Players in room");
    const Voice::RoomSnapshot& room = VoiceGame::Room();
    if (!room.online) {
        ImGui::TextDisabled("Not in an online room.");
        return;
    }
    const float lineHeight = ImGui::GetTextLineHeight();
    for (const Voice::RoomMember& member : room.members) {
        ImGui::PushID(member.aid);
        // Talking dot. Remote players light up from their decoded audio, you
        // from your own transmit state.
        const bool talking = member.local ? VoiceChat::Transmitting()
                                          : VoiceChat::SpeakerTalking(Voice::SpeakerIdFor(member));
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float radius = lineHeight * 0.3f;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(cursor.x + radius, cursor.y + lineHeight * 0.5f + ImGui::GetStyle().FramePadding.y), radius,
            talking ? IM_COL32(80, 220, 110, 255) : IM_COL32(90, 90, 100, 255));
        ImGui::Dummy(ImVec2(radius * 2.0f, ImGui::GetFrameHeight()));
        ImGui::SameLine();

        std::string label = member.name.empty() ? "Player " + std::to_string(member.aid + 1) : member.name;
        if (member.local) label += " (you)";
        else if (member.friendOfMine) label += " (friend)";
        ImGui::AlignTextToFramePadding();
        if (room.teamMode && member.team >= 0) {
            ImGui::TextColored(TeamColor(member.team), "%s", label.c_str());
        } else {
            ImGui::TextUnformatted(label.c_str());
        }

        if (!member.local) {
            ImGui::SameLine(200.0f);
            const bool identified = member.profileId != 0;
            ImGui::BeginDisabled(!identified);
            bool muted = VoiceGame::Muted(member);
            if (ImGui::Checkbox("Mute", &muted)) {
                VoiceGame::SetMuted(member, muted);
            }
            ImGui::EndDisabled();
            if (!identified && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("This player's profile has not been received yet.");
            }
            ImGui::SameLine();
            int percent = static_cast<int>(std::lround(VoiceGame::Volume(member) * 100.0f));
            ImGui::SetNextItemWidth(120.0f);
            // Volume is remembered per profile, so it needs the profile id too.
            ImGui::BeginDisabled(muted || !identified);
            if (ImGui::SliderInt("##volume", &percent, 0, 200, "%d%%")) {
                VoiceGame::SetVolume(member, static_cast<float>(percent) / 100.0f);
            }
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
}


void DrawSettings() {
    VoiceChat::Settings settings = VoiceChat::CurrentSettings();
    bool changed = false;

    if (ImGui::Checkbox("Enable voice chat", &settings.enabled)) {
        VoiceConfig::SetEnabled(settings.enabled);
        if (!settings.enabled) {
            VoiceChat::SetMicTest(false);
            g_capturingKey = false;
        }
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Talk with other WiiCompiled players in online rooms. Players without\n"
            "voice chat cannot hear you and play normally. The microphone is only\n"
            "used while this is on.");
    }

    ImGui::BeginDisabled(!settings.enabled);

    ImGui::SeparatorText("Talking");
    int mode = settings.pushToTalk ? 0 : 1;
    const bool pushToTalkPicked = ImGui::RadioButton("Push-to-talk", &mode, 0);
    ImGui::SameLine();
    const bool activationPicked = ImGui::RadioButton("Voice activated", &mode, 1);
    if (pushToTalkPicked || activationPicked) {
        settings.pushToTalk = mode == 0;
        VoiceConfig::SetPushToTalk(settings.pushToTalk);
        changed = true;
    }
    if (settings.pushToTalk) {
        DrawPushToTalkKey();
    } else {
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Activation level", &settings.activationThresholdDb, -80.0f, 0.0f, "%.0f dB")) {
            VoiceConfig::SetActivationThresholdDb(settings.activationThresholdDb);
            changed = true;
        }
    }
    DrawLevelMeter(settings);

    ImGui::SeparatorText("Microphone");
    const std::string current = settings.inputDevice.empty() ? "System default" : settings.inputDevice;
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("Device", current.c_str())) {
        if (ImGui::Selectable("System default", settings.inputDevice.empty())) {
            settings.inputDevice.clear();
            VoiceConfig::SetInputDevice(settings.inputDevice);
            changed = true;
        }
        for (const std::string& name : VoiceChat::InputDeviceNames()) {
            if (ImGui::Selectable(name.c_str(), name == settings.inputDevice)) {
                settings.inputDevice = name;
                VoiceConfig::SetInputDevice(settings.inputDevice);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    int inputPercent = static_cast<int>(std::lround(settings.inputVolume * 100.0f));
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Mic volume", &inputPercent, 0, 200, "%d%%")) {
        settings.inputVolume = static_cast<float>(inputPercent) / 100.0f;
        VoiceConfig::SetInputVolume(settings.inputVolume);
        changed = true;
    }
    bool micTest = VoiceChat::MicTestEnabled();
    if (ImGui::Checkbox("Hear myself (mic test)", &micTest)) {
        VoiceChat::SetMicTest(micTest);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Plays your voice back exactly as other players hear it.\n"
                          "Use headphones to avoid feedback.");
    }

    ImGui::SeparatorText("Listening");
    int volumePercent = static_cast<int>(std::lround(settings.volume * 100.0f));
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Voice volume", &volumePercent, 0, 100, "%d%%")) {
        settings.volume = static_cast<float>(volumePercent) / 100.0f;
        VoiceConfig::SetVolume(settings.volume);
        changed = true;
    }
    if (ImGui::Checkbox("Mute everyone", &settings.muteAll)) {
        VoiceConfig::SetMuteAll(settings.muteAll);
        changed = true;
    }

    DrawPlayers();

    ImGui::EndDisabled();

    if (changed) {
        VoiceChat::ApplySettings(settings);
    }
}

// A small marker while your microphone is live, so push-to-talk has feedback
// with the settings bar hidden.
void DrawTalkingIndicator() {
    if (!VoiceChat::Transmitting()) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    ImGui::SetNextWindowPos(ImVec2(kMargin, io.DisplaySize.y - kMargin), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Voice indicator", nullptr, kFlags)) {
        const float radius = ImGui::GetTextLineHeight() * 0.3f;
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(cursor.x + radius, cursor.y + ImGui::GetTextLineHeight() * 0.5f), radius,
            IM_COL32(80, 220, 110, 255));
        ImGui::Dummy(ImVec2(radius * 2.0f, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextUnformatted(VoiceChat::MicTestEnabled() ? "Mic test" : "Talking");
    }
    ImGui::End();
}

}  // namespace

void DrawMenu() {
    const char* label = VoiceChat::CurrentSettings().enabled ? "Voice chat: On###VoiceChatMenu"
                                                             : "Voice chat: Off###VoiceChatMenu";
    if (ImGui::BeginMenu(label)) {
        g_menuOpenFrame = ImGui::GetFrameCount();
        DrawSettings();
        ImGui::EndMenu();
    }
}

void DrawOverlays() {
    // This runs on the guest thread at the same point every frame, so it is
    // where voice chat reads the room from game memory.
    VoiceGame::Update();
    DrawTalkingIndicator();
}

void HandleSdlEvent(const SDL_Event& event, bool otherRebindActive) {
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        // The key-up would go to another window and leave the microphone live.
        SetPushToTalkHeld(false);
        return;
    }
    const bool keyDown = event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat;
    if (keyDown && CaptureActive()) {
        const SDL_Scancode code = event.key.scancode;
        if (code == SDL_SCANCODE_ESCAPE) {
            g_capturingKey = false;
        } else if (code == SDL_SCANCODE_BACKSPACE || code == SDL_SCANCODE_DELETE) {
            VoiceConfig::SetPushToTalkKey(0);
            g_capturingKey = false;
        } else if (code != SDL_SCANCODE_F10) {
            VoiceConfig::SetPushToTalkKey(static_cast<int32_t>(code));
            g_capturingKey = false;
        }
        return;
    }
    const int32_t key = VoiceConfig::PushToTalkKey();
    if (key <= 0) {
        return;
    }
    if (event.type == SDL_EVENT_KEY_UP && event.key.scancode == static_cast<SDL_Scancode>(key)) {
        SetPushToTalkHeld(false);
    } else if (keyDown && !otherRebindActive && event.key.scancode == static_cast<SDL_Scancode>(key)) {
        SetPushToTalkHeld(true);
    }
}

bool CapturingKey() {
    return CaptureActive();
}

}  // namespace VoiceSettingsUi
