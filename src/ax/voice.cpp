// SPDX-License-Identifier: MIT

#include "ax/voice.h"

#include <cmath>
#include <cstdint>

#include "ax/pb.h"

namespace wsf::ax
{

void Voice::SetState(VoiceState state)
{
    if (pb.state == static_cast<uint16_t>(state))
    {
        return;
    }

    pb.state = static_cast<uint16_t>(state);
    sync |= sync::kState;
    if (state == VoiceState::kStop)
    {
        depop = true;
    }
}

void Voice::SetAddr(const PbAddr& address)
{
    pb.addr = address;

    // PCM has no frames to decode. The decoder gets no coefficients and a gain that scales the samples to 16 bits.
    switch (static_cast<SampleFormat>(address.format))
    {
    case SampleFormat::kPcm16:
        pb.adpcm = PbAdpcm{};
        pb.adpcm.gain = 0x800;
        break;

    case SampleFormat::kPcm8:
        pb.adpcm = PbAdpcm{};
        pb.adpcm.gain = 0x100;
        break;

    default:
        break;
    }

    sync &= ~(sync::kLoopFlag | sync::kLoopAddr | sync::kEndAddr | sync::kCurrAddr);
    sync |= sync::kAddr | sync::kAdpcm;
}

LpfCoefs CalcLpfCoefs(uint16_t freq)
{
    // The game computes cos and sqrt in double precision (MSL's cos and sqrt) and rounds each result to single
    // precision. The arithmetic around those calls is in single precision. 6.2831855f is the float nearest to 2 pi.
    const float angle = 6.2831855f * static_cast<float>(freq) / static_cast<float>(kSampleRate);
    const float c = 2.0f - static_cast<float>(std::cos(static_cast<double>(angle)));
    const float root = static_cast<float>(std::sqrt(static_cast<double>(c * c - 1.0f)));
    const auto b0 = static_cast<uint16_t>(static_cast<int32_t>(32768.0f * -(root - c)));

    return {static_cast<uint16_t>(0x7fff - b0), b0};
}

} // namespace wsf::ax
