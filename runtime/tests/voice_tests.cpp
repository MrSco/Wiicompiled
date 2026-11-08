// Voice chat building blocks: the Opus wrappers, the jitter buffer and the
// signal helpers. No audio device or network is involved.

#include "voice/voice_codec.h"
#include "voice/voice_dsp.h"
#include "voice/voice_jitter_buffer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Voice::JitterBuffer;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

constexpr double kPi = 3.14159265358979323846;

std::array<float, Voice::kFrameSamples> Tone(int frameIndex, float amplitude, double hz) {
    std::array<float, Voice::kFrameSamples> frame{};
    for (int i = 0; i < Voice::kFrameSamples; ++i) {
        const double t = static_cast<double>(frameIndex * Voice::kFrameSamples + i) / Voice::kSampleRate;
        frame[i] = amplitude * static_cast<float>(std::sin(2.0 * kPi * hz * t));
    }
    return frame;
}

// --- codec -------------------------------------------------------------------

void TestCodecRoundTrip() {
    Voice::Encoder encoder;
    Voice::Decoder decoder;
    Check(encoder.Init(), "encoder init");
    Check(decoder.Init(), "decoder init");
    constexpr int kFrames = 50;  // one second
    size_t totalBytes = 0;
    float decodedLevel = Voice::kSilenceDb;
    for (int f = 0; f < kFrames; ++f) {
        auto pcm = Tone(f, 0.3f, 440.0);
        std::array<uint8_t, Voice::kMaxPacketBytes> packet{};
        const int size = encoder.Encode(pcm.data(), packet.data(), static_cast<int>(packet.size()));
        Check(size > 2 && size <= Voice::kMaxPacketBytes, "tone frame encodes to a real packet");
        totalBytes += static_cast<size_t>(std::max(size, 0));
        std::array<float, Voice::kFrameSamples> out{};
        Check(decoder.Decode(packet.data(), static_cast<size_t>(std::max(size, 0)), out.data()), "decode");
        if (f == kFrames - 1) {
            decodedLevel = Voice::LevelDb(out.data(), out.size());
        }
    }
    const float inputLevel = Voice::LevelDb(Tone(0, 0.3f, 440.0).data(), Voice::kFrameSamples);
    Check(std::fabs(decodedLevel - inputLevel) < 3.0f,
          "decoded level matches input (" + std::to_string(decodedLevel) + " vs " + std::to_string(inputLevel) + ")");
    // 24 kb/s is 3000 bytes a second; allow VBR headroom either way.
    Check(totalBytes > 1500 && totalBytes < 6000, "bitrate near 24 kb/s (" + std::to_string(totalBytes) + " bytes)");
}

void TestSilenceUsesDtx() {
    Voice::Encoder encoder;
    Check(encoder.Init(), "encoder init");
    std::array<float, Voice::kFrameSamples> silence{};
    // Opus enters DTX after roughly 200 ms of silence; from then on frames are
    // 1-2 byte DTX frames, apart from a comfort-noise refresh every 400 ms.
    constexpr int kSettleFrames = 20;
    int smallPackets = 0;
    for (int f = 0; f < 50; ++f) {
        std::array<uint8_t, Voice::kMaxPacketBytes> packet{};
        const int size = encoder.Encode(silence.data(), packet.data(), static_cast<int>(packet.size()));
        if (f >= kSettleFrames && size <= 2) {
            ++smallPackets;
        }
    }
    Check(smallPackets >= 50 - kSettleFrames - 2,
          "sustained silence becomes DTX frames (" + std::to_string(smallPackets) + "/30)");
}

void TestDecoderRejectsBadInput() {
    Voice::Decoder decoder;
    Check(decoder.Init(), "decoder init");
    std::array<float, Voice::kFrameSamples> out{};
    out.fill(1.0f);
    std::vector<uint8_t> oversized(Voice::kMaxPacketBytes + 1, 0x55);
    Check(!decoder.Decode(oversized.data(), oversized.size(), out.data()), "oversized packet rejected");
    Check(out[0] == 0.0f && out[Voice::kFrameSamples - 1] == 0.0f, "rejected packet yields silence");
    Check(!decoder.Decode(nullptr, 0, out.data()), "empty packet rejected");
    // Arbitrary bytes must never crash; they either decode or are rejected as silence.
    uint32_t state = 12345;
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<uint8_t> junk(1 + trial % 120);
        for (uint8_t& b : junk) {
            state = state * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(state >> 24);
        }
        decoder.Decode(junk.data(), junk.size(), out.data());
    }
    Check(decoder.Conceal(out.data()), "concealment produces a frame");
}

// --- jitter buffer -----------------------------------------------------------

std::vector<uint8_t> Payload(uint16_t seq) {
    return {static_cast<uint8_t>(seq & 0xFF), static_cast<uint8_t>(seq >> 8), 0xAB};
}

void Push(JitterBuffer& buffer, uint16_t seq) {
    const auto payload = Payload(seq);
    buffer.Push(seq, payload.data(), payload.size());
}

bool IsFrame(const JitterBuffer::Output& out, uint16_t seq) {
    return out.kind == JitterBuffer::Kind::Frame && out.packet && *out.packet == Payload(seq);
}

void TestJitterWaitsThenPlaysInOrder() {
    JitterBuffer buffer;
    Push(buffer, 10);
    Check(buffer.Pop().kind == JitterBuffer::Kind::Silent, "one frame is not enough to start");
    Push(buffer, 11);
    Check(buffer.Pop().kind == JitterBuffer::Kind::Silent, "two frames are not enough to start");
    Push(buffer, 12);
    Check(IsFrame(buffer.Pop(), 10), "starts at the first frame");
    Check(IsFrame(buffer.Pop(), 11), "second frame");
    Check(IsFrame(buffer.Pop(), 12), "third frame");
    Check(buffer.Pop().kind == JitterBuffer::Kind::Silent, "drained buffer goes quiet");
    Check(!buffer.Playing(), "and waits for the next spurt");
}

void TestJitterReorders() {
    JitterBuffer buffer;
    Push(buffer, 6);
    Push(buffer, 5);
    Push(buffer, 7);
    Check(IsFrame(buffer.Pop(), 5), "late-arriving earlier frame plays first");
    Check(IsFrame(buffer.Pop(), 6), "then the next");
    Check(IsFrame(buffer.Pop(), 7), "then the last");
}

void TestJitterReportsLossWithFec() {
    JitterBuffer buffer;
    for (uint16_t seq : {0, 1, 2, 4, 5}) {
        Push(buffer, seq);
    }
    Check(IsFrame(buffer.Pop(), 0), "frame 0");
    Check(IsFrame(buffer.Pop(), 1), "frame 1");
    Check(IsFrame(buffer.Pop(), 2), "frame 2");
    const auto lost = buffer.Pop();
    Check(lost.kind == JitterBuffer::Kind::Lost, "gap reported as lost");
    Check(lost.next && *lost.next == Payload(4), "lost frame offers the next packet for FEC");
    Check(IsFrame(buffer.Pop(), 4), "playback continues after the gap");
    Check(IsFrame(buffer.Pop(), 5), "frame 5");
}

void TestJitterDropsLateAndDuplicate() {
    JitterBuffer buffer;
    Push(buffer, 0);
    Push(buffer, 1);
    Push(buffer, 2);
    Push(buffer, 1);  // duplicate
    Check(buffer.Depth() == 3, "duplicate ignored");
    Check(IsFrame(buffer.Pop(), 0), "frame 0");
    Push(buffer, 0);  // its time has passed
    Check(buffer.Depth() == 2, "late frame ignored");
}

void TestJitterBoundsLatency() {
    JitterBuffer buffer;
    for (uint16_t seq = 0; seq < 20; ++seq) {
        Push(buffer, seq);
    }
    Check(buffer.Depth() == JitterBuffer::kMaxDepth, "depth capped");
    Check(IsFrame(buffer.Pop(), 20 - JitterBuffer::kMaxDepth), "oldest frames were dropped");
}

void TestJitterSequenceWrap() {
    JitterBuffer buffer;
    Push(buffer, 65534);
    Push(buffer, 65535);
    Push(buffer, 0);
    Push(buffer, 1);
    Check(IsFrame(buffer.Pop(), 65534), "65534");
    Check(IsFrame(buffer.Pop(), 65535), "65535");
    Check(IsFrame(buffer.Pop(), 0), "wraps to 0");
    Check(IsFrame(buffer.Pop(), 1), "then 1");
}

void TestJitterSenderRestart() {
    JitterBuffer buffer;
    Push(buffer, 100);
    Push(buffer, 101);
    Push(buffer, 102);
    Check(IsFrame(buffer.Pop(), 100), "frame 100");
    Push(buffer, 30000);  // sender restarted its counter
    Push(buffer, 30001);
    Push(buffer, 30002);
    Check(IsFrame(buffer.Pop(), 30000), "restarted stream plays from its new start");
}

void TestJitterLongGapStops() {
    JitterBuffer buffer;
    Push(buffer, 0);
    Push(buffer, 1);
    Push(buffer, 2);
    Push(buffer, 40);
    for (int i = 0; i < 3; ++i) {
        buffer.Pop();
    }
    JitterBuffer::Kind last = JitterBuffer::Kind::Lost;
    for (int i = 0; i <= JitterBuffer::kMaxMissStreak; ++i) {
        last = buffer.Pop().kind;
    }
    Check(last == JitterBuffer::Kind::Silent, "a long run of losses gives up instead of concealing forever");
    Check(!buffer.Playing(), "and rebuffers");
}

// --- signal helpers ------------------------------------------------------------

void TestLevelAndGate() {
    std::array<float, Voice::kFrameSamples> silence{};
    Check(Voice::LevelDb(silence.data(), silence.size()) == Voice::kSilenceDb, "silence level");
    const auto loud = Tone(0, 1.0f, 1000.0);
    const float loudDb = Voice::LevelDb(loud.data(), loud.size());
    Check(std::fabs(loudDb - (-3.01f)) < 0.2f, "full-scale sine is about -3 dBFS");

    Voice::ActivationGate gate;
    Check(!gate.Update(-60.0f, -40.0f), "quiet frame keeps the gate closed");
    Check(gate.Update(-20.0f, -40.0f), "loud frame opens it");
    for (int i = 0; i < Voice::ActivationGate::kHangoverFrames - 1; ++i) {
        Check(gate.Update(-60.0f, -40.0f), "hangover keeps it open");
    }
    Check(!gate.Update(-60.0f, -40.0f), "closes after the hangover");
}

void TestSoftLimit() {
    Check(Voice::SoftLimit(0.25f) == 0.25f && Voice::SoftLimit(-0.5f) == -0.5f, "transparent below the knee");
    Check(Voice::SoftLimit(4.0f) < 1.0f && Voice::SoftLimit(-4.0f) > -1.0f, "never exceeds full scale");
    Check(Voice::SoftLimit(0.9f) < Voice::SoftLimit(1.2f), "monotonic");
}

}  // namespace

int main() {
    TestCodecRoundTrip();
    TestSilenceUsesDtx();
    TestDecoderRejectsBadInput();
    TestJitterWaitsThenPlaysInOrder();
    TestJitterReorders();
    TestJitterReportsLossWithFec();
    TestJitterDropsLateAndDuplicate();
    TestJitterBoundsLatency();
    TestJitterSequenceWrap();
    TestJitterSenderRestart();
    TestJitterLongGapStops();
    TestLevelAndGate();
    TestSoftLimit();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("voice tests passed\n");
    return 0;
}
