#pragma once

// Bridge between the running game and voice chat. Once per presented frame, on
// the guest thread (the game is parked inside GXCopyDisp/VI retrace, so its
// memory is consistent), this reads who is in the online room and pushes each
// person's mix settings to the voice engine. It only ever reads game memory.

#include "voice/voice_roster.h"

namespace VoiceGame {

// Guest thread only. Cheap when voice chat is disabled.
void Update();

// The snapshot taken by the last Update(). Guest thread only (the settings
// overlay draws on that thread).
const Voice::RoomSnapshot& Room();

bool Muted(const Voice::RoomMember& member);
void SetMuted(const Voice::RoomMember& member, bool muted);  // persisted by profile id
float Volume(const Voice::RoomMember& member);
// True when this member's voice chat is connected (for yourself: enabled).
// Members without it play normally; they just are not heard.
bool HasVoiceChat(const Voice::RoomMember& member);
void SetVolume(const Voice::RoomMember& member, float volume);  // this session only

}  // namespace VoiceGame
