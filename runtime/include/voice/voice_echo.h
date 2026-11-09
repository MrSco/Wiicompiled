#pragma once

// Microphone clean-up for voice chat: acoustic echo cancellation and noise
// suppression (SpeexDSP).
//
// Echo is what the microphone picks up from the speakers: the game's sound and
// other players' voices. Unlike most voice chat programs, WiiCompiled knows
// exactly what the speakers are playing, because every sample of game audio
// and voice playback passes through the runtime. Both are recorded on a shared
// timeline (PlaybackReference) at the time they are expected to leave the
// speakers, and the canceller subtracts them from the microphone at the time
// it was captured. Whatever delay the estimate misses (device latency, the
// distance to the microphone) is covered by the canceller's filter length.
//
// Nothing here touches an audio device, so all of it is exercised by the
// voice echo tests.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Voice {

// The canceller works on 10 ms frames; voice chat's 20 ms frames are two of them.
inline constexpr int kEchoFrameSamples = 480;
// Game audio and voice playback are both stereo.
inline constexpr int kEchoReferenceChannels = 2;
// Echo arriving up to this long after the timeline says the sound left the
// speakers is cancelled: device latency plus the room's reverberation.
inline constexpr int kEchoTailSamples = 9600;  // 200 ms

// Keeps a running position for a stream whose start times are estimated from
// a clock: small estimation jitter is ignored so the stream stays continuous,
// slow drift is followed one sample at a time, and a large jump (a stall,
// a device change) resynchronises at once.
class TimelineCursor {
public:
    // Returns where a block of `frames` estimated to start at `estimate` goes.
    int64_t Place(int64_t estimate, size_t frames);
    void Reset() { m_valid = false; }

    static constexpr int64_t kDeadband = 96;   // 2 ms of jitter is ignored
    static constexpr int64_t kResync = 2400;   // beyond 50 ms the estimate wins

private:
    int64_t m_next = 0;
    bool m_valid = false;
};

// What the speakers play, on one timeline measured in 48 kHz frames. Several
// sources (the game, voice playback) are mixed where they overlap. Frames
// nobody wrote, or that are older than the buffer holds, read as silence.
class PlaybackReference {
public:
    static constexpr int kSourceCount = 2;
    static constexpr int kBlockFrames = kEchoFrameSamples;
    static constexpr size_t kBlockCount = 256;  // 2.56 s

    PlaybackReference();

    // Mixes interleaved stereo frames from `source` that are estimated to
    // start playing at `estimatedStart`.
    void Add(int source, int64_t estimatedStart, const float* stereo, size_t frames);
    // Reads the interleaved stereo frames that were playing from `start`.
    void Read(int64_t start, float* stereo, size_t frames) const;
    void Reset();

private:
    struct Block {
        int64_t index = -1;  // which timeline block this slot currently holds
        std::vector<float> samples;
    };
    Block& Writable(int64_t blockIndex);

    std::vector<Block> m_blocks;
    TimelineCursor m_cursors[kSourceCount];
};

// Converts a source's interleaved 16-bit stereo or mono audio at any rate to
// 48 kHz float stereo for PlaybackReference.
class ReferenceResampler {
public:
    ReferenceResampler();
    ~ReferenceResampler();
    ReferenceResampler(const ReferenceResampler&) = delete;
    ReferenceResampler& operator=(const ReferenceResampler&) = delete;

    // Appends the converted frames to `out` (cleared first), scaled by gain.
    // Returns false if the format cannot be converted.
    bool Convert(const int16_t* samples, size_t frames, uint32_t channels, uint32_t sampleRate, float gain,
                 std::vector<float>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Echo cancellation and noise suppression for one microphone.
class EchoCanceller {
public:
    EchoCanceller();
    ~EchoCanceller();
    EchoCanceller(const EchoCanceller&) = delete;
    EchoCanceller& operator=(const EchoCanceller&) = delete;

    bool Init();
    // Cleans kEchoFrameSamples of mono microphone audio in place. `reference`
    // holds the kEchoFrameSamples interleaved stereo frames that were playing
    // when it was captured. With both features off the audio is untouched.
    void Process(float* mic, const float* reference, bool cancelEcho, bool suppressNoise);
    // Forgets the learned echo path and noise profile (a new microphone).
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace Voice
