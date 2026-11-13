#pragma once

// Per-speaker playout buffer. Packets arrive out of order, late, duplicated or
// not at all; playout pulls exactly one frame every 20 ms. The buffer waits for
// a few frames before starting a talk spurt (absorbing network jitter), reports
// each gap so the decoder can use FEC or concealment, and drops the oldest
// frames if latency builds up. Single-threaded.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Voice {

class JitterBuffer {
public:
    enum class Kind {
        Silent,  // nothing to play: not started, or the speaker stopped talking
        Frame,   // play `packet`
        Lost,    // the frame is missing; `next` (may be null) carries its FEC
    };
    struct Output {
        Kind kind = Kind::Silent;
        const std::vector<uint8_t>* packet = nullptr;
        const std::vector<uint8_t>* next = nullptr;
    };

    static constexpr int kSlots = 64;
    // 3 frames (60 ms) before a talk spurt starts playing.
    static constexpr int kStartDepth = 3;
    // Beyond 12 frames (240 ms) the oldest are discarded to bound latency.
    static constexpr int kMaxDepth = 12;
    // A run this long of missing frames is treated as the speaker going away.
    static constexpr int kMaxMissStreak = 10;

    void Push(uint16_t seq, const uint8_t* data, size_t size);
    // Call once per 20 ms tick. The returned pointers stay valid until the next Pop/Push/Reset.
    Output Pop();
    void Reset();

    int Depth() const { return m_count; }
    bool Playing() const { return m_playing; }

private:
    struct Slot {
        bool filled = false;
        uint16_t seq = 0;
        std::vector<uint8_t> data;
    };

    Slot& SlotFor(uint16_t seq) { return m_slots[seq % kSlots]; }
    bool Has(uint16_t seq) const {
        const Slot& slot = m_slots[seq % kSlots];
        return slot.filled && slot.seq == seq;
    }
    void Clear(Slot& slot);
    void DropOldest();

    std::array<Slot, kSlots> m_slots{};
    std::vector<uint8_t> m_current;
    bool m_anchored = false;  // m_next is meaningful
    bool m_playing = false;
    uint16_t m_next = 0;      // sequence number the next Pop plays
    int m_count = 0;          // filled slots
    int m_missStreak = 0;
};

}  // namespace Voice
