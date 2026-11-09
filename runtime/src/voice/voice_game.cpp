#include "voice_game.h"

#include "guest_abi/guest_region.h"
#include "memory.h"
#include "voice_config.h"
#include "runtime_log.h"
#include "voice_chat.h"
#include "voice/voice_network.h"
#include "voice/voice_proximity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
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

// Every pointer in a chain is checked against guest RAM (MEM1/MEM2 live at
// 0x80000000 and up), so a null pointer plus a field offset is never read.
bool Readable(uint32_t address, size_t length) {
    return address >= 0x80000000u && Memory::Contains(address, length);
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
        if (member->local) {
            // DWC keeps this console's own node too; its local port is the port
            // the game's GT2 socket is bound to, which the voice socket follows.
            room.localGamePort = Memory::Read16(node + 0x0E);
            continue;
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

// The voice link lives as long as the process; it is open while voice chat is
// on and the game is in a room, and closed otherwise.
Voice::Link& GetLink() {
    static Voice::Link link([](uint64_t speakerId, uint16_t sequence, const uint8_t* data, size_t size) {
        VoiceChat::DeliverFrame(speakerId, sequence, data, size);
    });
    return link;
}

void SendFrame(uint16_t sequence, const uint8_t* data, size_t size) {
    GetLink().SendVoice(sequence, data, size);
}

struct State {
    Voice::RoomSnapshot room;
    Voice::PlayerPreferences preferences;
    bool preferencesLoaded = false;
    size_t loggedMemberCount = 0;
    uint16_t linkGamePort = 0;  // game port the open link was bound for
};

void UpdateLink(State& state) {
    Voice::Link& link = GetLink();
    const Voice::RoomSnapshot& room = state.room;
    if (!room.online) {
        if (link.IsOpen()) {
            link.Close();
        }
        return;
    }
    // A new GT2 socket (a new room) means a new port to follow.
    if (!link.IsOpen() || state.linkGamePort != room.localGamePort) {
        state.linkGamePort = room.localGamePort;
        link.Open(room.localGamePort);
        VoiceChat::SetFrameSink(&SendFrame);
    }
    std::vector<Voice::LinkPeer> peers;
    for (const Voice::RoomMember& member : room.members) {
        if (!member.local && member.publicIp != 0) {
            peers.push_back({Voice::SpeakerIdFor(member), member.profileId, member.publicIp, member.publicPort});
        }
    }
    const Voice::RoomMember* self = room.Local();
    link.SetRoom(self ? self->profileId : 0, peers);
}

State& GetState() {
    static State state;
    return state;
}

// --- Proximity -------------------------------------------------------------
//
//   Kart::Manager (0x809C18F8): +0x20 players (Player**), +0x24 count (u8);
//     index = race player id. Player +0x0 -> accessor; accessor +0x08 -> body
//     -> +0x90 physics holder -> +0x04 physics -> +0x68 position (Vec3).
//   accessor +0x24 -> that kart's RaceCamera: +0x64 position, +0x70 target.

bool ReadVec3(uint32_t address, Voice::Vec3& out) {
    if (!Readable(address, 12)) {
        return false;
    }
    out = {Memory::ReadFloat32(address), Memory::ReadFloat32(address + 4), Memory::ReadFloat32(address + 8)};
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

uint32_t KartAccessor(int playerId) {
    const uint32_t manager = ReadPointer(MKW_GADDR(809C18F8));
    if (playerId < 0 || !Readable(manager, 0x28) ||
        static_cast<uint32_t>(playerId) >= Memory::Read8(manager + 0x24)) {
        return 0;
    }
    const uint32_t players = Memory::Read32(manager + 0x20);
    return ReadPointer(ReadPointer(players + 4u * static_cast<uint32_t>(playerId)));
}

bool KartPosition(int playerId, Voice::Vec3& out) {
    const uint32_t accessor = KartAccessor(playerId);
    const uint32_t body = ReadPointer(accessor + 0x08);
    const uint32_t physics = ReadPointer(ReadPointer(body + 0x90) + 0x04);
    return accessor && body && ReadVec3(physics + 0x68, out);
}

// Hears from the local player's race camera, which also covers spectating and
// finishing early. Falls back to the kart itself, without panning.
bool LocalListener(int playerId, Voice::Listener& out) {
    const uint32_t camera = ReadPointer(KartAccessor(playerId) + 0x24);
    Voice::Vec3 position;
    Voice::Vec3 target;
    if (camera && ReadVec3(camera + 0x64, position) && ReadVec3(camera + 0x70, target)) {
        const Voice::Vec3 forward = target - position;
        // forward x world up (+Y) points to the camera's right.
        out.position = position;
        out.right = {-forward.z, 0.0f, forward.x};
        return true;
    }
    if (KartPosition(playerId, out.position)) {
        out.right = {};
        return true;
    }
    return false;
}

Voice::ProximitySettings ProximityFromConfig() {
    Voice::ProximitySettings settings;
    settings.enabled = VoiceConfig::Proximity();
    // World units are roughly centimetres; full volume over the first sixth.
    settings.range = VoiceConfig::ProximityRangeMeters() * 100.0f;
    settings.fullVolumeDistance = settings.range / 6.0f;
    settings.minimumGain = VoiceConfig::ProximityMinimumVolume();
    settings.stereo = VoiceConfig::ProximityStereo();
    return settings;
}

// MKW_VOICE_PROXIMITY_DEBUG=1 logs every kart's proximity mix relative to the
// local camera every two seconds, in any race (offline races included), so the
// position and camera reads can be checked against what is on screen.
bool ProximityDebugTick() {
    static const bool enabled = [] {
        const char* value = std::getenv("MKW_VOICE_PROXIMITY_DEBUG");
        return value && value[0] == '1';
    }();
    static uint32_t frame = 0;
    return enabled && (frame++ % 120) == 0;
}

// Logs each hop of the kart/camera pointer chain when no listener could be
// read, once per change of scene or kart manager.
void LogProximityChain(int playerId) {
    const uint32_t manager = ReadPointer(MKW_GADDR(809C18F8));
    const uint32_t section = ReadPointer(ReadPointer(MKW_GADDR(809C1E38)));
    const uint32_t sectionId = Readable(section, 4) ? Memory::Read32(section) : 0xFFFFFFFFu;
    static uint64_t lastKey = ~uint64_t{0};
    const uint64_t key = (static_cast<uint64_t>(sectionId) << 32) | manager;
    if (key == lastKey) {
        return;
    }
    lastKey = key;
    RT_LOGF(RT_TAG_AUDIO, "voice proximity: section %#x, kart manager %08x\n", sectionId, manager);
    const uint32_t count = Readable(manager, 0x28) ? Memory::Read8(manager + 0x24) : 0;
    const uint32_t players = Readable(manager, 0x28) ? Memory::Read32(manager + 0x20) : 0;
    const uint32_t accessor = KartAccessor(playerId);
    const uint32_t body = ReadPointer(accessor + 0x08);
    const uint32_t holder = ReadPointer(body + 0x90);
    RT_LOGF(RT_TAG_AUDIO,
            "voice proximity: no listener for player %d: manager %08x count %u players %08x accessor %08x "
            "body %08x holder %08x physics %08x camera %08x\n",
            playerId, manager, count, players, accessor, body, holder, ReadPointer(holder + 0x04),
            ReadPointer(accessor + 0x24));
}

// Debug cross-check of the pan direction: the camera's own view matrix
// (RaceCamera +0x4, Mtx34, row 0 = view-space x) says which side of the screen
// a point is drawn on. Returns 0 when it cannot be read.
int ScreenSide(int listenerPlayerId, const Voice::Vec3& point) {
    const uint32_t camera = ReadPointer(KartAccessor(listenerPlayerId) + 0x24);
    if (!Readable(camera + 0x4, 48)) {
        return 0;
    }
    const float viewX = Memory::ReadFloat32(camera + 0x4) * point.x + Memory::ReadFloat32(camera + 0x8) * point.y +
                        Memory::ReadFloat32(camera + 0xC) * point.z + Memory::ReadFloat32(camera + 0x10);
    return viewX > 0.0f ? 1 : -1;
}

void LogProximityDebug(int listenerPlayerId, const Voice::Listener& listener,
                       const Voice::ProximitySettings& settings) {
    RT_LOGF(RT_TAG_AUDIO, "voice proximity: listener player %d at (%.0f, %.0f, %.0f) right (%.2f, %.2f)\n",
            listenerPlayerId, listener.position.x, listener.position.y, listener.position.z, listener.right.x,
            listener.right.z);
    for (int playerId = 0; playerId < 12; ++playerId) {
        Voice::Vec3 position;
        if (playerId == listenerPlayerId || !KartPosition(playerId, position)) {
            continue;
        }
        const Voice::StereoGain gain = Voice::Spatialize(listener, position, false, settings);
        const float side = Voice::Dot(position - listener.position, listener.right);
        const int screenSide = ScreenSide(listenerPlayerId, position);
        const char* check = screenSide == 0 ? "unknown"
                            : (side > 0.0f) == (screenSide > 0) ? "matches screen"
                                                                : "OPPOSITE of screen";
        RT_LOGF(RT_TAG_AUDIO, "voice proximity:   player %d distance %.0f -> L %.2f R %.2f (pan %s)\n", playerId,
                Voice::Length(position - listener.position), gain.left, gain.right, check);
    }
}

void ApplyMix(const State& state) {
    const Voice::RoomSnapshot& room = state.room;
    const Voice::ProximitySettings settings = ProximityFromConfig();
    // Outside an online race everyone in the room is heard at full volume.
    const Voice::RoomMember* self = room.Local();
    const int listenerPlayerId = self ? self->playerIds[0] : 0;
    Voice::Listener listener;
    const bool haveListener = LocalListener(listenerPlayerId, listener);
    if (ProximityDebugTick()) {
        if (haveListener) {
            LogProximityDebug(listenerPlayerId, listener, settings);
        } else {
            LogProximityChain(listenerPlayerId);
        }
    }
    const bool proximity = room.inRace && settings.enabled && haveListener;

    for (const Voice::RoomMember& member : room.members) {
        if (member.local || member.publicIp == 0) {
            continue;
        }
        const float base = state.preferences.Gain(member.profileId);
        Voice::StereoGain spatial;
        Voice::Vec3 position;
        if (proximity && KartPosition(member.playerIds[0], position)) {
            spatial = Voice::Spatialize(listener, position, Voice::IsPartyMember(member, room), settings);
        }
        VoiceChat::SetSpeakerGain(Voice::SpeakerIdFor(member), base * spatial.left, base * spatial.right);
    }
}

}  // namespace

void Update() {
    State& state = GetState();
    if (!VoiceChat::CurrentSettings().enabled) {
        state.room = {};
        UpdateLink(state);
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
    UpdateLink(state);
    ApplyMix(state);
}

bool HasVoiceChat(const Voice::RoomMember& member) {
    return member.local ? VoiceChat::CurrentSettings().enabled : GetLink().Reached(Voice::SpeakerIdFor(member));
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
