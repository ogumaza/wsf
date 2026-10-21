// SPDX-License-Identifier: MIT

// WSF ripper (see rip.h): the parts that every game shares. The file of each sound library holds the rest (see
// rip/sound_libraries.h).

#include "rip/rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/disc/containers.h"
#include "ntdlib/disc/image.h"
#include "ntdlib/dol.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/psf/psf.h"

#include "rip/sound_libraries.h"
#include "wsf/format.h"

namespace wsf::rip
{

ntdlib::disc::GameOptions DiscOptions()
{
    ntdlib::disc::GameOptions options{ntdlib::disc::Console::kWii, "WSF"};
    options.image.open_container = ntdlib::disc::OpenContainer;
    options.titles = true;
    options.cpk_sound_archives = true;

    return options;
}

std::nullopt_t LibraryError(std::string* error, const std::string& message)
{
    if (error)
    {
        *error = message;
    }

    return std::nullopt;
}

std::optional<std::vector<uint8_t>> BuildGameLibrary(const ntdlib::disc::GameFiles& game, const ntdlib::Dol& dol,
                                                     const GameLibrary& library, const Tags& tags, std::string* error)
{
    // The game's code and data, then the driver. The game's main becomes a branch to the driver's entry (b, primary
    // opcode 18, with the offset in bits 6 to 29).
    ProgramBuilder program;
    for (const ntdlib::Dol::Section& section : dol.Sections())
    {
        program.AddMemory(section.address, section.data.data(), section.data.size());
    }

    program.AddMemory(library.base, library.driver);
    uint8_t branch[4];
    xsflib::StoreBe32(branch, 0x48000000 | ((library.entry - library.main) & 0x03fffffc));
    program.AddMemory(library.main, branch, sizeof(branch));

    // The partition's header and the disc's files.
    program.AddFile(kHeaderPath, game.boot);
    std::vector<uint8_t> bytes;
    for (const std::string& path : library.paths)
    {
        if (!game.read_file || !game.read_file(path, bytes))
        {
            return LibraryError(error, "can't read " + path + " from the disc");
        }

        program.AddFile(path, bytes);
    }

    if (const std::optional<std::string> too_large = ProgramTooLarge(program.Data().size()))
    {
        return LibraryError(error, *too_large + " " + library.too_large_advice);
    }

    constexpr std::size_t kTmdIosOffset = 0x188;
    ProcessDescriptor process;
    process.entry = dol.Entry();
    process.status_address = DriverBlock::kAddress + DriverBlock::kStatusOffset;
    process.ios_version =
        game.tmd.size() >= kTmdIosOffset + 4 ? xsflib::LoadBe32(&game.tmd[kTmdIosOffset]) : library.ios_version;

    return xsflib::psf::WritePsf(kPsfVersionWsf, process.Serialize(), program.Data(), tags);
}

std::optional<std::string> ProgramTooLarge(std::size_t size)
{
    if (size <= xsflib::psf::kMaxProgramSize)
    {
        return std::nullopt;
    }

    // Whole mebibytes, rounded up. A program just over the limit then shows as larger than it.
    const auto mebibytes = [](std::size_t bytes)
    {
        return std::to_string((bytes + (std::size_t{1} << 20) - 1) >> 20) + " MiB";
    };

    return "the library would hold " + mebibytes(size) + ". A player loads at most " +
           mebibytes(xsflib::psf::kMaxProgramSize) + ".";
}

namespace
{

// True if the file at `path` holds exactly `data`. The file is compared in blocks of 1 MiB. A library can be hundreds
// of megabytes.
bool FileHolds(const std::filesystem::path& path, std::span<const uint8_t> data)
{
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) != data.size() || ec)
    {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    std::vector<char> block(std::size_t{1} << 20);
    for (std::size_t at = 0; at < data.size(); at += block.size())
    {
        const std::size_t size = std::min(block.size(), data.size() - at);
        if (!file.read(block.data(), static_cast<std::streamsize>(size)) ||
            std::memcmp(block.data(), data.data() + at, size) != 0)
        {
            return false;
        }
    }

    return true;
}

} // namespace

std::string LibraryName(const std::filesystem::path& folder, xsflib::rip::FileNames& names, const std::string& base,
                        std::span<const uint8_t> library)
{
    std::string name = names.Library(base);
    std::error_code ec;
    while (std::filesystem::exists(folder / xsflib::PathFromUtf8(name), ec) &&
           !FileHolds(folder / xsflib::PathFromUtf8(name), library))
    {
        name = names.Library(base);
    }

    return name;
}

} // namespace wsf::rip
