#pragma once

// Wire format of voice chat's own UDP packets. These never travel on the
// game's GT2 socket; they go between voice sockets only (see voice_network.h).
//
//   magic "WCVC" (4) | version (1) | type (1) | sequence (2, BE) |
//   sender profile id (4, BE) | payload
//
//   Hello     payload: sender's millisecond clock (4, BE)
//   HelloAck  payload: the clock value from the Hello being answered (4, BE)
//   Voice     payload: one Opus frame
//   Bye       no payload; the sender left voice chat

#include "voice/voice_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Voice {

inline constexpr std::array<uint8_t, 4> kPacketMagic = {'W', 'C', 'V', 'C'};
inline constexpr uint8_t kProtocolVersion = 1;
inline constexpr size_t kPacketHeaderSize = 12;
inline constexpr size_t kMaxPacketSize = kPacketHeaderSize + kMaxPacketBytes;

enum class PacketType : uint8_t {
    Hello = 1,
    HelloAck = 2,
    Voice = 3,
    Bye = 4,
};

struct PacketView {
    PacketType type = PacketType::Hello;
    uint16_t sequence = 0;
    uint32_t senderProfileId = 0;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
};

inline std::vector<uint8_t> EncodePacket(PacketType type, uint16_t sequence, uint32_t senderProfileId,
                                         const uint8_t* payload, size_t payloadSize) {
    std::vector<uint8_t> out(kPacketHeaderSize + payloadSize);
    std::copy(kPacketMagic.begin(), kPacketMagic.end(), out.begin());
    out[4] = kProtocolVersion;
    out[5] = static_cast<uint8_t>(type);
    out[6] = static_cast<uint8_t>(sequence >> 8);
    out[7] = static_cast<uint8_t>(sequence);
    for (int i = 0; i < 4; ++i) {
        out[8 + i] = static_cast<uint8_t>(senderProfileId >> (24 - 8 * i));
    }
    if (payloadSize != 0) {
        std::copy(payload, payload + payloadSize, out.begin() + kPacketHeaderSize);
    }
    return out;
}

inline std::vector<uint8_t> EncodeClockPacket(PacketType type, uint16_t sequence, uint32_t senderProfileId,
                                              uint32_t clock) {
    const uint8_t payload[4] = {static_cast<uint8_t>(clock >> 24), static_cast<uint8_t>(clock >> 16),
                                static_cast<uint8_t>(clock >> 8), static_cast<uint8_t>(clock)};
    return EncodePacket(type, sequence, senderProfileId, payload, sizeof(payload));
}

inline uint32_t ReadClock(const PacketView& packet) {
    if (packet.payloadSize < 4) {
        return 0;
    }
    const uint8_t* p = packet.payload;
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

// Rejects anything that is not a well-formed packet of a version and type this
// build understands; nothing past the header is trusted until then.
inline std::optional<PacketView> DecodePacket(const uint8_t* data, size_t size) {
    if (!data || size < kPacketHeaderSize || size > kMaxPacketSize ||
        !std::equal(kPacketMagic.begin(), kPacketMagic.end(), data) || data[4] != kProtocolVersion) {
        return std::nullopt;
    }
    const uint8_t type = data[5];
    if (type < static_cast<uint8_t>(PacketType::Hello) || type > static_cast<uint8_t>(PacketType::Bye)) {
        return std::nullopt;
    }
    PacketView view;
    view.type = static_cast<PacketType>(type);
    view.sequence = static_cast<uint16_t>((data[6] << 8) | data[7]);
    view.senderProfileId = (uint32_t{data[8]} << 24) | (uint32_t{data[9]} << 16) | (uint32_t{data[10]} << 8) | data[11];
    view.payload = data + kPacketHeaderSize;
    view.payloadSize = size - kPacketHeaderSize;
    if (view.type == PacketType::Voice && view.payloadSize == 0) {
        return std::nullopt;
    }
    return view;
}

}  // namespace Voice
