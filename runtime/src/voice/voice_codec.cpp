#include "voice/voice_codec.h"

#include <opus.h>

#include <algorithm>

namespace Voice {

Encoder::~Encoder() {
    if (m_encoder) {
        opus_encoder_destroy(m_encoder);
    }
}

bool Encoder::Init(int bitrate) {
    if (m_encoder) {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }
    int error = OPUS_OK;
    m_encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !m_encoder) {
        m_encoder = nullptr;
        return false;
    }
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    // Mid complexity: ample quality for speech at a fraction of the top setting's CPU.
    opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(5));
    // In-band FEC lets the receiver rebuild a single lost frame from the next one.
    opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1));
    return true;
}

int Encoder::Encode(const float* pcm, uint8_t* out, int maxBytes) {
    if (!m_encoder) {
        return OPUS_INVALID_STATE;
    }
    return opus_encode_float(m_encoder, pcm, kFrameSamples, out, maxBytes);
}

Decoder::~Decoder() {
    if (m_decoder) {
        opus_decoder_destroy(m_decoder);
    }
}

bool Decoder::Init() {
    if (m_decoder) {
        opus_decoder_destroy(m_decoder);
        m_decoder = nullptr;
    }
    int error = OPUS_OK;
    m_decoder = opus_decoder_create(kSampleRate, 1, &error);
    if (error != OPUS_OK || !m_decoder) {
        m_decoder = nullptr;
        return false;
    }
    return true;
}

namespace {

bool FinishDecode(int decoded, float* out) {
    if (decoded == kFrameSamples) {
        return true;
    }
    std::fill(out, out + kFrameSamples, 0.0f);
    return false;
}

}  // namespace

bool Decoder::Decode(const uint8_t* data, size_t size, float* out) {
    if (!m_decoder || !data || size == 0 || size > static_cast<size_t>(kMaxPacketBytes)) {
        return FinishDecode(-1, out);
    }
    return FinishDecode(opus_decode_float(m_decoder, data, static_cast<opus_int32>(size), out, kFrameSamples, 0), out);
}

bool Decoder::DecodeFec(const uint8_t* nextData, size_t nextSize, float* out) {
    if (!m_decoder || !nextData || nextSize == 0 || nextSize > static_cast<size_t>(kMaxPacketBytes)) {
        return FinishDecode(-1, out);
    }
    return FinishDecode(opus_decode_float(m_decoder, nextData, static_cast<opus_int32>(nextSize), out,
                                    kFrameSamples, 1),
                  out);
}

bool Decoder::Conceal(float* out) {
    if (!m_decoder) {
        return FinishDecode(-1, out);
    }
    return FinishDecode(opus_decode_float(m_decoder, nullptr, 0, out, kFrameSamples, 0), out);
}

}  // namespace Voice
