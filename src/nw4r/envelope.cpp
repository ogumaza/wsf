// SPDX-License-Identifier: MIT

#include "nw4r/envelope.h"

#include <cstdint>

#include "nw4r/tables.h"

namespace wsf::nw4r
{
namespace
{

constexpr int kSineQuarter = 32; // entries in a quarter of the LFO's sine, less the last

// A sine of the LFO's phase in 128ths of a turn, from -127 to 127 (Lfo::GetSinIdx, inlined in Lfo::GetValue).
int Sine(int index)
{
    if (index < kSineQuarter)
    {
        return tables::kLfoSine[index];
    }
    if (index < kSineQuarter * 2)
    {
        return tables::kLfoSine[kSineQuarter - (index - kSineQuarter)];
    }
    if (index < kSineQuarter * 3)
    {
        return -tables::kLfoSine[index - kSineQuarter * 2];
    }

    return -tables::kLfoSine[kSineQuarter - (index - kSineQuarter * 3)];
}

} // namespace

void Envelope::Init(float db)
{
    SetAttack(127);
    SetDecay(127);
    SetSustain(127);
    SetRelease(127);
    hold_ = 0;
    Reset(db);
}

void Envelope::Reset(float db)
{
    value_ = db * 10.0f;
    status_ = Status::kAttack;
}

float Envelope::Value() const
{
    if (status_ == Status::kAttack && attack_ == 0.0f)
    {
        return 0.0f;
    }

    return value_ / 10.0f;
}

void Envelope::Update(int msec)
{
    // The attack multiplies the level towards 0 dB each millisecond. It gives way to the hold within 1/32 of a 0.1 dB
    // step. The rest of the frame passes at 0 dB. The hold counts its milliseconds down and passes what's left of the
    // frame to the decay. The decay falls at its rate to the sustain level. The release falls at its rate without end.
    switch (status_)
    {
    case Status::kAttack:
        for (; msec > 0; --msec)
        {
            value_ *= attack_;
            if (value_ > -(1.0f / 32.0f))
            {
                value_ = 0.0f;
                status_ = Status::kHold;
                hold_left_ = hold_;
                break;
            }
        }

        break;
    case Status::kHold:
        if (msec < hold_left_)
        {
            hold_left_ = static_cast<uint16_t>(hold_left_ - msec);
            break;
        }

        msec -= hold_left_;
        hold_left_ = 0;
        status_ = Status::kDecay;
        [[fallthrough]];

    case Status::kDecay:
        {
            const float sustain = static_cast<float>(tables::kDecibelSquare[sustain_]);
            value_ -= decay_ * static_cast<float>(msec);
            if (value_ > sustain)
            {
                break;
            }

            value_ = sustain;
            status_ = Status::kSustain;
            break;
        }

    case Status::kSustain:
        break;
    case Status::kRelease:
        value_ -= release_ * static_cast<float>(msec);
        break;
    }
}

void Envelope::SetAttack(int attack)
{
    attack_ = tables::kAttack[attack];
}

void Envelope::SetDecay(int decay)
{
    decay_ = CalcRelease(decay);
}

void Envelope::SetSustain(int sustain)
{
    sustain_ = static_cast<uint8_t>(sustain);
}

void Envelope::SetHold(int hold)
{
    hold_ = static_cast<uint16_t>((hold + 1) * (hold + 1) / 4);
}

void Envelope::SetRelease(int release)
{
    release_ = CalcRelease(release);
}

float Envelope::CalcRelease(int release)
{
    if (release == 127)
    {
        return 65535.0f;
    }
    if (release == 126)
    {
        return 24.0f;
    }
    if (release < 50)
    {
        return static_cast<float>(release * 2 + 1) * (1.0f / 128.0f) / 5.0f;
    }

    return 60.0f / static_cast<float>(126 - release) / 5.0f;
}

void Lfo::Reset()
{
    counter_ = 0.0f;
    delay_counter_ = 0;
}

void Lfo::Update(int msec)
{
    // The delay runs out first. Whatever is left of `msec` advances the phase. The phase wraps at 1.
    uint32_t time = static_cast<uint32_t>(msec);
    if (delay_counter_ < param_.delay)
    {
        if (delay_counter_ + time <= param_.delay)
        {
            delay_counter_ += time;

            return;
        }

        time -= param_.delay - delay_counter_;
        delay_counter_ = param_.delay;
    }

    counter_ += param_.speed * static_cast<float>(static_cast<int>(time)) / 1000.0f;
    counter_ -= static_cast<float>(static_cast<int>(counter_));
}

float Lfo::Value() const
{
    if (param_.depth == 0.0f || delay_counter_ < param_.delay)
    {
        return 0.0f;
    }

    const int index = static_cast<int>(4.0f * (32.0f * counter_));
    float value = static_cast<float>(Sine(index)) / 127.0f;
    value *= param_.depth;
    value *= static_cast<float>(param_.range);

    return value;
}

} // namespace wsf::nw4r
