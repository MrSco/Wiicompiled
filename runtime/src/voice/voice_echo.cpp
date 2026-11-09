#include "voice/voice_echo.h"

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <speex/speex_resampler.h>

#include <algorithm>
#include <cmath>

namespace Voice {
namespace {

constexpr uint32_t kTimelineRate = 48000;
// Background noise is pulled down by up to this much (SpeexDSP's default is -15).
constexpr int kNoiseSuppressDb = -30;

inline int16_t ToPcm16(float sample) {
    return static_cast<int16_t>(std::lrint(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
}

}  // namespace

// --- TimelineCursor ---------------------------------------------------------------

int64_t TimelineCursor::Place(int64_t estimate, size_t frames) {
    int64_t position = estimate;
    if (m_valid) {
        const int64_t drift = estimate - m_next;
        if (drift <= kResync && drift >= -kResync) {
            position = m_next;
            if (drift > kDeadband) {
                ++position;
            } else if (drift < -kDeadband) {
                --position;
            }
        }
    }
    m_next = position + static_cast<int64_t>(frames);
    m_valid = true;
    return position;
}

// --- PlaybackReference ------------------------------------------------------------

PlaybackReference::PlaybackReference() : m_blocks(kBlockCount) {
    for (Block& block : m_blocks) {
        block.samples.assign(static_cast<size_t>(kBlockFrames) * kEchoReferenceChannels, 0.0f);
    }
}

PlaybackReference::Block& PlaybackReference::Writable(int64_t blockIndex) {
    Block& block = m_blocks[static_cast<size_t>(blockIndex) % kBlockCount];
    if (block.index != blockIndex) {
        std::fill(block.samples.begin(), block.samples.end(), 0.0f);
        block.index = blockIndex;
    }
    return block;
}

void PlaybackReference::Add(int source, int64_t estimatedStart, const float* stereo, size_t frames) {
    if (source < 0 || source >= kSourceCount || frames == 0) {
        return;
    }
    const int64_t start = m_cursors[source].Place(estimatedStart, frames);
    for (size_t i = 0; i < frames;) {
        const int64_t t = start + static_cast<int64_t>(i);
        if (t < 0) {
            ++i;
            continue;
        }
        const int64_t blockIndex = t / kBlockFrames;
        const size_t offset = static_cast<size_t>(t % kBlockFrames);
        const size_t run = std::min(frames - i, static_cast<size_t>(kBlockFrames) - offset);
        const Block& existing = m_blocks[static_cast<size_t>(blockIndex) % kBlockCount];
        // Never overwrite a newer block with audio that is already too old to hold.
        if (existing.index <= blockIndex) {
            Block& block = Writable(blockIndex);
            float* dst = block.samples.data() + offset * kEchoReferenceChannels;
            const float* src = stereo + i * kEchoReferenceChannels;
            for (size_t s = 0; s < run * kEchoReferenceChannels; ++s) {
                dst[s] += src[s];
            }
        }
        i += run;
    }
}

void PlaybackReference::Read(int64_t start, float* stereo, size_t frames) const {
    for (size_t i = 0; i < frames;) {
        const int64_t t = start + static_cast<int64_t>(i);
        if (t < 0) {
            stereo[i * kEchoReferenceChannels] = 0.0f;
            stereo[i * kEchoReferenceChannels + 1] = 0.0f;
            ++i;
            continue;
        }
        const int64_t blockIndex = t / kBlockFrames;
        const size_t offset = static_cast<size_t>(t % kBlockFrames);
        const size_t run = std::min(frames - i, static_cast<size_t>(kBlockFrames) - offset);
        const Block& block = m_blocks[static_cast<size_t>(blockIndex) % kBlockCount];
        float* dst = stereo + i * kEchoReferenceChannels;
        if (block.index == blockIndex) {
            std::copy_n(block.samples.data() + offset * kEchoReferenceChannels, run * kEchoReferenceChannels, dst);
        } else {
            std::fill_n(dst, run * kEchoReferenceChannels, 0.0f);
        }
        i += run;
    }
}

void PlaybackReference::Reset() {
    for (Block& block : m_blocks) {
        block.index = -1;
    }
    for (TimelineCursor& cursor : m_cursors) {
        cursor.Reset();
    }
}

// --- ReferenceResampler -----------------------------------------------------------

struct ReferenceResampler::Impl {
    SpeexResamplerState* state = nullptr;
    uint32_t rate = 0;
    std::vector<float> input;

    ~Impl() {
        if (state) {
            speex_resampler_destroy(state);
        }
    }
};

ReferenceResampler::ReferenceResampler() : m_impl(std::make_unique<Impl>()) {}
ReferenceResampler::~ReferenceResampler() = default;

bool ReferenceResampler::Convert(const int16_t* samples, size_t frames, uint32_t channels, uint32_t sampleRate,
                                 float gain, std::vector<float>& out) {
    out.clear();
    if (!samples || frames == 0 || (channels != 1 && channels != 2) || sampleRate < 8000 || sampleRate > 192000) {
        return false;
    }
    Impl& impl = *m_impl;
    std::vector<float>& input = sampleRate == kTimelineRate ? out : impl.input;
    input.resize(frames * kEchoReferenceChannels);
    const float scale = gain / 32768.0f;
    for (size_t i = 0; i < frames; ++i) {
        const float left = samples[i * channels] * scale;
        const float right = channels == 2 ? samples[i * channels + 1] * scale : left;
        input[i * 2] = left;
        input[i * 2 + 1] = right;
    }
    if (sampleRate == kTimelineRate) {
        return true;
    }
    if (!impl.state || impl.rate != sampleRate) {
        if (impl.state) {
            speex_resampler_destroy(impl.state);
        }
        int error = 0;
        impl.state = speex_resampler_init(kEchoReferenceChannels, sampleRate, kTimelineRate,
                                          SPEEX_RESAMPLER_QUALITY_VOIP, &error);
        impl.rate = impl.state ? sampleRate : 0;
        if (!impl.state) {
            return false;
        }
    }
    const size_t capacity = frames * kTimelineRate / sampleRate + 16;
    out.resize(capacity * kEchoReferenceChannels);
    spx_uint32_t inFrames = static_cast<spx_uint32_t>(frames);
    spx_uint32_t outFrames = static_cast<spx_uint32_t>(capacity);
    speex_resampler_process_interleaved_float(impl.state, input.data(), &inFrames, out.data(), &outFrames);
    out.resize(static_cast<size_t>(outFrames) * kEchoReferenceChannels);
    return true;
}

// --- EchoCanceller ----------------------------------------------------------------

struct EchoCanceller::Impl {
    SpeexEchoState* echo = nullptr;
    SpeexPreprocessState* preprocess = nullptr;
    bool echoAttached = false;
    bool denoise = true;
    int16_t mic[kEchoFrameSamples] = {};
    int16_t cleaned[kEchoFrameSamples] = {};
    int16_t reference[kEchoFrameSamples * kEchoReferenceChannels] = {};

    ~Impl() {
        if (preprocess) {
            speex_preprocess_state_destroy(preprocess);
        }
        if (echo) {
            speex_echo_state_destroy(echo);
        }
    }
};

EchoCanceller::EchoCanceller() : m_impl(std::make_unique<Impl>()) {}
EchoCanceller::~EchoCanceller() = default;

bool EchoCanceller::Init() {
    Impl& impl = *m_impl;
    if (impl.echo && impl.preprocess) {
        return true;
    }
    impl.echo = speex_echo_state_init_mc(kEchoFrameSamples, kEchoTailSamples, 1, kEchoReferenceChannels);
    impl.preprocess = speex_preprocess_state_init(kEchoFrameSamples, static_cast<int>(kTimelineRate));
    if (!impl.echo || !impl.preprocess) {
        return false;
    }
    int rate = static_cast<int>(kTimelineRate);
    speex_echo_ctl(impl.echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    int on = 1;
    speex_preprocess_ctl(impl.preprocess, SPEEX_PREPROCESS_SET_DENOISE, &on);
    int suppress = kNoiseSuppressDb;
    speex_preprocess_ctl(impl.preprocess, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress);
    impl.denoise = true;
    impl.echoAttached = false;
    return true;
}

void EchoCanceller::Process(float* mic, const float* reference, bool cancelEcho, bool suppressNoise) {
    Impl& impl = *m_impl;
    if (!impl.echo || !impl.preprocess || (!cancelEcho && !suppressNoise)) {
        return;
    }
    for (int i = 0; i < kEchoFrameSamples; ++i) {
        impl.mic[i] = ToPcm16(mic[i]);
    }
    int16_t* signal = impl.mic;
    if (cancelEcho) {
        if (!impl.echoAttached) {
            // Start from a clean filter: the echo path may have changed while off.
            speex_echo_state_reset(impl.echo);
            speex_preprocess_ctl(impl.preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, impl.echo);
            impl.echoAttached = true;
        }
        for (int i = 0; i < kEchoFrameSamples * kEchoReferenceChannels; ++i) {
            impl.reference[i] = ToPcm16(reference[i]);
        }
        speex_echo_cancellation(impl.echo, impl.mic, impl.reference, impl.cleaned);
        signal = impl.cleaned;
    } else if (impl.echoAttached) {
        speex_preprocess_ctl(impl.preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, nullptr);
        impl.echoAttached = false;
    }
    if (impl.denoise != suppressNoise) {
        int denoise = suppressNoise ? 1 : 0;
        speex_preprocess_ctl(impl.preprocess, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
        impl.denoise = suppressNoise;
    }
    // Also removes the echo the filter could not (residual echo suppression).
    speex_preprocess_run(impl.preprocess, signal);
    for (int i = 0; i < kEchoFrameSamples; ++i) {
        mic[i] = static_cast<float>(signal[i]) / 32767.0f;
    }
}

void EchoCanceller::Reset() {
    m_impl = std::make_unique<Impl>();
    Init();
}

}  // namespace Voice
