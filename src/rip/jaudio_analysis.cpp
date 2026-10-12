// SPDX-License-Identifier: MIT

#include "rip/jaudio_analysis.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dsp/memory.h"
#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/game_lengths.h"
#include "xsflib/bytes.h"

#include "wii/system.h"
#include "wsf/player.h"

namespace wsf::rip
{
namespace
{

// The names of the set's files, for the player's reader.
constexpr char kLibName[] = "analysis.wsflib";
constexpr char kMiniName[] = "analysis.miniwsf";

// WSF's player as the machine that ntdlib's analysis watches: its output, the emulated Wii's memory at CPU addresses,
// and its CPU.
class Machine
{
public:
    Machine(Player& player, wii::System& system) : player_(player), system_(system)
    {
    }

    std::size_t Render(int16_t* out, std::size_t frames)
    {
        return player_.Render(out, frames);
    }

    uint32_t Word(uint32_t address)
    {
        const uint8_t* p = system_.Memory().At(ntdlib::dsp::ToPhysical(address), 4);
        return p ? xsflib::LoadBe32(p) : 0;
    }

    std::optional<uint8_t> Byte(uint32_t address)
    {
        const uint8_t* p = system_.Memory().At(ntdlib::dsp::ToPhysical(address), 1);
        return p ? std::optional<uint8_t>(*p) : std::nullopt;
    }

    ntdlib::ppc::Cpu& Cpu()
    {
        return system_.Cpu();
    }

private:
    Player& player_;
    wii::System& system_;
};

// The analysis's rules at WSF's output rate.
ntdlib::jaudio::GameLengthRules JAudioLengthRules()
{
    ntdlib::jaudio::GameLengthRules rules;
    rules.sample_rate = Player::kSampleRate;

    return rules;
}

} // namespace

std::optional<xsflib::rip::Timing> AnalyzeJAudioSound(const std::vector<uint8_t>& library, const JAudioProfile& profile,
                                                      uint32_t sound_id, std::span<const uint32_t> waves,
                                                      std::set<ntdlib::jaudio::WaveUse>* found, bool lookups_sound,
                                                      std::string* start_error)
{
    const std::map<std::string, std::vector<uint8_t>> files = {
        {kLibName, library},
        {kMiniName, BuildJAudioMini(sound_id, waves, kLibName, {})},
    };
    const FileReader reader = [&files](const std::string& path, std::vector<uint8_t>& data)
    {
        const auto file = files.find(path);
        if (file == files.end())
        {
            return false;
        }

        data = file->second;

        return true;
    };

    Player player;
    if (!player.Load(kMiniName, reader) || !player.Start() || !player.GameSystem())
    {
        if (start_error)
        {
            *start_error = player.Error();
        }

        return std::nullopt;
    }

    Machine machine(player, *player.GameSystem());
    return ntdlib::jaudio::AnalyzeGameSound(machine, kJAudioZ2Address + ntdlib::flipper::z2::kHandleOffset,
                                            profile.watch, JAudioLengthRules(), found, lookups_sound);
}

} // namespace wsf::rip
