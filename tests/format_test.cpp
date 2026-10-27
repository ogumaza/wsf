// SPDX-License-Identifier: MIT

// Unit tests for the WSF file format code (src/wsf/format.*): the version byte, the process and archive descriptors
// with the archive's effects and the tracks that the game plays, what WSF's format gives xsflib's set loader (its PSF
// version, its descriptors and its driver block), and the tags that arrange a sequence in archive mode. xsflib's
// psf_test and psf_set_test cover the PSF container, tags, times, program chunks, the loader and the variables tag's
// syntax.

#include "wsf/format.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

using xsflib::testing::LoadError;

namespace
{

using namespace wsf;

void TestVersionByte()
{
    // A file that WSF writes has version byte 0x27 (docs/wsf.md, "Version byte"). xsflib's psf_test checks the PSF
    // container with another version byte.
    const auto file = xsflib::psf::WritePsf(kPsfVersionWsf, {}, {}, {});

    XSFLIB_CHECK(file.size() >= 16 && std::memcmp(file.data(), "PSF", 3) == 0 && file[3] == 0x27);
}

void TestDescriptor()
{
    ProcessDescriptor d;
    d.version = 2;
    d.entry = 0x80004000;
    d.status_address = 0x81700018;
    d.ios_version = 21;
    d.flags = 3;

    const auto bytes = d.Serialize();
    const auto back = ProcessDescriptor::Parse(bytes);

    XSFLIB_CHECK(bytes.size() == 24);
    XSFLIB_CHECK(std::memcmp(bytes.data(), "WSFP", 4) == 0);
    XSFLIB_CHECK(back.has_value());
    XSFLIB_CHECK(back->version == d.version && back->entry == d.entry && back->status_address == d.status_address &&
                 back->ios_version == d.ios_version && back->flags == d.flags);

    // Data without the magic, or too short, isn't a descriptor.
    XSFLIB_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(24, 0)).has_value());
    XSFLIB_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(8, 0)).has_value());
}

void TestLoadSet()
{
    std::map<std::string, std::vector<uint8_t>> fs;
    const FileReader reader = xsflib::testing::MapReader(fs);

    // A .miniwsf and its .wsflib, with the library's process descriptor.
    ProcessDescriptor d;
    d.status_address = 0x1234;
    fs["dir/lib.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, d.Serialize(), {}, {});
    fs["dir/x.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, {}, {}, {{"_lib", "lib.wsflib"}});

    LoadedSet set;
    XSFLIB_CHECK(!LoadSet("dir/x.miniwsf", reader, set));
    XSFLIB_CHECK(set.descriptor && set.descriptor->status_address == 0x1234);

    // Another format's file and a set without a descriptor are errors.
    fs["dir/z.minincsf"] = xsflib::psf::WritePsf(0x25, {}, {}, {{"_lib", "lib.wsflib"}});
    fs["dir/w.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, {}, {}, {{"title", "w"}});

    XSFLIB_CHECK(LoadError<SetFormat>("dir/z.minincsf", reader).find("not a WSF file") != std::string::npos);
    XSFLIB_CHECK(LoadError<SetFormat>("dir/w.miniwsf", reader).starts_with("no process or archive descriptor"));

    // So is a descriptor with an unsupported version.
    ProcessDescriptor v2;
    v2.version = 2;
    fs["dir/v2.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, v2.Serialize(), {}, {});

    XSFLIB_CHECK(LoadSet("dir/v2.wsflib", reader, set).has_value());

    // Or flags other than bit 0 alone.
    for (const uint32_t flags : {0u, 3u, 0x80000001u})
    {
        ProcessDescriptor odd;
        odd.flags = flags;
        fs["dir/flags.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, odd.Serialize(), {}, {});

        XSFLIB_CHECK(LoadSet("dir/flags.wsflib", reader, set).has_value());
    }

    // A driver block of version 1 (NW4R SND's driver) or 2 (the JAudio drivers) plays. A block of another version
    // doesn't. Like the rest of the Wii's memory, the block's words are big-endian.
    const auto driver_block = [](uint32_t version)
    {
        std::vector<uint8_t> block(0x20, 0);
        xsflib::StoreBe32(&block[0], DriverBlock::kMagic);
        xsflib::StoreBe32(&block[DriverBlock::kVersionOffset], version);
        ProgramBuilder b;
        b.AddMemory(DriverBlock::kAddress, block);

        return b.Data();
    };

    fs["dir/driver1.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, d.Serialize(), driver_block(1), {});
    fs["dir/driver2.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, d.Serialize(), driver_block(2), {});
    fs["dir/driver3.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, d.Serialize(), driver_block(3), {});

    const std::optional<std::string> driver1_error = LoadSet("dir/driver1.wsflib", reader, set);
    const std::optional<std::string> driver2_error = LoadSet("dir/driver2.wsflib", reader, set);
    const std::optional<std::string> refused = LoadSet("dir/driver3.wsflib", reader, set);

    XSFLIB_CHECK(!driver1_error);
    XSFLIB_CHECK(!driver2_error);
    XSFLIB_CHECK(refused &&
                 refused->find("driver block version 3; this player supports versions 1 to 2") != std::string::npos);
}

void TestArchiveMode()
{
    ArchiveDescriptor d;
    d.version = 2;
    d.flags = 0;
    d.output_mode = 2;
    d.microcode_address = 0x803ac840;
    d.archive_path = "Sound/b.brsar";
    d.microcode_path = "ax_microcode.bin";
    d.snd_build = 20080308;

    const auto raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    XSFLIB_CHECK(raw.size() % 4 == 0);
    XSFLIB_CHECK(back && back->version == d.version && back->flags == d.flags && back->output_mode == d.output_mode &&
                 back->microcode_address == d.microcode_address && back->archive_path == d.archive_path &&
                 back->microcode_path == d.microcode_path && back->snd_build == d.snd_build);
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 22)).has_value());
    XSFLIB_CHECK(!ProcessDescriptor::Parse(raw).has_value());

    // A descriptor that ends after its paths names no build.
    const auto without_build = ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 4));

    XSFLIB_CHECK(without_build && without_build->microcode_path == d.microcode_path && without_build->snd_build == 0);

    // A player only accepts versions 1 to 3, with bit 0 of the flags set and no other, in stereo.
    XSFLIB_CHECK(d.Check().has_value());

    ArchiveDescriptor v1 = d;
    v1.version = 1;
    v1.flags = kFlagDspRequired;
    v1.output_mode = kOutputStereo;
    ArchiveDescriptor v2 = v1;
    v2.version = ArchiveDescriptor::kListVersion;
    ArchiveDescriptor v3 = v1;
    v3.version = ArchiveDescriptor::kMoodVersion;

    XSFLIB_CHECK(!v1.Check() && !v2.Check() && !v3.Check());
    for (const uint32_t version : {0u, 4u})
    {
        ArchiveDescriptor odd = v1;
        odd.version = version;

        XSFLIB_CHECK(odd.Check() == "archive descriptor version " + std::to_string(version) +
                                        "; this player supports versions 1 to 3");
    }

    for (const uint32_t flags : {0u, 3u, 0x80000001u})
    {
        ArchiveDescriptor odd = v1;
        odd.flags = flags;

        XSFLIB_CHECK(odd.Check().has_value());
    }

    for (const uint32_t mode : {0u, 2u, 3u})
    {
        ArchiveDescriptor odd = v1;
        odd.output_mode = mode;

        XSFLIB_CHECK(odd.Check().has_value());
    }

    // A later descriptor of the other kind sets the mode. The last SND chunk selects the sound.
    ProgramBuilder lib;
    lib.AddFile("Sound/b.brsar", {1, 2, 3});
    lib.AddSound(5);
    ProgramBuilder mini;
    mini.AddSound(7);
    ProcessDescriptor game;

    const auto raw1 = v1.Serialize();
    std::map<std::string, std::vector<uint8_t>> fs;
    fs["a/lib.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, raw1, lib.Data(), {});
    fs["a/x.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, {}, mini.Data(), {{"_lib", "lib.wsflib"}});
    fs["a/y.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, game.Serialize(), {}, {{"_lib", "lib.wsflib"}});
    fs["a/nosound.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, raw1, {}, {});
    fs["a/z.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, {}, {}, {{"_lib", "nosound.wsflib"}});

    const FileReader reader = xsflib::testing::MapReader(fs);
    LoadedSet set;

    XSFLIB_CHECK(!LoadSet("a/x.miniwsf", reader, set));
    XSFLIB_CHECK(set.archive.has_value() && !set.descriptor.has_value());
    XSFLIB_CHECK(set.sound == 7u);
    XSFLIB_CHECK(set.files.size() == 1 && set.files[0].path == "Sound/b.brsar");

    XSFLIB_CHECK(!LoadSet("a/y.miniwsf", reader, set));
    XSFLIB_CHECK(set.descriptor.has_value() && !set.archive.has_value());

    XSFLIB_CHECK(LoadSet("a/z.miniwsf", reader, set).has_value()); // archive mode without a sound
}

void TestSoundsThatPlayTogether()
{
    // A SND chunk with a list selects sounds that play together, in its order (docs/wsf.md, "Sounds that play
    // together"). Version 2 of the archive descriptor plays them all. Version 1 plays the first alone.
    ArchiveDescriptor v1;
    v1.version = ArchiveDescriptor::kVersion;
    v1.flags = kFlagDspRequired;
    v1.output_mode = kOutputStereo;
    v1.archive_path = "Sound/b.brsar";
    v1.microcode_path = "ax_microcode.bin";
    ArchiveDescriptor v2 = v1;
    v2.version = ArchiveDescriptor::kListVersion;

    ProgramBuilder lib;
    lib.AddFile("Sound/b.brsar", {1, 2, 3});
    lib.AddSound(5);
    ProgramBuilder list;
    const uint32_t kList[] = {9, 7, 8};
    list.AddSounds(kList);
    ProgramBuilder one;
    one.AddSound(4);

    std::map<std::string, std::vector<uint8_t>> fs;
    fs["a/v1.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, v1.Serialize(), lib.Data(), {});
    fs["a/v2.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, v2.Serialize(), lib.Data(), {});
    fs["a/list1.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, {}, list.Data(), {{"_lib", "v1.wsflib"}});
    fs["a/list2.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, {}, list.Data(), {{"_lib", "v2.wsflib"}});
    fs["a/one2.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, {}, one.Data(), {{"_lib", "v2.wsflib"}});
    fs["a/lib2.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, {}, {}, {{"_lib", "v2.wsflib"}});

    // The list's sounds load after the library's. The last SND chunk gives every sound.
    fs["a/later.miniwsf"] =
        xsflib::psf::WritePsf(kPsfVersionWsf, {}, list.Data(), {{"_lib", "v2.wsflib"}, {"_lib2", "one.wsflib"}});
    fs["a/one.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, {}, one.Data(), {});

    const FileReader reader = xsflib::testing::MapReader(fs);
    const std::vector<uint32_t> kAll = {9, 7, 8};
    LoadedSet set;

    XSFLIB_CHECK(!LoadSet("a/list2.miniwsf", reader, set));
    XSFLIB_CHECK(set.sound == 9u && set.sounds == kAll && SoundsToPlay(set) == kAll);

    XSFLIB_CHECK(!LoadSet("a/list1.miniwsf", reader, set));
    XSFLIB_CHECK(set.sounds == kAll && SoundsToPlay(set) == std::vector<uint32_t>{9});

    XSFLIB_CHECK(!LoadSet("a/one2.miniwsf", reader, set));
    XSFLIB_CHECK(SoundsToPlay(set) == std::vector<uint32_t>{4});

    XSFLIB_CHECK(!LoadSet("a/lib2.miniwsf", reader, set));
    XSFLIB_CHECK(SoundsToPlay(set) == std::vector<uint32_t>{5});

    XSFLIB_CHECK(!LoadSet("a/later.miniwsf", reader, set));
    XSFLIB_CHECK(SoundsToPlay(set) == std::vector<uint32_t>{4});

    // A set without an archive descriptor plays no sound of a list.
    set.archive.reset();

    XSFLIB_CHECK(SoundsToPlay(set).empty());
}

void TestArrangementTags()
{
    // wsf_tracks: track numbers and ranges, separated by commas or white space, in any order.
    XSFLIB_CHECK(ParseTracks("2, 8-14") == uint16_t{0x7f04});
    XSFLIB_CHECK(ParseTracks("15 0\n3-3,,") == uint16_t{0x8009});
    XSFLIB_CHECK(ParseTracks("0-15") == uint16_t{0xffff});
    for (const char* malformed : {"", " , ", "16", "-1", "3-", "-3", "5-2", "1-2-3", "0x3", "a", "1;2", "+1", "0-16"})
    {
        XSFLIB_CHECK(!ParseTracks(malformed));
    }

    // The tag that wsfrip writes: in increasing order, with runs of three or more as ranges. Every set of tracks reads
    // back.
    XSFLIB_CHECK(FormatTracks(0x7f04) == "2, 8-14");
    XSFLIB_CHECK(FormatTracks(0xe803) == "0, 1, 11, 13-15");
    XSFLIB_CHECK(FormatTracks(0xffff) == "0-15");
    bool round_trips = true;
    for (uint32_t tracks = 1; tracks <= 0xffff; tracks++)
    {
        round_trips &= ParseTracks(FormatTracks(static_cast<uint16_t>(tracks))) == tracks;
    }

    XSFLIB_CHECK(round_trips);

    // wsf_tempo: a decimal number from 0.25 to 4, rounded to single precision.
    XSFLIB_CHECK(ParseTempo("1.1") == 1.1f);
    XSFLIB_CHECK(ParseTempo(" 0.9\r\n") == 0.9f);
    XSFLIB_CHECK(ParseTempo("0.25") == 0.25f && ParseTempo("4") == 4.0f && ParseTempo("2,5") == 2.5f);
    for (const char* malformed : {"", " ", "0.2", "4.01", "-1", "1.1x", "x", "1 1", "nan", "inf", "0x2"})
    {
        XSFLIB_CHECK(!ParseTempo(malformed));
    }

    // A set's tags give the arrangement.
    nw4r::Arrangement arrangement =
        ArrangementFromTags({{"wsf_tracks", "11, 13-15"}, {"wsf_tempo", "1.1"}, {"wsf_var", "0=1, 17=-2"}});
    const xsflib::psf::Variables kVariables = {{0, 1}, {17, -2}};
    XSFLIB_CHECK(arrangement.tracks == 0xe800 && arrangement.tempo_ratio == 1.1f &&
                 arrangement.variables == kVariables);

    // Without the tags, and with malformed ones, nothing changes. Each tag counts on its own.
    for (const Tags& tags : {Tags{}, Tags{{"wsf_tracks", "16"}, {"wsf_tempo", "5"}, {"wsf_var", "0=1 x"}}})
    {
        arrangement = ArrangementFromTags(tags);
        XSFLIB_CHECK(arrangement.tracks == 0xffff && arrangement.tempo_ratio == 1.0f && arrangement.variables.empty());
    }

    arrangement = ArrangementFromTags({{"wsf_tracks", "0 1 2 x"}, {"wsf_tempo", "0.5"}});
    XSFLIB_CHECK(arrangement.tracks == 0xffff && arrangement.tempo_ratio == 0.5f);
}

void TestVariantArrangement()
{
    // The wsf_tempo tag that wsfrip writes for a variant's tempo: the shortest decimal that reads back as the same
    // float. Every 1/1024 from 0.25 to 4, and the floats on each side of it, read back.
    XSFLIB_CHECK(FormatTempo(1.0878f) == "1.0878" && FormatTempo(1.1f) == "1.1" && FormatTempo(0.25f) == "0.25" &&
                 FormatTempo(4.0f) == "4");

    bool round_trips = true;
    for (int step = 256; step <= 4096; step++)
    {
        const float ratio = static_cast<float>(step) / 1024.0f;
        for (const float value : {std::nextafter(ratio, 0.0f), ratio, std::nextafter(ratio, 5.0f)})
        {
            round_trips &= value < 0.25f || value > 4.0f || ParseTempo(FormatTempo(value)) == value;
        }
    }

    XSFLIB_CHECK(round_trips);

    // A variant's arrangement as tags: wsf_tracks always, and wsf_tempo and wsf_var where it sets them. The tags read
    // back as the arrangement.
    nw4r::Arrangement scene;
    scene.tracks = 0xf5ff;
    scene.tempo_ratio = 1.0878f;
    scene.variables = {{3, 7}};

    const Tags tags = ArrangementTags(scene);
    const nw4r::Arrangement back = ArrangementFromTags(tags);

    XSFLIB_CHECK(tags.size() == 3 && tags.at("wsf_tracks") == "0-8, 10, 12-15" && tags.at("wsf_tempo") == "1.0878");
    XSFLIB_CHECK(back.tracks == scene.tracks && back.tempo_ratio == scene.tempo_ratio &&
                 back.variables == scene.variables);
    XSFLIB_CHECK(ArrangementTags(nw4r::Arrangement{}) == (Tags{{"wsf_tracks", "0-15"}}));

    // A variant with an arrangement leaves out the tracks that the archive's effects give its sounds. Its tags give
    // them in their place. The other sounds keep theirs.
    nw4r::ArchiveEffects effects;
    effects.tracks = {{2, 0xf3ff}, {5, 0x00ff}};
    const nw4r::EffectVariant variant{
        .name = " scene 4", .sounds = {2}, .setups = {}, .sends = {}, .arrangement = scene};
    effects.variants = {variant};

    const nw4r::ArchiveEffects with = nw4r::WithVariant(effects, variant);
    nw4r::EffectVariant plain = variant;
    plain.arrangement.reset();
    const nw4r::ArchiveEffects with_plain = nw4r::WithVariant(effects, plain);

    XSFLIB_CHECK(with.tracks.size() == 1 && nw4r::TracksOf(with.tracks, 2) == nw4r::kAllTracks &&
                 nw4r::TracksOf(with.tracks, 5) == 0x00ff && with.variants.empty());
    XSFLIB_CHECK(with_plain.tracks.size() == 2 && nw4r::TracksOf(with_plain.tracks, 2) == 0xf3ff);
}

void TestEffects()
{
    // The effects block follows the build (docs/wsf.md, "Archive descriptor").
    ArchiveDescriptor d;
    d.archive_path = "Sound/b.brsar";
    d.microcode_path = "ax_microcode.bin";
    d.snd_build = 20060000;

    nw4r::EffectSetup reverb;
    reverb.type = nw4r::EffectType::kReverbHi;
    reverb.params = {std::bit_cast<uint32_t>(0.02f), std::bit_cast<uint32_t>(3.0f)};
    nw4r::EffectSetup chorus;
    chorus.bus = 2;
    chorus.type = nw4r::EffectType::kChorus;
    chorus.presets = {{1, 2, 3}, {4, 5, 6}};
    chorus.sounds = {7, 70000};
    d.effects.setups = {reverb, chorus};
    d.effects.sends = {{12, 0, 0.25f}, {13, 1, 0.5f}};

    ArchiveDescriptor dry = d;
    dry.effects.setups.clear();
    dry.effects.sends.clear();
    const std::vector<uint8_t> raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    // 4 for the count, 12 + 6 * 4 for the reverb, 12 + 2 * 3 * 4 + 2 * 4 for the chorus, and 4 + 2 * 12 for the sends.
    XSFLIB_CHECK(raw.size() == dry.Serialize().size() + 4 + 36 + 44 + 28);
    XSFLIB_CHECK(back && back->effects.setups.size() == 2 && back->effects.sends.size() == 2);
    XSFLIB_CHECK(back && back->effects.setups[0].bus == 0 &&
                 back->effects.setups[0].type == nw4r::EffectType::kReverbHi &&
                 back->effects.setups[0].params == reverb.params && back->effects.setups[0].presets.empty() &&
                 back->effects.setups[0].sounds.empty());
    XSFLIB_CHECK(back && back->effects.setups[1].bus == 2 &&
                 back->effects.setups[1].type == nw4r::EffectType::kChorus &&
                 back->effects.setups[1].presets == chorus.presets && back->effects.setups[1].sounds == chorus.sounds);
    XSFLIB_CHECK(back && back->effects.sends[1].sound == 13 && back->effects.sends[1].bus == 1 &&
                 back->effects.sends[1].send == 0.5f);

    // Only the presets' sounds follow the variables. Each sound gets its sends by bus.
    XSFLIB_CHECK(nw4r::FollowsVariables(d.effects.setups, 70000) && !nw4r::FollowsVariables(d.effects.setups, 12));
    XSFLIB_CHECK((nw4r::SendsOf(d.effects.sends, 13) == std::array<float, 3>{0.0f, 0.5f, 0.0f}));
    XSFLIB_CHECK((nw4r::SendsOf(d.effects.sends, 14) == std::array<float, 3>{}));

    // A descriptor without effects has neither block.
    const auto dry_back = ArchiveDescriptor::Parse(dry.Serialize());

    XSFLIB_CHECK(dry_back && dry_back->effects.setups.empty() && dry_back->effects.sends.empty());

    // An effect of a type that the player doesn't know is skipped. One whose words don't fit its type, or whose bus
    // isn't A, B or C, is skipped too.
    std::vector<uint8_t> odd = raw;
    const std::size_t first_effect = dry.Serialize().size() + 4;
    odd[first_effect + 1] = 9;
    const auto skipped = ArchiveDescriptor::Parse(odd);

    XSFLIB_CHECK(skipped && skipped->effects.setups.size() == 1 &&
                 skipped->effects.setups[0].type == nw4r::EffectType::kChorus && skipped->effects.sends.size() == 2);

    odd = raw;
    odd[first_effect] = 3;

    XSFLIB_CHECK(ArchiveDescriptor::Parse(odd) && ArchiveDescriptor::Parse(odd)->effects.setups.size() == 1);

    // Effects that run past the end of the descriptor make it unreadable.
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 30)).has_value());
}

void TestReverbHi2008()
{
    // FxReverbHi of 2008 takes 11 words: the six of the older builds, then earlyMode, preDelayTimeMax, fusedMode,
    // earlyGain and fusedGain. Each word reads back in its place.
    ArchiveDescriptor d;
    d.archive_path = "arc/sound/c.brsar";
    d.microcode_path = "ax_microcode.bin";
    d.snd_build = 20090701;

    nw4r::EffectSetup reverb;
    reverb.type = nw4r::EffectType::kReverbHi2008;
    for (std::size_t i = 0; i < reverb.params.size(); i++)
    {
        reverb.params[i] = std::bit_cast<uint32_t>(0.25f * static_cast<float>(i + 1));
    }
    d.effects.setups = {reverb};

    ArchiveDescriptor dry = d;
    dry.effects.setups.clear();
    const std::vector<uint8_t> raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    // 4 for the count, 12 + 11 * 4 for the reverb, and 4 for a count of 0 sends.
    XSFLIB_CHECK(raw.size() == dry.Serialize().size() + 4 + 56 + 4);
    XSFLIB_CHECK(back && back->effects.setups.size() == 1 && back->effects.setups[0].bus == 0 &&
                 back->effects.setups[0].type == nw4r::EffectType::kReverbHi2008 &&
                 back->effects.setups[0].params == reverb.params);
}

void TestMoodEffect()
{
    // The Photo Channel 1.1's mood effect has one word, and a send can go to the main mix (docs/wsf.md, "Archive
    // descriptor"). Either requires version 3.
    ArchiveDescriptor d;
    d.version = ArchiveDescriptor::kMoodVersion;
    d.archive_path = "Sound/b.brsar";
    d.microcode_path = "ax_microcode.bin";
    d.snd_build = 20081120;

    nw4r::EffectSetup mood;
    mood.bus = 1;
    mood.type = nw4r::EffectType::kPhotoMood;
    mood.params = {4};
    mood.sounds = {2};
    d.effects.setups = {mood};
    d.effects.sends = {{2, nw4r::kMainSendBus, -1.0f}, {2, 2, -1.0f}, {2, 1, 1.0f}};
    const auto back = ArchiveDescriptor::Parse(d.Serialize());

    XSFLIB_CHECK(back && back->version == 3 && !back->Check() && back->effects.setups.size() == 1 &&
                 back->effects.setups[0].type == nw4r::EffectType::kPhotoMood &&
                 back->effects.setups[0].params == mood.params && back->effects.setups[0].sounds == mood.sounds);
    XSFLIB_CHECK(back && back->effects.sends.size() == 3 && nw4r::MainSendOf(back->effects.sends, 2) == -1.0f &&
                 nw4r::MainSendOf(back->effects.sends, 3) == 0.0f &&
                 (nw4r::SendsOf(back->effects.sends, 2) == std::array<float, 3>{0.0f, 1.0f, -1.0f}));
    XSFLIB_CHECK(nw4r::RequiresMoodVersion(d.effects));

    nw4r::ArchiveEffects reverb_only;
    reverb_only.setups = {{.bus = 2, .type = nw4r::EffectType::kReverbHi, .params = {}, .presets = {}, .sounds = {}}};

    XSFLIB_CHECK(!nw4r::RequiresMoodVersion(reverb_only));

    nw4r::ArchiveEffects main_only;
    main_only.sends = {{2, nw4r::kMainSendBus, -1.0f}};

    XSFLIB_CHECK(nw4r::RequiresMoodVersion(main_only));

    // A variant adds its effects and sends after the archive's.
    const nw4r::EffectVariant variant{.name = " mood 4", .sounds = {2}, .setups = {mood}, .sends = d.effects.sends};
    reverb_only.variants = {variant};
    const nw4r::ArchiveEffects with = nw4r::WithVariant(reverb_only, variant);

    XSFLIB_CHECK(with.setups.size() == 2 && with.setups[0].type == nw4r::EffectType::kReverbHi &&
                 with.setups[1].type == nw4r::EffectType::kPhotoMood && with.sends.size() == 3 &&
                 with.variants.empty());

    // A mini's descriptor of version 3 takes the place of its library's descriptor of version 1. Players of 1.7.0 and
    // older refuse the mini. This player plays it.
    ArchiveDescriptor v1 = d;
    v1.version = ArchiveDescriptor::kVersion;
    v1.effects = {};
    ProgramBuilder lib;
    lib.AddFile("Sound/b.brsar", {1, 2, 3});
    ProgramBuilder mini;
    mini.AddSound(2);

    std::map<std::string, std::vector<uint8_t>> fs;
    fs["a/lib.wsflib"] = xsflib::psf::WritePsf(kPsfVersionWsf, v1.Serialize(), lib.Data(), {});
    fs["a/plain.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, {}, mini.Data(), {{"_lib", "lib.wsflib"}});
    fs["a/mood.miniwsf"] = xsflib::psf::WritePsf(kPsfVersionWsf, d.Serialize(), mini.Data(), {{"_lib", "lib.wsflib"}});
    const FileReader reader = xsflib::testing::MapReader(fs);
    LoadedSet set;

    XSFLIB_CHECK(!LoadSet("a/plain.miniwsf", reader, set) && set.archive && set.archive->version == 1 &&
                 set.archive->effects.setups.empty());
    XSFLIB_CHECK(!LoadSet("a/mood.miniwsf", reader, set) && set.archive && set.archive->version == 3 &&
                 set.archive->effects.setups.size() == 1 && SoundsToPlay(set) == std::vector<uint32_t>{2});
}

void TestTracks()
{
    // The tracks that the game plays of some sounds follow the sends (docs/wsf.md, "Archive descriptor").
    ArchiveDescriptor d;
    d.archive_path = "Sound/b.brsar";
    d.microcode_path = "ax_microcode.bin";
    d.snd_build = 20080000;
    d.effects.sends = {{12, 0, 0.25f}};
    d.effects.tracks = {{12, 0x07ff}, {13, 0x7fff}};

    ArchiveDescriptor without = d;
    without.effects.tracks.clear();
    const std::vector<uint8_t> raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    // 4 for the count, and 2 * 8 for the sounds and their tracks.
    XSFLIB_CHECK(raw.size() == without.Serialize().size() + 4 + 16);
    XSFLIB_CHECK(back && back->effects.sends.size() == 1 && back->effects.tracks.size() == 2 &&
                 back->effects.tracks[0].sound == 12 && back->effects.tracks[0].tracks == 0x07ff &&
                 back->effects.tracks[1].sound == 13 && back->effects.tracks[1].tracks == 0x7fff);

    // A sound that the block doesn't list plays every track.
    XSFLIB_CHECK(nw4r::TracksOf(d.effects.tracks, 13) == 0x7fff &&
                 nw4r::TracksOf(d.effects.tracks, 14) == nw4r::kAllTracks);

    // Tracks without effects or sends follow empty blocks of both.
    ArchiveDescriptor plain = without;
    plain.effects.sends.clear();
    ArchiveDescriptor only = plain;
    only.effects.tracks = {{5, 0x0001}};
    const auto only_back = ArchiveDescriptor::Parse(only.Serialize());

    XSFLIB_CHECK(only.Serialize().size() == plain.Serialize().size() + 4 + 4 + 4 + 8);
    XSFLIB_CHECK(only_back && only_back->effects.setups.empty() && only_back->effects.sends.empty() &&
                 only_back->effects.tracks.size() == 1 && only_back->effects.tracks[0].sound == 5 &&
                 only_back->effects.tracks[0].tracks == 0x0001);

    // A sound's tracks above track 15 make the player skip the sound's entry. Tracks that run past the end of the
    // descriptor make it unreadable.
    std::vector<uint8_t> odd = raw;
    odd[raw.size() - 16 + 6] = 1;
    const auto skipped = ArchiveDescriptor::Parse(odd);

    XSFLIB_CHECK(skipped && skipped->effects.tracks.size() == 1 && skipped->effects.tracks[0].sound == 13);
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 4)).has_value());

    // A wsf_tracks tag replaces the tracks that the descriptor gives a sound. A malformed one leaves them.
    XSFLIB_CHECK(ArrangementFromTags({}, 0x07ff).tracks == 0x07ff);
    XSFLIB_CHECK(ArrangementFromTags({{"wsf_tracks", "11-13"}}, 0x07ff).tracks == 0x3800);
    XSFLIB_CHECK(ArrangementFromTags({{"wsf_tracks", "16"}}, 0x07ff).tracks == 0x07ff);
}

void TestEffectsBySound()
{
    // Bus A: a reverb for the sounds that no other effect on bus A lists, and one for sounds 5 and 6. Bus B: a chorus
    // for sound 6 alone. Bus C: a delay whose presets sound 7 picks. Each effect shows as its type and first word.
    using Types = std::vector<std::pair<nw4r::EffectType, uint32_t>>;
    constexpr auto kReverb = nw4r::EffectType::kReverbHi;
    constexpr auto kChorus = nw4r::EffectType::kChorus;
    constexpr auto kDelay = nw4r::EffectType::kDelay;
    nw4r::EffectSetup reverb;
    nw4r::EffectSetup other_reverb;
    other_reverb.params[0] = 1;
    other_reverb.sounds = {5, 6};
    nw4r::EffectSetup chorus;
    chorus.bus = 1;
    chorus.type = kChorus;
    chorus.sounds = {6};
    nw4r::EffectSetup delay;
    delay.bus = 2;
    delay.type = kDelay;
    delay.presets = {{1}, {2}};
    delay.sounds = {7};
    const std::vector<nw4r::EffectSetup> setups = {reverb, other_reverb, chorus, delay};
    const auto types_for = [&setups](uint32_t sound)
    {
        Types types;
        for (const nw4r::EffectSetup& setup : nw4r::EffectsFor(setups, sound))
        {
            types.emplace_back(setup.type, setup.params[0]);
        }

        return types;
    };

    const Types unlisted = types_for(nw4r::kUnlistedSound);
    const Types five = types_for(5);
    const Types six = types_for(6);
    const Types seven = types_for(7);

    // An effect with presets counts for every sound. Its sounds pick the presets.
    XSFLIB_CHECK((unlisted == Types{{kReverb, 0}, {kDelay, 0}}));
    XSFLIB_CHECK((five == Types{{kReverb, 1}, {kDelay, 0}}));
    XSFLIB_CHECK((six == Types{{kReverb, 1}, {kChorus, 0}, {kDelay, 0}}));
    XSFLIB_CHECK(seven == unlisted && nw4r::FollowsVariables(setups, 7) && !nw4r::FollowsVariables(setups, 5));
}

} // namespace

int main()
{
    TestArchiveMode();
    TestSoundsThatPlayTogether();
    TestEffects();
    TestReverbHi2008();
    TestMoodEffect();
    TestEffectsBySound();
    TestTracks();
    TestArrangementTags();
    TestVariantArrangement();
    TestDescriptor();
    TestLoadSet();
    TestVersionByte();

    return xsflib::testing::Summary("format");
}
