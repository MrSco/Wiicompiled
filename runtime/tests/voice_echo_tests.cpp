// Voice chat's microphone clean-up: the playback timeline the echo canceller
// reads from, the resampler that feeds it, and the canceller itself on
// synthetic signals (no audio device needed).

#include "voice/voice_echo.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

// Deterministic white noise in [-1, 1].
struct Noise {
    uint32_t state;
    explicit Noise(uint32_t seed) : state(seed) {}
    float Next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<int32_t>(state)) / 2147483648.0f;
    }
};

double Energy(const std::vector<float>& signal, size_t from, size_t to) {
    double sum = 0.0;
    for (size_t i = from; i < to; ++i) {
        sum += static_cast<double>(signal[i]) * signal[i];
    }
    return sum / static_cast<double>(to - from) + 1e-20;
}

double Db(double ratio) {
    return 10.0 * std::log10(ratio);
}

// --- timeline ------------------------------------------------------------------

void TestCursor() {
    Voice::TimelineCursor cursor;
    Check(cursor.Place(1000, 480) == 1000, "first block goes where it is estimated");
    Check(cursor.Place(1480 + 40, 480) == 1480, "small jitter keeps the stream continuous");
    Check(cursor.Place(1960 + 500, 480) == 1961, "drift past the deadband is followed one sample at a time");
    Check(cursor.Place(2441 - 500, 480) == 2440, "and in the other direction");
    Check(cursor.Place(100000, 480) == 100000, "a large jump resynchronises");
    cursor.Reset();
    Check(cursor.Place(5, 480) == 5, "reset forgets the position");
}

void TestReferenceMixAndRead() {
    Voice::PlaybackReference reference;
    const size_t frames = 1000;  // spans block boundaries
    std::vector<float> game(frames * 2), voice(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        game[i * 2] = 0.25f;
        game[i * 2 + 1] = -0.25f;
        voice[i * 2] = 0.5f;
        voice[i * 2 + 1] = 0.5f;
    }
    const int64_t start = 48000 * 10 + 123;
    reference.Add(0, start, game.data(), frames);
    reference.Add(1, start + 500, voice.data(), frames);

    std::vector<float> out(1600 * 2, 9.0f);
    reference.Read(start - 100, out.data(), 1600);
    Check(out[0] == 0.0f && out[1] == 0.0f, "before anything was written: silence");
    Check(out[100 * 2] == 0.25f && out[100 * 2 + 1] == -0.25f, "game audio alone");
    Check(out[700 * 2] == 0.75f && out[700 * 2 + 1] == 0.25f, "game and voice mixed where they overlap");
    Check(out[1200 * 2] == 0.5f, "voice alone after the game stops");
    Check(out[1599 * 2] == 0.5f, "voice continues to the end");

    // Older than the buffer holds: the slot has been reused, so it reads as silence.
    const int64_t later = start + static_cast<int64_t>(Voice::PlaybackReference::kBlockCount) *
                                      Voice::PlaybackReference::kBlockFrames;
    reference.Add(0, later, game.data(), frames);
    reference.Read(start + 100, out.data(), 1);
    Check(out[0] == 0.0f, "audio older than the buffer reads as silence");
    // A late write for that old time must not damage the newer audio. (The
    // first write moves the voice source's cursor far away, so the second one
    // is placed exactly at the old time.)
    reference.Add(1, later + 50000, voice.data(), 10);
    reference.Add(1, start + 100, voice.data(), 10);
    reference.Read(later, out.data(), 1);
    Check(out[0] == 0.25f, "a stale write does not overwrite newer audio");

    reference.Reset();
    reference.Read(later, out.data(), 1);
    Check(out[0] == 0.0f, "reset clears everything");
}

void TestResampler() {
    Voice::ReferenceResampler resampler;
    std::vector<float> out;
    const int16_t mono[4] = {16384, -16384, 0, 32767};
    Check(resampler.Convert(mono, 4, 1, 48000, 0.5f, out), "48 kHz mono converts");
    Check(out.size() == 8 && out[0] == 0.25f && out[1] == 0.25f && out[2] == -0.25f,
          "mono is duplicated to both channels and scaled by the gain");

    // 32 kHz stereo (the Wii's output rate) becomes 1.5x as many frames.
    std::vector<int16_t> stereo(3200 * 2);
    for (size_t i = 0; i < 3200; ++i) {
        const double phase = 2.0 * 3.14159265358979 * 440.0 * static_cast<double>(i) / 32000.0;
        stereo[i * 2] = static_cast<int16_t>(std::lround(std::sin(phase) * 16000.0));
        stereo[i * 2 + 1] = 0;
    }
    size_t total = 0;
    double leftEnergy = 0.0;
    for (int block = 0; block < 10; ++block) {
        Check(resampler.Convert(stereo.data() + block * 320 * 2, 320, 2, 32000, 1.0f, out), "32 kHz converts");
        total += out.size() / 2;
        for (size_t i = 0; i < out.size() / 2; ++i) {
            leftEnergy += static_cast<double>(out[i * 2]) * out[i * 2];
        }
    }
    Check(total > 4700 && total <= 4800, "32 kHz -> 48 kHz gives 1.5x the frames");
    const double rms = std::sqrt(leftEnergy / static_cast<double>(total));
    Check(std::fabs(rms - (16000.0 / 32768.0) / std::sqrt(2.0)) < 0.03, "the tone keeps its level");
    Check(!resampler.Convert(mono, 1, 3, 48000, 1.0f, out), "three channels are refused");
}

// --- canceller -----------------------------------------------------------------

constexpr int kFrame = Voice::kEchoFrameSamples;

void TestBothOffLeavesAudioAlone() {
    Voice::EchoCanceller canceller;
    Check(canceller.Init(), "canceller initialises");
    std::vector<float> mic(kFrame), reference(kFrame * 2, 0.3f);
    for (int i = 0; i < kFrame; ++i) {
        mic[i] = 0.001f * static_cast<float>(i % 100);
    }
    const std::vector<float> original = mic;
    canceller.Process(mic.data(), reference.data(), false, false);
    Check(mic == original, "with both features off the audio is untouched");
}

void TestCancelsEcho() {
    // The speakers play independent noise on each channel; the microphone hears
    // each one delayed and quieter, the way a room would.
    Voice::EchoCanceller canceller;
    canceller.Init();
    Noise left(1), right(2);
    constexpr int kSeconds = 8;
    constexpr int kFrames = kSeconds * 100;
    constexpr size_t kDelayLeft = 1440;   // 30 ms
    constexpr size_t kDelayRight = 1700;  // 35 ms
    const size_t total = static_cast<size_t>(kFrames) * kFrame;
    std::vector<float> speakers(total * 2);
    for (size_t i = 0; i < total; ++i) {
        speakers[i * 2] = 0.2f * left.Next();
        speakers[i * 2 + 1] = 0.2f * right.Next();
    }
    std::vector<float> mic(total, 0.0f);
    for (size_t i = 0; i < total; ++i) {
        if (i >= kDelayLeft) mic[i] += 0.5f * speakers[(i - kDelayLeft) * 2];
        if (i >= kDelayRight) mic[i] += 0.3f * speakers[(i - kDelayRight) * 2 + 1];
    }
    std::vector<float> out = mic;
    for (int f = 0; f < kFrames; ++f) {
        canceller.Process(out.data() + static_cast<size_t>(f) * kFrame, speakers.data() + static_cast<size_t>(f) * kFrame * 2,
                          true, false);
    }
    const size_t lastSecond = total - 48000;
    const double reduction = Db(Energy(mic, lastSecond, total) / Energy(out, lastSecond, total));
    std::printf("  echo reduced by %.1f dB\n", reduction);
    // About 22 dB here; the margin keeps other compilers' float rounding from failing it.
    Check(reduction > 15.0, "game sound from the speakers is cancelled by at least 15 dB");
}

void TestKeepsYourVoice() {
    // With the speakers silent, echo cancellation must leave your own voice alone.
    Voice::EchoCanceller canceller;
    canceller.Init();
    constexpr int kFrames = 300;
    const size_t total = static_cast<size_t>(kFrames) * kFrame;
    std::vector<float> mic(total);
    Noise voice(7);
    for (size_t i = 0; i < total; ++i) {
        // A syllable-like burst pattern: 200 ms on, 100 ms off.
        const bool on = (i / 4800) % 3 != 2;
        mic[i] = on ? 0.2f * voice.Next() : 0.0f;
    }
    std::vector<float> out = mic;
    const std::vector<float> silence(static_cast<size_t>(kFrame) * 2, 0.0f);
    for (int f = 0; f < kFrames; ++f) {
        canceller.Process(out.data() + static_cast<size_t>(f) * kFrame, silence.data(), true, false);
    }
    const double change = Db(Energy(out, total / 2, total) / Energy(mic, total / 2, total));
    std::printf("  voice level changed by %.1f dB\n", change);
    Check(change > -3.0 && change < 1.0, "your voice passes through when nothing is playing");
}

void TestSuppressesNoise() {
    // A constant fan-like hiss is learned as noise and pulled down.
    Voice::EchoCanceller canceller;
    canceller.Init();
    constexpr int kFrames = 400;
    const size_t total = static_cast<size_t>(kFrames) * kFrame;
    std::vector<float> mic(total);
    Noise hiss(11);
    for (size_t i = 0; i < total; ++i) {
        mic[i] = 0.05f * hiss.Next();
    }
    std::vector<float> out = mic;
    const std::vector<float> silence(static_cast<size_t>(kFrame) * 2, 0.0f);
    for (int f = 0; f < kFrames; ++f) {
        canceller.Process(out.data() + static_cast<size_t>(f) * kFrame, silence.data(), false, true);
    }
    const double reduction = Db(Energy(mic, total - 48000, total) / Energy(out, total - 48000, total));
    std::printf("  steady noise reduced by %.1f dB\n", reduction);
    Check(reduction > 10.0, "steady background noise is reduced by at least 10 dB");
}

}  // namespace

int main() {
    TestCursor();
    TestReferenceMixAndRead();
    TestResampler();
    TestBothOffLeavesAudioAlone();
    TestCancelsEcho();
    TestKeepsYourVoice();
    TestSuppressesNoise();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("voice echo tests passed\n");
    return 0;
}
