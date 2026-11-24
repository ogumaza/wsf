// SPDX-License-Identifier: MIT

// Game mode's length analysis and wave discovery for a JAudio game (see jaudio_analysis.h).

#include "rip/jaudio_analysis.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/flipper/game_source.h"
#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/beside_tracks.h"
#include "ntdlib/jaudio/game_lengths.h"
#include "ntdlib/jaudio/jungle_beat_states.h"
#include "ntdlib/ppc/cpu.h"

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

// WSF's player as the machine that ntdlib's analysis watches: its output, the emulated Wii's memory at CPU addresses,
// and its CPU. For sound `sound_id`.
class Machine
{
public:
    Machine(Player& player, wii::System& system, const JAudioProfile& profile, uint32_t sound_id)
        : player_(player),
          system_(system),
          profile_(profile),
          chord_layer_(!profile.chord_layers.empty() && (sound_id & ntdlib::jaudio::kChordLayer) != 0),
          cpu_(system.Cpu(), [this](uint32_t pc) { return OnBackgroundTrack(pc); })
    {
    }

    std::size_t Render(int16_t* out, std::size_t frames)
    {
        return player_.Render(out, frames);
    }

    uint32_t Word(uint32_t address)
    {
        return ntdlib::flipper::WordAt(system_.Memory(), address).value_or(0);
    }

    std::optional<uint8_t> Byte(uint32_t address)
    {
        return ntdlib::flipper::ByteAt(system_.Memory(), address);
    }

    ntdlib::jaudio::SkippingCpu<ntdlib::ppc::Cpu>& Cpu()
    {
        return cpu_;
    }

private:
    // Whether the watched instruction at `pc` parses a track of the sound that the driver plays beside the ripped one
    // (see JAudioProfile::background_sound), or of a chord layer's effect (JAudioProfile::chord_layers). A chord
    // layer's effects play beside the music that the driver reports. The analysis follows the music alone.
    bool OnBackgroundTrack(uint32_t pc)
    {
        if (profile_.background_sound < 0)
        {
            return false;
        }

        const std::optional<uint32_t> track = ntdlib::jaudio::ParsedTrack(profile_.watch, pc, system_.Cpu().state_.gpr);
        if (!track)
        {
            return false;
        }

        const auto word = [this](uint32_t address)
        {
            return Word(address);
        };
        const uint32_t sound = Word(kJAudioParamsAddress + 4 * static_cast<uint32_t>(profile_.background_sound));
        return ntdlib::jaudio::IsTrackBeside(profile_.watch, sound, *track, profile_.track_parent_offset, chord_layer_,
                                             word);
    }

    Player& player_;
    wii::System& system_;
    const JAudioProfile& profile_;
    bool chord_layer_;
    ntdlib::jaudio::SkippingCpu<ntdlib::ppc::Cpu> cpu_;
};

// The analysis's rules at WSF's output rate. With `limit` kLong, the analysis runs to xsflib's 3,600 s. A sound at
// either limit gets 600 s.
ntdlib::jaudio::GameLengthRules JAudioLengthRules(xsflib::rip::Limit limit)
{
    ntdlib::jaudio::GameLengthRules rules;
    rules.sample_rate = Player::kSampleRate;

    return ntdlib::jaudio::WithLimit(rules, limit);
}

// Runs ntdlib's analysis of the game's code (see AnalyzeJAudioSound) on the set of `library` and a mini for sound
// `sound_id`, with `waves` loaded and the piece's `state`. With `lookups_sound`, each lookup of a wave counts as a
// sound (see FindJAudioWaves). With `luigi`, the mini plays the sound's Luigi variant. `limit` sets the analysis's
// limit (JAudioLengthRules).
std::optional<xsflib::rip::Timing> PlayAndWatch(const std::vector<uint8_t>& library, const JAudioProfile& profile,
                                                uint32_t sound_id, std::span<const uint32_t> waves,
                                                std::set<ntdlib::jaudio::WaveUse>* found, bool lookups_sound,
                                                std::string* start_error, const ntdlib::flipper::z2::PieceState* state,
                                                bool luigi, xsflib::rip::Limit limit)
{
    // The reader copies the library straight into the player's set. A copy in a map of files first would hold a
    // second copy of the library for each analysis that runs.
    const std::vector<uint8_t> mini =
        BuildJAudioMini(sound_id, waves, kLibName, {}, state, luigi ? profile.luigi_word : -1);
    const FileReader reader = [&library, &mini](const std::string& path, std::vector<uint8_t>& data)
    {
        if (path != kLibName && path != kMiniName)
        {
            return false;
        }

        data = path == kLibName ? library : mini;

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

    Machine machine(player, *player.GameSystem(), profile, sound_id);

    // For a JAudio1 game, the sound's ID distinguishes a sequence from an effect.
    return ntdlib::jaudio::AnalyzeGameSound(machine, kJAudioZ2Address + ntdlib::flipper::z2::kHandleOffset,
                                            WatchOf(profile, sound_id), JAudioLengthRules(limit), found, lookups_sound,
                                            sound_id);
}

} // namespace

std::optional<xsflib::rip::Timing> AnalyzeJAudioSound(const std::vector<uint8_t>& library, const JAudioProfile& profile,
                                                      uint32_t sound_id, std::span<const uint32_t> waves,
                                                      std::set<ntdlib::jaudio::WaveUse>* found,
                                                      std::string* start_error,
                                                      const ntdlib::flipper::z2::PieceState* state, bool luigi,
                                                      xsflib::rip::Limit limit)
{
    return PlayAndWatch(library, profile, sound_id, waves, found, false, start_error, state, luigi, limit);
}

bool FindJAudioWaves(const std::vector<uint8_t>& library, const JAudioProfile& profile, uint32_t sound_id,
                     std::set<ntdlib::jaudio::WaveUse>& found, const ntdlib::flipper::z2::PieceState* state, bool luigi)
{
    const auto limit = xsflib::rip::Limit::kDefault;
    return PlayAndWatch(library, profile, sound_id, {}, &found, true, nullptr, state, luigi, limit).has_value();
}

} // namespace wsf::rip
