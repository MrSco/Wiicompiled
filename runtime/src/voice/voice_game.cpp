#include "voice_game.h"

#include "guest_abi/guest_region.h"
#include "memory.h"
#include "voice_config.h"
#include "runtime_log.h"
#include "voice_chat.h"

#include <algorithm>
#include <array>
#include <cstring>

// Game structures read here, all PAL addresses translated per region by
// MKW_GADDR. Layouts follow the community documentation of the game:
// Pulsar's GameSource headers (MelgMKW/Pulsar), mkw-sp, CLF78's OpenPayload
// and SeekyCt's mkw-structures.
//
//   RKNet::Controller::sInstance       0x809C20D8
//     +0x28    connectionState (0 = shut down)
//     +0x38    subs[2], 0x58 bytes each
//                +0x10 availableAids bitmask, +0x21 localAid
//     +0xE8    roomType (1-4 worldwide/regional VS/battle, 5/6 friend room host/guest)
//     +0x291C  currentSub
//     +0x2920  aidsBelongingToPlayerIds[12] (race player id -> aid)
//   RKNet::USERHandler::sInstance      0x809C2108
//     +0x8     toSendPacket (this console), +0xC8 receivedPackets[12], 0xC0 bytes each
//                packet +0x0A Mii 0 name (10 UTF-16BE units), +0xA8 friend code (u64)
//     +0x9E4   aidsThatHaveGivenMiis bitmask
//   stpMatchCnt (DWC match control)    0x8038630C
//     +0x30    nodeCount, +0x38 DWCNodeInfo[32], 0x30 bytes each
//                +0x00 profileId, +0x04 publicIp, +0x0C publicPort,
//                +0x10 gt2Ip, +0x14 gt2Port, +0x16 aid
//     +0x78C   friend roster (AccFriendData*, 0xC bytes each), +0x790 count;
//                entry +0x04 is the friend's profile id
//   Racedata::sInstance                0x809BD728
//     +0x20    race scenario: +0x08 players[12] (0xF0 bytes each, team at +0xCC),
//                +0xB48+0x28 modeFlags (bit 0x2 = teams)
//   SectionMgr::sInstance              0x809C1E38 -> current Section* -> sectionId
//   Kart::Manager::sInstance           0x809C18F8 (non-null during a race)

namespace VoiceGame {
namespace {

constexpr uint32_t kMaxAids = 12;
constexpr uint32_t kMaxPlayers = 12;

// Sections 0x68-0x77 are online races and battles, including live view.
constexpr uint32_t kFirstOnlineRaceSection = 0x68;
constexpr uint32_t kLastOnlineRaceSection = 0x77;

bool Readable(uint32_t address, size_t length) {
    return address != 0 && Memory::Contains(address, length);
}

uint32_t ReadPointer(uint32_t address) {
    return Readable(address, 4) ? Memory::Read32(address) : 0;
}

// The guest is big-endian, so a guest u32/u16 holding an IP/port already has
// the value in network order; this produces the host's network-order bytes.
uint32_t ToNetworkOrder32(uint32_t value) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                              static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    uint32_t out = 0;
    std::memcpy(&out, bytes, sizeof(out));
    return out;
}

uint16_t ToNetworkOrder16(uint16_t value) {
    const uint8_t bytes[2] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    uint16_t out = 0;
    std::memcpy(&out, bytes, sizeof(out));
    return out;
}

std::string ReadMiiName(uint32_t address) {
    constexpr size_t kUnits = 10;
    if (!Readable(address, kUnits * 2)) {
        return {};
    }
    std::array<uint16_t, kUnits> units{};
    for (size_t i = 0; i < kUnits; ++i) {
        units[i] = Memory::Read16(address + static_cast<uint32_t>(i * 2));
    }
    return Voice::Utf16ToUtf8(units.data(), units.size());
}

Voice::RoomMember* FindAid(Voice::RoomSnapshot& room, uint32_t aid) {
    for (Voice::RoomMember& member : room.members) {
        if (member.aid == aid) {
            return &member;
        }
    }
    return nullptr;
}

bool InOnlineRace() {
    const uint32_t section = ReadPointer(ReadPointer(MKW_GADDR(809C1E38)));
    if (!Readable(section, 4)) {
        return false;
    }
    const uint32_t sectionId = Memory::Read32(section);
    return sectionId >= kFirstOnlineRaceSection && sectionId <= kLastOnlineRaceSection &&
           ReadPointer(MKW_GADDR(809C18F8)) != 0;
}

void ReadUserPackets(Voice::RoomSnapshot& room) {
    const uint32_t user = ReadPointer(MKW_GADDR(809C2108));
    if (!Readable(user, 0x9E8)) {
        return;
    }
    const uint32_t gaveMiis = Memory::Read32(user + 0x9E4);
    for (Voice::RoomMember& member : room.members) {
        uint32_t packet = 0;
        if (member.local) {
            packet = user + 0x8;
        } else if ((gaveMiis >> member.aid) & 1u) {
            packet = user + 0xC8 + 0xC0 * member.aid;
        }
        if (packet == 0) {
            continue;
        }
        member.name = ReadMiiName(packet + 0x0A);
        // A friend code is (checksum << 32) | profile id.
        member.profileId = Memory::Read32(packet + 0xAC);
    }
}

void ReadDwcNodes(Voice::RoomSnapshot& room) {
    const uint32_t match = ReadPointer(MKW_GADDR(8038630C));
    constexpr uint32_t kNodes = 0x38;
    constexpr uint32_t kNodeSize = 0x30;
    constexpr uint32_t kMaxNodes = 32;
    if (!Readable(match, kNodes + kNodeSize * kMaxNodes)) {
        return;
    }
    const uint32_t count = std::min(Memory::Read32(match + 0x30), kMaxNodes);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t node = match + kNodes + kNodeSize * i;
        Voice::RoomMember* member = FindAid(room, Memory::Read8(node + 0x16));
        if (!member) {
            continue;
        }
        if (const uint32_t profileId = Memory::Read32(node); profileId != 0) {
            member->profileId = profileId;
        }
        // The GT2 address is the one the game's peer socket talks to; fall
        // back to the public address before the GT2 connection exists.
        uint32_t ip = Memory::Read32(node + 0x10);
        uint16_t port = Memory::Read16(node + 0x14);
        if (ip == 0) {
            ip = Memory::Read32(node + 0x04);
            port = Memory::Read16(node + 0x0C);
        }
        member->publicIp = ToNetworkOrder32(ip);
        member->publicPort = ToNetworkOrder16(port);
    }

    if (!Readable(match + 0x78C, 8)) {
        return;
    }
    const uint32_t roster = Memory::Read32(match + 0x78C);
    const uint32_t rosterCount = std::min<uint32_t>(Memory::Read32(match + 0x790), 100);
    if (!Readable(roster, rosterCount * 0xC)) {
        return;
    }
    for (Voice::RoomMember& member : room.members) {
        if (member.local || member.profileId == 0) {
            continue;
        }
        for (uint32_t i = 0; i < rosterCount; ++i) {
            if (Memory::Read32(roster + i * 0xC + 4) == member.profileId) {
                member.friendOfMine = true;
                break;
            }
        }
    }
}

void ReadTeams(Voice::RoomSnapshot& room) {
    const uint32_t racedata = ReadPointer(MKW_GADDR(809BD728));
    const uint32_t scenario = racedata + 0x20;
    if (!Readable(scenario, 0xB48 + 0x2C)) {
        return;
    }
    room.teamMode = (Memory::Read32(scenario + 0xB48 + 0x28) & 0x2u) != 0;
    if (!room.teamMode) {
        return;
    }
    for (Voice::RoomMember& member : room.members) {
        const int8_t playerId = member.playerIds[0];
        if (playerId < 0) {
            continue;
        }
        const uint32_t team = Memory::Read32(scenario + 0x8 + 0xF0 * static_cast<uint32_t>(playerId) + 0xCC);
        member.team = team <= 1 ? static_cast<int8_t>(team) : -1;  // 0 red, 1 blue, 2 none
    }
}

Voice::RoomSnapshot CaptureRoom() {
    Voice::RoomSnapshot room;
    const uint32_t controller = ReadPointer(MKW_GADDR(809C20D8));
    if (!Readable(controller, 0x2940) || Memory::Read32(controller + 0x28) == 0) {
        return room;
    }
    const uint32_t roomType = Memory::Read32(controller + 0xE8);
    const uint32_t currentSub = Memory::Read32(controller + 0x291C);
    if (roomType < 1 || roomType > 6 || currentSub > 1) {
        return room;  // not in a room yet (searching/joining) or shut down
    }
    const uint32_t sub = controller + 0x38 + 0x58 * currentSub;
    const uint32_t availableAids = Memory::Read32(sub + 0x10);
    const uint8_t localAid = Memory::Read8(sub + 0x21);
    for (uint32_t aid = 0; aid < kMaxAids; ++aid) {
        if (((availableAids >> aid) & 1u) || aid == localAid) {
            Voice::RoomMember member;
            member.aid = static_cast<uint8_t>(aid);
            member.local = aid == localAid;
            room.members.push_back(member);
        }
    }
    if (room.members.empty()) {
        return room;
    }
    room.online = true;
    for (uint32_t playerId = 0; playerId < kMaxPlayers; ++playerId) {
        Voice::RoomMember* member = FindAid(room, Memory::Read8(controller + 0x2920 + playerId));
        if (!member) {
            continue;
        }
        auto slot = std::find(member->playerIds.begin(), member->playerIds.end(), int8_t{-1});
        if (slot != member->playerIds.end()) {
            *slot = static_cast<int8_t>(playerId);
        }
    }
    ReadUserPackets(room);
    ReadDwcNodes(room);
    room.inRace = InOnlineRace();
    if (room.inRace) {
        ReadTeams(room);
    }
    return room;
}

struct State {
    Voice::RoomSnapshot room;
    Voice::PlayerPreferences preferences;
    bool preferencesLoaded = false;
    size_t loggedMemberCount = 0;
};

State& GetState() {
    static State state;
    return state;
}

void ApplyMix(const State& state) {
    for (const Voice::RoomMember& member : state.room.members) {
        if (member.local || member.publicIp == 0) {
            continue;
        }
        VoiceChat::SetSpeakerGain(Voice::SpeakerIdFor(member), state.preferences.Gain(member.profileId));
    }
}

}  // namespace

void Update() {
    State& state = GetState();
    if (!VoiceChat::CurrentSettings().enabled) {
        state.room = {};
        return;
    }
    if (!state.preferencesLoaded) {
        state.preferences.LoadMuted(VoiceConfig::MutedPlayers());
        state.preferencesLoaded = true;
    }
    state.room = CaptureRoom();
    if (state.room.members.size() != state.loggedMemberCount) {
        state.loggedMemberCount = state.room.members.size();
        RT_LOGF(RT_TAG_AUDIO, "voice chat: room has %zu member(s)\n", state.loggedMemberCount);
    }
    ApplyMix(state);
}

const Voice::RoomSnapshot& Room() {
    return GetState().room;
}

bool Muted(const Voice::RoomMember& member) {
    return GetState().preferences.Muted(member.profileId);
}

void SetMuted(const Voice::RoomMember& member, bool muted) {
    State& state = GetState();
    if (state.preferences.SetMuted(member.profileId, muted)) {
        VoiceConfig::SetMutedPlayers(state.preferences.MutedList());
    }
    ApplyMix(state);
}

float Volume(const Voice::RoomMember& member) {
    return GetState().preferences.Volume(member.profileId);
}

void SetVolume(const Voice::RoomMember& member, float volume) {
    State& state = GetState();
    state.preferences.SetVolume(member.profileId, volume);
    ApplyMix(state);
}

}  // namespace VoiceGame
