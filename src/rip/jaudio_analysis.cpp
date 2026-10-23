// SPDX-License-Identifier: MIT

// Game mode's length analysis and wave discovery for a JAudio game (see jaudio_analysis.h).

#include "rip/jaudio_analysis.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/dsp/memory.h"
#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/game_lengths.h"
#include "ntdlib/ppc/cpu.h"
#include "xsflib/bytes.h"

#include "wii/system.h"
#include "wsf/format.h"
#include "wsf/player.h"

namespace wsf::rip
{
namespace
{

// The names of the set's files, for the player's reader.
constexpr char kLibName[] = "analysis.wsflib";
constexpr char kMiniName[] = "analysis.miniwsf";

// The most levels of parents that the test of a track's sound climbs.
constexpr int kMaxTrackDepth = 16;

// The CPU for ntdlib's analysis: the registers, and the watch of the game's code. The watch leaves out the events at
// the places for which `skip` returns true. The registers keep the name that ntdlib::ppc::Cpu gives them. The analysis
// reads them by that name.
class AnalysisCpu
{
public:
    AnalysisCpu(ntdlib::ppc::Cpu& cpu, std::function<bool(uint32_t pc)> skip)
        : state_(cpu.state_), cpu_(cpu), skip_(std::move(skip))
    {
    }

    void Watch(std::vector<uint32_t> pcs, std::function<void(uint32_t pc)> on_watch)
    {
        cpu_.Watch(std::move(pcs),
                   [this, on_watch = std::move(on_watch)](uint32_t pc)
                   {
                       if (!skip_(pc))
                       {
                           on_watch(pc);
                       }
                   });
    }

    ntdlib::ppc::State& state_;

private:
    ntdlib::ppc::Cpu& cpu_;
    std::function<bool(uint32_t pc)> skip_;
};

// WSF's player as the machine that ntdlib's analysis watches: its output, the emulated Wii's memory at CPU addresses,
// and its CPU.
class Machine
{
public:
    Machine(Player& player, wii::System& system, const JAudioProfile& profile)
        : player_(player),
          system_(system),
          profile_(profile),
          cpu_(system.Cpu(), [this](uint32_t pc) { return OnBackgroundTrack(pc); })
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

    AnalysisCpu& Cpu()
    {
        return cpu_;
    }

private:
    // Whether the watched instruction at `pc` parses a track of the sound that the driver plays beside the ripped one
    // (see JAudioProfile::background_sound). A track belongs to that sound if it or one of its parents is the sound's
    // root track. The register that holds the track is the one that ntdlib's analysis reads.
    bool OnBackgroundTrack(uint32_t pc)
    {
        const ntdlib::jaudio::GameWatch& watch = profile_.watch;
        const bool conditional = watch.conditional_jump != 0 && pc == watch.conditional_jump;
        if (profile_.background_sound < 0 || (pc != watch.parse_command && pc != watch.jump_handler && !conditional))
        {
            return false;
        }

        const uint32_t sound = Word(kJAudioZ2Address + ntdlib::flipper::z2::kProfileOffset +
                                    4 * static_cast<uint32_t>(profile_.background_sound));
        if (sound == 0 || Word(sound + watch.vtable_offset) != watch.sequence_vtable)
        {
            return false;
        }

        const uint32_t root = sound + watch.sequence_track_offset;
        const uint32_t reg = pc == watch.parse_command ? watch.command_track_register
                             : conditional             ? watch.conditional_track_register
                                                       : watch.jump_track_register;
        uint32_t track = system_.Cpu().state_.gpr[reg];
        for (int level = 0; track != 0 && level < kMaxTrackDepth; level++)
        {
            if (track == root)
            {
                return true;
            }

            track = Word(track + profile_.track_parent_offset);
        }

        return false;
    }

    Player& player_;
    wii::System& system_;
    const JAudioProfile& profile_;
    AnalysisCpu cpu_;
};

// The analysis's rules at WSF's output rate.
ntdlib::jaudio::GameLengthRules JAudioLengthRules()
{
    ntdlib::jaudio::GameLengthRules rules;
    rules.sample_rate = Player::kSampleRate;

    return rules;
}

// Runs ntdlib's analysis of the game's code (see AnalyzeJAudioSound) on the set of `library` and a mini for sound
// `sound_id`, with `waves` loaded. With `lookups_sound`, each lookup of a wave counts as a sound (see FindJAudioWaves).
std::optional<xsflib::rip::Timing> PlayAndWatch(const std::vector<uint8_t>& library, const JAudioProfile& profile,
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

    Machine machine(player, *player.GameSystem(), profile);
    // For a JAudio1 game, the sound's ID distinguishes a sequence from an effect.
    return ntdlib::jaudio::AnalyzeGameSound(machine, kJAudioZ2Address + ntdlib::flipper::z2::kHandleOffset,
                                            WatchOf(profile, sound_id), JAudioLengthRules(), found, lookups_sound,
                                            sound_id);
}

} // namespace

std::optional<xsflib::rip::Timing> AnalyzeJAudioSound(const std::vector<uint8_t>& library, const JAudioProfile& profile,
                                                      uint32_t sound_id, std::span<const uint32_t> waves,
                                                      std::set<ntdlib::jaudio::WaveUse>* found,
                                                      std::string* start_error)
{
    return PlayAndWatch(library, profile, sound_id, waves, found, false, start_error);
}

bool FindJAudioWaves(const std::vector<uint8_t>& library, const JAudioProfile& profile, uint32_t sound_id,
                     std::set<ntdlib::jaudio::WaveUse>& found)
{
    return PlayAndWatch(library, profile, sound_id, {}, &found, true, nullptr).has_value();
}

} // namespace wsf::rip
