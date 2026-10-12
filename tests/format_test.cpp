// SPDX-License-Identifier: MIT

// Unit tests for the WSF file format code (src/wsf/format.*): the version byte, the process and archive descriptors,
// and what WSF's format gives xsflib's set loader: its PSF version, its descriptors and its driver block. xsflib's
// psf_test and psf_set_test cover the PSF container, tags, times, program chunks and the loader.

#include "wsf/format.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

using xsflib::testing::LoadError;

using namespace wsf;

static void TestVersionByte()
{
    // A file that WSF writes has version byte 0x27 (docs/wsf.md, "Version byte"). xsflib's psf_test checks the PSF
    // container with another version byte.
    const auto file = xsflib::psf::WritePsf(kPsfVersionWsf, {}, {}, {});

    XSFLIB_CHECK(file.size() >= 16 && std::memcmp(file.data(), "PSF", 3) == 0 && file[3] == 0x27);
}

static void TestDescriptor()
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

static void TestLoadSet()
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

    // A driver block of version 1 (NW4R SND's driver) or 2 (JAudio2's) plays. A block of another version doesn't. Like
    // the rest of the Wii's memory, the block's words are big-endian.
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

static void TestArchiveMode()
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

    // A player only accepts version 1, with bit 0 of the flags set and no other, in stereo.
    XSFLIB_CHECK(d.Check().has_value());

    ArchiveDescriptor v1 = d;
    v1.version = 1;
    v1.flags = kFlagDspRequired;
    v1.output_mode = kOutputStereo;

    XSFLIB_CHECK(!v1.Check());

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

int main()
{
    TestArchiveMode();
    TestDescriptor();
    TestLoadSet();
    TestVersionByte();

    return xsflib::testing::Summary("format");
}
