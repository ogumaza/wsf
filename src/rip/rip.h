// SPDX-License-Identifier: MIT

// WSF ripper. For a game with NW4R SND, it builds a .wsflib for one of the game's sound archives and one .miniwsf per
// sequence. Game mode rips the game's code, for games with a driver profile. Archive mode rips a sound archive and AX's
// microcode for WSF's model of the SDK's sound player. The profiles of the games are in profiles.h, and the rips of
// the JAudio games in jaudio.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/arrangement.h"
#include "ntdlib/rvl/snd/effect_setup.h"
#include "ntdlib/rvl/snd/util.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/sound_pieces.h"
#include "xsflib/rip/length.h"

#include "rip/profiles.h"
#include "wsf/format.h"
#include "wsf/models.h"

namespace wsf::rip
{

// What game mode loads to play a sequence: the groups that hold its sequence and its bank (with the bank's waves), and
// the size of the sound heap they require.
struct SoundData
{
    uint32_t group;
    std::optional<uint32_t> bank_group; // the group with the bank, when `group` lacks it
    uint32_t heap_size;
};

// Returns the groups that hold a sequence's data: one group with both its sequence and its bank, or else the first
// group that holds each. Returns nothing if no group in the archive holds one of them.
std::optional<SoundData> FindSoundData(const ntdlib::rsar::SoundArchive& archive, uint32_t index);

// Game mode: a copy of the archive for a library that plays the sequences at `indices`. For each sequence, the driver
// loads the groups that FindSoundData gives. The copy keeps what the game reads of them (ntdlib::rsar::KeepGroups).
std::vector<uint8_t> KeepLoadedGroups(const ntdlib::rsar::SoundArchive& archive, const std::vector<uint32_t>& indices);

// Game mode: builds the .wsflib that plays the sequences at `indices` of the sound archive at `archive_path` on the
// disc (no leading '/'). The library holds the game's code with main patched to branch to the driver, the driver, the
// partition's header and the archive. In the archive, only the groups that the driver loads for those sequences keep
// their data. The rest of the FILE block is zeros (KeepLoadedGroups). The driver sets up the effects of `effects` that
// a sound gets when no effect lists it, with the functions of the game's effect profile (FindEffectProfile). Returns
// nullopt and sets `error` if main.dol isn't a DOL, the archive's path is longer than the driver supports, the driver's
// block doesn't have the layout that the ripper expects, the driver can't set up all of the effects, silence the game's
// tracks or set its variables, or the library is larger than a player loads.
std::optional<std::vector<uint8_t>> BuildLibrary(const ntdlib::disc::GameFiles& game, const GameProfile& profile,
                                                 const std::string& archive_path,
                                                 const ntdlib::rsar::SoundArchive& archive,
                                                 const std::vector<uint32_t>& indices, const Tags& tags,
                                                 std::string* error = nullptr,
                                                 const nw4r::ArchiveEffects& effects = {});

// Game mode: a .miniwsf that plays sequence `index`, with `data`, from `lib_name`. It tells the driver whether the
// sound's variables pick the presets of `effects`, and the send that the game sets on the sound. For a sound that gets
// other effects than the library's, it gives the driver those effects. Their classes' functions come from the game's
// effect profile, `effect_profile`.
std::vector<uint8_t> BuildMini(uint32_t index, const SoundData& data, const std::string& lib_name, Tags tags,
                               const nw4r::ArchiveEffects& effects = {},
                               const std::optional<EffectProfile>& effect_profile = std::nullopt);

// Archive mode: the archive descriptor of a set whose FILE chunks hold the sound archive and AX's microcode at the
// given paths. The descriptor names the game's build of NW4R SND, and gives the effects. The model follows that build.
// The descriptor has version `version`: ArchiveDescriptor::kListVersion when a mini of the set plays several sounds,
// and ArchiveDescriptor::kMoodVersion for effects that only version 3 may hold (RequiresMoodVersion).
ArchiveDescriptor BuildArchiveDescriptor(const std::string& archive_path, const std::string& microcode_path,
                                         const ntdlib::Microcode& microcode, nw4r::SndBuild build,
                                         const nw4r::ArchiveEffects& effects, uint32_t version);

// Archive mode: a .wsflib that holds the sound archive and AX's microcode as FILE chunks at the given paths, with the
// descriptor that BuildArchiveDescriptor gives.
std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& microcode_path, const ntdlib::Microcode& microcode,
                                         nw4r::SndBuild build, const Tags& tags,
                                         const nw4r::ArchiveEffects& effects = {},
                                         uint32_t version = ArchiveDescriptor::kVersion);

// Archive mode: a .miniwsf that plays sequence `index` from `lib_name`.
std::vector<uint8_t> BuildArchiveMini(uint32_t index, const std::string& lib_name, Tags tags);

// Archive mode: writes the variables that `effects` gives `sound` into `tags`, under those that the tags give already
// (docs/wsf.md, "Variables that the game sets"). The archive descriptor holds no variables. A rip's tag gives them.
void AddSoundVariables(const nw4r::ArchiveEffects& effects, uint32_t sound, Tags& tags);

// Archive mode: a .miniwsf that plays the sequences at `indices` together from `lib_name`, started in that order
// (docs/wsf.md, "Sounds that play together"). The library's descriptor must have ArchiveDescriptor::kListVersion when
// `indices` holds more than one sound. A variant's rip holds `descriptor`. That descriptor takes the place of the
// library's.
std::vector<uint8_t> BuildArchiveMini(const std::vector<uint32_t>& indices, const std::string& lib_name, Tags tags,
                                      const std::optional<ArchiveDescriptor>& descriptor = std::nullopt);

// A player loads a program of 1 GiB at most (xsflib's kMaxProgramSize). For a larger program of `size` bytes, returns
// the rip's message. The message gives both sizes. Returns nothing for a program that a player loads.
std::optional<std::string> ProgramTooLarge(std::size_t size);

// A piece of music that a game plays as separate sequences, started together (docs/wsf.md, "Sounds that play
// together"): the piece's name, and the sequences in the order in which the game starts them.
using Piece = ntdlib::SoundPiece;

// The pieces of `archive` that the game plays as separate sequences. A sequence of a piece rips only in its pieces.
// - Mega Man 9 plays each piece of its music as four sequences, one for each channel of the NES's sound chip. It
//   starts them together: the two pulse channels, the triangle and the noise. Their labels are the piece's name
//   followed by _S1, _S2, _TR and _NO (ntdlib::kMegaMan9Channels, found by ntdlib::FindSoundPieces).
// - The Forecast and News channels play a piece as a base sequence and a layer that some views add. A table of labels
//   gives these pieces (kLayeredPieces in nw4r.cpp). Each base also makes a piece of one sequence: the views without
//   the layer. A sequence can be in several pieces.
// The pieces come in the order of their first sounds. Pieces with the same first sound keep the table's order.
std::vector<Piece> FindPieces(const ntdlib::rsar::SoundArchive& archive);

// A sequence's length and fade in milliseconds, and whether it never makes a sound.
using Timing = xsflib::rip::Timing;

// The timing of sequences that play together: the latest end and the longest fade (xsflib/rip/length.h).
using xsflib::rip::TimingOfPiece;

// Whether archive mode's model can drive `microcode`. The microcode must start and mix the model's first frame. Some
// games have older AX microcodes that accept other command lists. The model doesn't make those lists.
bool MicrocodeWorks(const ntdlib::Microcode& microcode, const ntdlib::rsar::SoundArchive& archive);

// Works out how long sequences play by running them on the model with AX's microcode on the DSP. When the model can't
// drive the microcode, the analyzer runs the model alone. The model then follows the voices' positions in place of the
// microcode (see nw4r::ArchiveModel::RunFrameWithoutDsp). A frame then has sound while any note plays, even on a track
// that the arrangement silences. Analyze can be called from several threads at once.
class LengthAnalyzer
{
public:
    // The model follows the game's `build` of NW4R SND. Each sequence plays in `arrangement`, with `effects`. A
    // sequence with effects ends where their output falls silent. A sequence that `effects` gives tracks for plays only
    // those of the arrangement's tracks. `wsfrip --tracks` gives effects without tracks: its tracks replace the game's.
    LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const ntdlib::Microcode& microcode, nw4r::SndBuild build,
                   const nw4r::Arrangement& arrangement = {}, const nw4r::ArchiveEffects& effects = {});

    ~LengthAnalyzer();

    // A looping sequence gets two loops and a 10 s fade. Any other sequence runs to its end or to where it falls silent
    // for good, for at most 600 s. A sequence that never makes a sound is marked silent and gets the 0.5 s that follows
    // a sequence's last sound. Returns nullopt when the length can't be worked out. No length tag is written then.
    // Throws std::bad_alloc when memory runs out. The effects are those of `effects_sound`, by default the sound
    // itself. A sequence that plays together with others gets the effects of the first.
    std::optional<Timing> Analyze(uint32_t sound_index, std::optional<uint32_t> effects_sound = std::nullopt) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace wsf::rip
