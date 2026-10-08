// SPDX-License-Identifier: MIT

// Unit tests for the WSF file format code (src/wsf/format.*): PSF container, tags, times, program chunks, process and
// archive descriptors, the driver block's check and _lib loading.

#include "wsf/format.h"

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "common/bytes.h"

using namespace wsf;

static void TestTimes()
{
    WSF_CHECK(ParseTime("") == -1);
    WSF_CHECK(ParseTime("abc") == -1);
    WSF_CHECK(ParseTime("5") == 5000);
    WSF_CHECK(ParseTime("1.5") == 1500);
    WSF_CHECK(ParseTime("1,25") == 1250);
    WSF_CHECK(ParseTime("2:03") == 123000);
    WSF_CHECK(ParseTime("2:03.5") == 123500);
    WSF_CHECK(ParseTime("1:00:00") == 3600000);
    WSF_CHECK(ParseTime(" 0:10.000 ") == 10000);
    WSF_CHECK(ParseTime("1:2:3:4") == -1);
    WSF_CHECK(ParseTime("-1") == -1);
    WSF_CHECK(ParseTime("nan") == -1);
    WSF_CHECK(ParseTime("inf") == -1);
    WSF_CHECK(ParseTime("1e300") == -1);
    WSF_CHECK(ParseTime("99999999999999999999:00") == -1);

    WSF_CHECK(FormatTime(0) == "0:00.000");
    WSF_CHECK(FormatTime(65998) == "1:05.998");
    WSF_CHECK(FormatTime(600000) == "10:00.000");

    for (long long ms : {0LL, 1LL, 999LL, 59999LL, 61001LL, 3599999LL})
    {
        WSF_CHECK(ParseTime(FormatTime(ms)) == ms);
    }
}

static void TestPsfRoundTrip()
{
    std::vector<uint8_t> reserved = {1, 2, 3, 4, 5};
    Tags tags = {{"title", "Test"}, {"comment", "line one\nline two"}, {"_lib", "x.wsflib"}};
    std::vector<uint8_t> program(100000);
    for (std::size_t i = 0; i < program.size(); i++)
    {
        program[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    }

    const auto file = WritePsf(kPsfVersionWsf, reserved, program, tags);
    PsfFile psf;
    const auto err = ParsePsf(file, psf);

    WSF_CHECK(file.size() > 16 && std::memcmp(file.data(), "PSF", 3) == 0 && file[3] == 0x27);
    WSF_CHECK(!err);
    WSF_CHECK(psf.version == kPsfVersionWsf);
    WSF_CHECK(psf.reserved == reserved);
    WSF_CHECK(psf.program == program);
    WSF_CHECK(psf.tags == tags);

    // Corruption of the compressed program is detected by the CRC.
    auto bad = file;
    bad[16 + reserved.size() + 10] ^= 0x55;

    WSF_CHECK(ParsePsf(bad, psf) == "program CRC mismatch");

    // Truncation.
    auto cut = file;
    cut.resize(20);

    WSF_CHECK(ParsePsf(cut, psf) == "truncated PSF file");
    WSF_CHECK(ParsePsf(std::vector<uint8_t>{'P', 'S'}, psf) == "not a PSF file");

    // A program whose zlib stream stops short, with a size and CRC that match what's left of it.
    const uint32_t half = LoadLe32(&file[8]) / 2;
    std::vector<uint8_t> short_stream(file.begin(), file.begin() + 16 + reserved.size() + half);
    StoreLe32(&short_stream[8], half);
    StoreLe32(&short_stream[12], static_cast<uint32_t>(crc32(0, &short_stream[16 + reserved.size()], half)));

    WSF_CHECK(ParsePsf(short_stream, psf) == "truncated program data");
}

static void TestTagParsing()
{
    const std::string kHeader = std::string("PSF") + static_cast<char>(kPsfVersionWsf) + std::string(12, '\0');
    const std::string kTaggedPsf = kHeader +
                                   "[TAG]Title = Hello \nTITLE2=x\ncomment=a\ncomment=b\nartist=me\ncomment=c\n"
                                   "no equals sign\n=empty name\nlength=1:00\r\n";
    std::vector<uint8_t> data(kTaggedPsf.begin(), kTaggedPsf.end());
    PsfFile psf;

    WSF_CHECK(!ParsePsf(data, psf));
    WSF_CHECK(psf.tags["title"] == "Hello");
    WSF_CHECK(psf.tags["title2"] == "x");
    WSF_CHECK(psf.tags["comment"] == "c"); // a and b join, and the later c replaces them
    WSF_CHECK(psf.tags["artist"] == "me");
    WSF_CHECK(psf.tags["length"] == "1:00");
    WSF_CHECK(psf.tags.count("") == 0);

    // Consecutive lines with the same name join.
    const std::string kRepeatedTagPsf = kHeader + "[TAG]comment=a\ncomment=b\n";
    std::vector<uint8_t> data2(kRepeatedTagPsf.begin(), kRepeatedTagPsf.end());

    WSF_CHECK(!ParsePsf(data2, psf));
    WSF_CHECK(psf.tags["comment"] == "a\nb");
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

    WSF_CHECK(bytes.size() == 24);
    WSF_CHECK(std::memcmp(bytes.data(), "WSFP", 4) == 0);
    WSF_CHECK(back.has_value());
    WSF_CHECK(back->version == d.version && back->entry == d.entry && back->status_address == d.status_address &&
              back->ios_version == d.ios_version && back->flags == d.flags);

    // Data without the magic, or too short, isn't a descriptor.
    WSF_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(24, 0)).has_value());
    WSF_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(8, 0)).has_value());
}

static void TestChunks()
{
    ProgramBuilder b;
    constexpr uint8_t kMemoryBytes[] = {1, 2, 3};
    b.AddMemory(0x80004000, kMemoryBytes, sizeof(kMemoryBytes));
    b.AddFile("Sound/a.brsar", std::vector<uint8_t>{9, 8, 7, 6, 5});
    b.AddMemory(0x8170001c, std::vector<uint8_t>{0, 0, 0, 1});

    ProgramChunks chunks;
    const auto err = ParseProgram(b.Data(), chunks);
    const auto& mem = chunks.memory;
    const auto& files = chunks.files;

    WSF_CHECK(b.Data().size() % 4 == 0);
    WSF_CHECK(!err);
    WSF_CHECK(mem.size() == 2 && files.size() == 1);
    WSF_CHECK(mem[0].address == 0x80004000 && mem[0].data == std::vector<uint8_t>(kMemoryBytes, kMemoryBytes + 3));
    WSF_CHECK(mem[1].address == 0x8170001c && mem[1].data.size() == 4);
    WSF_CHECK(files[0].path == "Sound/a.brsar" && files[0].data == (std::vector<uint8_t>{9, 8, 7, 6, 5}));

    // Unknown chunks are skipped.
    std::vector<uint8_t> prog = {'X', 'X', 'X', 'X', 4, 0, 0, 0, 1, 2, 3, 4};
    prog.insert(prog.end(), b.Data().begin(), b.Data().end());
    ProgramChunks skipped;

    WSF_CHECK(!ParseProgram(prog, skipped));
    WSF_CHECK(skipped.memory.size() == 2 && skipped.files.size() == 1);

    // A truncated chunk is an error, even when its size is so large that adding it to the position wraps around on a
    // 32-bit host.
    std::vector<uint8_t> trunc = {'M', 'E', 'M', ' ', 100, 0, 0, 0, 1, 2};
    std::vector<uint8_t> huge = {'M', 'E', 'M', ' ', 0xf8, 0xff, 0xff, 0xff, 1, 2, 3, 4};
    ProgramChunks truncated;

    WSF_CHECK(ParseProgram(trunc, truncated).has_value());
    WSF_CHECK(ParseProgram(huge, truncated).has_value());
}

// The error LoadSet gives for `path`, or an empty string if the set loads.
static std::string LoadError(const std::string& path, const FileReader& reader)
{
    LoadedSet set;
    const auto err = LoadSet(path, reader, set);

    return err ? *err : std::string();
}

static void TestLoadSet()
{
    // dir/lib.wsflib holds the descriptor and the base memory. dir/sub/x.miniwsf loads it with _lib=../lib.wsflib, and
    // loads extra.miniwsf from the same directory with _lib2.
    std::map<std::string, std::vector<uint8_t>> fs;
    ProcessDescriptor d;
    d.status_address = 0x1234;

    {
        ProgramBuilder b;
        b.AddMemory(0x1000, std::vector<uint8_t>{1, 1, 1, 1});
        b.AddFile("f", std::vector<uint8_t>{1});
        fs["dir/lib.wsflib"] = WritePsf(kPsfVersionWsf, d.Serialize(), b.Data(), {{"game", "G"}});
    }

    {
        ProgramBuilder b;
        b.AddMemory(0x1000, std::vector<uint8_t>{2});
        fs["dir/sub/x.miniwsf"] =
            WritePsf(kPsfVersionWsf, {}, b.Data(),
                     {{"_lib", "../lib.wsflib"}, {"_lib2", "extra.miniwsf"}, {"title", "X"}, {"length", "1:00"}});
    }

    {
        ProgramBuilder b;
        b.AddMemory(0x1000, std::vector<uint8_t>{3});
        fs["dir/sub/extra.miniwsf"] = WritePsf(kPsfVersionWsf, {}, b.Data(), {{"title", "extra"}});
    }

    const FileReader reader = [&](const std::string& path, std::vector<uint8_t>& out)
    {
        auto it = fs.find(std::filesystem::path(path).lexically_normal().generic_string());
        if (it == fs.end())
        {
            return false;
        }

        out = it->second;

        return true;
    };

    LoadedSet set;
    const auto err = LoadSet("dir/sub/x.miniwsf", reader, set);

    WSF_CHECK(!err);
    if (err || set.sources.size() != 3 || set.memory.size() != 3)
    {
        std::fprintf(stderr, "LoadSet: %s\n", err ? err->c_str() : "wrong result");
        failures++;
        return;
    }

    WSF_CHECK(set.sources[0] == "dir/sub/../lib.wsflib");
    WSF_CHECK(set.sources[1] == "dir/sub/x.miniwsf");
    WSF_CHECK(set.sources[2] == "dir/sub/extra.miniwsf");
    WSF_CHECK(set.memory[0].data[0] == 1 && set.memory[1].data[0] == 2 && set.memory[2].data[0] == 3);
    WSF_CHECK(set.files.size() == 1);
    WSF_CHECK(set.descriptor && set.descriptor->status_address == 0x1234);
    WSF_CHECK(set.tags["title"] == "X" && set.tags["length"] == "1:00" && set.tags.count("game") == 0);

    // An empty _lib2 names no library, like an empty _lib, and _lib3 still loads.
    {
        ProgramBuilder b;
        b.AddMemory(0x1000, std::vector<uint8_t>{4});
        fs["dir/sub/v.miniwsf"] = WritePsf(kPsfVersionWsf, {}, b.Data(),
                                           {{"_lib", "../lib.wsflib"}, {"_lib2", ""}, {"_lib3", "extra.miniwsf"}});
    }

    LoadedSet gap;
    WSF_CHECK(!LoadSet("dir/sub/v.miniwsf", reader, gap));
    WSF_CHECK(gap.sources.size() == 3 && gap.sources[2] == "dir/sub/extra.miniwsf");

    // A missing library, a wrong PSF version and a set without a descriptor are errors.
    fs["dir/y.miniwsf"] = WritePsf(kPsfVersionWsf, {}, {}, {{"_lib", "missing.wsflib"}});
    fs["dir/z.minincsf"] = WritePsf(0x25, {}, {}, {{"_lib", "lib.wsflib"}});
    fs["dir/w.miniwsf"] = WritePsf(kPsfVersionWsf, {}, {}, {{"title", "w"}});

    WSF_CHECK(LoadError("dir/y.miniwsf", reader) == "can't read dir/missing.wsflib");
    WSF_CHECK(LoadError("dir/z.minincsf", reader).find("not a WSF file") != std::string::npos);
    WSF_CHECK(LoadError("dir/w.miniwsf", reader).starts_with("no process or archive descriptor"));

    // So is a descriptor with an unsupported version.
    ProcessDescriptor v2;
    v2.version = 2;
    fs["dir/v2.wsflib"] = WritePsf(kPsfVersionWsf, v2.Serialize(), {}, {});

    WSF_CHECK(LoadSet("dir/v2.wsflib", reader, set).has_value());

    // Or flags other than bit 0 alone.
    for (const uint32_t flags : {0u, 3u, 0x80000001u})
    {
        ProcessDescriptor odd;
        odd.flags = flags;
        fs["dir/flags.wsflib"] = WritePsf(kPsfVersionWsf, odd.Serialize(), {}, {});
        WSF_CHECK(LoadSet("dir/flags.wsflib", reader, set).has_value());
    }

    // A driver block of version 1 plays. A block of another version doesn't, including when a .miniwsf patches the
    // version. Like the rest of the Wii's memory, the block's words are big-endian.
    const auto driver_block = [](uint32_t version)
    {
        std::vector<uint8_t> block(0x20, 0);
        StoreBe32(&block[0], DriverBlock::kMagic);
        StoreBe32(&block[DriverBlock::kVersionOffset], version);
        ProgramBuilder b;
        b.AddMemory(DriverBlock::kAddress, block);

        return b.Data();
    };
    fs["dir/driver1.wsflib"] = WritePsf(kPsfVersionWsf, d.Serialize(), driver_block(1), {});
    fs["dir/driver2.wsflib"] = WritePsf(kPsfVersionWsf, d.Serialize(), driver_block(2), {});
    {
        ProgramBuilder b;
        b.AddMemory(DriverBlock::kAddress + DriverBlock::kVersionOffset, std::vector<uint8_t>{0, 0, 0, 2});
        fs["dir/patched.miniwsf"] = WritePsf(kPsfVersionWsf, {}, b.Data(), {{"_lib", "driver1.wsflib"}});
    }

    WSF_CHECK(!LoadSet("dir/driver1.wsflib", reader, set));
    WSF_CHECK(LoadSet("dir/driver2.wsflib", reader, set).has_value());
    WSF_CHECK(LoadSet("dir/patched.miniwsf", reader, set).has_value());

    // A _lib cycle ends with an error instead of recursing forever.
    fs["dir/c1.miniwsf"] = WritePsf(kPsfVersionWsf, {}, {}, {{"_lib", "c2.miniwsf"}});
    fs["dir/c2.miniwsf"] = WritePsf(kPsfVersionWsf, {}, {}, {{"_lib", "c1.miniwsf"}});

    WSF_CHECK(LoadError("dir/c1.miniwsf", reader) == "_lib nesting too deep");

    // So does a set that names more files than a set can have. Each of these names the next one ten times. Loading them
    // all would take 10^10 loads.
    for (int level = 0; level < 10; level++)
    {
        Tags tags;
        for (int n = 2; n <= 11; n++)
        {
            tags["_lib" + std::to_string(n)] = "f" + std::to_string(level + 1) + ".miniwsf";
        }

        fs["dir/f" + std::to_string(level) + ".miniwsf"] = WritePsf(kPsfVersionWsf, {}, {}, tags);
    }

    fs["dir/f10.miniwsf"] = WritePsf(kPsfVersionWsf, {}, {}, {});

    WSF_CHECK(LoadError("dir/f0.miniwsf", reader) == "more than 64 files in the set");
}

static void TestLoadSetInArchive()
{
    // foobar2000's path for a file in an archive: unpack://zip|<length>|<the archive's path>|<its path in the archive>.
    const std::string archive = "unpack://zip|23|file://C:\\music\\set.zip|";
    std::map<std::string, std::vector<uint8_t>> fs;
    fs[archive + "set.wsflib"] = WritePsf(kPsfVersionWsf, ProcessDescriptor{}.Serialize(), {}, {});
    fs[archive + "x.miniwsf"] = WritePsf(kPsfVersionWsf, {}, {}, {{"_lib", "set.wsflib"}});

    const FileReader reader = [&](const std::string& path, std::vector<uint8_t>& out)
    {
        auto it = fs.find(path);
        if (it == fs.end())
        {
            return false;
        }

        out = it->second;

        return true;
    };

    LoadedSet set;
    const auto err = LoadSet(archive + "x.miniwsf", reader, set, "/\\|");
    const std::string without = LoadError(archive + "x.miniwsf", reader);

    // With | as a separator, the library is found in the archive. Without it, the library is found beside the archive.
    WSF_CHECK(!err && set.sources.size() == 2 && set.sources[0] == archive + "set.wsflib");
    WSF_CHECK(without == "can't read unpack://zip|23|file://C:\\music\\set.wsflib");
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

    WSF_CHECK(raw.size() % 4 == 0);
    WSF_CHECK(back && back->version == d.version && back->flags == d.flags && back->output_mode == d.output_mode &&
              back->microcode_address == d.microcode_address && back->archive_path == d.archive_path &&
              back->microcode_path == d.microcode_path && back->snd_build == d.snd_build);
    WSF_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 22)).has_value());

    // A descriptor that ends after its paths names no build.
    const auto without_build = ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 4));
    WSF_CHECK(without_build && without_build->microcode_path == d.microcode_path && without_build->snd_build == 0);
    WSF_CHECK(!ProcessDescriptor::Parse(raw).has_value());

    // A player only accepts version 1, with bit 0 of the flags set and no other, in stereo.
    WSF_CHECK(d.Check().has_value());

    ArchiveDescriptor v1 = d;
    v1.version = 1;
    v1.flags = kFlagDspRequired;
    v1.output_mode = kOutputStereo;

    WSF_CHECK(!v1.Check());

    for (const uint32_t flags : {0u, 3u, 0x80000001u})
    {
        ArchiveDescriptor odd = v1;
        odd.flags = flags;
        WSF_CHECK(odd.Check().has_value());
    }

    for (const uint32_t mode : {0u, 2u, 3u})
    {
        ArchiveDescriptor odd = v1;
        odd.output_mode = mode;
        WSF_CHECK(odd.Check().has_value());
    }

    const auto raw1 = v1.Serialize();

    // A later descriptor of the other kind decides the mode. The last SND chunk selects the sound.
    ProgramBuilder lib;
    lib.AddFile("Sound/b.brsar", {1, 2, 3});
    lib.AddSound(5);
    ProgramBuilder mini;
    mini.AddSound(7);
    ProcessDescriptor game;

    std::map<std::string, std::vector<uint8_t>> fs;
    fs["a/lib.wsflib"] = WritePsf(kPsfVersionWsf, raw1, lib.Data(), {});
    fs["a/x.miniwsf"] = WritePsf(kPsfVersionWsf, {}, mini.Data(), {{"_lib", "lib.wsflib"}});
    fs["a/y.miniwsf"] = WritePsf(kPsfVersionWsf, game.Serialize(), {}, {{"_lib", "lib.wsflib"}});
    fs["a/nosound.wsflib"] = WritePsf(kPsfVersionWsf, raw1, {}, {});
    fs["a/z.miniwsf"] = WritePsf(kPsfVersionWsf, {}, {}, {{"_lib", "nosound.wsflib"}});

    const FileReader reader = [&](const std::string& path, std::vector<uint8_t>& out)
    {
        auto it = fs.find(path);
        if (it == fs.end())
        {
            return false;
        }

        out = it->second;

        return true;
    };

    LoadedSet set;

    WSF_CHECK(!LoadSet("a/x.miniwsf", reader, set));
    WSF_CHECK(set.archive.has_value() && !set.descriptor.has_value());
    WSF_CHECK(set.sound == 7u);
    WSF_CHECK(set.files.size() == 1 && set.files[0].path == "Sound/b.brsar");

    WSF_CHECK(!LoadSet("a/y.miniwsf", reader, set));
    WSF_CHECK(set.descriptor.has_value() && !set.archive.has_value());

    WSF_CHECK(LoadSet("a/z.miniwsf", reader, set).has_value()); // archive mode without a sound
}

int main()
{
    TestTimes();
    TestArchiveMode();
    TestPsfRoundTrip();
    TestTagParsing();
    TestDescriptor();
    TestChunks();
    TestLoadSet();
    TestLoadSetInArchive();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("format tests passed\n");

    return 0;
}
