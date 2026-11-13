#pragma once

// Voice chat's part of the F10 settings bar. settings_overlay.cpp calls these
// few hooks; everything else about voice chat's UI lives in voice_settings_ui.cpp.

union SDL_Event;

namespace VoiceSettingsUi {

// The "Voice chat" menu in the top bar. Call between BeginMainMenuBar/EndMainMenuBar.
void DrawMenu();
// The same settings as the Voice chat tab of the VR headset settings panel.
void DrawPanel();
// Once per presented frame, on the guest thread: refreshes the room from game
// memory and draws things outside the menu (the talking indicator).
void DrawOverlays();
// Every SDL event: push-to-talk and the push-to-talk key capture. While another
// rebind prompt is open, key presses are left to it.
void HandleSdlEvent(const SDL_Event& event, bool otherRebindActive);
// True while waiting for the user to press a new push-to-talk key, so the
// overlay does not also treat that key (e.g. Escape) as one of its own.
bool CapturingKey();

}  // namespace VoiceSettingsUi
