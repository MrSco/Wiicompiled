#pragma once

// Small, allocation-free signal helpers for voice chat: level metering, the
// voice-activation gate, gain and a soft limiter for the mixed output.

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Voice {

inline constexpr float kSilenceDb = -96.0f;

// RMS level of a block in dBFS (0 dB = full-scale sine-ish level).
inline float LevelDb(const float* samples, size_t count) {
    if (!samples || count == 0) {
        return kSilenceDb;
    }
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    const double rms = std::sqrt(sum / static_cast<double>(count));
    return rms <= 1e-5 ? kSilenceDb : static_cast<float>(20.0 * std::log10(rms));
}

inline void ApplyGain(float* samples, size_t count, float gain) {
    if (gain == 1.0f) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        samples[i] *= gain;
    }
}

// Smoothly limits the mix to (-1, 1) instead of hard clipping when several
// people talk at once. Transparent below ~-6 dBFS.
inline float SoftLimit(float x) {
    constexpr float knee = 0.5f;
    const float magnitude = std::fabs(x);
    if (magnitude <= knee) {
        return x;
    }
    const float over = magnitude - knee;
    const float limited = knee + (1.0f - knee) * std::tanh(over / (1.0f - knee));
    return std::copysign(limited, x);
}

// Opens when a frame's level passes the threshold and stays open for a short
// hangover so word endings and brief pauses are not chopped off.
class ActivationGate {
public:
    static constexpr int kHangoverFrames = 15;  // 300 ms at 20 ms frames

    bool Update(float levelDb, float thresholdDb) {
        if (levelDb >= thresholdDb) {
            m_hangover = kHangoverFrames;
        } else if (m_hangover > 0) {
            --m_hangover;
        }
        return m_hangover > 0;
    }
    void Reset() { m_hangover = 0; }

private:
    int m_hangover = 0;
};

}  // namespace Voice
