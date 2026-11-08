#include "voice/voice_jitter_buffer.h"

#include <algorithm>

namespace Voice {

namespace {

// Signed distance from `from` to `to` in 16-bit sequence space.
int VoiceSeqDelta(uint16_t to, uint16_t from) {
    return static_cast<int16_t>(static_cast<uint16_t>(to - from));
}

}  // namespace

void JitterBuffer::Clear(Slot& slot) {
    if (slot.filled) {
        slot.filled = false;
        --m_count;
    }
}

void JitterBuffer::Reset() {
    for (Slot& slot : m_slots) {
        slot.filled = false;
    }
    m_count = 0;
    m_anchored = false;
    m_playing = false;
    m_missStreak = 0;
}

void JitterBuffer::Push(uint16_t seq, const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return;
    }
    if (!m_anchored) {
        m_anchored = true;
        m_next = seq;
    }
    const int delta = VoiceSeqDelta(seq, m_next);
    if (delta < 0) {
        if (m_playing || delta <= -kSlots) {
            return;  // too late: that frame's playout time has passed
        }
        // Not playing yet: an earlier packet arrived after a later one, so the
        // spurt really starts here. Move the start back unless that would
        // overflow the window.
        int newest = 0;
        for (const Slot& slot : m_slots) {
            if (slot.filled) {
                newest = std::max(newest, VoiceSeqDelta(slot.seq, seq));
            }
        }
        if (newest >= kSlots) {
            return;
        }
        m_next = seq;
    } else if (delta >= kSlots) {
        // Far ahead of anything we can hold: the sender restarted its sequence.
        Reset();
        m_anchored = true;
        m_next = seq;
    }
    Slot& slot = SlotFor(seq);
    if (slot.filled && slot.seq == seq) {
        return;  // duplicate
    }
    if (slot.filled) {
        --m_count;  // stale occupant from a previous lap of the ring
    }
    slot.filled = true;
    slot.seq = seq;
    slot.data.assign(data, data + size);
    ++m_count;
    while (m_count > kMaxDepth) {
        DropOldest();
    }
}

void JitterBuffer::DropOldest() {
    for (int i = 0; i < kSlots && m_count > 0; ++i) {
        Slot& slot = SlotFor(m_next);
        const bool had = slot.filled && slot.seq == m_next;
        if (had) {
            Clear(slot);
        }
        ++m_next;
        if (had) {
            return;
        }
    }
}

JitterBuffer::Output JitterBuffer::Pop() {
    Output output;
    if (!m_playing) {
        if (m_count < kStartDepth) {
            return output;
        }
        m_playing = true;
        m_missStreak = 0;
    }
    if (m_count == 0) {
        // The speaker stopped (push-to-talk released, or DTX silence). The next
        // packet starts a new spurt with fresh buffering.
        m_playing = false;
        m_anchored = false;
        return output;
    }
    Slot& slot = SlotFor(m_next);
    if (slot.filled && slot.seq == m_next) {
        m_current.swap(slot.data);
        Clear(slot);
        ++m_next;
        m_missStreak = 0;
        output.kind = Kind::Frame;
        output.packet = &m_current;
        return output;
    }
    output.kind = Kind::Lost;
    const uint16_t following = static_cast<uint16_t>(m_next + 1);
    if (Has(following)) {
        output.next = &SlotFor(following).data;
    }
    ++m_next;
    if (++m_missStreak > kMaxMissStreak) {
        Reset();
        output = Output{};
    }
    return output;
}

}  // namespace Voice
