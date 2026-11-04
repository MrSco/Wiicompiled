#include "abi_bridge.h"
#include "isa/big_endian.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "guest_function_imports.h"
#include "runtime_log.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>


#if defined(__clang__)
// PowerPC uses discrete fmuls/fadds; a fused multiply-add would change sample rounding.
#pragma clang fp contract(off)
#endif

namespace {
inline float LoadFloat(const uint8_t* host) {
    return BigEndian::ReadFloat32(host);
}

inline void StoreFloat(uint8_t* host, float value) {
    BigEndian::WriteFloat32(host, value);
}

inline int32_t LoadS32(const uint8_t* host) {
    return static_cast<int32_t>(BigEndian::Read32(host));
}

inline void StoreS32(uint8_t* host, int32_t value) {
    BigEndian::Write32(host, static_cast<uint32_t>(value));
}

// PowerPC fctiwz: round toward zero, saturating out-of-range and NaN exactly the
// way runtime/src/fpu_helpers.cpp does for the translated form.
inline int32_t ConvertToIntegerWord(float value) {
    const double wide = static_cast<double>(value);
    if (std::isnan(wide)) {
        return static_cast<int32_t>(0x80000000u);
    }
    if (wide >= 2147483647.0) {
        return 2147483647;
    }
    if (wide <= -2147483648.0) {
        return static_cast<int32_t>(0x80000000u);
    }
    return static_cast<int32_t>(wide);
}

// Guest-thread-only range resolver. Deliberately NOT the mix's MixResolveRange: this
// callback must materialize deferred GX reads through the page table, which the
// worker-safe resolver refuses to do by design.
uint8_t* ResolveGuestThreadRange(uint32_t addr, size_t bytes) {
    if (addr == 0 || bytes == 0) {
        return nullptr;
    }
    if (uint8_t* fast = MemoryInline::GetPointerFast(addr, bytes)) {
        return fast;
    }
    // A ring buffer may straddle the inline page granularity; the region lookup
    // still returns one contiguous host mapping for the whole range.
    try {
        return Memory::GetPointer(addr, bytes);
    } catch (const Memory::AccessViolation&) {
        return nullptr;
    }
}

namespace ReverbStd {

constexpr uint32_t kSamplesPerFrame = 96;
constexpr uint32_t kChannels = 3;

// .sdata2 constants the guest function loads through r2.
const uint32_t kOneConstantAddr = MKW_GADDR(80388588);   // 1.0f
const uint32_t kScaleConstantAddr = MKW_GADDR(8038858C); // 0.6f send pre-scale

// AXFX_REVERBSTD_EXP field offsets (byte offsets into the struct in r4).
constexpr uint32_t kFieldPreDelayCoef = 0x18;
constexpr uint32_t kFieldEarlyLength = 0x2C;
constexpr uint32_t kFieldComb1Coef = 0x64;
constexpr uint32_t kFieldComb2Coef = 0x68;
constexpr uint32_t kFieldAllpassCoef = 0x9C;
constexpr uint32_t kFieldLastAllpass = 0xA0; // + channel * 4
constexpr uint32_t kFieldDamping = 0xAC;
constexpr uint32_t kFieldFlags = 0xB0;
constexpr uint32_t kFieldDryPreScale = 0xD0;
constexpr uint32_t kFieldWetPreScale = 0xD4;
constexpr uint32_t kFieldAuxInputBuffers = 0xD8;
constexpr uint32_t kFieldAuxOutputBuffers = 0xDC;
constexpr uint32_t kFieldMainOutGain = 0xE0;
constexpr uint32_t kFieldAuxOutGain = 0xE4;
constexpr uint32_t kStateStructBytes = 0xE8;

enum RingId : uint32_t {
    kRingPreDelay = 0,
    kRingEarly,
    kRingComb1,
    kRingComb2,
    kRingAllpass1,
    kRingAllpass2,
    kRingCount,
};

struct RingLayout {
    uint32_t bufferField;   // Channel 0 buffer pointer.
    uint32_t channelStride; // Byte stride between channel buffer pointers.
    uint32_t indexField;
    uint32_t lengthField;
};

constexpr RingLayout kRingLayout[kRingCount] = {
    {0x00, 4, 0x0C, 0x10}, // Pre-delay comb.
    {0x1C, 4, 0x28, 0x2C}, // Early reflection tap (optional).
    {0x34, 8, 0x4C, 0x54}, // Comb 1 (per-channel pointers interleave with comb 2).
    {0x38, 8, 0x50, 0x58}, // Comb 2.
    {0x6C, 8, 0x84, 0x8C}, // Allpass 1 (interleaves with allpass 2).
    {0x70, 8, 0x88, 0x90}, // Allpass 2.
};

struct Frame {
    uint8_t* ring[kRingCount][kChannels]{};
    uint32_t ringIndex[kRingCount]{};
    uint32_t ringLength[kRingCount]{};
    uint8_t* main[kChannels]{};
    const uint8_t* auxIn[kChannels]{};
    uint8_t* auxOut[kChannels]{};
    float lastAllpass[kChannels]{};
    float preDelayCoef = 0.0f;
    float comb1Coef = 0.0f;
    float comb2Coef = 0.0f;
    float allpassCoef = 0.0f;
    float damping = 0.0f;
    float oneMinusDamping = 0.0f;
    float dryScale = 0.0f;
    float wetScale = 0.0f;
    float mainGain = 0.0f;
    float auxGain = 0.0f;
    bool hasEarly = false;
    bool hasAuxIn = false;
    bool hasAuxOut = false;
};

// Collects everything the render loop needs. Returns false when the layout is
// not one this port can serve bit-exactly, in which case the caller must run the
// translated function instead.
bool BuildFrame(uint32_t buffersAddr, uint32_t stateAddr, Frame& frame) {
    if (buffersAddr == 0 || stateAddr == 0) {
        return false;
    }
    if (!Memory::Contains(stateAddr, kStateStructBytes)) {
        return false;
    }

    const uint32_t auxInputBuffers = Memory::Read32(stateAddr + kFieldAuxInputBuffers);
    const uint32_t auxOutputBuffers = Memory::Read32(stateAddr + kFieldAuxOutputBuffers);
    frame.hasAuxIn = auxInputBuffers != 0;
    frame.hasAuxOut = auxOutputBuffers != 0;

    constexpr size_t kFrameBytes = kSamplesPerFrame * sizeof(int32_t);
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        frame.main[channel] =
            ResolveGuestThreadRange(Memory::Read32(buffersAddr + channel * 4), kFrameBytes);
        if (!frame.main[channel]) {
            return false;
        }
        if (frame.hasAuxIn) {
            frame.auxIn[channel] =
                ResolveGuestThreadRange(Memory::Read32(auxInputBuffers + channel * 4), kFrameBytes);
            if (!frame.auxIn[channel]) {
                return false;
            }
        }
        if (frame.hasAuxOut) {
            frame.auxOut[channel] =
                ResolveGuestThreadRange(Memory::Read32(auxOutputBuffers + channel * 4), kFrameBytes);
            if (!frame.auxOut[channel]) {
                return false;
            }
        }
    }

    frame.hasEarly = Memory::Read32(stateAddr + kFieldEarlyLength) != 0;
    for (uint32_t ring = 0; ring < kRingCount; ++ring) {
        const RingLayout& layout = kRingLayout[ring];
        frame.ringLength[ring] = Memory::Read32(stateAddr + layout.lengthField);
        frame.ringIndex[ring] = Memory::Read32(stateAddr + layout.indexField);
        if (ring == kRingEarly && !frame.hasEarly) {
            continue;
        }
        // The guest maintains index < length; anything else means the struct is
        // not initialized the way this port assumes.
        if (frame.ringLength[ring] == 0 || frame.ringIndex[ring] >= frame.ringLength[ring]) {
            return false;
        }
        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            frame.ring[ring][channel] = ResolveGuestThreadRange(
                Memory::Read32(stateAddr + layout.bufferField + channel * layout.channelStride),
                static_cast<size_t>(frame.ringLength[ring]) * sizeof(float));
            if (!frame.ring[ring][channel]) {
                return false;
            }
        }
    }

    const float sendScale = Memory::ReadFloat32(kScaleConstantAddr);
    const float one = Memory::ReadFloat32(kOneConstantAddr);
    frame.damping = Memory::ReadFloat32(stateAddr + kFieldDamping);
    frame.oneMinusDamping = one - frame.damping;
    frame.dryScale = sendScale * Memory::ReadFloat32(stateAddr + kFieldDryPreScale);
    frame.wetScale = sendScale * Memory::ReadFloat32(stateAddr + kFieldWetPreScale);
    frame.preDelayCoef = Memory::ReadFloat32(stateAddr + kFieldPreDelayCoef);
    frame.comb1Coef = Memory::ReadFloat32(stateAddr + kFieldComb1Coef);
    frame.comb2Coef = Memory::ReadFloat32(stateAddr + kFieldComb2Coef);
    frame.allpassCoef = Memory::ReadFloat32(stateAddr + kFieldAllpassCoef);
    frame.mainGain = Memory::ReadFloat32(stateAddr + kFieldMainOutGain);
    frame.auxGain = Memory::ReadFloat32(stateAddr + kFieldAuxOutGain);
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        frame.lastAllpass[channel] = Memory::ReadFloat32(stateAddr + kFieldLastAllpass + channel * 4);
    }
    return true;
}

void Render(uint32_t stateAddr, Frame& frame) {
    uint32_t index[kRingCount];
    for (uint32_t ring = 0; ring < kRingCount; ++ring) {
        index[ring] = frame.ringIndex[ring];
    }

    for (uint32_t sample = 0; sample < kSamplesPerFrame; ++sample) {
        const uint32_t preDelayOffset = index[kRingPreDelay] * 4u;
        const uint32_t earlyOffset = index[kRingEarly] * 4u;
        const uint32_t comb1Offset = index[kRingComb1] * 4u;
        const uint32_t comb2Offset = index[kRingComb2] * 4u;
        const uint32_t allpass1Offset = index[kRingAllpass1] * 4u;
        const uint32_t allpass2Offset = index[kRingAllpass2] * 4u;
        const uint32_t frameOffset = sample * 4u;

        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            uint8_t* const mainSlot = frame.main[channel] + frameOffset;
            int32_t rawInput = LoadS32(mainSlot);
            if (frame.hasAuxIn) {
                rawInput = static_cast<int32_t>(
                    static_cast<uint32_t>(rawInput) +
                    static_cast<uint32_t>(LoadS32(frame.auxIn[channel] + frameOffset)));
            }
            const float input = static_cast<float>(rawInput);

            // Pre-delay comb: the tap that leaves the buffer also feeds the dry
            // (early) send. Every product is its own statement so the host
            // compiler cannot fuse a multiply into the following add.
            uint8_t* const preDelaySlot = frame.ring[kRingPreDelay][channel] + preDelayOffset;
            const float preDelayTap = LoadFloat(preDelaySlot);
            const float preDelayFeedback = preDelayTap * frame.preDelayCoef;
            StoreFloat(preDelaySlot, input + preDelayFeedback);

            float excite = input;
            if (frame.hasEarly) {
                uint8_t* const earlySlot = frame.ring[kRingEarly][channel] + earlyOffset;
                excite = LoadFloat(earlySlot);
                StoreFloat(earlySlot, input);
            }

            const float dry = preDelayTap * frame.dryScale;

            uint8_t* const comb1Slot = frame.ring[kRingComb1][channel] + comb1Offset;
            const float comb1Tap = LoadFloat(comb1Slot);
            const float comb1Feedback = comb1Tap * frame.comb1Coef;
            StoreFloat(comb1Slot, excite + comb1Feedback);

            uint8_t* const comb2Slot = frame.ring[kRingComb2][channel] + comb2Offset;
            const float comb2Tap = LoadFloat(comb2Slot);
            const float comb2Feedback = comb2Tap * frame.comb2Coef;
            const float combSum = comb1Tap + comb2Tap;
            StoreFloat(comb2Slot, excite + comb2Feedback);

            uint8_t* const allpass1Slot = frame.ring[kRingAllpass1][channel] + allpass1Offset;
            const float allpass1Tap = LoadFloat(allpass1Slot);
            const float allpass1Feedback = allpass1Tap * frame.allpassCoef;
            const float allpass1Store = combSum + allpass1Feedback;
            StoreFloat(allpass1Slot, allpass1Store);
            const float allpass1Feedforward = allpass1Store * frame.allpassCoef;
            const float allpass1Out = allpass1Tap - allpass1Feedforward;

            const float dampedNew = frame.oneMinusDamping * allpass1Out;
            const float dampedOld = frame.damping * frame.lastAllpass[channel];
            const float damped = dampedNew + dampedOld;
            frame.lastAllpass[channel] = damped;

            uint8_t* const allpass2Slot = frame.ring[kRingAllpass2][channel] + allpass2Offset;
            const float allpass2Tap = LoadFloat(allpass2Slot);
            const float allpass2Feedback = allpass2Tap * frame.allpassCoef;
            const float allpass2Store = damped + allpass2Feedback;
            StoreFloat(allpass2Slot, allpass2Store);
            const float allpass2Feedforward = allpass2Store * frame.allpassCoef;
            const float allpass2Out = allpass2Tap - allpass2Feedforward;

            const float wet = allpass2Out * frame.wetScale;
            const float mixed = dry + wet;
            const float mainSample = mixed * frame.mainGain;
            StoreS32(mainSlot, ConvertToIntegerWord(mainSample));
            if (frame.hasAuxOut) {
                const float auxSample = mixed * frame.auxGain;
                StoreS32(frame.auxOut[channel] + frameOffset, ConvertToIntegerWord(auxSample));
            }
        }

        for (uint32_t ring = 0; ring < kRingCount; ++ring) {
            if (ring == kRingEarly && !frame.hasEarly) {
                continue;
            }
            const uint32_t next = index[ring] + 1u;
            index[ring] = next < frame.ringLength[ring] ? next : 0u;
        }
    }

    // The guest writes these back every sample; nothing can observe the
    // intermediate values, so one store per field at the end is equivalent.
    for (uint32_t ring = 0; ring < kRingCount; ++ring) {
        if (ring == kRingEarly && !frame.hasEarly) {
            continue;
        }
        Memory::Write32(stateAddr + kRingLayout[ring].indexField, index[ring]);
    }
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        Memory::WriteFloat32(stateAddr + kFieldLastAllpass + channel * 4,
                             static_cast<double>(frame.lastAllpass[channel]));
    }
}

} // namespace ReverbStd

namespace ReverbHi {

constexpr uint32_t kSamplesPerFrame = 96;
constexpr uint32_t kChannels = 3;
constexpr uint32_t kEarlyTaps = 3;
constexpr uint32_t kCombs = 3;
constexpr uint32_t kAllpasses = 2;

// .sdata2 constants the guest function loads through r2 (_SDA2_BASE_ = 0x8038EFA0).
const uint32_t kZeroConstantAddr = MKW_GADDR(803884D4); // 0.0f, the comb accumulator seed
const uint32_t kOneConstantAddr = MKW_GADDR(803884D8);  // 1.0f
const uint32_t kWetConstantAddr = MKW_GADDR(803884DC);  // 0.6f wet pre-scale
const uint32_t kMixConstantAddr = MKW_GADDR(803884E0);  // 0.5f channel cross-mix

// AXFX_REVERBHI_EXP field offsets (byte offsets into the struct in r4). Every one
// is read off the translated body at 0x801284B4 rather than guessed: the arrays
// sit back to back, which is what makes the strides below self-checking.
constexpr uint32_t kFieldEarlyLine = 0x00;      // + channel * 4
constexpr uint32_t kFieldEarlyPos = 0x0C;       // + tap * 4, one set shared by all channels
constexpr uint32_t kFieldEarlyLength = 0x18;
constexpr uint32_t kFieldEarlyCoef = 0x20;      // + tap * 4
constexpr uint32_t kFieldPreDelayLine = 0x2C;   // + channel * 4
constexpr uint32_t kFieldPreDelayPos = 0x38;
constexpr uint32_t kFieldPreDelayLength = 0x3C; // 0 bypasses the pre-delay
constexpr uint32_t kFieldCombLine = 0x44;       // + channel * 12 + comb * 4
constexpr uint32_t kFieldCombPos = 0x68;        // + comb * 4
constexpr uint32_t kFieldCombLength = 0x74;     // + comb * 4
constexpr uint32_t kFieldCombCoef = 0x8C;       // + comb * 4
constexpr uint32_t kFieldAllpassLine = 0x98;    // + channel * 8 + allpass * 4
constexpr uint32_t kFieldAllpassPos = 0xB0;     // + allpass * 4
constexpr uint32_t kFieldAllpassLength = 0xB8;  // + allpass * 4
constexpr uint32_t kFieldLastApLine = 0xC8;     // + channel * 4
constexpr uint32_t kFieldLastApPos = 0xD4;      // + channel * 4
constexpr uint32_t kFieldLastApLength = 0xE0;   // + channel * 4
constexpr uint32_t kFieldAllpassCoef = 0xF8;
constexpr uint32_t kFieldLastLpfOut = 0xFC;     // + channel * 4
constexpr uint32_t kFieldDamping = 0x108;
constexpr uint32_t kFieldFlags = 0x10C;
constexpr uint32_t kFieldMixPreScale = 0x12C;
constexpr uint32_t kFieldWetPreScale = 0x134;
constexpr uint32_t kFieldAuxInputBuffers = 0x138;
constexpr uint32_t kFieldAuxOutputBuffers = 0x13C;
constexpr uint32_t kFieldMainOutGain = 0x140;
constexpr uint32_t kFieldAuxOutGain = 0x144;
constexpr uint32_t kStateStructBytes = 0x148;

struct Frame {
    uint8_t* early[kChannels]{};
    uint8_t* preDelay[kChannels]{};
    uint8_t* comb[kChannels][kCombs]{};
    uint8_t* allpass[kChannels][kAllpasses]{};
    uint8_t* lastAp[kChannels]{};
    uint8_t* main[kChannels]{};
    const uint8_t* auxIn[kChannels]{};
    uint8_t* auxOut[kChannels]{};

    uint32_t earlyPos[kEarlyTaps]{};
    uint32_t earlyLength = 0;
    float earlyCoef[kEarlyTaps]{};

    uint32_t preDelayPos = 0;
    uint32_t preDelayLength = 0;

    uint32_t combPos[kCombs]{};
    uint32_t combLength[kCombs]{};
    float combCoef[kCombs]{};

    uint32_t allpassPos[kAllpasses]{};
    uint32_t allpassLength[kAllpasses]{};

    uint32_t lastApPos[kChannels]{};
    uint32_t lastApLength[kChannels]{};
    float lastLpfOut[kChannels]{};

    float zero = 0.0f;
    float allpassCoef = 0.0f;
    float damping = 0.0f;
    float oneMinusDamping = 0.0f;
    float wetScale = 0.0f;
    float mixScale = 0.0f;
    float mainGain = 0.0f;
    float auxGain = 0.0f;
    bool hasAuxIn = false;
    bool hasAuxOut = false;
};

// Same contract as ReverbStd::BuildFrame: false means this port cannot serve the
// layout bit-exactly, and the caller runs the translated function instead.
bool BuildFrame(uint32_t buffersAddr, uint32_t stateAddr, Frame& frame) {
    if (buffersAddr == 0 || stateAddr == 0) {
        return false;
    }
    if (!Memory::Contains(stateAddr, kStateStructBytes)) {
        return false;
    }

    const uint32_t auxInputBuffers = Memory::Read32(stateAddr + kFieldAuxInputBuffers);
    const uint32_t auxOutputBuffers = Memory::Read32(stateAddr + kFieldAuxOutputBuffers);
    frame.hasAuxIn = auxInputBuffers != 0;
    frame.hasAuxOut = auxOutputBuffers != 0;

    constexpr size_t kFrameBytes = kSamplesPerFrame * sizeof(int32_t);
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        frame.main[channel] =
            ResolveGuestThreadRange(Memory::Read32(buffersAddr + channel * 4), kFrameBytes);
        if (!frame.main[channel]) {
            return false;
        }
        if (frame.hasAuxIn) {
            frame.auxIn[channel] =
                ResolveGuestThreadRange(Memory::Read32(auxInputBuffers + channel * 4), kFrameBytes);
            if (!frame.auxIn[channel]) {
                return false;
            }
        }
        if (frame.hasAuxOut) {
            frame.auxOut[channel] =
                ResolveGuestThreadRange(Memory::Read32(auxOutputBuffers + channel * 4), kFrameBytes);
            if (!frame.auxOut[channel]) {
                return false;
            }
        }
    }

    // Early reflections: three read taps into one per-channel line, all wrapping
    // against a single shared length.
    frame.earlyLength = Memory::Read32(stateAddr + kFieldEarlyLength);
    if (frame.earlyLength == 0) {
        return false;
    }
    for (uint32_t tap = 0; tap < kEarlyTaps; ++tap) {
        frame.earlyPos[tap] = Memory::Read32(stateAddr + kFieldEarlyPos + tap * 4);
        if (frame.earlyPos[tap] >= frame.earlyLength) {
            return false;
        }
        frame.earlyCoef[tap] = Memory::ReadFloat32(stateAddr + kFieldEarlyCoef + tap * 4);
    }
    const size_t earlyBytes = static_cast<size_t>(frame.earlyLength) * sizeof(float);
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        frame.early[channel] = ResolveGuestThreadRange(
            Memory::Read32(stateAddr + kFieldEarlyLine + channel * 4), earlyBytes);
        if (!frame.early[channel]) {
            return false;
        }
    }

    // A zero pre-delay length is the guest's own bypass, not a broken layout, so
    // the lines are only required when it is armed.
    frame.preDelayLength = Memory::Read32(stateAddr + kFieldPreDelayLength);
    frame.preDelayPos = Memory::Read32(stateAddr + kFieldPreDelayPos);
    if (frame.preDelayLength != 0) {
        if (frame.preDelayPos >= frame.preDelayLength) {
            return false;
        }
        const size_t preDelayBytes = static_cast<size_t>(frame.preDelayLength) * sizeof(float);
        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            frame.preDelay[channel] = ResolveGuestThreadRange(
                Memory::Read32(stateAddr + kFieldPreDelayLine + channel * 4), preDelayBytes);
            if (!frame.preDelay[channel]) {
                return false;
            }
        }
    }

    for (uint32_t comb = 0; comb < kCombs; ++comb) {
        frame.combLength[comb] = Memory::Read32(stateAddr + kFieldCombLength + comb * 4);
        frame.combPos[comb] = Memory::Read32(stateAddr + kFieldCombPos + comb * 4);
        frame.combCoef[comb] = Memory::ReadFloat32(stateAddr + kFieldCombCoef + comb * 4);
        if (frame.combLength[comb] == 0 || frame.combPos[comb] >= frame.combLength[comb]) {
            return false;
        }
        const size_t combBytes = static_cast<size_t>(frame.combLength[comb]) * sizeof(float);
        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            frame.comb[channel][comb] = ResolveGuestThreadRange(
                Memory::Read32(stateAddr + kFieldCombLine + channel * 12 + comb * 4), combBytes);
            if (!frame.comb[channel][comb]) {
                return false;
            }
        }
    }

    for (uint32_t allpass = 0; allpass < kAllpasses; ++allpass) {
        frame.allpassLength[allpass] = Memory::Read32(stateAddr + kFieldAllpassLength + allpass * 4);
        frame.allpassPos[allpass] = Memory::Read32(stateAddr + kFieldAllpassPos + allpass * 4);
        if (frame.allpassLength[allpass] == 0 ||
            frame.allpassPos[allpass] >= frame.allpassLength[allpass]) {
            return false;
        }
        const size_t allpassBytes =
            static_cast<size_t>(frame.allpassLength[allpass]) * sizeof(float);
        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            frame.allpass[channel][allpass] = ResolveGuestThreadRange(
                Memory::Read32(stateAddr + kFieldAllpassLine + channel * 8 + allpass * 4),
                allpassBytes);
            if (!frame.allpass[channel][allpass]) {
                return false;
            }
        }
    }

    // The trailing allpass is the one stage whose index and length are per channel;
    // the guest advances it inside the channel loop rather than once per sample.
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        frame.lastApLength[channel] = Memory::Read32(stateAddr + kFieldLastApLength + channel * 4);
        frame.lastApPos[channel] = Memory::Read32(stateAddr + kFieldLastApPos + channel * 4);
        if (frame.lastApLength[channel] == 0 ||
            frame.lastApPos[channel] >= frame.lastApLength[channel]) {
            return false;
        }
        frame.lastAp[channel] = ResolveGuestThreadRange(
            Memory::Read32(stateAddr + kFieldLastApLine + channel * 4),
            static_cast<size_t>(frame.lastApLength[channel]) * sizeof(float));
        if (!frame.lastAp[channel]) {
            return false;
        }
        frame.lastLpfOut[channel] = Memory::ReadFloat32(stateAddr + kFieldLastLpfOut + channel * 4);
    }

    const float one = Memory::ReadFloat32(kOneConstantAddr);
    const float wetConstant = Memory::ReadFloat32(kWetConstantAddr);
    const float mixConstant = Memory::ReadFloat32(kMixConstantAddr);
    frame.zero = Memory::ReadFloat32(kZeroConstantAddr);
    frame.damping = Memory::ReadFloat32(stateAddr + kFieldDamping);
    frame.oneMinusDamping = one - frame.damping;
    frame.wetScale = wetConstant * Memory::ReadFloat32(stateAddr + kFieldWetPreScale);
    frame.mixScale = mixConstant * Memory::ReadFloat32(stateAddr + kFieldMixPreScale);
    frame.allpassCoef = Memory::ReadFloat32(stateAddr + kFieldAllpassCoef);
    frame.mainGain = Memory::ReadFloat32(stateAddr + kFieldMainOutGain);
    frame.auxGain = Memory::ReadFloat32(stateAddr + kFieldAuxOutGain);
    return true;
}

void Render(uint32_t stateAddr, Frame& frame) {
    uint32_t earlyPos[kEarlyTaps];
    for (uint32_t tap = 0; tap < kEarlyTaps; ++tap) {
        earlyPos[tap] = frame.earlyPos[tap];
    }
    uint32_t preDelayPos = frame.preDelayPos;
    uint32_t combPos[kCombs];
    for (uint32_t comb = 0; comb < kCombs; ++comb) {
        combPos[comb] = frame.combPos[comb];
    }
    uint32_t allpassPos[kAllpasses];
    for (uint32_t allpass = 0; allpass < kAllpasses; ++allpass) {
        allpassPos[allpass] = frame.allpassPos[allpass];
    }

    for (uint32_t sample = 0; sample < kSamplesPerFrame; ++sample) {
        const uint32_t frameOffset = sample * 4u;
        float mixed[kChannels];

        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            uint8_t* const mainSlot = frame.main[channel] + frameOffset;
            int32_t rawInput = LoadS32(mainSlot);
            if (frame.hasAuxIn) {
                rawInput = static_cast<int32_t>(
                    static_cast<uint32_t>(rawInput) +
                    static_cast<uint32_t>(LoadS32(frame.auxIn[channel] + frameOffset)));
            }
            const float input = static_cast<float>(rawInput);

            // All three early taps read before the newest sample overwrites the
            // third tap's slot. Every product is its own statement so the host
            // compiler cannot fuse a multiply into the following add.
            uint8_t* const earlyLine = frame.early[channel];
            const float earlyTap0 = LoadFloat(earlyLine + earlyPos[0] * 4u);
            const float earlyTap1 = LoadFloat(earlyLine + earlyPos[1] * 4u);
            const float earlyTap2 = LoadFloat(earlyLine + earlyPos[2] * 4u);
            StoreFloat(earlyLine + earlyPos[2] * 4u, input);
            const float early0 = frame.earlyCoef[0] * earlyTap0;
            const float early1 = frame.earlyCoef[1] * earlyTap1;
            const float early2 = frame.earlyCoef[2] * earlyTap2;
            const float earlySum = early0 + early1;
            const float early = early2 + earlySum;

            // Pure delay, no feedback coefficient.
            float excite = input;
            if (frame.preDelayLength != 0) {
                uint8_t* const preDelaySlot = frame.preDelay[channel] + preDelayPos * 4u;
                excite = LoadFloat(preDelaySlot);
                StoreFloat(preDelaySlot, input);
            }

            // Three combs, all fed the same pre-delay output; their taps sum into
            // the allpass chain.
            float combSum = frame.zero;
            for (uint32_t comb = 0; comb < kCombs; ++comb) {
                uint8_t* const combSlot = frame.comb[channel][comb] + combPos[comb] * 4u;
                const float combTap = LoadFloat(combSlot);
                const float combFeedback = combTap * frame.combCoef[comb];
                combSum = combSum + combTap;
                StoreFloat(combSlot, excite + combFeedback);
            }

            float allpassOut = combSum;
            for (uint32_t allpass = 0; allpass < kAllpasses; ++allpass) {
                uint8_t* const allpassSlot =
                    frame.allpass[channel][allpass] + allpassPos[allpass] * 4u;
                const float allpassTap = LoadFloat(allpassSlot);
                const float allpassFeedback = allpassTap * frame.allpassCoef;
                const float allpassStore = allpassOut + allpassFeedback;
                StoreFloat(allpassSlot, allpassStore);
                const float allpassFeedforward = allpassStore * frame.allpassCoef;
                allpassOut = allpassTap - allpassFeedforward;
            }

            const float dampedOld = frame.damping * frame.lastLpfOut[channel];
            const float dampedNew = frame.oneMinusDamping * allpassOut;
            const float damped = dampedNew + dampedOld;
            frame.lastLpfOut[channel] = damped;

            uint8_t* const lastSlot = frame.lastAp[channel] + frame.lastApPos[channel] * 4u;
            const float lastTap = LoadFloat(lastSlot);
            const float lastFeedback = lastTap * frame.allpassCoef;
            const float lastStore = damped + lastFeedback;
            StoreFloat(lastSlot, lastStore);
            const float lastFeedforward = lastStore * frame.allpassCoef;
            const float lastOut = lastTap - lastFeedforward;

            const uint32_t nextLastPos = frame.lastApPos[channel] + 1u;
            frame.lastApPos[channel] =
                nextLastPos < frame.lastApLength[channel] ? nextLastPos : 0u;

            const float wet = lastOut * frame.wetScale;
            mixed[channel] = wet + early;
        }

        // Each output channel takes the other two through the shared cross-mix.
        const float sum12 = mixed[1] + mixed[2];
        const float sum02 = mixed[0] + mixed[2];
        const float sum01 = mixed[0] + mixed[1];
        const float cross0 = sum12 * frame.mixScale;
        const float cross1 = sum02 * frame.mixScale;
        const float cross2 = sum01 * frame.mixScale;
        const float out[kChannels] = {
            mixed[0] + cross0,
            mixed[1] + cross1,
            mixed[2] + cross2,
        };

        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            const float mainSample = out[channel] * frame.mainGain;
            StoreS32(frame.main[channel] + frameOffset, ConvertToIntegerWord(mainSample));
            if (frame.hasAuxOut) {
                const float auxSample = out[channel] * frame.auxGain;
                StoreS32(frame.auxOut[channel] + frameOffset, ConvertToIntegerWord(auxSample));
            }
        }

        for (uint32_t tap = 0; tap < kEarlyTaps; ++tap) {
            const uint32_t next = earlyPos[tap] + 1u;
            earlyPos[tap] = next < frame.earlyLength ? next : 0u;
        }
        if (frame.preDelayLength != 0) {
            const uint32_t next = preDelayPos + 1u;
            preDelayPos = next < frame.preDelayLength ? next : 0u;
        }
        for (uint32_t comb = 0; comb < kCombs; ++comb) {
            const uint32_t next = combPos[comb] + 1u;
            combPos[comb] = next < frame.combLength[comb] ? next : 0u;
        }
        for (uint32_t allpass = 0; allpass < kAllpasses; ++allpass) {
            const uint32_t next = allpassPos[allpass] + 1u;
            allpassPos[allpass] = next < frame.allpassLength[allpass] ? next : 0u;
        }
    }

    // The guest rewrites these every sample; nothing can observe the intermediate
    // values, so one store per field at the end is equivalent.
    for (uint32_t tap = 0; tap < kEarlyTaps; ++tap) {
        Memory::Write32(stateAddr + kFieldEarlyPos + tap * 4, earlyPos[tap]);
    }
    if (frame.preDelayLength != 0) {
        Memory::Write32(stateAddr + kFieldPreDelayPos, preDelayPos);
    }
    for (uint32_t comb = 0; comb < kCombs; ++comb) {
        Memory::Write32(stateAddr + kFieldCombPos + comb * 4, combPos[comb]);
    }
    for (uint32_t allpass = 0; allpass < kAllpasses; ++allpass) {
        Memory::Write32(stateAddr + kFieldAllpassPos + allpass * 4, allpassPos[allpass]);
    }
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        Memory::Write32(stateAddr + kFieldLastApPos + channel * 4, frame.lastApPos[channel]);
        Memory::WriteFloat32(stateAddr + kFieldLastLpfOut + channel * 4,
                             static_cast<double>(frame.lastLpfOut[channel]));
    }
}

// Every byte this callback may write, so a differential run can snapshot, replay
// and compare it. Ring lines are listed once per channel because the guest gives
// each channel its own buffer.
void CollectWritableRegions(const Frame& frame,
                            std::vector<std::pair<uint8_t*, size_t>>& regions) {
    constexpr size_t kFrameBytes = kSamplesPerFrame * sizeof(int32_t);
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        regions.emplace_back(frame.main[channel], kFrameBytes);
        if (frame.hasAuxOut) {
            regions.emplace_back(frame.auxOut[channel], kFrameBytes);
        }
        regions.emplace_back(frame.early[channel],
                             static_cast<size_t>(frame.earlyLength) * sizeof(float));
        if (frame.preDelayLength != 0) {
            regions.emplace_back(frame.preDelay[channel],
                                 static_cast<size_t>(frame.preDelayLength) * sizeof(float));
        }
        for (uint32_t comb = 0; comb < kCombs; ++comb) {
            regions.emplace_back(frame.comb[channel][comb],
                                 static_cast<size_t>(frame.combLength[comb]) * sizeof(float));
        }
        for (uint32_t allpass = 0; allpass < kAllpasses; ++allpass) {
            regions.emplace_back(frame.allpass[channel][allpass],
                                 static_cast<size_t>(frame.allpassLength[allpass]) * sizeof(float));
        }
        regions.emplace_back(frame.lastAp[channel],
                             static_cast<size_t>(frame.lastApLength[channel]) * sizeof(float));
    }
}

bool VerificationEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("MKW_VERIFY_AXFX_REVERB");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// Runs the translated body and this port over identical state and compares every
// byte either can write, so a wrong field offset surfaces as a loud mismatch
// instead of subtly wrong audio. Validation only; off unless the env var is set.
void RenderVerified(CpuContext* ctx, uint32_t stateAddr, Frame& frame) {
    std::vector<std::pair<uint8_t*, size_t>> regions;
    CollectWritableRegions(frame, regions);

    uint8_t* const stateHost = ResolveGuestThreadRange(stateAddr, kStateStructBytes);
    if (!stateHost) {
        Render(stateAddr, frame);
        return;
    }
    regions.emplace_back(stateHost, kStateStructBytes);

    std::vector<std::vector<uint8_t>> before(regions.size());
    for (size_t i = 0; i < regions.size(); ++i) {
        before[i].assign(regions[i].first, regions[i].first + regions[i].second);
    }

    const CpuContext savedContext = *ctx;
    MKW_GUEST_FUNC(801284B4)(ctx);
    *ctx = savedContext;

    std::vector<std::vector<uint8_t>> expected(regions.size());
    for (size_t i = 0; i < regions.size(); ++i) {
        expected[i].assign(regions[i].first, regions[i].first + regions[i].second);
        std::memcpy(regions[i].first, before[i].data(), before[i].size());
    }

    Render(stateAddr, frame);

    static bool reported = false;
    if (reported) {
        return;
    }
    for (size_t i = 0; i < regions.size(); ++i) {
        if (std::memcmp(regions[i].first, expected[i].data(), expected[i].size()) == 0) {
            continue;
        }
        size_t offset = 0;
        while (offset < expected[i].size() && regions[i].first[offset] == expected[i][offset]) {
            ++offset;
        }
        reported = true;
        RT_LOGF(RT_TAG_AUDIO,
                "AXFXReverbHiExp native output diverges from the translated body: "
                "region %zu of %zu, first differing byte %zu of %zu\n",
                i, regions.size(), offset, expected[i].size());
        break;
    }
}

} // namespace ReverbHi
} // namespace

extern "C" void AXFXReverbStdExpCallback_8012b830(CpuContext* ctx) {
    if (!ctx) {
        return;
    }

    const uint32_t buffersAddr = ctx->gpr[3];
    const uint32_t stateAddr = ctx->gpr[4];

    uint32_t flags = 0;
    try {
        flags = Memory::Read32(stateAddr + ReverbStd::kFieldFlags);
    } catch (const Memory::AccessViolation&) {
        MKW_GUEST_FUNC(8012B830)(ctx);
        return;
    }
    if (flags != 0) {
        // Reset request: the guest clears the "in progress" bit and skips the
        // frame entirely.
        Memory::Write32(stateAddr + ReverbStd::kFieldFlags, flags & ~2u);
        return;
    }

    ReverbStd::Frame frame;
    bool built = false;
    try {
        built = ReverbStd::BuildFrame(buffersAddr, stateAddr, frame);
    } catch (const Memory::AccessViolation&) {
        built = false;
    }
    if (!built) {
        MKW_GUEST_FUNC(8012B830)(ctx);
        return;
    }

    ReverbStd::Render(stateAddr, frame);
}

REGISTER_NATIVE_FUNCTION_AS(0x8012B830, AXFXReverbStdExpCallback_8012b830,
                            "AXFXReverbStdExpCallback_8012b830");

extern "C" void AXFXReverbHiExpCallback_801284b4(CpuContext* ctx) {
    if (!ctx) {
        return;
    }

    const uint32_t buffersAddr = ctx->gpr[3];
    const uint32_t stateAddr = ctx->gpr[4];

    uint32_t flags = 0;
    try {
        flags = Memory::Read32(stateAddr + ReverbHi::kFieldFlags);
    } catch (const Memory::AccessViolation&) {
        MKW_GUEST_FUNC(801284B4)(ctx);
        return;
    }
    if (flags != 0) {
        // Reset request: the guest clears the "in progress" bit and skips the
        // frame entirely.
        Memory::Write32(stateAddr + ReverbHi::kFieldFlags, flags & ~2u);
        return;
    }

    ReverbHi::Frame frame;
    bool built = false;
    try {
        built = ReverbHi::BuildFrame(buffersAddr, stateAddr, frame);
    } catch (const Memory::AccessViolation&) {
        built = false;
    }
    if (!built) {
        MKW_GUEST_FUNC(801284B4)(ctx);
        return;
    }

    if (ReverbHi::VerificationEnabled()) {
        ReverbHi::RenderVerified(ctx, stateAddr, frame);
    } else {
        ReverbHi::Render(stateAddr, frame);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x801284B4, AXFXReverbHiExpCallback_801284b4,
                            "AXFXReverbHiExpCallback_801284b4");
