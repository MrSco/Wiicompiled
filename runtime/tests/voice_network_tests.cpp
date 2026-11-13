// Voice chat's wire format and network link, exercised over real UDP sockets
// on localhost: two "consoles" find each other through port prediction,
// exchange voice, ignore an impostor and a member without voice chat, and see
// each other leave.

#include "voice/voice_network.h"
#include "voice/voice_packet.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
// Loopback only, so running the tests never triggers a firewall prompt.
const uint32_t kLoopback = htonl(INADDR_LOOPBACK);

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

template <typename Condition>
bool WaitFor(Condition condition, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

// --- wire format ---------------------------------------------------------------

void TestPacketRoundTrip() {
    const uint8_t frame[] = {1, 2, 3, 4, 5};
    const auto bytes = Voice::EncodePacket(Voice::PacketType::Voice, 0xBEEF, 0x01020304, frame, sizeof(frame));
    const auto packet = Voice::DecodePacket(bytes.data(), bytes.size());
    Check(packet.has_value(), "voice packet decodes");
    if (packet) {
        Check(packet->type == Voice::PacketType::Voice, "type");
        Check(packet->sequence == 0xBEEF, "sequence");
        Check(packet->senderProfileId == 0x01020304, "sender profile id");
        Check(packet->payloadSize == sizeof(frame) && packet->payload[4] == 5, "payload");
    }
    const auto hello = Voice::EncodeClockPacket(Voice::PacketType::Hello, 1, 7, 123456789u);
    const auto helloView = Voice::DecodePacket(hello.data(), hello.size());
    Check(helloView && Voice::ReadClock(*helloView) == 123456789u, "hello clock");
}

void TestPacketRejectsGarbage() {
    const uint8_t frame[] = {9};
    auto good = Voice::EncodePacket(Voice::PacketType::Voice, 1, 1, frame, 1);
    Check(!Voice::DecodePacket(good.data(), Voice::kPacketHeaderSize - 1), "truncated header");
    auto badMagic = good;
    badMagic[0] = 'X';
    Check(!Voice::DecodePacket(badMagic.data(), badMagic.size()), "wrong magic");
    auto badVersion = good;
    badVersion[4] = 99;
    Check(!Voice::DecodePacket(badVersion.data(), badVersion.size()), "unknown version");
    auto badType = good;
    badType[5] = 42;
    Check(!Voice::DecodePacket(badType.data(), badType.size()), "unknown type");
    const auto emptyVoice = Voice::EncodePacket(Voice::PacketType::Voice, 1, 1, nullptr, 0);
    Check(!Voice::DecodePacket(emptyVoice.data(), emptyVoice.size()), "voice packet without a frame");
    std::vector<uint8_t> huge(Voice::kMaxPacketSize + 1, 0);
    std::copy(good.begin(), good.begin() + Voice::kPacketHeaderSize, huge.begin());
    Check(!Voice::DecodePacket(huge.data(), huge.size()), "oversized packet");
    // A GT2 packet (FE FE ...) must never be mistaken for voice chat.
    const uint8_t gt2[] = {0xFE, 0xFE, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Check(!Voice::DecodePacket(gt2, sizeof(gt2)), "GT2 packet ignored");
}

// --- link ---------------------------------------------------------------------------

struct Inbox {
    std::mutex mutex;
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> frames;
    size_t Count() {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.size();
    }
};

Voice::Link::Deliver Collect(Inbox& inbox) {
    return [&inbox](uint64_t speaker, uint16_t, const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lock(inbox.mutex);
        inbox.frames.push_back({speaker, std::vector<uint8_t>(data, data + size)});
    };
}

Voice::LinkPeer Peer(uint64_t speakerId, uint32_t profileId, uint16_t gamePortHost) {
    Voice::LinkPeer peer;
    peer.speakerId = speakerId;
    peer.profileId = profileId;
    peer.ip = htonl(INADDR_LOOPBACK);
    peer.gamePort = htons(gamePortHost);
    return peer;
}

void TestLinkEndToEnd() {
    // Pretend game ports; each voice socket binds the port after its own.
    constexpr uint16_t kGamePortA = 47110;
    constexpr uint16_t kGamePortB = 47120;
    constexpr uint16_t kGamePortNoVoice = 47130;  // nothing will listen on 47131
    constexpr uint64_t kSpeakerA = 0xA;
    constexpr uint64_t kSpeakerB = 0xB;
    constexpr uint64_t kSpeakerNoVoice = 0xC;
    constexpr uint32_t kProfileA = 111;
    constexpr uint32_t kProfileB = 222;

    Inbox inboxA;
    Inbox inboxB;
    Voice::Link a(Collect(inboxA));
    Voice::Link b(Collect(inboxB));
    Check(a.Open(kGamePortA, kLoopback), "link A opens");
    Check(b.Open(kGamePortB, kLoopback), "link B opens");
    Check(a.BoundPort() == kGamePortA + 1, "A binds its game port + 1");
    Check(b.BoundPort() == kGamePortB + 1, "B binds its game port + 1");

    a.SetRoom(kProfileA, {Peer(kSpeakerB, kProfileB, kGamePortB), Peer(kSpeakerNoVoice, 333, kGamePortNoVoice)});
    b.SetRoom(kProfileB, {Peer(kSpeakerA, kProfileA, kGamePortA)});

    Check(WaitFor([&] { return a.Reached(kSpeakerB) && b.Reached(kSpeakerA); }),
          "A and B find each other through port prediction");
    Check(!a.Reached(kSpeakerNoVoice), "a member without voice chat is never reached");

    const uint8_t frame[] = {0x11, 0x22, 0x33};
    a.SendVoice(7, frame, sizeof(frame));
    Check(WaitFor([&] { return inboxB.Count() == 1; }), "B receives A's voice");
    {
        std::lock_guard<std::mutex> lock(inboxB.mutex);
        Check(!inboxB.frames.empty() && inboxB.frames[0].first == kSpeakerA &&
                  inboxB.frames[0].second == std::vector<uint8_t>(frame, frame + sizeof(frame)),
              "frame arrives intact, attributed to A");
    }
    Check(inboxA.Count() == 0, "the sender does not hear itself");
    const Voice::LinkStats statsA = a.Stats();
    Check(statsA.membersSeen == 2 && statsA.membersReached == 1, "A saw two members and reached one");
    Check(statsA.framesSpoken == 1 && statsA.framesSent == 1, "A counts the frame it sent");
    Check(b.Stats().framesReceived == 1, "B counts the frame it received");

    // An impostor on the same address claiming B as a peer, with a profile id
    // B does not expect, gets no answer and can never send B voice.
    Inbox inboxImpostor;
    Voice::Link impostor(Collect(inboxImpostor));
    Check(impostor.Open(47140, kLoopback), "impostor opens");
    impostor.SetRoom(999, {Peer(kSpeakerB, kProfileB, kGamePortB)});
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    Check(!impostor.Reached(kSpeakerB), "B ignores a sender whose profile id is not in its room");
    impostor.SendVoice(1, frame, sizeof(frame));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check(inboxB.Count() == 1, "no voice from the impostor");
    Check(impostor.Stats().framesSpoken == 1 && impostor.Stats().framesSent == 0,
          "a frame nobody could hear is spoken but not sent");
    impostor.Close();

    b.Close();
    Check(WaitFor([&] { return !a.Reached(kSpeakerB); }), "A sees B leave (Bye)");
    a.Close();
    Check(!a.IsOpen() && a.BoundPort() == 0, "closed link releases its socket");
    Check(a.Stats().membersReached == 1, "stats survive Close for the log line");
}

void TestBusyPortFallsBack() {
    Voice::Link first(nullptr);
    Voice::Link second(nullptr);
    Check(first.Open(47150, kLoopback), "first link opens");
    Check(second.Open(47150, kLoopback), "second link still opens when game port + 1 is taken");
    Check(second.BoundPort() != 0 && second.BoundPort() != first.BoundPort(), "falls back to another port");
}

}  // namespace

int main() {
    TestPacketRoundTrip();
    TestPacketRejectsGarbage();
    TestLinkEndToEnd();
    TestBusyPortFallsBack();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("voice network tests passed\n");
    return 0;
}
