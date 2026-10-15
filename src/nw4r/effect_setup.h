// SPDX-License-Identifier: MIT

// The effects that a game puts on AX's aux buses through NW4R SND (docs/wsf.md, "Effects"): each one's bus, its class
// and the parameters that the game's code gives the class's SetParam. The archive descriptor carries them for archive
// mode, and the driver's block for game mode.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wsf::nw4r
{

// NW4R SND's effect classes. The parameters are words of the class's parameter structure, in order: floats as their
// bits, and the 2008 reverb's two modes as integers.
enum class EffectType : uint8_t
{
    // FxReverbHi of 2006 and 2007: preDelayTime, fusedTime, coloration, damping, crossTalk, outGain.
    kReverbHi = 1,
    // FxReverbHi of 2008: those six, then earlyMode, preDelayTimeMax, fusedMode, earlyGain, fusedGain.
    kReverbHi2008 = 2,
    // FxChorus: delayTime (ms), depth, rate (Hz).
    kChorus = 3,
    // FxDelay: delay (ms), feedback, outGain.
    kDelay = 4,
    // WarioWare: Smooth Moves' standard reverb: preDelay, time, coloration, damping, mix.
    kReverbStd = 5,
    // FxReverbStd of 2008 (Mario Kart Wii and Wii Music): preDelayTime, fusedTime, coloration, damping, outGain,
    // earlyMode, preDelayTimeMax, fusedMode, earlyGain, fusedGain.
    kReverbStd2008 = 6,
};

// The most words that a parameter structure has.
constexpr int kEffectParamWords = 11;

// The number of words in `type`'s parameter structure, or 0 for an unknown type.
constexpr int EffectParamWords(EffectType type)
{
    switch (type)
    {
    case EffectType::kReverbHi:
        return 6;
    case EffectType::kReverbHi2008:
        return 11;
    case EffectType::kChorus:
    case EffectType::kDelay:
        return 3;
    case EffectType::kReverbStd:
        return 5;
    case EffectType::kReverbStd2008:
        return 10;
    }

    return 0;
}

using EffectParams = std::array<uint32_t, kEffectParamWords>;

// One effect on a bus. With no presets, the effect is set up as the sound starts, with `params`, for the sounds in
// `sounds`. An effect that lists no sounds is set up for every sound that no other effect without presets on its bus
// lists. A game that changes a bus's effect from scene to scene (Mario Kart Wii, Wii Music) has an effect for each
// setting, with the sounds of the scenes that use it.
//
// With presets, the bus starts without the effect. The sequences of `sounds` then pick a preset (WarioWare: Smooth
// Moves, which reads the variables of its music): in each AX frame, a value from 1 to the number of presets in the
// sequence's local variable 0, then 1, sets the preset with that number and appends the effect again, and 0 takes the
// bus's effects away. Other values change nothing. Other sounds leave the bus without the effect.
struct EffectSetup
{
    int bus = 0; // 0 A, 1 B, 2 C
    EffectType type = EffectType::kReverbHi;
    EffectParams params{};
    std::vector<EffectParams> presets;
    std::vector<uint32_t> sounds; // indices in the archive's sound table
};

// A sound index that no effect lists. A sound that no effect lists gets the effects that EffectsFor gives for it.
constexpr uint32_t kUnlistedSound = 0xffffffff;

// Whether `setup` lists `sound`.
inline bool ListsSound(const EffectSetup& setup, uint32_t sound)
{
    return std::find(setup.sounds.begin(), setup.sounds.end(), sound) != setup.sounds.end();
}

// Whether the variables of `sound` pick the presets of an effect in `setups`.
inline bool FollowsVariables(const std::vector<EffectSetup>& setups, uint32_t sound)
{
    const auto picks_presets = [sound](const EffectSetup& setup)
    {
        return !setup.presets.empty() && ListsSound(setup, sound);
    };

    return std::any_of(setups.begin(), setups.end(), picks_presets);
}

// Whether the game sets up `setups[index]` for `sound`. An effect with presets is always set up. The sounds that it
// lists are the ones whose variables pick a preset. An effect without presets is set up for the sounds that it lists.
// One that lists none is set up for every sound that no other effect without presets on its bus lists.
inline bool SetUpFor(const std::vector<EffectSetup>& setups, std::size_t index, uint32_t sound)
{
    const EffectSetup& setup = setups[index];
    if (!setup.presets.empty())
    {
        return true;
    }

    if (!setup.sounds.empty())
    {
        return ListsSound(setup, sound);
    }

    const auto takes_sound = [&setup, sound](const EffectSetup& other)
    {
        return other.presets.empty() && other.bus == setup.bus && ListsSound(other, sound);
    };

    return std::none_of(setups.begin(), setups.end(), takes_sound);
}

// The effects of `setups` that the game sets up for `sound` (see SetUpFor), in order.
inline std::vector<EffectSetup> EffectsFor(const std::vector<EffectSetup>& setups, uint32_t sound)
{
    std::vector<EffectSetup> found;
    for (std::size_t i = 0; i < setups.size(); i++)
    {
        if (SetUpFor(setups, i, sound))
        {
            found.push_back(setups[i]);
        }
    }

    return found;
}

// A send to an aux bus that the game sets on a sound (BasicSound::SetFxSend, Animal Crossing: City Folk).
struct SoundSend
{
    uint32_t sound = 0; // the index in the archive's sound table
    int bus = 0;
    float send = 0.0f;
};

// The sends that the game sets on `sound`, by bus: 0 for a bus without one. Of two sends to one bus, the later counts.
inline std::array<float, 3> SendsOf(const std::vector<SoundSend>& sends, uint32_t sound)
{
    std::array<float, 3> found{};
    for (const SoundSend& send : sends)
    {
        if (send.sound == sound && send.bus >= 0 && send.bus < 3)
        {
            found[static_cast<std::size_t>(send.bus)] = send.send;
        }
    }

    return found;
}

} // namespace wsf::nw4r
