// SPDX-License-Identifier: MIT

// NW4R SND's envelope (EnvGenerator) and LFO (Lfo) in Wii Sports' main.dol (see util.h).

#pragma once

#include <cstdint>

namespace wsf::nw4r
{

// An attack, hold, decay, sustain and release envelope, in 0.1 dB steps from 0 dB down (EnvGenerator). The hold came
// with the builds of 2008 (Mario Kart Wii: EnvGenerator::Update, 0x80091700). Older builds have no hold. The model
// gives them a hold of 0. Their decay then follows the attack directly.
class Envelope
{
public:
    enum class Status
    {
        kAttack,
        kHold,
        kDecay,
        kSustain,
        kRelease,
    };

    // The level a new envelope starts from: -90.4 dB, the table's quietest (EnvGenerator::VOLUME_INIT).
    static constexpr float kInitialDb = -90.4f;

    // Sets every rate to 127 and starts the attack from `db` (EnvGenerator::Init, 0x80038f5c).
    void Init(float db = kInitialDb);

    // Starts the attack again from `db` (0x80038f98).
    void Reset(float db = kInitialDb);

    // The level in dB, or 0 while an instant attack (rate 127) hasn't run yet (0x80038fb0).
    float Value() const;

    // Advances the envelope by `msec` milliseconds (0x80038fdc).
    void Update(int msec);

    void SetAttack(int attack);   // 0x80039100
    void SetDecay(int decay);     // 0x80039118
    void SetSustain(int sustain); // 0x800391b8
    void SetRelease(int release); // 0x800391c0

    // SetHold (Mario Kart Wii, 0x80091880): a hold of (hold + 1)^2 / 4 milliseconds at the attack's peak.
    void SetHold(int hold);

    void SetStatus(Status status)
    {
        status_ = status;
    }

    Status GetStatus() const
    {
        return status_;
    }

private:
    // A decay or release rate in 0.1 dB per millisecond (EnvGenerator::CalcRelease, inlined in SetDecay and
    // SetRelease).
    static float CalcRelease(int release);

    Status status_ = Status::kAttack; // EnvGenerator + 0x0
    float value_ = 0;                 // + 0x4, in 0.1 dB
    float decay_ = 0;                 // + 0x8
    float release_ = 0;               // + 0xc
    float attack_ = 0;                // + 0x10, a multiplier for each millisecond
    uint8_t sustain_ = 127;           // + 0x14
    uint16_t hold_ = 0;               // in milliseconds (2008 builds)
    uint16_t hold_left_ = 0;          // of the current hold
};

// The LFO's settings (LfoParam): its depth (0..1), speed in Hz, delay in milliseconds and range.
struct LfoParam
{
    float depth = 0.0f;  // + 0x0
    float speed = 6.25f; // + 0x4
    uint32_t delay = 0;  // + 0x8
    uint8_t range = 1;   // + 0xc

    bool operator==(const LfoParam&) const = default;
};

// A sine LFO (Lfo).
class Lfo
{
public:
    // Starts the LFO from its beginning (0x8003b26c).
    void Reset();

    // Advances the LFO by `msec` milliseconds, past its delay first (0x8003b280).
    void Update(int msec);

    // Its current value, from -range * depth to range * depth (0x8003b31c).
    float Value() const;

    void SetParam(const LfoParam& param)
    {
        param_ = param;
    }

private:
    LfoParam param_;             // Lfo + 0x0
    uint32_t delay_counter_ = 0; // + 0x10
    float counter_ = 0.0f;       // + 0x14, the phase from 0 to 1
};

} // namespace wsf::nw4r
