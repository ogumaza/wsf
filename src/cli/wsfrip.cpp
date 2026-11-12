// SPDX-License-Identifier: MIT

// wsfrip: builds WSF sets (.wsflib + .miniwsf files) from Wii games and sound archives.
//
// usage: wsfrip [options] <input> [<input> ...]
//   <input>          a Wii disc image with a decrypted game partition (NKit ISO or GCZ, RVZ, WIA, or an ISO, GCZ, WBFS,
//                    CISO or WDF made from one), an extracted disc, an unpacked WiiWare title (a folder with the
//                    title's TMD and decrypted contents), or a sound archive (.brsar). A title rips in archive mode
//                    only. A game whose sound library is JAudio (JAudio1 or JAudio2) rips in game mode only, and
//                    requires a driver profile.
//                    A main.dol among the inputs isn't ripped. The sound archives among the inputs take AX's microcode
//                    and the build of NW4R SND from it.
//   -o, --output DIR the folder for the rips (default: a folder next to each input, named after the input with "_wsf"
//                    added)
//   --list           list the game's sound archives, or a JAudio game's sounds (and a JAudio2 game's sections), and
//                    whether it has a driver profile, then stop
//   --mode game|archive
//                    game mode runs the game's code and requires a driver profile for the game (src/rip/profiles.h).
//                    It's the default when a profile exists. Archive mode plays the sound archives with WSF's model of
//                    the SDK's sound player instead.
//   --microcode PATH the game code for a sound archive: the main.dol of the game that the archive comes from, the
//                    game's main program compressed with the CX library (a WiiWare title's content 1), or the game's
//                    extracted disc or unpacked title. Archive mode on a sound archive reads AX's microcode and the
//                    build of NW4R SND from it. By default they come from a main.dol given with the archive, or else
//                    from one in the archive's folder. Either may be compressed too.
//   --archive PATH   rip only this sound archive of a game (its path on the disc). A JAudio game has none.
//   --tracks LIST    archive mode: play only these tracks (0 to 15, and ranges such as 8-14), and write them in the
//                    wsf_tracks tag. The other tracks get a track volume of 0.
//   --tempo RATIO    archive mode: multiply the tempo by RATIO (0.25 to 4), and write it in the wsf_tempo tag
//   --var N=V        archive mode: set variable N (0 to 15 the player's, 16 to 31 the global ones) to V before each
//                    sequence starts, and write it in the wsf_var tag; repeat it for more variables
//   --suffix TEXT    archive mode: add TEXT to each rip's title and file name. Rips of one sound in several
//                    arrangements can then share a folder.
//   --only REGEX     rip only sounds whose label matches (ECMAScript regex)
//   --bgm            rip only the music (ntdlib::rsar::IsMusic): sequences labelled as BGM, or that play on a music
//                    player. In a JAudio2 game, the sounds labelled as BGM or in the section named BGM. In a JAudio1
//                    game, every piece of music.
//   --no-length      skip length analysis (no length or fade tags, only the sequences at a volume of 0 are found to
//                    make no sound, and every JAudio sound is taken to start)
//   --jobs N         the number of length analyses run in parallel (default: the number of CPU threads, at most 8 in a
//                    32-bit build)
//   --game NAME      game tag (default: the profile's name, or else the game's title, or the archive's name)
//   --artist NAME, --year YEAR, --copyright TEXT, --by NAME (wsfby)
//   --version        print the version
//
// Each of a game's sound archives gets a separate folder in the output folder. The folder is named after the archive.
// Where two of the game's archives have the same name, the names of the folders above them are added. A JAudio game's
// sounds make one set, in the output folder. Sequences whose data isn't all there are listed and skipped. So are those
// that make no sound: those whose volume in the archive is 0, and those that the length analysis finds never sound. The
// inputs are ripped one after another. An input that fails doesn't stop the rest. Files dropped on the program become
// inputs.

#include "cli/wsfrip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/disc/cx.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sound_build.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/cli/rip.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"
#include "xsflib/sha1.h"
#include "xsflib/text.h"

#include "cli/archive_rip.h"
#include "cli/jaudio_games.h"
#include "rip/jaudio.h"
#include "rip/profiles.h"
#include "wsf/format.h"
#include "wsf/models.h"

namespace wsf::cli
{
namespace
{

namespace fs = std::filesystem;

// The most that a compressed main program may decompress to: the Wii's memory, 24 MiB of MEM1 and 64 MiB of MEM2.
constexpr std::size_t kMaxMainProgramSize = std::size_t{88} << 20;

// The notices for archive mode: on a game without a driver profile, on a sound archive, and on a game with a profile.
// The notice on a sound archive says where exact rips come from. On a game with a profile, only --mode archive picks
// archive mode. The notice for that case says to leave the option out.
constexpr char kArchiveModeNotice[] =
    "note: in archive mode, WSF's model of the SDK's sound player plays the sounds instead of the\n"
    "      game's code. The DSP still runs the game's AX microcode.\n";
constexpr char kArchiveFileNotice[] =
    "note: in archive mode, WSF's model of the SDK's sound player plays the sounds instead of the\n"
    "      game's code. The DSP still runs the game's AX microcode. For exact rips of a game with a\n"
    "      driver profile, rip the game itself.\n";
constexpr char kArchiveModeChosenNotice[] =
    "note: in archive mode, WSF's model of the SDK's sound player plays the sounds instead of the\n"
    "      game's code. The DSP still runs the game's AX microcode. For exact rips of this game,\n"
    "      leave out --mode archive.\n";

// The notice for a title. Game mode emulates a disc. A title's game reads its contents from the Wii's NAND instead.
constexpr char kTitleNotice[] =
    "note: a title, such as a WiiWare game or a channel, rips in archive mode only. WSF's model\n"
    "      of the SDK's sound player plays the sounds instead of the title's code. The DSP still\n"
    "      runs the title's AX microcode.\n";

// What --list says, and what a rip's error says, about a game with neither kind of sound archive that WSF reads.
// Such a game holds nothing that WSF rips.
constexpr char kNoSoundArchive[] =
    "the game has no sound archive that WSF reads: no NW4R SND archive (.brsar) and no JAudio2 start-up archive";

constexpr char kUsage[] =
    "usage: wsfrip [options] <input> [<input> ...]\n"
    "\n"
    "Rips each input into a .wsflib and a .miniwsf per sound. The output folder is next to the\n"
    "input and has the input's name with \"_wsf\" added. An input is a Wii disc image (ISO, NKit,\n"
    "RVZ, WIA, GCZ, WBFS, CISO or WDF), an extracted disc, an unpacked title (a WiiWare game or a\n"
    "channel) or a sound archive (.brsar). The game partition of a disc image or extracted disc\n"
    "must be stored decrypted. NKit, RVZ and WIA images store it that way. An unpacked title is a\n"
    "folder with the title's TMD (.tmd) and its decrypted contents (.app). A title rips in archive\n"
    "mode only. A game whose sound library is JAudio rips in game mode only. A sound archive\n"
    "requires the main.dol of its game. The ripper takes AX's microcode and the build of the sound\n"
    "library from it. Give the main.dol with the archive or put it in the archive's folder. A\n"
    "WiiWare game or a channel has no main.dol. Give its unpacked title with --microcode instead.\n"
    "You can also drop files on the program. Sequences that make no sound, and those whose data\n"
    "isn't all there, are listed and skipped.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list a game's sound archives, or a JAudio game's sounds, and whether\n"
    "                       it has a driver profile, then stop\n"
    "  --mode game|archive  game mode runs the game's code and requires a driver profile for the\n"
    "                       game. It's the default when a profile exists. Archive mode plays the\n"
    "                       sound archives with WSF's model of the SDK's sound player.\n"
    "  --microcode PATH     the game code for a sound archive, for AX's microcode and the build of\n"
    "                       the sound library in archive mode: the main.dol of the archive's\n"
    "                       game, the game's compressed main program (a WiiWare title's content\n"
    "                       1), or the game's extracted disc or unpacked title\n"
    "  --archive PATH       rip only this sound archive of a game (its path on the disc)\n"
    "  --tracks LIST        archive mode: play only these tracks (0-15, and ranges such as 8-14).\n"
    "                       The other tracks are silent. Writes the wsf_tracks tag.\n"
    "  --tempo RATIO        archive mode: multiply the tempo by RATIO (0.25 to 4). Writes the\n"
    "                       wsf_tempo tag.\n"
    "  --var N=V            archive mode: set variable N (0-15 the player's, 16-31 the global\n"
    "                       ones) to V before each sequence starts. N and V can be hex (0x...).\n"
    "                       Repeat it for more variables. Writes the wsf_var tag.\n"
    "  --suffix TEXT        archive mode: add TEXT to each rip's title and file name. Rips of a\n"
    "                       sound in several arrangements can then share a folder.\n"
    "  --only REGEX         rip only the sounds whose label matches\n"
    "  --bgm                rip only the music: sequences labelled as BGM, or that play on a player\n"
    "                       named for music (BGM, MUSIC, SONG or FANFARE). In a JAudio2 game, the\n"
    "                       sounds labelled as BGM or in the section named BGM. In a JAudio1\n"
    "                       game, the pieces of music.\n"
    "  --no-length          skip length analysis and omit length and fade tags. The analysis also\n"
    "                       finds the sequences that make no sound, and the JAudio sounds that\n"
    "                       can't start. Without it, only the sequences at a volume of 0 are\n"
    "                       skipped.\n"
    "  --jobs N             the number of length analyses that run at once (default: the number\n"
    "                       of CPU threads, and at most 8 in a 32-bit build)\n"
    "  --game NAME, --artist NAME, --year YEAR, --copyright TEXT, --by NAME\n"
    "                       tags (the game defaults to the profile's name or else the game's title;\n"
    "                       for a sound archive, to its file name without the extension)\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

// What WSF's model reads from `dol`, the main.dol of the game with the ID `game_id`, or nothing if `dol` holds no AX
// microcode. A loose main.dol has no game ID.
std::optional<GameCode> GameCodeOf(const std::vector<uint8_t>& dol, const std::string& game_id)
{
    const std::optional<ntdlib::Microcode> microcode = ntdlib::FindMicrocode(dol);
    if (!microcode)
    {
        return std::nullopt;
    }

    return GameCode{*microcode, ntdlib::FindSndBuild(dol), xsflib::Sha1::Hex(xsflib::Sha1::Of(dol)),
                    rip::FindEffectProfile(dol, game_id)};
}

// Notes once for each game in a run that WSF has no effect profile for it (see rip::FindEffectProfile).
void NoteNoEffectProfile(const GameCode& code)
{
    static std::set<std::string> noted;
    if (!code.effect_profile && noted.insert(code.dol_sha1).second)
    {
        std::fprintf(stderr,
                     "note: WSF has no effect profile for this game: its rips play without the game's reverb\n"
                     "      and other effects\n");
    }
}

// Lists a game's sound archives and their contents.
void ListArchives(const ntdlib::disc::GameFiles& game)
{
    const auto archives = game.SoundArchives();
    std::fprintf(stderr, "%s:\n", Counted(archives.size(), "sound archive").c_str());
    for (const auto& path : archives)
    {
        std::vector<uint8_t> bytes;
        if (!game.read_file(path, bytes))
        {
            std::fprintf(stderr, "  %s (unreadable)\n", path.c_str());
            continue;
        }

        try
        {
            const auto archive = ntdlib::rsar::SoundArchive::Load(bytes, wsf::nw4r::kRsarOptions);
            std::size_t sequences = 0, wave_sounds = 0, streams = 0;
            for (const auto& s : archive.Sounds())
            {
                sequences += s.type == ntdlib::rsar::SoundType::kSequence;
                wave_sounds += s.type == ntdlib::rsar::SoundType::kWaveSound;
                streams += s.type == ntdlib::rsar::SoundType::kStream;
            }

            std::fprintf(stderr, "  %s: %zu bytes, %s, %s, %s\n", path.c_str(), bytes.size(),
                         Counted(sequences, "sequence").c_str(), Counted(wave_sounds, "wave sound").c_str(),
                         Counted(streams, "stream").c_str());
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "  %s: %s\n", path.c_str(), e.what());
        }
    }
}

// Says where a profile comes from when it was derived from another release's (see rip::FindProfile).
void NoteDerivedProfile(const rip::GameProfile& profile)
{
    if (!profile.derived_from.empty())
    {
        std::fprintf(stderr,
                     "note: this profile comes from the one for %s. Its functions were found in this main.dol.\n",
                     profile.derived_from.c_str());
    }
}

// The driver profile of the main.dol whose SHA-1 is `dol_sha1`, whatever disc it comes from, or nullptr.
const rip::GameProfile* ProfileOfDol(const std::string& dol_sha1)
{
    for (const rip::GameProfile& profile : rip::KnownProfiles())
    {
        if (dol_sha1 == profile.dol_sha1)
        {
            return &profile;
        }
    }

    return nullptr;
}

// The --list output for a game of NW4R SND: its sound archives, then how WSF rips them. `profile` is the game's driver
// profile, and `other_release` the profile of another release of the game, or null.
void ListGame(const ntdlib::disc::GameFiles& game, const rip::GameProfile* profile,
              const rip::GameProfile* other_release)
{
    ListArchives(game);
    if (game.SoundArchives().empty())
    {
        std::fprintf(stderr, "%s\n", kNoSoundArchive);
    }
    else if (game.IsTitle())
    {
        std::fprintf(stderr, "a title rips in archive mode only\n");
    }
    else if (profile)
    {
        std::fprintf(stderr, "profile: %s (game mode)\n", profile->name.c_str());
        NoteDerivedProfile(*profile);
    }
    else if (other_release)
    {
        std::fprintf(stderr, "no driver profile for this release: archive mode only\n");
        NoteOtherRelease(other_release->name);
    }
    else
    {
        std::fprintf(stderr, "no driver profile for this game: archive mode only\n");
    }
}

// Notes how the game rips in `mode`, "game" or "archive", and why archive mode rips it.
void NoteMode(const std::string& mode, bool title, const rip::GameProfile* profile,
              const rip::GameProfile* other_release)
{
    if (mode == "game")
    {
        NoteDerivedProfile(*profile);
        return;
    }

    if (title)
    {
        std::fprintf(stderr, "%s", kTitleNotice);
        return;
    }

    if (other_release)
    {
        std::fprintf(stderr, "no driver profile for this release: ripping in archive mode\n");
        NoteOtherRelease(other_release->name);
    }
    else if (!profile)
    {
        std::fprintf(stderr, "no driver profile for this game: ripping in archive mode\n");
    }

    std::fprintf(stderr, "%s", profile ? kArchiveModeChosenNotice : kArchiveModeNotice);
}

// What RipArchives did with a game's sound archives.
struct ArchivesRipped
{
    int files = 0;           // the .miniwsf files written
    std::size_t matched = 0; // the sounds that --only and --bgm selected
    bool failed = false;     // whether an archive failed to rip
    bool rejected = false;   // whether the model couldn't drive the game's AX microcode
};

// Rips the game's sound archives `archives`, or the one that --archive names, in `mode`. Each archive's set goes in a
// folder of its own in `out_dir`. The folder takes its registry of file names from `registries`. An archive that fails
// doesn't stop the others.
ArchivesRipped RipArchives(const fs::path& out_dir, const ntdlib::disc::GameFiles& game,
                           const std::vector<std::string>& archives, const std::string& mode,
                           const rip::GameProfile* profile, const GameCode& code, const std::string& game_name,
                           const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    ArchivesRipped ripped;
    const std::vector<std::string> folders = xsflib::rip::ArchiveFolders(archives);
    for (std::size_t a = 0; a < archives.size(); a++)
    {
        const std::string& path = archives[a];
        if (!o.only_archive.empty() && path != o.only_archive)
        {
            continue;
        }

        std::vector<uint8_t> archive_bytes;
        if (!game.read_file(path, archive_bytes))
        {
            std::fprintf(stderr, "error: can't read %s\n", path.c_str());
            ripped.failed = true;
            continue;
        }

        const fs::path archive_dir = out_dir / folders[a];
        xsflib::rip::FileNames& names = xsflib::cli::NamesIn(registries, archive_dir, ".wsflib", ".miniwsf");
        const std::string lib_base = game_name + " " + folders[a];
        int n = 0;
        try
        {
            n = mode == "game"
                    ? RipArchiveInGameMode(archive_dir, names, lib_base, game, *profile, path, archive_bytes, code,
                                           game_name, o, ripped.matched)
                    : RipArchive(archive_dir, names, lib_base, path, archive_bytes, code, game_name, o, ripped.matched);
        }
        catch (const xsflib::FormatError& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", path.c_str(), e.what());
            n = -1;
        }

        if (n < 0)
        {
            ripped.failed = true;
            ripped.rejected |= n == kMicrocodeRejected;
            continue;
        }

        ripped.files += n;
    }

    return ripped;
}

// Rips `game` from `input` if a JAudio profile gives it (see RipGame). Returns the exit code, or nothing for a game
// that no JAudio profile gives.
std::optional<int> RipProfiledJAudioGame(const std::string& input, const fs::path& out_dir,
                                         const ntdlib::disc::GameFiles& game, const Options& o,
                                         xsflib::cli::FileNameRegistries& registries)
{
    const std::optional<rip::JAudioProfile> jaudio = rip::FindJAudioProfile(game);
    if (!jaudio)
    {
        return std::nullopt;
    }

    std::string error;
    const std::optional<rip::JAudioFiles> files = rip::FindJAudioFiles(game, *jaudio, &error);
    if (!files)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    return RipJAudioGame(input, out_dir, game, *files, &*jaudio, o, registries);
}

// The exit code for a rip in `mode` that the options or a missing driver profile refuse, after its error. Nothing when
// the rip can go on.
std::optional<int> RefuseGameMode(const std::string& mode, const rip::GameProfile* profile, const Options& o)
{
    if (const std::string given = o.ArchiveOptionsGiven(); mode == "game" && !given.empty())
    {
        std::fprintf(stderr, "error: %s for archive mode. Add --mode archive.\n", given.c_str());
        return 2;
    }

    if (mode == "game" && !profile)
    {
        std::fprintf(stderr,
                     "error: no driver profile for this game (known: %s); --mode archive rips its sound archives "
                     "instead\n",
                     ProfileNames(rip::KnownProfiles()).c_str());
        return 1;
    }

    return std::nullopt;
}

// Reports what RipGame ripped from `input` into `out_dir`, with a note when game mode can rip the game or another
// release of it. Returns the exit code.
int ReportGameRip(const std::string& input, const fs::path& out_dir, const ArchivesRipped& ripped,
                  const rip::GameProfile* profile, const rip::GameProfile* other_release, const Options& o)
{
    if (ripped.rejected && profile)
    {
        std::fprintf(stderr, "note: game mode can rip this game. Leave out --mode archive.\n");
    }
    else if (ripped.rejected && other_release)
    {
        std::fprintf(stderr, "note: game mode can rip the release with the driver profile: %s.\n",
                     other_release->name.c_str());
    }

    if (ripped.matched == 0 && (!o.only_text.empty() || o.bgm) && !ripped.failed)
    {
        return NoSoundMatches(o);
    }

    std::fprintf(stderr, "%s: %s\n", xsflib::rip::InputPath(input).filename().string().c_str(),
                 Counted(ripped.files, "miniwsf file").c_str());
    if (ripped.files > 0)
    {
        xsflib::rip::ReportOutput(out_dir);
    }

    return ripped.failed ? 1 : 0;
}

// Rips a game image or folder: each of its sound archives, or the one that --archive names. Uses game mode if a driver
// profile exists. Otherwise uses archive mode. --mode overrides the choice. Each archive's folder takes its registry of
// file names from `registries` (see xsflib::cli::NamesIn). RipJAudioGame rips a JAudio game. Returns the exit code.
int RipGame(const std::string& input, const fs::path& out_dir, const Options& o,
            xsflib::cli::FileNameRegistries& registries)
{
    ntdlib::disc::GameFiles game;
    if (const auto error = ntdlib::disc::LoadGame(fs::path(input), game, rip::DiscOptions()))
    {
        std::fprintf(stderr, "error: %s\n", error->c_str());
        return 1;
    }

    std::fprintf(stderr, "%s, \"%s\" (%s)\n", game.GameId().c_str(), game.Title().c_str(), game.source.c_str());

    // Only archive mode rips a title. The ripper looks up no driver profile for one. A disc with a JAudio profile is a
    // JAudio game. So is a disc that has no profile for NW4R SND and holds JAudio2's start-up archive. The sound
    // archives of NW4R SND on such a disc, like the Home Button menu's archive, aren't the game's music.
    const bool title = game.IsTitle();
    const std::optional<int> jaudio = title ? std::nullopt : RipProfiledJAudioGame(input, out_dir, game, o, registries);
    if (jaudio)
    {
        return *jaudio;
    }

    const std::optional<rip::GameProfile> found = title ? std::nullopt : rip::FindProfile(game);
    const rip::GameProfile* profile = found ? &*found : nullptr;
    if (!title && !profile)
    {
        if (const std::optional<rip::JAudioFiles> files = rip::FindJAudio2Files(game))
        {
            return RipJAudioGame(input, out_dir, game, *files, nullptr, o, registries);
        }
    }

    const rip::GameProfile* other_release =
        profile || title ? nullptr : ProfileOfOtherRelease(rip::KnownProfiles(), game.GameId());
    if (o.list)
    {
        ListGame(game, profile, other_release);
        return 0;
    }

    if (title && o.mode == "game")
    {
        std::fprintf(stderr, "error: a title rips in archive mode only. Leave out --mode game.\n");
        return 2;
    }

    // Without a sound archive of NW4R SND, a game that isn't a JAudio2 game holds nothing that WSF rips.
    const auto archives = game.SoundArchives();
    if (archives.empty())
    {
        std::fprintf(stderr, "error: %s\n", kNoSoundArchive);
        return 1;
    }

    const std::optional<GameCode> code = GameCodeOf(game.dol, game.GameId());
    if (!code)
    {
        std::fprintf(stderr, "error: the game's main.dol has no AX microcode\n");
        return 1;
    }

    const std::string mode = o.mode.empty() ? (profile ? "game" : "archive") : o.mode;
    if (const std::optional<int> refused = RefuseGameMode(mode, profile, o))
    {
        return *refused;
    }

    NoteMode(mode, title, profile, other_release);

    if (!o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end())
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
        return 1;
    }

    NoteNoEffectProfile(*code);

    const std::string game_name = o.game_name.empty() ? (profile ? profile->name : game.Title()) : o.game_name;
    const ArchivesRipped ripped = RipArchives(out_dir, game, archives, mode, profile, *code, game_name, o, registries);

    return ReportGameRip(input, out_dir, ripped, profile, other_release, o);
}

// Reads AX's microcode and the build of NW4R SND from a game's code (see GameCodeOf). `path` is a main.dol, a main
// program compressed with the CX library (a WiiWare title's content 1), or a folder that holds the game: an extracted
// disc or an unpacked title. Only a folder gives the game's ID. Prints an error and returns nothing if the code can't
// be read or holds no AX microcode.
std::optional<GameCode> ReadGameCode(const std::string& path)
{
    std::vector<uint8_t> dol;
    std::string game_id;
    std::error_code ec;
    if (fs::is_directory(xsflib::rip::InputPath(path), ec))
    {
        ntdlib::disc::GameFiles game;
        if (const auto error = ntdlib::disc::LoadGame(xsflib::rip::InputPath(path), game, rip::DiscOptions()))
        {
            std::fprintf(stderr, "error: %s: %s\n", path.c_str(), error->c_str());
            return std::nullopt;
        }

        dol = std::move(game.dol);
        game_id = game.GameId();
    }
    else if (!xsflib::psf::ReadWholeFile(path, dol))
    {
        std::fprintf(stderr, "error: can't read %s\n", path.c_str());
        return std::nullopt;
    }
    else if (ntdlib::disc::IsCx(dol))
    {
        try
        {
            dol = ntdlib::disc::DecompressCx(dol, kMaxMainProgramSize);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", path.c_str(), e.what());
            return std::nullopt;
        }
    }

    std::optional<GameCode> code = GameCodeOf(dol, game_id);
    if (!code)
    {
        std::fprintf(stderr, "error: %s has no AX microcode\n", path.c_str());
    }

    return code;
}

// The game's code for the loose sound archive `input`. The main.dol comes from --microcode or the inputs, or else from
// the archive's folder. `file` gives the main.dol's path.
std::optional<GameCode> LooseArchiveCode(const std::string& input, const Options& o, std::string& file)
{
    file = o.microcode_file;
    if (o.microcode)
    {
        return o.microcode;
    }

    const fs::path beside = xsflib::rip::InputPath(input).parent_path() / "main.dol";
    std::error_code ec;
    if (!fs::is_regular_file(beside, ec))
    {
        return std::nullopt;
    }

    file = beside.string();

    return ReadGameCode(file);
}

// Rips a loose sound archive in archive mode into `out_dir`. `names` is the registry of file names for `out_dir`.
// Returns the exit code.
int RipLooseArchive(const std::string& input, const fs::path& out_dir, const Options& o, xsflib::rip::FileNames& names)
{
    const std::string file_name = xsflib::rip::InputPath(input).filename().string();
    if (o.mode == "game")
    {
        std::fprintf(stderr, "error: %s: game mode requires the game itself, not just its sound archive\n",
                     file_name.c_str());
        return 2;
    }
    if (!o.only_archive.empty())
    {
        std::fprintf(stderr, "error: %s is already a sound archive; --archive selects one of a game's sound archives\n",
                     input.c_str());
        return 2;
    }

    std::vector<uint8_t> archive_bytes;
    if (!xsflib::psf::ReadWholeFile(input, archive_bytes))
    {
        std::fprintf(stderr, "error: can't read %s\n", input.c_str());
        return 1;
    }

    std::string microcode_file;
    const std::optional<GameCode> code = LooseArchiveCode(input, o, microcode_file);

    if (o.list)
    {
        std::fprintf(stderr, "microcode: %s\n",
                     code ? microcode_file.c_str() : "none found; ripping requires the main.dol of the archive's game");
        std::fprintf(stderr, "%s: a sound archive (archive mode)\n", input.c_str());
        return 0;
    }

    if (!code)
    {
        std::fprintf(stderr,
                     "error: %s: a sound archive doesn't contain AX's microcode. Give the main.dol of the\n"
                     "       game it comes from with the archive (or with --microcode), or put that main.dol in\n"
                     "       the archive's folder. For an archive from a WiiWare game or a channel, give the\n"
                     "       unpacked title with --microcode. The rip stores a copy of the microcode.\n",
                     file_name.c_str());
        return 1;
    }

    std::fprintf(stderr, "microcode: from %s\n", microcode_file.c_str());
    std::fprintf(stderr, "%s", kArchiveFileNotice);
    NoteNoEffectProfile(*code);

    const std::string game_name = o.game_name.empty() ? xsflib::rip::NameOfInput(input) : o.game_name;
    std::size_t matched = 0;
    const int count = RipArchive(out_dir, names, game_name, file_name, archive_bytes, *code, game_name, o, matched);
    const rip::GameProfile* profile = ProfileOfDol(code->dol_sha1);
    if (count == kMicrocodeRejected && profile)
    {
        std::fprintf(stderr, "note: game mode can rip %s. Rip its disc image or extracted folder instead.\n",
                     profile->name.c_str());
    }

    if (count < 0)
    {
        return 1;
    }

    if (matched == 0 && (!o.only_text.empty() || o.bgm))
    {
        return NoSoundMatches(o);
    }

    if (count > 0)
    {
        xsflib::rip::ReportOutput(out_dir);
    }

    return 0;
}

// True for a main.dol. The .dol extension identifies it. No disc image or sound archive uses that extension.
bool IsDol(const std::string& path)
{
    return xsflib::AsciiLower(fs::path(path).extension().string()) == ".dol";
}

// Removes the main.dol files from the inputs. Loose sound archives require the AX microcode and the build of NW4R SND
// in them. A main.dol among the inputs takes the place of --microcode. Returns 0, or the exit code.
int TakeDolInputs(std::vector<std::string>& inputs, Options& o)
{
    const auto dols_begin =
        std::stable_partition(inputs.begin(), inputs.end(), [](const auto& i) { return !IsDol(i); });
    const std::vector<std::string> dols(dols_begin, inputs.end());
    inputs.erase(dols_begin, inputs.end());

    if (dols.size() + !o.microcode_file.empty() > 1)
    {
        std::fprintf(stderr, "error: give only one main.dol\n");
        return 2;
    }

    if (!dols.empty())
    {
        o.microcode_file = dols[0];
        o.microcode = ReadGameCode(dols[0]);
        if (!o.microcode)
        {
            return 1;
        }
    }

    if (inputs.empty())
    {
        std::fprintf(stderr,
                     "error: %s is a main.dol. It only provides AX's microcode and the build of NW4R SND for sound "
                     "archives. Give it together with the archives.\n",
                     dols[0].c_str());
        return 2;
    }

    return 0;
}

// Rips one input. The output folders take their registries of file names from `registries`. Returns the exit code.
int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    const std::string head = xsflib::rip::ReadHead(input, 4);
    if (head.starts_with("CSAR") || head.starts_with("FSAR"))
    {
        std::fprintf(stderr, "error: %s is a %s sound archive; WSF rips Wii sound archives (RSAR, .brsar)\n",
                     input.c_str(), head.starts_with("CSAR") ? "3DS" : "Wii U or Switch");
        return 1;
    }

    const fs::path out_dir = o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_wsf") : fs::path(o.out_dir);
    if (head.starts_with("RSAR"))
    {
        return RipLooseArchive(input, out_dir, o, xsflib::cli::NamesIn(registries, out_dir, ".wsflib", ".miniwsf"));
    }

    return RipGame(input, out_dir, o, registries);
}

// Parses one of wsfrip's options: --microcode, --tracks, --tempo, --var and --suffix. `next` gives the option's
// value. Returns 0, 2 after an error, or -1 for an option that isn't wsfrip's.
int ParseOption(const std::string& option, const std::function<std::string()>& next, Options& o)
{
    if (option == "--microcode")
    {
        o.microcode_file = next();
    }
    else if (option == "--tracks")
    {
        const std::string value = next();
        o.tracks = ParseTracks(value);
        if (!o.tracks)
        {
            std::fprintf(stderr,
                         "error: --tracks takes track numbers from 0 to 15 and ranges such as 8-14, separated by "
                         "commas, not '%s'\n",
                         value.c_str());
            return 2;
        }
    }
    else if (option == "--tempo")
    {
        o.tempo_text = next();
        o.tempo = ParseTempo(o.tempo_text);
        if (!o.tempo)
        {
            std::fprintf(stderr, "error: --tempo takes a number from 0.25 to 4, such as 1.1, not '%s'\n",
                         o.tempo_text.c_str());
            return 2;
        }
    }
    else if (option == "--var")
    {
        const std::string value = next();
        const std::optional<xsflib::Assignment> assignment = xsflib::ParseAssignment(value);
        if (!assignment || assignment->index < 0 || assignment->index > 31 || assignment->value < INT16_MIN ||
            assignment->value > INT16_MAX)
        {
            std::fprintf(stderr,
                         "error: --var takes N=V, with N from 0 to 31 and V from -32768 to 32767, not '%s'\n",
                         value.c_str());
            return 2;
        }

        o.variables[static_cast<int>(assignment->index)] = static_cast<int16_t>(assignment->value);
    }
    else if (option == "--suffix")
    {
        o.suffix = next();
    }
    else
    {
        return -1;
    }

    return 0;
}

} // namespace
} // namespace wsf::cli

int main(int argc, char** argv)
{
    xsflib::cli::RipTool<wsf::cli::Options> tool;
    tool.name = "wsfrip";
    tool.version = WSF_VERSION;
    tool.usage = wsf::cli::kUsage;

    // Each analysis emulates 88 MiB of main memory. A 32-bit build has only 2 GiB of address space. It runs at most 8
    // analyses.
    tool.max_jobs_32bit = 8;

    tool.parse_option = wsf::cli::ParseOption;

    // The main.dol from --microcode, then the main.dol files among the inputs.
    tool.before_inputs = [](std::vector<std::string>& inputs, wsf::cli::Options& o)
    {
        if (!o.microcode_file.empty())
        {
            o.microcode = wsf::cli::ReadGameCode(o.microcode_file);
            if (!o.microcode)
            {
                return 1;
            }
        }

        return wsf::cli::TakeDolInputs(inputs, o);
    };

    tool.rip_input = wsf::cli::RipInput;

    return xsflib::cli::RunRipTool(tool, argc, argv);
}
