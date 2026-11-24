// SPDX-License-Identifier: MIT

// The ripper's parts for games whose sound library is JAudio2, JAudio1 or Jac: the games' sound files and sounds, and
// the variants of the pieces. jaudio_library.cpp builds the library and the minis. The length analysis and the wave
// discovery play a rip on the player (see rip/jaudio_analysis.h).

#include "rip/jaudio.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/flipper/piece_variants.h"
#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/barc.h"
#include "ntdlib/jaudio/bx.h"
#include "ntdlib/jaudio/game_lengths.h"
#include "ntdlib/jaudio/jac_effects.h"
#include "ntdlib/jaudio/pikmin2_pieces.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/yaz0.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"
#include "xsflib/text.h"

namespace wsf::rip
{
namespace
{

// Whether `data` starts with the four bytes of `magic`.
bool StartsWith(std::span<const uint8_t> data, const char* magic)
{
    return data.size() >= 4 && std::memcmp(data.data(), magic, 4) == 0;
}

// A Jac game's sound effects, from the catalogue in `dol` (see ReadJacSounds). An effect that the game's code starts
// itself is named after its kind and number, such as "system-19", or is "formation".
std::vector<JAudioSound> ReadJacEffects(const ntdlib::Dol& dol, const JAudioProfile& profile)
{
    const auto name_of_other = [](uint32_t id)
    {
        const auto kind = static_cast<JacEffect>(id >> 24 & 0x7f);
        if (kind == JacEffect::kFormation)
        {
            return std::string("formation");
        }

        const char* const word = kind == JacEffect::kSystem   ? "system"
                                 : kind == JacEffect::kPlayer ? "player"
                                                              : "event";
        char text[32];
        std::snprintf(text, sizeof(text), "%s-%x", word, static_cast<unsigned>(id >> 8 & 0xffff));

        return std::string(text);
    };

    std::vector<JAudioSound> effects;
    for (ntdlib::jaudio::JacEffectSound& effect :
         ntdlib::jaudio::ListJacEffects(dol, profile.jac_effects, name_of_other))
    {
        effects.push_back(
            {.id = effect.id, .label = std::move(effect.name), .section = "SE", .waves = profile.effect_groups});
    }

    return effects;
}

} // namespace

std::optional<JAudioFiles> FindJAudio2Files(const ntdlib::disc::GameFiles& game)
{
    if (!game.read_file)
    {
        return std::nullopt;
    }

    // A start-up archive is a .baa file, or a .szs file that holds one compressed with Yaz0.
    std::optional<JAudioFiles> found;
    std::vector<uint8_t> data;
    for (const ntdlib::disc::GameFile& file : game.files)
    {
        const std::string path = xsflib::AsciiLower(file.path);
        if ((path.ends_with(".baa") || path.ends_with(".szs")) && ReadSoundFile(game, file.path, data) &&
            StartsWith(data, "AA_<"))
        {
            found = JAudioFiles{file.path, {}};
            break;
        }
    }
    if (!found)
    {
        return std::nullopt;
    }

    // The RARC archives in the start-up archive's folder and below it, compressed with Yaz0 or not.
    const std::string folder = xsflib::psf::DirectoryOf(found->baa, "/");
    for (const ntdlib::disc::GameFile& file : game.files)
    {
        if (file.path.starts_with(folder) && xsflib::AsciiLower(file.path).ends_with(".arc") &&
            ReadSoundFile(game, file.path, data) && StartsWith(data, "RARC"))
        {
            found->archives.push_back(file.path);
        }
    }

    return found;
}

bool ReadSoundFile(const ntdlib::disc::GameFiles& game, const std::string& path, std::vector<uint8_t>& data)
{
    if (!game.read_file || !game.read_file(path, data))
    {
        return false;
    }

    if (ntdlib::IsYaz0(data))
    {
        try
        {
            data = ntdlib::DecodeYaz0(data);
        }
        catch (const xsflib::FormatError&)
        {
            return false;
        }
    }

    return true;
}

std::vector<std::string> LibraryFiles(const JAudioFiles& files, const JAudioProfile& profile)
{
    const std::vector<std::string>& others = profile.files.empty() ? files.archives : profile.files;
    std::vector<std::string> paths = {files.baa};
    paths.insert(paths.end(), others.begin(), others.end());

    return paths;
}

std::optional<std::vector<std::string>> FindAwFiles(const ntdlib::disc::GameFiles& game, const JAudioFiles& files,
                                                    const std::vector<ntdlib::jaudio::WaveSystem>& systems,
                                                    std::span<const uint32_t> groups, std::string* missing)
{
    const std::string folder = xsflib::psf::DirectoryOf(files.baa, "/");
    std::vector<std::string> paths;
    for (const std::string& name : ntdlib::jaudio::GroupArchives(systems, groups))
    {
        const auto matches = [&folder, &name](const ntdlib::disc::GameFile& file)
        {
            return file.path == folder + name || (file.path.starts_with(folder) && file.path.ends_with("/" + name));
        };
        const auto file = std::find_if(game.files.begin(), game.files.end(), matches);
        if (file == game.files.end())
        {
            if (missing)
            {
                *missing = name;
            }

            return std::nullopt;
        }

        paths.push_back(file->path);
    }

    return paths;
}

std::vector<JAudioSound> ReadJAudio2Sounds(std::span<const uint8_t> baa)
{
    std::vector<JAudioSound> sounds;
    for (const ntdlib::jaudio::SoundEntry& entry : ntdlib::jaudio::ReadSounds(baa, {.section_names = true}))
    {
        char id[16];
        std::snprintf(id, sizeof(id), "0x%08x", entry.Id());
        std::string label = entry.name.empty() ? id : entry.name;
        const bool music = xsflib::AsciiLower(label).find("bgm") != std::string::npos || entry.section_name == "BGM";
        sounds.push_back({.id = entry.Id(),
                          .label = std::move(label),
                          .section = entry.section_name,
                          .music = music,
                          .stream = ntdlib::jaudio::LayoutOf(entry.type).stream});
    }

    return sounds;
}

std::optional<JAudioFiles> FindJAudio1Files(const ntdlib::disc::GameFiles& game, const JAudioProfile& profile)
{
    const auto on_disc = [&game](const std::string& path)
    {
        return std::any_of(game.files.begin(), game.files.end(),
                           [&path](const ntdlib::disc::GameFile& file) { return file.path == path; });
    };

    if (!on_disc(profile.sound_archive) || !on_disc(profile.sequence_archive))
    {
        return std::nullopt;
    }

    return JAudioFiles{profile.sound_archive, {profile.sequence_archive}};
}

std::optional<JAudioFiles> FindJAudioFiles(const ntdlib::disc::GameFiles& game, const JAudioProfile& profile,
                                           std::string* error)
{
    if (profile.library == JAudioLibrary::kJAudio2)
    {
        std::optional<JAudioFiles> files = FindJAudio2Files(game);
        if (!files && error)
        {
            *error = "the game has no JAudio2 start-up archive (.baa)";
        }

        return files;
    }

    std::optional<JAudioFiles> files = FindJAudio1Files(game, profile);
    if (!files && error)
    {
        *error = "the disc lacks " + profile.sound_archive + " or " + profile.sequence_archive;
    }

    return files;
}

std::vector<ntdlib::jaudio::WaveSystem> ReadWaveSystems(JAudioLibrary library, std::span<const uint8_t> start_up)
{
    switch (library)
    {
    case JAudioLibrary::kJAudio2:
        return ntdlib::jaudio::ReadWaveSystems(start_up);
    case JAudioLibrary::kJac:
        return ntdlib::jaudio::ReadBxWaveSystems(start_up);
    case JAudioLibrary::kAaf:
        return ntdlib::jaudio::ReadAafWaveSystems(start_up);
    }

    return {};
}

std::vector<JAudioSound> ReadJacSounds(std::span<const uint8_t> dol, const JAudioProfile& profile)
{
    constexpr std::size_t kHeader = 0x20;

    // The index runs from the profile's address to the end of the section that holds it.
    const ntdlib::Dol parsed = ntdlib::Dol::ParseMain(dol);
    const std::span<const uint8_t> index = parsed.At(profile.sequence_index);
    const xsflib::Reader r(index, true);
    if (index.size() < kHeader || !r.Magic(0, "BARC"))
    {
        throw xsflib::FormatError("main.dol has no sequence index at " + xsflib::Hex(profile.sequence_index, 8));
    }

    // A sound's ID holds the piece's number in its low byte.
    constexpr uint32_t kMaxPieces = 0x100;
    const uint32_t count = r.U32(0xc);
    if (count > kMaxPieces)
    {
        throw xsflib::FormatError("main.dol's sequence index has " + std::to_string(count) + " files");
    }

    const std::vector<ntdlib::jaudio::BarcEntry> entries = ntdlib::jaudio::ReadBarcEntries(index);
    std::vector<JAudioSound> sounds;
    for (uint32_t number = kJacFirstPiece; number < count; number++)
    {
        if (entries[number].size == 0)
        {
            continue;
        }

        std::string label = entries[number].name.substr(0, entries[number].name.rfind('.'));
        if (number != profile.parted_piece)
        {
            sounds.push_back({.id = ntdlib::jaudio::kAafSequence | number,
                              .label = std::move(label),
                              .music = true,
                              .waves = {number << 16}});
            continue;
        }

        for (const JacPart& part : profile.parts)
        {
            sounds.push_back({.id = ntdlib::jaudio::kAafSequence | uint32_t{part.number} << 8 | number,
                              .label = label + "-" + std::to_string(part.number),
                              .music = true,
                              .waves = {uint32_t{part.system} << 16 | part.group}});
        }
    }

    std::vector<JAudioSound> effects = ReadJacEffects(parsed, profile);
    sounds.insert(sounds.end(), std::make_move_iterator(effects.begin()), std::make_move_iterator(effects.end()));

    return sounds;
}

std::vector<JAudioSound> ReadPikmin2Sounds(std::span<const uint8_t> aaf, std::span<const uint8_t> sequence_archive,
                                           std::span<const uint8_t> names, const JAudioProfile& profile)
{
    std::vector<uint8_t> decoded;
    sequence_archive = ntdlib::DecodeIfYaz0(sequence_archive, decoded);

    // The pieces, each with group 0 and its scene's group of wave system 0.
    const std::vector<ntdlib::jaudio::ArchiveFile> files = ntdlib::jaudio::ReadRarc(sequence_archive);
    std::vector<JAudioSound> sounds;
    for (const ntdlib::jaudio::Pikmin2Piece& piece : ntdlib::jaudio::kPikmin2Pieces)
    {
        const std::string_view name = piece.sequence;
        const auto is_piece = [name](const ntdlib::jaudio::ArchiveFile& file)
        {
            return file.name == name;
        };
        if (std::none_of(files.begin(), files.end(), is_piece))
        {
            throw xsflib::FormatError("the sequence archive lacks " + std::string(name));
        }

        JAudioSound sound = {.id = ntdlib::jaudio::kAafSequence | static_cast<uint32_t>(sounds.size()),
                             .label = std::string(piece.Name()),
                             .section = "BGM",
                             .music = true};
        if (piece.wave_scene != ntdlib::jaudio::Pikmin2Piece::kFindWaves)
        {
            sound.waves = {0};
            if (piece.wave_scene != 0)
            {
                sound.waves.push_back(piece.wave_scene);
            }
        }

        sounds.push_back(std::move(sound));
    }

    // The effects, by their names in the name table, or else by their categories and indices.
    const std::map<uint32_t, std::string> named =
        names.empty() ? std::map<uint32_t, std::string>{} : ntdlib::jaudio::ReadAsnNames(names);
    for (const ntdlib::jaudio::SoundEntry& entry : ntdlib::jaudio::ReadAafSounds(aaf))
    {
        if (entry.source == ntdlib::jaudio::SoundEntry::Source::kArchive)
        {
            continue;
        }

        const uint32_t id = entry.Id();
        const auto name = named.find(ntdlib::jaudio::AsnNameKey(id));
        char label[32];
        std::snprintf(label, sizeof(label), "se%u-%04u", static_cast<unsigned>(ntdlib::jaudio::AafCategory(id)),
                      static_cast<unsigned>(id & ntdlib::jaudio::kAafIndexMask));
        const auto start = std::find_if(profile.random_starts.begin(), profile.random_starts.end(),
                                        [id](const RandomStart& s) { return s.id == id; });
        const uint32_t draws = start != profile.random_starts.end() ? start->draws : 0;
        sounds.push_back({.id = id | draws << kRandomDrawsShift,
                          .label = name != named.end() ? name->second : label,
                          .section = "SE",
                          .waves = profile.static_waves});
    }

    return sounds;
}

namespace
{

// A variant of `sound`: a sound with `suffix` on its label that shares the sound's number. The variant keeps the label
// of the sound that it plays (JAudioSound::sound_label).
JAudioSound VariantOf(const JAudioSound& sound, std::string_view suffix)
{
    JAudioSound variant = sound;
    variant.sound_label = sound.sound_label.empty() ? sound.label : sound.sound_label;
    variant.label += suffix;
    variant.shares_number = true;

    return variant;
}

// Adds the states that the game's code sets for its music during play to `sounds`, each after its piece
// (JAudioProfile::layer_states). A state's label is the label of the piece, "_" and the suffix of the state, such as
// "JA_BGM_MAIN_01_combo1". The states share the piece's number.
void AddLayerStates(const JAudioProfile& profile, std::vector<JAudioSound>& sounds)
{
    if (profile.layer_states.empty())
    {
        return;
    }

    const auto states_of = [&profile](const JAudioSound& base)
    {
        std::vector<JAudioSound> states;
        for (const ntdlib::jaudio::JungleBeatState& state : profile.layer_states)
        {
            if (state.piece != base.id)
            {
                continue;
            }

            JAudioSound variant = VariantOf(base, std::string("_") + state.suffix);
            variant.id = state.piece | state.bits;
            states.push_back(std::move(variant));
        }

        return states;
    };
    ntdlib::flipper::AddVariantsAfterEach(sounds, states_of);
}

// The words of a Galaxy state's .miniwsf (JAudioProfile::state_word): the state, the tempo ratio, the pitch ratio, the
// pitched children, the quiet child and its volume (driver/galaxy_state.h). A ratio of 1 leaves the tempo or the
// pitch as it is.
ntdlib::flipper::z2::PieceState::Memory GalaxyStateWords(const JAudioProfile& profile,
                                                         const ntdlib::jaudio::GalaxyState& state)
{
    constexpr uint32_t kNone = 0xffffffff;
    const uint32_t pitched = state.pitch != 1.0f ? state.pitched_children : 0;
    const std::array<uint32_t, 6> words = {
        static_cast<uint32_t>(state.state),
        state.tempo != 1.0f ? std::bit_cast<uint32_t>(state.tempo) : 0,
        std::bit_cast<uint32_t>(state.pitch),
        pitched,
        state.quiet_child >= 0 ? static_cast<uint32_t>(state.quiet_child) : kNone,
        std::bit_cast<uint32_t>(state.quiet_volume),
    };
    ntdlib::flipper::z2::PieceState::Memory memory{.address = kJAudioParamsAddress +
                                                              static_cast<uint32_t>(profile.state_word) * 4};
    memory.data.resize(words.size() * 4);
    for (std::size_t i = 0; i < words.size(); i++)
    {
        xsflib::StoreBe32(memory.data.data() + i * 4, words[i]);
    }

    return memory;
}

// Adds the states that the Galaxy games' code gives their music during play to `sounds`, each after its piece and the
// piece's other variants (JAudioProfile::galaxy_states). A state's label is the piece's with the state's suffix, such
// as "BGM_TAMAKORO_speed_low". The states share the piece's number.
void AddGalaxyStates(const JAudioProfile& profile, std::vector<JAudioSound>& sounds)
{
    if (profile.galaxy_states.empty() || profile.state_word < 0)
    {
        return;
    }

    const auto states_of = [&profile](const JAudioSound& piece)
    {
        std::vector<JAudioSound> states;
        for (const ntdlib::jaudio::GalaxyState& state : profile.galaxy_states)
        {
            if (state.sound != piece.id)
            {
                continue;
            }

            JAudioSound variant = VariantOf(piece, state.suffix);
            variant.piece_state.memory.push_back(GalaxyStateWords(profile, state));
            states.push_back(std::move(variant));
        }

        return states;
    };
    ntdlib::flipper::AddPieceVariants(sounds, states_of);
}

// Adds the chord effects that the game plays beside its music to `sounds`, each after its piece
// (JAudioProfile::chord_layers). A layer's label is the label of the piece, "_" and the suffix of the layer, such as
// "JA_BGM_MAIN_01_enemy_sleeping". The layers share the piece's number.
void AddChordLayers(const JAudioProfile& profile, std::vector<JAudioSound>& sounds)
{
    if (profile.chord_layers.empty())
    {
        return;
    }

    const auto layers_of = [&profile](const JAudioSound& base)
    {
        std::vector<JAudioSound> layers;
        for (std::size_t i = 0; i < profile.chord_layers.size(); i++)
        {
            const ntdlib::jaudio::JungleBeatChordLayer& layer = profile.chord_layers[i];
            if (layer.piece != base.id)
            {
                continue;
            }

            JAudioSound variant = VariantOf(base, std::string("_") + layer.suffix);
            variant.id = ntdlib::jaudio::ChordLayerId(layer.piece, static_cast<uint32_t>(i));
            layers.push_back(std::move(variant));
        }

        return layers;
    };
    ntdlib::flipper::AddVariantsAfterEach(sounds, layers_of);
}

// For each sound in `sounds`, calls `write_for` with the sound ID and adds the write to main memory that it returns, if
// any, to the mini file of that sound.
template <typename WriteFor>
void AddMemoryWrites(std::vector<JAudioSound>& sounds, const WriteFor& write_for)
{
    for (JAudioSound& sound : sounds)
    {
        if (std::optional<ntdlib::flipper::z2::PieceState::Memory> write = write_for(sound.id))
        {
            sound.piece_state.memory.push_back(std::move(*write));
        }
    }
}

// Gives each chord layer whose piece starts with every track off the piece's second state in place of its first
// (JAudioProfile::state_table). The driver starts a layer's music through the stage's start. That start applies the
// first state. The piece alone gets the second state from the driver.
void AddSilentStarts(const ntdlib::disc::GameFiles& game, const JAudioProfile& profile,
                     std::vector<JAudioSound>& sounds)
{
    if (profile.chord_layers.empty() || profile.state_table.rows == 0)
    {
        return;
    }

    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(game.dol);
    const auto silent_start = [&dol, &profile](uint32_t id)
    {
        return ntdlib::jaudio::JungleBeatSilentStart(dol, profile.state_table, id);
    };
    AddMemoryWrites(sounds, silent_start);
}

} // namespace

void AddSurvivalTests(const JAudioProfile& profile, std::vector<JAudioSound>& sounds)
{
    if (profile.survival_test == 0)
    {
        return;
    }

    const auto survival_test = [&profile](uint32_t id)
    {
        return ntdlib::jaudio::JungleBeatSurvivalTest(profile.chord_layers, profile.survival_test, id);
    };
    AddMemoryWrites(sounds, survival_test);
}

std::optional<std::vector<JAudioSound>> ReadJAudioSounds(const ntdlib::disc::GameFiles& game, const JAudioFiles& files,
                                                         const JAudioProfile* profile,
                                                         std::span<const uint8_t> start_up, std::string* error)
{
    const JAudioLibrary library = SoundLibraryOf(profile);
    if (library == JAudioLibrary::kJAudio2)
    {
        std::vector<JAudioSound> sounds = ReadJAudio2Sounds(start_up);
        if (profile)
        {
            ntdlib::flipper::AddMutedStarts(profile->muted_starts, sounds, VariantOf);
            ntdlib::flipper::AddPieceStates(profile->piece_states, sounds, VariantOf);
            AddChordLayers(*profile, sounds); // before the states, which then come first after each piece
            AddFightMusic(*profile, sounds);
            AddLayerStates(*profile, sounds);
            AddGalaxyStates(*profile, sounds);
            AddSurvivalTests(*profile, sounds);
            try
            {
                AddSilentStarts(game, *profile, sounds);
            }
            catch (const xsflib::FormatError& e)
            {
                if (error)
                {
                    *error = e.what();
                }

                return std::nullopt;
            }
        }

        return sounds;
    }
    if (library == JAudioLibrary::kJac)
    {
        return ReadJacSounds(game.dol, *profile);
    }

    // Pikmin 2's sequence archive, and its name table.
    std::vector<uint8_t> sequences;
    std::vector<uint8_t> name_table;

    const auto read = [&game, error](const std::string& path, std::vector<uint8_t>& data)
    {
        if (game.read_file && game.read_file(path, data))
        {
            return true;
        }

        if (error)
        {
            *error = "can't read " + path;
        }

        return false;
    };

    if (!read(files.archives.front(), sequences) ||
        (!profile->sound_names.empty() && !read(profile->sound_names, name_table)))
    {
        return std::nullopt;
    }

    return ReadPikmin2Sounds(start_up, sequences, name_table, *profile);
}

ntdlib::jaudio::GameWatch WatchOf(const JAudioProfile& profile, uint32_t sound_id)
{
    // An AutoBgm piece never repeats. Once the piece's root track idles in its polling routine, the passes of the other
    // tracks would still look like a loop. The root track's polls therefore stay moves. They hold back every loop, and
    // the piece plays to the analysis's limit.
    ntdlib::jaudio::GameWatch watch = profile.watch;
    using Kind = ntdlib::jaudio::Pikmin2Piece::Kind;
    const ntdlib::jaudio::Pikmin2Piece* piece =
        profile.driver == JAudioDriver::kPikmin2 ? ntdlib::jaudio::Pikmin2PieceOf(sound_id) : nullptr;
    if (piece && (piece->kind == Kind::kAuto || piece->kind == Kind::kAutoFree))
    {
        watch.idle_in_calls = false;
    }

    return watch;
}

void AddFightMusic(const JAudioProfile& profile, std::vector<JAudioSound>& sounds)
{
    if (profile.stage_table == 0)
    {
        return;
    }

    const auto fight_music = [&profile](uint32_t id)
    {
        return ntdlib::jaudio::JungleBeatFightMusic(profile.chord_layers, profile.stage_table, id);
    };
    AddMemoryWrites(sounds, fight_music);
}

} // namespace wsf::rip
