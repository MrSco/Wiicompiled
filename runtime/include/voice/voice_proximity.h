#pragma once

// Proximity voice: during a race, opponents get quieter with distance and are
// panned toward where their kart is. Party members (same console, teammates,
// friends) are never attenuated or panned. Pure math, no game access.
//
// Mario Kart Wii world units are roughly centimetres (a kart is ~150-200
// units long), so the defaults below are 20 m of full volume fading out by 120 m.

#include "voice/voice_roster.h"

#include <algorithm>
#include <cmath>

namespace Voice {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }

// Where the local player hears from: their kart (or camera) position and the
// direction to their right, used for panning. `right` need not be normalised.
struct Listener {
    Vec3 position;
    Vec3 right{1.0f, 0.0f, 0.0f};
};

struct ProximitySettings {
    bool enabled = true;
    float fullVolumeDistance = 2000.0f;  // units; full volume inside this
    float range = 12000.0f;              // units; reaches minimumGain here
    float minimumGain = 0.0f;            // 0 = silent beyond range
    bool stereo = true;
};

struct StereoGain {
    float left = 1.0f;
    float right = 1.0f;
};

// Fraction of full volume for a source `distance` units away.
inline float DistanceGain(float distance, const ProximitySettings& settings) {
    const float nearDistance = std::max(0.0f, settings.fullVolumeDistance);
    const float farDistance = std::max(nearDistance + 1.0f, settings.range);
    if (!(distance > nearDistance)) {  // also catches NaN
        return 1.0f;
    }
    const float t = std::min(1.0f, (distance - nearDistance) / (farDistance - nearDistance));
    // Smoothstep: no audible corner where the fade starts or ends.
    const float eased = t * t * (3.0f - 2.0f * t);
    const float floor = std::clamp(settings.minimumGain, 0.0f, 1.0f);
    return 1.0f + (floor - 1.0f) * eased;
}

// How far toward one ear a source fully to the side is pushed: the far ear
// drops to (1 - kPanDepth). Never louder than a centred source.
inline constexpr float kPanDepth = 0.6f;

// Party members always play at full volume, never faded or panned: anyone on
// this console, a teammate in a team race or battle, or someone on this
// profile's friend list.
inline bool IsPartyMember(const RoomMember& member, const RoomSnapshot& room) {
    if (member.local || member.friendOfMine) {
        return true;
    }
    const RoomMember* self = room.Local();
    return room.teamMode && self && self->team >= 0 && member.team == self->team;
}

inline StereoGain Spatialize(const Listener& listener, const Vec3& source, bool partyMember,
                             const ProximitySettings& settings) {
    if (partyMember || !settings.enabled) {
        return {};
    }
    const Vec3 offset = source - listener.position;
    const float distance = Length(offset);
    const float gain = DistanceGain(distance, settings);
    StereoGain out{gain, gain};
    const float rightLength = Length(listener.right);
    if (!settings.stereo || distance < 1.0f || !(rightLength > 0.0f)) {
        return out;
    }
    // -1 = fully left, +1 = fully right.
    const float pan = std::clamp(Dot(offset, listener.right) / (distance * rightLength), -1.0f, 1.0f);
    out.left *= 1.0f - kPanDepth * std::max(0.0f, pan);
    out.right *= 1.0f - kPanDepth * std::max(0.0f, -pan);
    return out;
}

}  // namespace Voice
