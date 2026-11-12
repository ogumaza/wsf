// SPDX-License-Identifier: MIT

// The effect profiles of the Wii games that WSF rips: what each game's code puts on the aux buses for its sounds (see
// profiles.h).

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/dol.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/music.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/rvl/snd/effect_setup.h"
#include "ntdlib/rvl/snd/game_setups.h"
#include "xsflib/ascii.h"
#include "xsflib/sha1.h"

#include "rip/dol_search.h"
#include "rip/profiles.h"
#include "rip/rip.h"
#include "wsf/models.h"

namespace wsf::rip
{
namespace
{

// The main.dol files of the games with profiles (ntdlib/rvl/snd/game_setups.h). Each of the six games has a driver
// profile (see KnownProfiles) and an effect profile (see KnownEffectProfiles). The five channels and Pokémon Rumble
// have an effect profile and no driver profile. They rip in archive mode only.
using nw4r::kCityFolkSha1;
using nw4r::kForecastChannelSha1;
using nw4r::kMarioKartSha1;
using nw4r::kMiiChannelSha1;
using nw4r::kNewsChannelSha1;
using nw4r::kPhotoChannelSha1;
using nw4r::kPokemonRumbleSha1;
using nw4r::kSmoothMovesSha1;
using nw4r::kWiiMusicSha1;
using nw4r::kWiiPlaySha1;
using nw4r::kWiiShopChannelSha1;
using nw4r::kWiiSportsSha1;

// An effect's parameters as the words of its parameter structure: floats as their bits.
nw4r::EffectParams Words(std::initializer_list<float> values)
{
    nw4r::EffectParams params{};
    std::size_t i = 0;
    for (const float value : values)
    {
        params[i++] = std::bit_cast<uint32_t>(value);
    }

    return params;
}

nw4r::EffectSetup Fixed(int bus, nw4r::EffectType type, const nw4r::EffectParams& params)
{
    nw4r::EffectSetup setup;
    setup.bus = bus;
    setup.type = type;
    setup.params = params;

    return setup;
}

// FxReverbHi's parameters: preDelayTime, fusedTime, coloration, damping, crossTalk and outGain.
nw4r::EffectSetup ReverbHi(float pre_delay, float time, float coloration, float damping, float crosstalk, float out)
{
    return Fixed(0, nw4r::EffectType::kReverbHi, Words({pre_delay, time, coloration, damping, crosstalk, out}));
}

// FxChorus's default parameters (FxChorus::FxChorus, 0x80039d88): delayTime 10 ms, depth 0.5 and rate 1 Hz.
nw4r::EffectSetup DefaultChorus(int bus)
{
    return Fixed(bus, nw4r::EffectType::kChorus, Words({10.0f, 0.5f, 1.0f}));
}

// The modes and gains of the reverbs of 2008: earlyMode, preDelayTimeMax, fusedMode, earlyGain and fusedGain.
struct Reverb2008
{
    uint32_t early_mode;
    float pre_delay_time_max;
    uint32_t fused_mode;
    float early_gain;
    float fused_gain;
};

// Puts a 2008 reverb's outGain, modes and gains into `params`, at the places that `words` gives.
void PutReverb2008(nw4r::EffectParams& params, const nw4r::Reverb2008Words& words, float out, const Reverb2008& more)
{
    const auto put = [&params](int word, uint32_t value)
    {
        params[static_cast<std::size_t>(word)] = value;
    };

    put(words.out_gain, std::bit_cast<uint32_t>(out));
    put(words.early_mode, more.early_mode);
    put(words.pre_delay_time_max, std::bit_cast<uint32_t>(more.pre_delay_time_max));
    put(words.fused_mode, more.fused_mode);
    put(words.early_gain, std::bit_cast<uint32_t>(more.early_gain));
    put(words.fused_gain, std::bit_cast<uint32_t>(more.fused_gain));
}

// FxReverbHi of 2008 on bus A: preDelayTime, fusedTime, coloration, damping, crossTalk, outGain, then the modes and
// gains.
nw4r::EffectSetup ReverbHi2008(float pre_delay, float time, float coloration, float damping, float crosstalk, float out,
                               const Reverb2008& more)
{
    nw4r::EffectParams params = Words({pre_delay, time, coloration, damping, crosstalk});
    PutReverb2008(params, nw4r::kReverbHi2008Words, out, more);

    return Fixed(0, nw4r::EffectType::kReverbHi2008, params);
}

// FxReverbStd of 2008 on bus A: preDelayTime, fusedTime, coloration, damping, outGain, then the modes and gains.
nw4r::EffectSetup ReverbStd2008(float pre_delay, float time, float coloration, float damping, float out,
                                const Reverb2008& more)
{
    nw4r::EffectParams params = Words({pre_delay, time, coloration, damping});
    PutReverb2008(params, nw4r::kReverbStd2008Words, out, more);

    return Fixed(0, nw4r::EffectType::kReverbStd2008, params);
}

// The sounds of `archive` whose names `matches` accepts.
template <typename Matches>
std::vector<uint32_t> SoundsWhere(const ntdlib::rsar::SoundArchive& archive, Matches matches)
{
    std::vector<uint32_t> sounds;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        if (matches(archive.Sounds()[i].name))
        {
            sounds.push_back(i);
        }
    }

    return sounds;
}

// The sounds of `archive` with one of `names`.
std::vector<uint32_t> SoundsNamed(const ntdlib::rsar::SoundArchive& archive,
                                  std::initializer_list<std::string_view> names)
{
    const auto is_named = [names](const std::string& sound)
    {
        return std::find(names.begin(), names.end(), sound) != names.end();
    };
    return SoundsWhere(archive, is_named);
}

// The file name of the archive at `path`, in lower case.
std::string ArchiveFileName(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return xsflib::AsciiLower(slash == std::string::npos ? path : path.substr(slash + 1));
}

// The effect variant that plays `variant` of sound `sound`. The variant's name is its state after a space.
nw4r::EffectVariant TrackVariantOf(uint32_t sound, const nw4r::TrackVariant& variant)
{
    nw4r::Arrangement arrangement;
    arrangement.tracks = variant.tracks;
    arrangement.tempo_ratio = variant.tempo_ratio;

    return {.name = " " + std::string(variant.state),
            .sounds = {sound},
            .setups = {},
            .sends = {},
            .arrangement = arrangement};
}

// Adds to `found` what a game sets up for the sounds of `archive` that the tables name, in the order of the sounds:
// the tracks that each sound starts with, its variants and the values of its variables.
void AddSoundSetups(const ntdlib::rsar::SoundArchive& archive, std::span<const nw4r::StartTracks> starts,
                    std::span<const nw4r::TrackVariant> variants, std::span<const nw4r::StartVariable> values,
                    nw4r::ArchiveEffects& found)
{
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const std::string& sound = archive.Sounds()[i].name;
        for (const nw4r::StartTracks& start : starts)
        {
            if (sound == start.sound)
            {
                found.tracks.push_back({i, start.tracks});
            }
        }

        for (const nw4r::TrackVariant& variant : variants)
        {
            if (sound == variant.sound)
            {
                found.variants.push_back(TrackVariantOf(i, variant));
            }
        }

        std::map<int, int16_t> written;
        for (const nw4r::StartVariable& value : values)
        {
            if (sound == value.sound)
            {
                written[value.variable] = value.value;
            }
        }

        if (!written.empty())
        {
            found.variables.push_back({.sound = i, .variables = std::move(written)});
        }
    }
}

// The effects of Wii Sports. The menus have NW4R SND's default reverb on bus A (RPSndAudioMgr::setDefaultFx,
// 0x801b6d00, with the parameters at 0x8037ae68). Each sport sets bus A again as its scene starts. Tennis (0x80214e40),
// baseball (0x8024e7dc) and bowling (0x803153c0) keep that reverb and add FxChorus's default on bus B. Golf
// (0x8028e664) and boxing (0x802ee324) have reverbs with other times and levels. Golf writes the wind's speed and
// direction to the variables of its wind, SE_LV_Wind. The rip plays a speed of 8, the middle of the speeds that sound,
// and direction 0 (nw4r::kWiiSportsWind).
nw4r::ArchiveEffects WiiSportsEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects found;
    const nw4r::EffectSetup menu = ReverbHi(0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f);
    if (name == "rp_common_sports.brsar")
    {
        found.setups = {menu};
    }
    else if (name == "rp_tns_sound.brsar" || name == "rp_bsb_sound.brsar" || name == "rp_bow_sound.brsar")
    {
        found.setups = {menu, DefaultChorus(1)};
    }
    else if (name == "rp_gol_sound.brsar")
    {
        found.setups = {ReverbHi(0.02f, 1.2f, 0.6f, 0.4f, 0.1f, 0.7f)};
        AddSoundSetups(archive, {}, {}, nw4r::kWiiSportsWind, found);
    }
    else if (name == "rp_box_sound.brsar")
    {
        found.setups = {ReverbHi(0.02f, 2.2f, 0.6f, 0.4f, 0.1f, 0.5f)};
    }

    return found;
}

// The effects of Wii Play. Every scene has the same effects (setDefaultFx, 0x801b3588, with the parameters at
// 0x803182c0): a reverb on bus A, a chorus on bus B and a delay on bus C. Six minigames also change the tracks of their
// music as a game goes on. A rip plays the tracks that sound as the music starts (nw4r::kWiiPlayStarts). The piece also
// rips once with each later set of tracks (nw4r::kWiiPlayVariants). The variants play at the sound's full volume, where
// Billiards halves it in its states 2 and 3. Table Tennis also sets the tempo of a rally (0x80235540). The rips play at
// the ratio of 1 that the piece starts with (0x80235978).
nw4r::ArchiveEffects WiiPlayEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects found;
    if (!name.starts_with("rp_"))
    {
        return found;
    }

    found.setups = {ReverbHi(0.03f, 1.5f, 0.0f, 0.1f, 0.2f, 0.5f),
                    Fixed(1, nw4r::EffectType::kChorus, Words({5.0f, 1.0f, 0.6f})),
                    Fixed(2, nw4r::EffectType::kDelay, Words({40.0f, 0.0f, 0.6f}))};
    AddSoundSetups(archive, nw4r::kWiiPlayStarts, nw4r::kWiiPlayVariants, {}, found);

    return found;
}

// The effects of WarioWare: Smooth Moves. The start-up of the sound object (0x80100198) puts a delay on bus B
// (parameters at 0x804da1e4) and a chorus on bus C (0x804da25c). The music's variables pick bus A's reverb from the 10
// rows at 0x804da11c: preDelay, time, coloration, damping and mix.
//
// Bus A keeps the reverb of the last piece of music that gave variable 0 a value. A sequence's variables start at -1
// (0x800e58c8). A value of -1 leaves the bus as it is (0x80100e40). The opening of each stage and the music between its
// microgames pick a row or clear the bus. The microgames' music picks nothing. Dribble and Spitz's stage plays their
// songs between its microgames (CStageDribble, 0x8016b6f4). The songs pick nothing either. In the game, each of these
// pieces plays through its stage's reverb. A rip of such a piece plays the reverb of the stage that plays it first in
// the story. For every microgame, that's the stage of its character: bits 0 to 9 of the flags at +0xc of its record in
// the table at 0x804dd970. Each character's boss ends that character's stage. The stage switch's table at 0x80547568
// gives each stage's setup. The setups give the lists of microgames. The remix stages of Jimmy T., Jimmy P., Tiny Wario
// and Dr. Crygor play some of the microgames again later. The openings of the stages of Wario, Mona, Kat and Ana,
// Dribble and Spitz and Orbulon give variable 0 the value 1: the first row. Ashley's gives 4: the fourth row. The
// stages of Young Cricket, Penny, 9-Volt and Tiny Wario clear the bus. Their pieces rip without the reverb. The lists
// hold only pieces that send to bus A.
nw4r::ArchiveEffects SmoothMovesEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects found;
    if (name == "sound_data_jp.brsar")
    {
        nw4r::EffectSetup reverb;
        reverb.type = nw4r::EffectType::kReverbStd;
        reverb.presets = {Words({0.02f, 3.0f, 0.6f, 0.4f, 0.25f}), Words({0.02f, 1.5f, 0.6f, 0.4f, 0.25f}),
                          Words({0.02f, 3.0f, 0.6f, 0.4f, 0.25f}), Words({0.03f, 3.0f, 0.5f, 0.4f, 0.5f}),
                          Words({0.02f, 1.5f, 0.6f, 0.4f, 0.25f}), Words({0.03f, 2.0f, 0.6f, 0.4f, 0.3f}),
                          Words({0.08f, 3.0f, 0.6f, 0.4f, 0.25f}), Words({0.04f, 3.0f, 0.6f, 0.4f, 0.25f}),
                          Words({0.04f, 3.0f, 0.6f, 0.4f, 0.25f}), Words({0.08f, 3.0f, 0.6f, 0.4f, 0.25f})};
        for (uint32_t i = 0; i < archive.Sounds().size(); i++)
        {
            if (ntdlib::rsar::IsMusic(archive, i))
            {
                reverb.sounds.push_back(i);
            }
        }

        // The pieces that play first in each stage whose opening gives variable 0 a value.
        const auto give = [&found](const std::vector<uint32_t>& sounds, int16_t value)
        {
            for (const uint32_t sound : sounds)
            {
                found.variables.push_back({.sound = sound, .variables = {{0, value}}});
            }
        };
        const auto is_song = [](const std::string& sound)
        {
            return sound.starts_with("SEQ_M_PETIT_DRIBBLE_SONG_");
        };
        // Wario's stage.
        give(SoundsNamed(archive,
                         {"SEQ_M_BOSS_MATCHLESS1_A", "SEQ_M_BOSS_MATCHLESS1_A_OK", "SEQ_M_BOSS_MATCHLESS1_B",
                          "SEQ_M_BOSS_MATCHLESS1_B_OK", "SEQ_M_PETIT_BARBER1", "SEQ_M_PETIT_FENCING1",
                          "SEQ_M_PETIT_MASTER1", "SEQ_M_PETIT_MATCHLESS1", "SEQ_M_PETIT_MONSTER1", "SEQ_M_PETIT_NOSE1",
                          "SEQ_M_PETIT_PITFALL1", "SEQ_M_PETIT_SMELLTHING1", "SEQ_M_PETIT_WOK1"}),
             1);
        // Mona's stage.
        give(SoundsNamed(
                 archive,
                 {"SEQ_M_BOSS_TOILET1", "SEQ_M_BOSS_TOILET1_OK", "SEQ_M_PETIT_BATON1", "SEQ_M_PETIT_CLOCK1",
                  "SEQ_M_PETIT_CUPBALL1", "SEQ_M_PETIT_DRAFT1", "SEQ_M_PETIT_FLYBEATING1", "SEQ_M_PETIT_GONDOLA1",
                  "SEQ_M_PETIT_LONGRADISH1", "SEQ_M_PETIT_PANCAKE1", "SEQ_M_PETIT_PIZZA1", "SEQ_M_PETIT_PUPPET1",
                  "SEQ_M_PETIT_RESCUE9111", "SEQ_M_PETIT_RIBBON1", "SEQ_M_PETIT_RUNNER1", "SEQ_M_PETIT_TAKEIT1"}),
             1);
        // Kat and Ana's stage.
        give(SoundsNamed(
                 archive,
                 {"SEQ_M_BOSS_DRIVE1",     "SEQ_M_BOSS_DRIVE1_OK", "SEQ_M_PETIT_ALLANGLE1",   "SEQ_M_PETIT_BILL1",
                  "SEQ_M_PETIT_CAT1",      "SEQ_M_PETIT_COOKIE1",  "SEQ_M_PETIT_DRIVE1",      "SEQ_M_PETIT_FALSETOOTH1",
                  "SEQ_M_PETIT_KATANUKI1", "SEQ_M_PETIT_LIFTING1", "SEQ_M_PETIT_PAPERPLANE1", "SEQ_M_PETIT_POLICE1",
                  "SEQ_M_PETIT_PUSHING1",  "SEQ_M_PETIT_ROAST1",   "SEQ_M_PETIT_SAMISEN1",    "SEQ_M_PETIT_SAMISEN1_OK",
                  "SEQ_M_PETIT_SCAN1",     "SEQ_M_PETIT_SPIT1",    "SEQ_M_PETIT_WAVE1_L1",    "SEQ_M_PETIT_WAVE1_L2",
                  "SEQ_M_PETIT_WAVE1_L3",  "SEQ_M_PETIT_WAVE1_OK", "SEQ_M_PETIT_WAVE2_OK",    "SEQ_M_PETIT_WAVE3_OK",
                  "SEQ_M_PETIT_WOOL1"}),
             1);
        // Dribble and Spitz's stage.
        give(SoundsNamed(archive, {"SEQ_M_BOSS_CHONMAGEACTION1_A",
                                   "SEQ_M_BOSS_CHONMAGEACTION1_A_OK",
                                   "SEQ_M_BOSS_CHONMAGEACTION1_B",
                                   "SEQ_M_BOSS_CHONMAGEACTION1_B_OK",
                                   "SEQ_M_PETIT_ARMWRESTLING1",
                                   "SEQ_M_PETIT_ARMWRESTLING1_OK",
                                   "SEQ_M_PETIT_BELL1",
                                   "SEQ_M_PETIT_CLIPPER1",
                                   "SEQ_M_PETIT_EAT1",
                                   "SEQ_M_PETIT_EAT1_OK",
                                   "SEQ_M_PETIT_MAINDISH1",
                                   "SEQ_M_PETIT_MAINDISH21",
                                   "SEQ_M_PETIT_PENLIGHT1",
                                   "SEQ_M_PETIT_PINGPONG1",
                                   "SEQ_M_PETIT_SERVING1",
                                   "SEQ_M_PETIT_SIDETHROW1",
                                   "SEQ_M_PETIT_SPRAY1",
                                   "SEQ_M_PETIT_SPRING1",
                                   "SEQ_M_PETIT_SWEEPING1",
                                   "SEQ_M_PETIT_UFO1",
                                   "SEQ_M_PETIT_UMBRELLACAT1",
                                   "SEQ_M_PETIT_ZAZEN1"}),
             1);
        // Dribble and Spitz's songs.
        give(SoundsWhere(archive, is_song), 1);
        // Orbulon's stage.
        give(
            SoundsNamed(archive, {"SEQ_M_BOSS_BOXING1",          "SEQ_M_BOSS_BOXING1_OK",   "SEQ_M_PETIT_BICYCLE1",
                                  "SEQ_M_PETIT_BRIDGE1",         "SEQ_M_PETIT_CLOSE1",      "SEQ_M_PETIT_CLOSE1_OK",
                                  "SEQ_M_PETIT_FLAG1",           "SEQ_M_PETIT_FLAG1_OK",    "SEQ_M_PETIT_FLOG1",
                                  "SEQ_M_PETIT_FLOG1_OK",        "SEQ_M_PETIT_FROG1",       "SEQ_M_PETIT_HELP1",
                                  "SEQ_M_PETIT_HELP1_OK",        "SEQ_M_PETIT_KILLERSHOT1", "SEQ_M_PETIT_LADDER1",
                                  "SEQ_M_PETIT_MARACAS1",        "SEQ_M_PETIT_MARACAS1_OK", "SEQ_M_PETIT_MONKEYDANCE1",
                                  "SEQ_M_PETIT_MONKEYDANCE1_OK", "SEQ_M_PETIT_ROWER1",      "SEQ_M_PETIT_SCISSORS1",
                                  "SEQ_M_PETIT_SCISSORS1_OK",    "SEQ_M_PETIT_STRONGBOX1",  "SEQ_M_PETIT_STRONGBOX1_OK",
                                  "SEQ_M_PETIT_WATCH1"}),
            1);
        // Ashley's stage.
        give(SoundsNamed(archive, {"SEQ_M_BOSS_BURGER1",        "SEQ_M_BOSS_BURGER1_OK",     "SEQ_M_PETIT_BELL2",
                                   "SEQ_M_PETIT_BILLIARDS1",    "SEQ_M_PETIT_BOTTLEOPENER1", "SEQ_M_PETIT_CARD1",
                                   "SEQ_M_PETIT_CHAMPAGNE1",    "SEQ_M_PETIT_DICE1",         "SEQ_M_PETIT_GEARCHANGE1",
                                   "SEQ_M_PETIT_GRATER1",       "SEQ_M_PETIT_HULAHOOP1",     "SEQ_M_PETIT_IGNITION1",
                                   "SEQ_M_PETIT_KINGSKIPPING1", "SEQ_M_PETIT_LINEUP1",       "SEQ_M_PETIT_SCULPTURE1",
                                   "SEQ_M_PETIT_SHOOTING1",     "SEQ_M_PETIT_STEALER1",      "SEQ_M_PETIT_STIR1",
                                   "SEQ_M_PETIT_UMBRELLA1",     "SEQ_M_PETIT_WORD1"}),
             4);

        found.setups = {Fixed(1, nw4r::EffectType::kDelay, Words({160.0f, 0.4f, 1.0f})),
                        Fixed(2, nw4r::EffectType::kChorus, Words({10.0f, 0.5f, 1.0f})), reverb};
    }

    return found;
}

// What Animal Crossing: City Folk sets up for its sounds. The sound object's start-up (0x801ce58c) puts FxReverbHi on
// bus A with the parameters of 2008: those of the older builds, then earlyMode, preDelayTimeMax, fusedMode, earlyGain
// and fusedGain. The start of the music (0x801cbbbc) sends 0.25 of the outdoor and town music to bus A, and 0.28 of
// K.K. Slider's songs.
//
// The outdoor and town music and BGM_MUSEUM play some of their tracks (nw4r::kCityFolkPieces). A rip plays the tracks
// that the game starts each piece with (nw4r::kCityFolkStarts). The piece also rips once more in each arrangement
// that the game gives it later (nw4r::kCityFolkVariants): the outdoor and town music in rain and in snow, and
// BGM_MUSEUM in each arrangement that a stage gives it. The rips follow the order of the tables.
nw4r::ArchiveEffects CityFolkEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects found;
    if (name != "rv_forest.brsar")
    {
        return found;
    }

    found.setups = {ReverbHi2008(0.01f, 0.5f, 0.6f, 0.4f, 0.1f, 1.0f, {4, 0.05f, 2, 0.3f, 0.3f})};
    for (const nw4r::WeatherTracks& piece : nw4r::kCityFolkPieces)
    {
        if (const std::optional<uint32_t> sound = archive.FindSound(std::string(piece.sound)))
        {
            found.sends.push_back({*sound, 0, nw4r::kCityFolkMusicSend});
        }
    }

    const auto is_song = [](const std::string& sound)
    {
        return sound.starts_with(nw4r::kCityFolkSongPrefix);
    };
    for (const uint32_t sound : SoundsWhere(archive, is_song))
    {
        found.sends.push_back({sound, 0, nw4r::kCityFolkSongSend});
    }

    for (const nw4r::StartTracks& start : nw4r::kCityFolkStarts)
    {
        if (const std::optional<uint32_t> sound = archive.FindSound(std::string(start.sound)))
        {
            found.tracks.push_back({*sound, start.tracks});
        }
    }

    for (const nw4r::TrackVariant& variant : nw4r::kCityFolkVariants)
    {
        if (const std::optional<uint32_t> sound = archive.FindSound(std::string(variant.sound)))
        {
            found.variants.push_back(TrackVariantOf(*sound, variant));
        }
    }

    return found;
}

// The effects of Mario Kart Wii. Only bus A has an effect: the standard reverb of 2008 that AudioFx::create
// (0x802119f4) makes for type 4. As each scene starts, code in the game's REL module sets it up from one set of
// parameters. A race picks the set by course (.text + 0x2027dc). Mushroom Gorge and the N64 courses Mario Raceway,
// Sherbet Land and DK's Jungle Parkway get a shorter pre-delay and less damping. The names of their sounds start with
// SE_CN_KNK_, SE_CR_64M_, SE_CR_64S_ and SE_CR_64D_. A menu picks the set by the sound type of its section (.text +
// 0x2021c0). The type is the group of the section's music. SEQ_O_SELECT_CH (group 2) and SEQ_O_EARTH (group 3) get a
// lower outGain. SEQ_O_RANKING_CH (group 4) gets another reverb. Other sounds get the set as it is. Only six of the
// archive's sequences send anything to bus A: these three, SEQ_O_OPTION_CH and two sounds of Sherbet Land.
//
// Two pieces of the menus' music play some of their tracks (nw4r::kMarioKartStarts). A rip plays state 0 of each.
// Each piece rips once more in each other state (nw4r::kMarioKartVariants). In SEQ_O_SELECT's states 1 and 2, track 0
// plays at 0.93 of its volume. An arrangement holds no volume, and the variants play the track 0.63 dB louder. Seven
// sound effects read their variable 0. A rip plays the value of the effect's first play (nw4r::kMarioKartValues).
nw4r::ArchiveEffects MarioKartEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    if (name != "revo_kart.brsar")
    {
        return {};
    }

    nw4r::ArchiveEffects found;
    const auto is_course_sound = [](const std::string& sound)
    {
        return sound.starts_with("SE_CN_KNK_") || sound.starts_with("SE_CR_64M_") || sound.starts_with("SE_CR_64S_") ||
               sound.starts_with("SE_CR_64D_");
    };

    nw4r::EffectSetup courses = ReverbStd2008(0.015f, 3.0f, 0.6f, 0.35f, 1.0f, {5, 0.02f, 5, 0.0f, 1.0f});
    courses.sounds = SoundsWhere(archive, is_course_sound);
    nw4r::EffectSetup menus = ReverbStd2008(0.02f, 3.0f, 0.6f, 0.4f, 0.73f, {5, 0.02f, 0, 0.0f, 1.0f});
    menus.sounds = SoundsNamed(archive, {"SEQ_O_SELECT_CH", "SEQ_O_EARTH"});
    nw4r::EffectSetup ranking = ReverbStd2008(0.025f, 1.5f, 0.5f, 0.5f, 1.0f, {5, 0.025f, 0, 0.0f, 0.5f});
    ranking.sounds = SoundsNamed(archive, {"SEQ_O_RANKING_CH"});
    found.setups = {ReverbStd2008(0.02f, 3.0f, 0.6f, 0.4f, 1.0f, {5, 0.02f, 0, 0.0f, 1.0f}), courses, menus, ranking};
    AddSoundSetups(archive, nw4r::kMarioKartStarts, nw4r::kMarioKartVariants, nw4r::kMarioKartValues, found);

    return found;
}

// The effects of Wii Music. Each scene sets the buses from a row of the table at 0x8059f978 (0x803cddc0). In every
// scene but Mii Maestro's, bus A has the standard reverb of 2008 with row 0 of the parameters at 0x8059fe20. Mii
// Maestro has FxReverbHi with row 2 of the parameters at 0x8059fd58. The names of its sounds start with RP_OCS_. The
// game sets the longest pre-delay of both to 0.09 (0x803cdfb4). Some scenes put a reverb on bus B too. No sequence
// sends anything to bus B there. The game sets sends on the instruments of a jam session by stage. A rip doesn't know
// the stage and leaves those sends out.
//
// The main menu and Pitch Perfect play some of the tracks of their music (nw4r::kWiiMusicStarts). A rip plays the
// tracks of the menu's state 0, at full volume, and those of Pitch Perfect's lowest levels. Each piece also rips once
// with each other set of tracks (nw4r::kWiiMusicVariants). Pitch Perfect's phrases and the jingles of its pictures
// send 0.2 to bus A (nw4r::kPitchPerfectSend).
nw4r::ArchiveEffects WiiMusicEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    if (!name.starts_with("rp_"))
    {
        return {};
    }

    nw4r::ArchiveEffects found;
    const auto is_orchestra_sound = [](const std::string& sound)
    {
        return sound.starts_with("RP_OCS_");
    };

    nw4r::EffectSetup orchestra = ReverbHi2008(0.02f, 1.2f, 0.5f, 0.3f, 0.1f, 0.9f, {5, 0.09f, 4, 0.5f, 1.0f});
    orchestra.sounds = SoundsWhere(archive, is_orchestra_sound);
    found.setups = {ReverbStd2008(0.0f, 1.3f, 0.5f, 0.5f, 1.0f, {5, 0.09f, 4, 0.5f, 1.0f}), orchestra};

    if (name != "rp_music_sound.brsar")
    {
        return found;
    }

    for (const uint32_t sound : SoundsWhere(archive, nw4r::IsPitchPerfectSendSound))
    {
        found.sends.push_back({sound, 0, nw4r::kPitchPerfectSend});
    }

    AddSoundSetups(archive, nw4r::kWiiMusicStarts, nw4r::kWiiMusicVariants, {}, found);

    return found;
}

// The effects of the Wii Shop Channel. Its sound start-up (0x80023aa0) puts FxReverbHi of 2008 on bus A (0x80023b80)
// with the parameters that 0x800239e0 sets: no pre-delay, fusedTime 2.5, coloration 0.5, no damping or crossTalk and an
// outGain of 1. The channel clears the buses as it pauses its sounds (0x800241a0) and sets the reverb up again as they
// resume (0x80024230). It sets no send on a sound.
nw4r::ArchiveEffects WiiShopChannelEffects(const std::string& name, const ntdlib::rsar::SoundArchive&)
{
    nw4r::ArchiveEffects found;
    if (name == "iplsound.brsar")
    {
        found.setups = {ReverbHi2008(0.0f, 2.5f, 0.5f, 0.0f, 0.0f, 1.0f, {5, 0.0f, 0, 0.0f, 1.0f})};
    }

    return found;
}

// The effects of the Mii Channel. The sound object's start-up (0x80017ff4) puts FxReverbHi on bus A through EGG's
// AudioFxMgr (0x80151424). The parameters at 0x80206878 give no pre-delay, fusedTime 3, coloration 0.5, no damping or
// crossTalk and an outGain of 0.25. Each of the channel's starters of sound effects sends 0.2 of its sound to bus A.
// The music, SEQ_TMP_BGM, starts without a send. A rip plays what the channel plays from boot: every track of the
// music but 10 and 11 (nw4r::kMiiChannelStarts). The music also rips once in each of its other states
// (nw4r::kMiiChannelVariants). The channel also turns the music down to a volume of 0.4. An arrangement holds no
// volume. That state therefore has no rip.
nw4r::ArchiveEffects MiiChannelEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects found;
    if (name != "nigaoesound.brsar")
    {
        return found;
    }

    found.setups = {ReverbHi(0.0f, 3.0f, 0.5f, 0.0f, 0.0f, 0.25f)};
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        if (archive.Sounds()[i].name != nw4r::kMiiChannelStarts[0].sound)
        {
            found.sends.push_back({i, 0, nw4r::kMiiChannelEffectSend});
        }
    }

    AddSoundSetups(archive, nw4r::kMiiChannelStarts, nw4r::kMiiChannelVariants, {}, found);

    return found;
}

// The Photo Channel 1.1, the Forecast Channel and the News Channel share a sound manager. As each channel starts, the
// sound manager puts FxReverbHi on bus C with preDelayTime 0.1, fusedTime 2.1, coloration 0.6, damping 0.4, crossTalk
// 0.1 and outGain 0.2 (AppendEffect is called at 0x80048a0c in the Photo Channel 1.1, 0x80041b60 in the Forecast
// Channel and 0x8004ee00 in the News Channel). Nothing changes the reverb later. The music sends to bus C from its
// sequences. The sound manager also puts an effect that the channel defines on bus B. That effect starts off. Nothing
// sends to bus B. Only the Photo Channel turns it on. While a photo with a mood is shown, the music of the photos
// (BGM_MAIN and BGM_MAIN_KEIJI) moves from the main mix and bus C to bus B (0x80049614). Both pieces also play without
// a mood. Their plain rips play the channel as it starts, with nothing on bus B. Each piece also rips once with each
// mood.
constexpr float kChannelPreDelay = 0.1f;
constexpr float kChannelTime = 2.1f;
constexpr float kChannelColoration = 0.6f;
constexpr float kChannelDamping = 0.4f;
constexpr float kChannelCrosstalk = 0.1f;
constexpr float kChannelOut = 0.2f;

// The modes that the Photo Channel's moods 1 to 4 set (0x800212c8).
constexpr std::array<uint32_t, 4> kMoodModes = {1, 4, 3, 2};

// The variants of the Photo Channel's pieces with a mood. A photo's record gives its mood at +0x18c. While the photo is
// shown with BGM_MAIN or BGM_MAIN_KEIJI, 0x80020d64 turns the effect on bus B on. 0x8002123c sets the mood's mode.
// 0x80049614 sets the music's main send to -1, its send to bus C to -1 and its send to bus B to 1. The music then plays
// through the effect alone.
std::vector<nw4r::EffectVariant> PhotoMoods(const ntdlib::rsar::SoundArchive& archive)
{
    const std::vector<uint32_t> photos = SoundsNamed(archive, {"BGM_MAIN", "BGM_MAIN_KEIJI"});
    std::vector<nw4r::EffectVariant> moods;
    for (std::size_t mood = 0; mood < kMoodModes.size(); mood++)
    {
        nw4r::EffectSetup effect = Fixed(1, nw4r::EffectType::kPhotoMood, {kMoodModes[mood]});
        effect.sounds = photos;
        nw4r::EffectVariant variant{
            .name = " mood " + std::to_string(mood + 1), .sounds = photos, .setups = {effect}, .sends = {}};
        for (const uint32_t sound : photos)
        {
            variant.sends.push_back({sound, nw4r::kMainSendBus, -1.0f});
            variant.sends.push_back({sound, 2, -1.0f});
            variant.sends.push_back({sound, 1, 1.0f});
        }

        moods.push_back(variant);
    }

    return moods;
}

// The Photo Channel's NW4R SND of 2008 reads five more words. They keep the defaults that the sound manager's
// constructor gives them (0x8004b9a8). FxReverbHi::SetParam then bounds the pre-delay by preDelayTimeMax, 0.02.
nw4r::ArchiveEffects PhotoChannelEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects found;
    if (name == "rev_photo.brsar")
    {
        nw4r::EffectSetup reverb = ReverbHi2008(kChannelPreDelay, kChannelTime, kChannelColoration, kChannelDamping,
                                                kChannelCrosstalk, kChannelOut, {5, 0.02f, 0, 0.0f, 1.0f});
        reverb.bus = 2;
        found.setups = {reverb};
        found.variants = PhotoMoods(archive);
    }

    return found;
}

// The older FxReverbHi of the Forecast Channel and the News Channel, for the channel's archive.
nw4r::ArchiveEffects OlderChannelEffects(const std::string& name, const char* archive_name)
{
    nw4r::ArchiveEffects found;
    if (name == archive_name)
    {
        found.setups = {Fixed(2, nw4r::EffectType::kReverbHi,
                              Words({kChannelPreDelay, kChannelTime, kChannelColoration, kChannelDamping,
                                     kChannelCrosstalk, kChannelOut}))};
    }

    return found;
}

nw4r::ArchiveEffects ForecastChannelEffects(const std::string& name, const ntdlib::rsar::SoundArchive&)
{
    return OlderChannelEffects(name, "rev_wtr.brsar");
}

nw4r::ArchiveEffects NewsChannelEffects(const std::string& name, const ntdlib::rsar::SoundArchive&)
{
    return OlderChannelEffects(name, "rev_news.brsar");
}

// The effects of preset `preset` of Pokémon Rumble, from 1 to 9. 0x80125660 sets a preset up from the table at
// 0x803b4120, 0x74 bytes for each preset. 0x80125360 fills the table. Every preset puts the standard reverb of 2008 on
// bus A. Presets 1, 2 and 8 have the longer reverb. Presets 2 to 9 add FxDelay of 2009 on bus B. Presets 4, 5, 6 and 9
// add FxChorus of 2009 on bus C.
std::vector<nw4r::EffectSetup> PokemonRumblePreset(uint32_t preset)
{
    const bool longer = preset == 1 || preset == 2 || preset == 8;
    std::vector<nw4r::EffectSetup> setups = {
        longer ? ReverbStd2008(0.02f, 3.0f, 0.6f, 0.4f, 1.0f, {5, 0.15f, 0, 0.0f, 1.0f})
               : ReverbStd2008(0.02f, 2.5f, 0.6f, 0.2f, 1.0f, {5, 0.15f, 4, 0.0f, 1.0f})};

    // The delays in ms and the feedbacks of presets 2 to 9. Each delay has an outGain of 1, a maxDelay of 500 ms and an
    // lpf of 1.
    static constexpr std::array<float, 8> kDelays = {176.47f, 166.66f, 176.47f, 222.22f,
                                                     300.0f,  437.5f,  437.5f,  273.0f};
    static constexpr std::array<float, 8> kFeedbacks = {0.4f, 0.4f, 0.4f, 0.8f, 0.8f, 0.4f, 0.4f, 0.6f};
    if (preset >= 2)
    {
        const float delay = kDelays[preset - 2];
        const float feedback = kFeedbacks[preset - 2];
        setups.push_back(Fixed(1, nw4r::EffectType::kDelay2009, Words({delay, feedback, 1.0f, 500.0f, 1.0f})));
    }

    // The chorus: delayTime 0.8 ms, depth 0.9, rate 0.5 Hz, feedback 0.9 and outGain 1.
    if (preset == 4 || preset == 5 || preset == 6 || preset == 9)
    {
        setups.push_back(Fixed(2, nw4r::EffectType::kChorus2009, Words({0.8f, 0.9f, 0.5f, 0.9f, 1.0f})));
    }

    return setups;
}

// The effects of Pokémon Rumble. The game's sound object (0x801e6d80) holds an FxReverbStd, an FxChorus and an FxDelay.
// As a piece of music starts (0x80125840 and 0x801258f0), the game reads the first user parameter of the piece's sound
// (0x80283ee0). If it differs from the preset that the game set up last (0x804b9084), the game clears the three buses
// at once and sets that preset up. The pieces are the sounds named BGM_*. Each gets the effects of its preset. The
// sound effects play through the effects of the music that plays with them. A rip of a sound effect plays without
// effects: the state of the game before its first piece of music.
nw4r::ArchiveEffects PokemonRumbleEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    if (name != "waza_p.brsar")
    {
        return {};
    }

    // One setup for each set of parameters on a bus. It lists the sounds of every preset that has it.
    nw4r::ArchiveEffects found;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const ntdlib::rsar::SoundInfo& sound = archive.Sounds()[i];
        const uint32_t preset = sound.user_params[0];
        if (!sound.name.starts_with("BGM_") || preset < 1 || preset > 9)
        {
            continue;
        }

        for (nw4r::EffectSetup& setup : PokemonRumblePreset(preset))
        {
            const auto same = [&setup](const nw4r::EffectSetup& other)
            {
                return other.bus == setup.bus && other.type == setup.type && other.params == setup.params;
            };
            const auto listed = std::find_if(found.setups.begin(), found.setups.end(), same);
            if (listed != found.setups.end())
            {
                listed->sounds.push_back(i);
                continue;
            }

            setup.sounds = {i};
            found.setups.push_back(setup);
        }
    }

    return found;
}

// The addresses of `profile`'s functions, in the order of its signatures: AXRegisterCallback, AxManager's GetInstance,
// AppendEffect and ClearEffect, SeqSoundHandle's constructor and destructor and SeqSound::ReadVariable where the
// profile has them, then each class's functions.
std::vector<uint32_t*> EffectFunctions(EffectProfile& profile)
{
    std::vector<uint32_t*> functions = {&profile.ax_register_callback, &profile.ax_manager, &profile.append_effect,
                                        &profile.clear_effect};
    for (uint32_t* function : {&profile.seq_handle_ctor, &profile.seq_handle_dtor, &profile.read_variable})
    {
        if (*function != 0)
        {
            functions.push_back(function);
        }
    }

    for (EffectClass& effect_class : profile.classes)
    {
        functions.insert(functions.end(),
                         {&effect_class.ctor, &effect_class.mem_size, &effect_class.assign, &effect_class.set_param,
                          &effect_class.start_up, &effect_class.shutdown, &effect_class.update_buffer});
    }

    return functions;
}

const std::vector<EffectProfile>& KnownEffectProfiles()
{
    static const std::vector<EffectProfile> kProfiles = {
        {
            .dol_sha1 = kWiiSportsSha1,
            .game_id = "RSPE01",
            .effects = WiiSportsEffects,
            .axfx_build = axfx::Build::kDecember2006,
            .ax_register_callback = 0x80106260,
            .ax_manager = 0x800312e8,
            .append_effect = 0x80031d5c,
            .clear_effect = 0x80031f60,
            // SeqSound::WriteVariable (0x8003f9c8) passes the SeqPlayer at +0xd8 to SeqPlayer::SetLocalVariable
            // (0x8003e830). The local variables lie at +0xf8 of the SeqPlayer.
            .seq_variables = 0xd8 + 0xf8,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi, 0x8003a668, 0x8003a6f4, 0x8003a720, 0x8003a84c, 0x8003a730,
                     0x8003a7ec, 0x8003aa38},
                    {nw4r::EffectType::kChorus, 0x80039d88, 0x80039dfc, 0x80039e28, 0x80039f54, 0x80039e38, 0x80039ef4,
                     0x8003a0c0},
                },
            .anchors = {0x80104450},
            .signatures =
                {
                    {0x44, 0x9421fff0, 0xa0d7e48a180ad991},  {0x64, 0x9421fff0, 0xc6bcc4e1821f0a4e},
                    {0x204, 0x9421ffc0, 0x9d5cc845ad848986}, {0xc8, 0x9421ffe0, 0xe2ec3a9c84a610f3},
                    {0x8c, 0x9421ffd0, 0xcb31914fa11fed70},  {0x2c, 0x9421fff0, 0x03071cb3942a82a7},
                    {0xcc, 0x38630000, 0x894189134e969a59},  {0x1ec, 0x9421ffe0, 0xe49a28b6e723d392},
                    {0xbc, 0x9421ffe0, 0x27575a32c85c8399},  {0x60, 0x9421ffe0, 0x2e338e3485d8bc6b},
                    {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},  {0x74, 0x9421ffe0, 0x4242d47254e35f43},
                    {0x2c, 0x9421fff0, 0x03071cb3942a82a7},  {0xcc, 0x38630000, 0x894189134e969a59},
                    {0x16c, 0x9421ffe0, 0x8cd480b7638c6104}, {0xbc, 0x9421ffe0, 0x27575a32c85c8399},
                    {0x60, 0x9421ffe0, 0x2e338e3485d8bc6b},  {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},
                    {0x54, 0x9421fff0, 0x60936706004ec277},
                },
        },
        {
            .dol_sha1 = kWiiPlaySha1,
            .game_id = "RHAE01",
            .effects = WiiPlayEffects,
            .axfx_build = axfx::Build::kDecember2006,
            .ax_register_callback = 0x80105c48,
            .ax_manager = 0x800312e0,
            .append_effect = 0x80031d54,
            .clear_effect = 0x80031f58,
            .seq_tracks = 0xd8 + 0xb8, // SeqSound's SeqPlayer (0x8003fa84), and its tracks (0x8003e9e4)
            .track_volume = 0x8,       // SeqTrack::SetVolume (0x80040cb4)
            .classes =
                {
                    {nw4r::EffectType::kReverbHi, 0x8003a600, 0x8003a68c, 0x8003a6b8, 0x8003a7e4, 0x8003a6c8,
                     0x8003a784, 0x8003a9d0},
                    {nw4r::EffectType::kChorus, 0x80039d20, 0x80039d94, 0x80039dc0, 0x80039eec, 0x80039dd0, 0x80039e8c,
                     0x8003a058},
                    {nw4r::EffectType::kDelay, 0x8003a118, 0x8003a28c, 0x8003a2b8, 0x8003a3e4, 0x8003a2c8, 0x8003a384,
                     0x8003a540},
                },
            .anchors = {0x80103e38},
            .signatures =
                {
                    {0x44, 0x9421fff0, 0xa0d7e48a180ad991},  {0x64, 0x9421fff0, 0xc6bcc4e1821f0a4e},
                    {0x204, 0x9421ffc0, 0x9d5cc845ad848986}, {0xc8, 0x9421ffe0, 0xe2ec3a9c84a610f3},
                    {0x8c, 0x9421ffd0, 0xcb31914fa11fed70},  {0x2c, 0x9421fff0, 0x03071cb3942a82a7},
                    {0xcc, 0x38630000, 0x894189134e969a59},  {0x1ec, 0x9421ffe0, 0xe49a28b6e723d392},
                    {0xbc, 0x9421ffe0, 0x27575a32c85c8399},  {0x60, 0x9421ffe0, 0x2e338e3485d8bc6b},
                    {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},  {0x74, 0x9421ffe0, 0x4242d47254e35f43},
                    {0x2c, 0x9421fff0, 0x03071cb3942a82a7},  {0xcc, 0x38630000, 0x894189134e969a59},
                    {0x16c, 0x9421ffe0, 0x8cd480b7638c6104}, {0xbc, 0x9421ffe0, 0x27575a32c85c8399},
                    {0x60, 0x9421ffe0, 0x2e338e3485d8bc6b},  {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},
                    {0x174, 0x9421ffd0, 0xd3122aeba6f9ba41}, {0x2c, 0x9421fff0, 0x03071cb3942a82a7},
                    {0xcc, 0x38630000, 0x894189134e969a59},  {0x15c, 0x9421ffe0, 0x71e20de898e8cb90},
                    {0xbc, 0x9421ffe0, 0x27575a32c85c8399},  {0x60, 0x9421ffe0, 0x2e338e3485d8bc6b},
                    {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},  {0x54, 0x9421fff0, 0x60936706004ec277},
                },
        },
        {
            .dol_sha1 = kSmoothMovesSha1,
            .game_id = "RODE01",
            .effects = SmoothMovesEffects,
            .axfx_build = axfx::Build::kOctober2006,
            .ax_register_callback = 0x8002ff80,
            .ax_manager = 0x800d97e8,
            .append_effect = 0x800da878,
            .clear_effect = 0x800daa7c,
            .seq_handle_ctor = 0x800e6d68,
            .seq_handle_dtor = 0x800feef0,
            .read_variable = 0x800e6cbc,
            // SeqSound::ReadVariable (0x800e6cbc) passes the SeqPlayer at +0xd8 to 0x800e62ec. That reads the local
            // variables at +0xdc of the SeqPlayer. The game links no SeqSound::WriteVariable.
            .seq_variables = 0xd8 + 0xdc,
            .classes =
                {
                    {nw4r::EffectType::kReverbStd, 0x800e31d8, 0x800e325c, 0x800e3288, 0x800e33ac, 0x800e3298,
                     0x800e334c, 0x800e3538},
                    {nw4r::EffectType::kChorus, 0x800e2514, 0x800e2588, 0x800e25b4, 0x800e26d8, 0x800e25c4, 0x800e2678,
                     0x800e2820},
                    {nw4r::EffectType::kDelay, 0x800e28e0, 0x800e2a34, 0x800e2a60, 0x800e2b84, 0x800e2a70, 0x800e2b24,
                     0x800e2cbc},
                },
            // AXRegisterCallback and SeqSoundHandle's destructor match in several places. AXInit places the first. The
            // game's function that reads the music's variables (0x800fee28, which driver.c follows) places the second.
            .anchors = {0x8002e3f0, 0x800fee28},
            .signatures =
                {
                    {0x44, 0x9421fff0, 0xa0d7e48a180ad991},  {0x64, 0x9421fff0, 0xc6bcc4e1821f0a4e},
                    {0x204, 0x9421ffc0, 0x9d5cc845ad848986}, {0xc8, 0x9421ffe0, 0xe2ec3a9c84a610f3},
                    {0x108, 0x9421ffe0, 0x3ab8381b7a6d4019}, {0x54, 0x9421fff0, 0x8c27b2db545ed4aa},
                    {0x50, 0x9421fff0, 0xe4d13d3b9e57f7e0},  {0x84, 0x9421ffd0, 0xe7af0ca778fbb3b0},
                    {0x2c, 0x9421fff0, 0x03071cb3942a82a7},  {0xc4, 0x38630000, 0x68cbcd27cd639700},
                    {0x18c, 0x9421fff0, 0xc5bafced5b1e6603}, {0xb4, 0x9421ffe0, 0x9e48aa8c140d1b40},
                    {0x60, 0x9421fff0, 0x8995c55a93d3a9c2},  {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},
                    {0x74, 0x9421ffe0, 0x4242d47254e35f43},  {0x2c, 0x9421fff0, 0x03071cb3942a82a7},
                    {0xc4, 0x38630000, 0x68cbcd27cd639700},  {0x148, 0x9421ffe0, 0x9fa6a8ea745a3664},
                    {0xb4, 0x9421ffe0, 0x9e48aa8c140d1b40},  {0x60, 0x9421fff0, 0x8995c55a93d3a9c2},
                    {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},  {0x154, 0x9421ffe0, 0x875ce753b7fed176},
                    {0x2c, 0x9421fff0, 0x03071cb3942a82a7},  {0xc4, 0x38630000, 0x68cbcd27cd639700},
                    {0x138, 0x9421fff0, 0x3d63a8a733f6c736}, {0xb4, 0x9421ffe0, 0x9e48aa8c140d1b40},
                    {0x60, 0x9421fff0, 0x8995c55a93d3a9c2},  {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},
                    {0x40, 0x9421fff0, 0x50ffca50e67c275d},  {0xc8, 0x9421ffe0, 0x535d001042662a33},
                },
        },
        {
            .dol_sha1 = kCityFolkSha1,
            .game_id = "RUUE01",
            .effects = CityFolkEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x80376748,
            .ax_manager = 0x8025914c,
            .append_effect = 0x80259b98,
            .clear_effect = 0x80259d9c,
            .sound_sends = 0xd0,
            .seq_tracks = 0x22c,
            .track_volume = 0x8,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi2008, 0x80262d10, 0x80262de0, 0x80262e34, 0x80262fe8, 0x80262e44,
                     0x80262f6c, 0x8026333c},
                },
            .anchors = {0x80374924},
            .signatures =
                {
                    {0x44, 0x9421fff0, 0xa0d7e48a180ad991},
                    {0x64, 0x9421fff0, 0xc6bcc4e1821f0a4e},
                    {0x204, 0x9421ffc0, 0x9d5cc845ad848986},
                    {0x15c, 0x9421ffe0, 0x9befc0e47bab4e14},
                    {0xd0, 0x9421ffc0, 0x039354d5ccec9971},
                    {0x54, 0x9421fff0, 0xafb952a0e6c9afa7},
                    {0x138, 0x38630000, 0x97851330e118afe4},
                    {0x354, 0x9421ffe0, 0x17d7f12fd32b83d0},
                    {0x128, 0x9421ffe0, 0x6c652ee81a3bc9a4},
                    {0x7c, 0x9421ffe0, 0xd0bfd2100e516bd0},
                    {0xa0, 0x9421ffd0, 0xf04fd0562541f958},
                    {0x54, 0x9421fff0, 0x60936706004ec277},
                },
        },
        {
            .dol_sha1 = kMarioKartSha1,
            .game_id = "RMCE01",
            .effects = MarioKartEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x80126c04,
            .ax_manager = 0x80088750,
            .append_effect = 0x80089170,
            .clear_effect = 0x80089360,
            .seq_tracks = 0x100 + 0x114, // SeqSound's SeqPlayer (0x8009a990), and its tracks (0x800997e0)
            .track_volume = 0x8,         // SeqTrack::SetVolume (0x8009bdd0)
            // SeqSound::WriteVariable (0x8009a9b0) passes the SeqPlayer at +0x100 to SeqPlayer::SetLocalVariable
            // (0x80099760). The local variables lie at +0x154 of the SeqPlayer.
            .seq_variables = 0x100 + 0x154,
            .classes =
                {
                    {nw4r::EffectType::kReverbStd2008, 0x80095080, 0x80095150, 0x800951b0, 0x80095380, 0x800951d0,
                     0x80095300, 0x800956a0},
                },
            .signatures =
                {
                    {0x44, 0x9421fff0, 0xa0d7e48a180ad991},
                    {0x64, 0x9421fff0, 0xc6bcc4e1821f0a4e},
                    {0x1f0, 0x9421ffc0, 0x727cd9d284365b23},
                    {0xc8, 0x9421ffe0, 0xe0fe1cc79461efd3},
                    {0xc4, 0x9421ffc0, 0xec0a766fee3b8b41},
                    {0x54, 0x9421fff0, 0xafb952a0e6c9afa7},
                    {0x148, 0x38630000, 0x934ca3d31fef2304},
                    {0x31c, 0x9421ffe0, 0xaa7c744d0175d5a2},
                    {0x128, 0x9421ffe0, 0xbd6df491c46977e4},
                    {0x7c, 0x9421ffe0, 0x40b3d5b5b84ab030},
                    {0xa0, 0x9421ffd0, 0x5854c13449161e78},
                },
        },
        {
            .dol_sha1 = kWiiMusicSha1,
            .game_id = "R64E01",
            .effects = WiiMusicEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x800e8b48,
            .ax_manager = 0x800ac63c,
            .append_effect = 0x800ad130,
            .clear_effect = 0x800ad334,
            .sound_sends = 0xd0,         // BasicSound::SetFxSend (0x800b38e4) stores the send at +0xd0 + 4 × the bus
            .seq_tracks = 0x10c + 0x120, // SeqSound's SeqPlayer (0x800bd810), and its tracks (0x800bc658)
            .track_volume = 0x8,         // SeqTrack::SetVolume (0x800bed68)
            .classes =
                {
                    {nw4r::EffectType::kReverbStd2008, 0x800b7f24, 0x800b7fe8, 0x800b8014, 0x800b813c, 0x800b8024,
                     0x800b80d8, 0x800b83d4},
                    {nw4r::EffectType::kReverbHi2008, 0x800b71a8, 0x800b7278, 0x800b72a4, 0x800b73cc, 0x800b72b4,
                     0x800b7368, 0x800b7698},
                },
            .signatures =
                {
                    {0x44, 0x9421fff0, 0xa0d7e48a180ad991},
                    {0x64, 0x9421fff0, 0xc6bcc4e1821f0a4e},
                    {0x204, 0x9421ffc0, 0x9d5cc845ad848986},
                    {0x15c, 0x9421ffe0, 0x9befc0e47bab4e14},
                    {0xc4, 0x9421ffc0, 0xec0a766fee3b8b41},
                    {0x2c, 0x9421fff0, 0x03071cb3942a82a7},
                    {0xc4, 0x38630000, 0xe7f5b805ad388eb6},
                    {0x298, 0x9421ffe0, 0xa0ce37ffefa99ff6},
                    {0xb4, 0x9421ffe0, 0xe553350da1d254f6},
                    {0x64, 0x9421ffe0, 0x34b83493f8cebe5a},
                    {0x58, 0x9421ffe0, 0xb75671833a45eae1},
                    {0xd0, 0x9421ffc0, 0x039354d5ccec9971},
                    {0x2c, 0x9421fff0, 0x03071cb3942a82a7},
                    {0xc4, 0x38630000, 0xe7f5b805ad388eb6},
                    {0x2cc, 0x9421ffe0, 0x9324aaef50f3b9d2},
                    {0xb4, 0x9421ffe0, 0xe553350da1d254f6},
                    {0x64, 0x9421ffe0, 0x34b83493f8cebe5a},
                    {0x58, 0x9421ffe0, 0xb75671833a45eae1},
                },
        },
        // The channels. A title rips in archive mode only. The addresses for game mode's driver are those of the
        // channel's functions.
        {
            .dol_sha1 = kWiiShopChannelSha1,
            .effects = WiiShopChannelEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x801ec8e0,
            .ax_manager = 0x801824e0,
            .append_effect = 0x80182f70,
            .clear_effect = 0x80183160,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi2008, 0x8018d6b0, 0x8018d780, 0x8018d7b0, 0x8018d900, 0x8018d7d0,
                     0x8018d890, 0x8018dbd0},
                },
        },
        {
            .dol_sha1 = kMiiChannelSha1,
            .effects = MiiChannelEffects,
            .axfx_build = axfx::Build::kDecember2006,
            .ax_register_callback = 0x801a4dd8,
            .ax_manager = 0x800f0e60,
            .append_effect = 0x800f1e40,
            .clear_effect = 0x800f205c,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi, 0x800f9ac0, 0x800f9b4c, 0x800f9b78, 0x800f9c9c, 0x800f9b88,
                     0x800f9c3c, 0x800f9e5c},
                },
        },
        {
            .dol_sha1 = kPhotoChannelSha1,
            .effects = PhotoChannelEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x8016e90c,
            .ax_manager = 0x801169c4,
            .append_effect = 0x801174b8,
            .clear_effect = 0x801176bc,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi2008, 0x80120854, 0x80120924, 0x80120950, 0x80120a78, 0x80120960,
                     0x80120a14, 0x80120d44},
                },
        },
        {
            .dol_sha1 = kForecastChannelSha1,
            .effects = ForecastChannelEffects,
            .axfx_build = axfx::Build::kDecember2006,
            .ax_register_callback = 0x8010d1cc,
            .ax_manager = 0x800b618c,
            .append_effect = 0x800b6a6c,
            .clear_effect = 0x800b6c70,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi, 0x800bcaa4, 0x800bcb30, 0x800bcb5c, 0x800bcc6c, 0x800bcb6c,
                     0x800bcc18, 0x800bce48},
                },
        },
        {
            .dol_sha1 = kNewsChannelSha1,
            .effects = NewsChannelEffects,
            .axfx_build = axfx::Build::kDecember2006,
            .ax_register_callback = 0x80125d34,
            .ax_manager = 0x800cec58,
            .append_effect = 0x800cf538,
            .clear_effect = 0x800cf73c,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi, 0x800d5570, 0x800d55fc, 0x800d5628, 0x800d5738, 0x800d5638,
                     0x800d56e4, 0x800d5914},
                },
        },
        // Pokémon Rumble rips in archive mode only. The addresses for game mode's driver are those of the game's
        // functions. The AXFX of February 2009 has the reverbs of September 2008.
        {
            .dol_sha1 = kPokemonRumbleSha1,
            .effects = PokemonRumbleEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x802dc400,
            .ax_manager = 0x802720a0,
            .append_effect = 0x80272be0,
            .clear_effect = 0x80272dd0,
            .classes =
                {
                    {nw4r::EffectType::kReverbStd2008, 0x8027be70, 0x8027bf40, 0x8027bf70, 0x8027c0c0, 0x8027bf90,
                     0x8027c050, 0x8027c360},
                    {nw4r::EffectType::kChorus2009, 0x8027b5f0, 0x8027b680, 0x8027b6b0, 0x8027b800, 0x8027b6d0,
                     0x8027b790, 0x8027b9b0},
                    {nw4r::EffectType::kDelay2009, 0x8027ba10, 0x8027baa0, 0x8027bad0, 0x8027bc20, 0x8027baf0,
                     0x8027bbb0, 0x8027be10},
                },
        },
    };
    return kProfiles;
}

} // namespace

std::optional<EffectProfile> FindEffectProfile(std::span<const uint8_t> dol, const std::string& game_id)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(dol));
    for (const EffectProfile& profile : KnownEffectProfiles())
    {
        if (sha1 == profile.dol_sha1)
        {
            return profile;
        }
    }

    // Another release of a game with a profile.
    for (const EffectProfile& source : KnownEffectProfiles())
    {
        if (!SameGame(source.game_id, game_id))
        {
            continue;
        }

        std::optional<EffectProfile> derived = DeriveEffectProfile(source, dol);
        if (derived)
        {
            derived->dol_sha1 = sha1;
            derived->game_id = game_id;

            return derived;
        }
    }

    return std::nullopt;
}

std::optional<EffectProfile> DeriveEffectProfile(const EffectProfile& source, std::span<const uint8_t> dol_bytes)
{
    // The functions to find: the profile's, then its anchors.
    EffectProfile derived = source;
    std::vector<uint32_t*> functions = EffectFunctions(derived);
    for (uint32_t& anchor : derived.anchors)
    {
        functions.push_back(&anchor);
    }

    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(dol_bytes);
    if (source.signatures.size() != functions.size() || !dol)
    {
        return std::nullopt;
    }

    std::vector<uint32_t> addresses;
    for (const uint32_t* function : functions)
    {
        addresses.push_back(*function);
    }

    const std::optional<std::vector<uint32_t>> found = FindFunctions(*dol, addresses, source.signatures);
    if (!found)
    {
        return std::nullopt;
    }

    for (std::size_t i = 0; i < functions.size(); i++)
    {
        *functions[i] = (*found)[i];
    }

    return derived;
}

nw4r::ArchiveEffects FindEffects(const std::optional<EffectProfile>& profile, const std::string& archive_path,
                                 const ntdlib::rsar::SoundArchive& archive)
{
    return profile ? profile->effects(ArchiveFileName(archive_path), archive) : nw4r::ArchiveEffects{};
}

} // namespace wsf::rip
