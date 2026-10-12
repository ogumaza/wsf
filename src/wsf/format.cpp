// SPDX-License-Identifier: MIT

// The WSF file format (see format.h and docs/wsf.md).

#include "wsf/format.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/streaming.h"

namespace wsf
{

std::vector<uint8_t> ProcessDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    xsflib::psf::Put32(v, kMagic);
    xsflib::psf::Put32(v, version);
    xsflib::psf::Put32(v, entry);
    xsflib::psf::Put32(v, status_address);
    xsflib::psf::Put32(v, ios_version);
    xsflib::psf::Put32(v, flags);

    return v;
}

std::optional<ProcessDescriptor> ProcessDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 24 || xsflib::LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ProcessDescriptor d;
    d.version = xsflib::LoadLe32(&data[4]);
    d.entry = xsflib::LoadLe32(&data[8]);
    d.status_address = xsflib::LoadLe32(&data[12]);
    d.ios_version = xsflib::LoadLe32(&data[16]);
    d.flags = xsflib::LoadLe32(&data[20]);

    return d;
}

std::optional<std::string> ProcessDescriptor::Check() const
{
    if (auto err = xsflib::psf::CheckDescriptorVersion("process", version, kVersion))
    {
        return err;
    }

    return xsflib::psf::CheckDescriptorFlags("process", flags, kFlagDspRequired);
}

std::vector<uint8_t> ArchiveDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    xsflib::psf::Put32(v, kMagic);
    xsflib::psf::Put32(v, version);
    xsflib::psf::Put32(v, flags);
    xsflib::psf::Put32(v, output_mode);
    xsflib::psf::Put32(v, microcode_address);

    xsflib::psf::PutPath(v, archive_path);
    xsflib::psf::PutPath(v, microcode_path);
    xsflib::psf::PadTo4(v);
    xsflib::psf::Put32(v, snd_build);

    return v;
}

std::optional<ArchiveDescriptor> ArchiveDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 24 || xsflib::LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ArchiveDescriptor d;
    d.version = xsflib::LoadLe32(&data[4]);
    d.flags = xsflib::LoadLe32(&data[8]);
    d.output_mode = xsflib::LoadLe32(&data[12]);
    d.microcode_address = xsflib::LoadLe32(&data[16]);

    std::size_t pos = 20;
    const std::optional<std::string> archive = xsflib::psf::ReadPath(data, pos);
    const std::optional<std::string> microcode = archive ? xsflib::psf::ReadPath(data, pos) : std::nullopt;
    if (!microcode)
    {
        return std::nullopt;
    }

    d.archive_path = *archive;
    d.microcode_path = *microcode;

    // The build of NW4R SND follows the paths' padding. A descriptor that ends before the build gives none.
    pos = (pos + 3) & ~std::size_t{3};
    if (pos + 4 <= data.size())
    {
        d.snd_build = xsflib::LoadLe32(&data[pos]);
    }

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    if (auto err = xsflib::psf::CheckDescriptorVersion("archive", version, kVersion))
    {
        return err;
    }

    if (auto err = xsflib::psf::CheckDescriptorFlags("archive", flags, kFlagDspRequired))
    {
        return err;
    }

    return xsflib::psf::CheckDescriptorOutputMode("archive", output_mode);
}

} // namespace wsf
