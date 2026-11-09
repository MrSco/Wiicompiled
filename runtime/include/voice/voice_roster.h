#pragma once

// Who is in the online room, as voice chat sees it, plus the player's own
// per-person choices (mute, volume). Pure data: filled from game memory by
// voice_game.cpp, consumed by the settings UI and the speaker mixer.

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Voice {

struct RoomMember {
    uint8_t aid = 0xFF;            // DWC connection slot, 0-11
    bool local = false;            // this console
    uint32_t profileId = 0;        // stable per Wii/WiiCompiled profile; 0 if unknown
    std::string name;              // UTF-8 Mii name; empty if not received yet
    bool friendOfMine = false;     // profileId is on this profile's friend roster
    int8_t team = -1;              // team in team races/battles, -1 otherwise
    std::array<int8_t, 2> playerIds{{-1, -1}};  // race player slots this console drives
    uint32_t publicIp = 0;         // network byte order; 0 if unknown
    uint16_t publicPort = 0;       // network byte order
};

struct RoomSnapshot {
    bool online = false;           // connected to a room
    bool inRace = false;           // a race or battle is running
    bool teamMode = false;
    std::vector<RoomMember> members;

    const RoomMember* Local() const {
        for (const RoomMember& member : members) {
            if (member.local) {
                return &member;
            }
        }
        return nullptr;
    }
};

// Remote speakers are keyed by the public game endpoint the room uses to reach
// that console; the network transport delivers frames under the same id.
inline uint64_t SpeakerIdFor(uint32_t publicIpNetworkOrder, uint16_t publicPortNetworkOrder) {
    return (static_cast<uint64_t>(publicIpNetworkOrder) << 16) | publicPortNetworkOrder;
}

inline uint64_t SpeakerIdFor(const RoomMember& member) {
    return SpeakerIdFor(member.publicIp, member.publicPort);
}

// Mute is remembered by profile id across sessions; per-player volume is kept
// for the session only.
class PlayerPreferences {
public:
    static constexpr float kDefaultVolume = 1.0f;

    void LoadMuted(const std::vector<uint32_t>& profileIds) {
        m_muted = std::set<uint32_t>(profileIds.begin(), profileIds.end());
        m_muted.erase(0u);
    }
    std::vector<uint32_t> MutedList() const { return {m_muted.begin(), m_muted.end()}; }

    bool Muted(uint32_t profileId) const { return profileId != 0 && m_muted.count(profileId) != 0; }
    // Returns true when the persisted list changed.
    bool SetMuted(uint32_t profileId, bool muted) {
        if (profileId == 0) {
            return false;
        }
        return muted ? m_muted.insert(profileId).second : m_muted.erase(profileId) != 0;
    }

    float Volume(uint32_t profileId) const {
        const auto it = m_volume.find(profileId);
        return it == m_volume.end() ? kDefaultVolume : it->second;
    }
    void SetVolume(uint32_t profileId, float volume) { m_volume[profileId] = std::clamp(volume, 0.0f, 2.0f); }

    // Linear gain for this person before any proximity attenuation.
    float Gain(uint32_t profileId) const { return Muted(profileId) ? 0.0f : Volume(profileId); }

private:
    std::set<uint32_t> m_muted;
    std::map<uint32_t, float> m_volume;
};

// Mii names are big-endian UTF-16 code units, NUL-terminated or full-length.
// Unpaired surrogates become U+FFFD.
inline std::string Utf16ToUtf8(const uint16_t* units, size_t maxUnits) {
    std::string out;
    auto put = [&out](uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    for (size_t i = 0; i < maxUnits && units[i] != 0; ++i) {
        const uint32_t unit = units[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < maxUnits && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            put(0x10000 + ((unit - 0xD800) << 10) + (units[i + 1] - 0xDC00));
            ++i;
        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            put(0xFFFD);
        } else {
            put(unit);
        }
    }
    return out;
}

}  // namespace Voice
