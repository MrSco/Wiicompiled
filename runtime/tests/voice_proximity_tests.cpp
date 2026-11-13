// Proximity voice math: distance falloff, stereo panning and the party rule.

#include "voice/voice_proximity.h"

#include <cmath>
#include <cstdio>
#include <string>

using Voice::Listener;
using Voice::ProximitySettings;
using Voice::RoomMember;
using Voice::RoomSnapshot;
using Voice::Spatialize;
using Voice::Vec3;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool Near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

void TestDistanceFalloff() {
    ProximitySettings settings;  // full inside 2000, silent by 12000
    Check(Voice::DistanceGain(0.0f, settings) == 1.0f, "on top of you: full volume");
    Check(Voice::DistanceGain(2000.0f, settings) == 1.0f, "edge of the full-volume radius");
    Check(Near(Voice::DistanceGain(7000.0f, settings), 0.5f), "halfway through the fade is half volume");
    Check(Voice::DistanceGain(12000.0f, settings) == 0.0f, "silent at the range");
    Check(Voice::DistanceGain(50000.0f, settings) == 0.0f, "silent beyond it");
    float previous = 1.0f;
    for (float d = 2000.0f; d <= 12000.0f; d += 250.0f) {
        const float gain = Voice::DistanceGain(d, settings);
        Check(gain <= previous + 1e-6f, "never gets louder with distance");
        previous = gain;
    }
    settings.minimumGain = 0.2f;
    Check(Near(Voice::DistanceGain(50000.0f, settings), 0.2f), "minimum volume floor");
    Check(Voice::DistanceGain(std::nanf(""), settings) == 1.0f, "a bad position never silences anyone");
}

void TestPanning() {
    ProximitySettings settings;
    Listener listener;
    listener.position = {0.0f, 0.0f, 0.0f};
    listener.right = {2.0f, 0.0f, 0.0f};  // not normalised on purpose

    const auto ahead = Spatialize(listener, {0.0f, 0.0f, 1000.0f}, false, settings);
    Check(ahead.left == 1.0f && ahead.right == 1.0f, "straight ahead: centred");

    const auto right = Spatialize(listener, {1000.0f, 0.0f, 0.0f}, false, settings);
    Check(Near(right.right, 1.0f) && Near(right.left, 1.0f - Voice::kPanDepth), "fully right");

    const auto left = Spatialize(listener, {-1000.0f, 0.0f, 0.0f}, false, settings);
    Check(Near(left.left, 1.0f) && Near(left.right, 1.0f - Voice::kPanDepth), "fully left");

    const auto behindRight = Spatialize(listener, {700.0f, 0.0f, -700.0f}, false, settings);
    Check(behindRight.right > behindRight.left && behindRight.right <= 1.0f, "partly right, never louder than centre");

    settings.stereo = false;
    const auto mono = Spatialize(listener, {1000.0f, 0.0f, 0.0f}, false, settings);
    Check(mono.left == mono.right, "stereo off: no panning");

    settings.stereo = true;
    const auto far = Spatialize(listener, {20000.0f, 0.0f, 0.0f}, false, settings);
    Check(far.left == 0.0f && far.right == 0.0f, "out of range: silent in both ears");
}

void TestPartyAlwaysFull() {
    ProximitySettings settings;
    Listener listener;
    const auto party = Spatialize(listener, {50000.0f, 0.0f, 0.0f}, true, settings);
    Check(party.left == 1.0f && party.right == 1.0f, "party member far away and to the side: full and centred");
    settings.enabled = false;
    const auto disabled = Spatialize(listener, {50000.0f, 0.0f, 0.0f}, false, settings);
    Check(disabled.left == 1.0f && disabled.right == 1.0f, "proximity off: everyone full volume");
}

void TestPartyRule() {
    RoomSnapshot room;
    room.online = true;
    RoomMember self;
    self.aid = 0;
    self.local = true;
    self.team = 1;
    RoomMember teammate;
    teammate.aid = 1;
    teammate.team = 1;
    RoomMember opponent;
    opponent.aid = 2;
    opponent.team = 0;
    RoomMember friendOpponent = opponent;
    friendOpponent.aid = 3;
    friendOpponent.friendOfMine = true;
    room.members = {self, teammate, opponent, friendOpponent};

    Check(Voice::IsPartyMember(self, room), "yourself");
    Check(Voice::IsPartyMember(friendOpponent, room), "a friend, even on the other team");
    room.teamMode = false;
    Check(!Voice::IsPartyMember(teammate, room), "no teams outside team modes");
    Check(!Voice::IsPartyMember(opponent, room), "stranger in free-for-all");
    room.teamMode = true;
    Check(Voice::IsPartyMember(teammate, room), "teammate in a team race");
    Check(!Voice::IsPartyMember(opponent, room), "the other team");

    RoomMember noTeam;
    noTeam.team = -1;
    room.members[0].team = -1;
    Check(!Voice::IsPartyMember(noTeam, room), "unknown teams never match each other");
}

}  // namespace

int main() {
    TestDistanceFalloff();
    TestPanning();
    TestPartyAlwaysFull();
    TestPartyRule();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("voice proximity tests passed\n");
    return 0;
}
