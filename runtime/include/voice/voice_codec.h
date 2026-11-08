#pragma once

// Thin RAII wrappers around libopus for voice chat. One format everywhere:
// 48 kHz mono float, 20 ms frames. Neither class is thread-safe; each is owned
// by a single thread (the voice worker).

#include <cstddef>
#include <cstdint>

struct OpusEncoder;
struct OpusDecoder;

namespace Voice {

inline constexpr int kSampleRate = 48000;
inline constexpr int kFrameSamples = 960;  // 20 ms
inline constexpr int kFrameMilliseconds = 20;
// Largest packet the encoder may produce. 24 kb/s averages ~60 bytes a frame;
// the cap only bounds bursts and keeps every packet well under a UDP MTU.
inline constexpr int kMaxPacketBytes = 400;
inline constexpr int kDefaultBitrate = 24000;

class Encoder {
public:
    Encoder() = default;
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    bool Init(int bitrate = kDefaultBitrate);
    // Encodes exactly kFrameSamples samples. Returns the packet size, or a
    // negative value on error. With DTX on, silence yields packets of 1-2 bytes.
    int Encode(const float* pcm, uint8_t* out, int maxBytes);

private:
    OpusEncoder* m_encoder = nullptr;
};

class Decoder {
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool Init();
    // Each call writes exactly kFrameSamples samples and returns false on a
    // decoder error (out is then silence).
    bool Decode(const uint8_t* data, size_t size, float* out);
    // Recovers a lost frame from the in-band FEC carried by the next packet.
    bool DecodeFec(const uint8_t* nextData, size_t nextSize, float* out);
    // Packet loss concealment when nothing about the frame is known.
    bool Conceal(float* out);

private:
    OpusDecoder* m_decoder = nullptr;
};

}  // namespace Voice
