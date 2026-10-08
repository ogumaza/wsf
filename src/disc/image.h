// SPDX-License-Identifier: MIT

// Wii disc images in the common container formats: ISO (including NKit), GCZ, CISO, WBFS, WDF, WIA and RVZ, whole or
// split into pieces. An Image provides the disc's bytes. WIA and RVZ store a Wii partition's data decrypted and without
// its hashes. They store it separately from the rest of the disc. PartitionData provides that data.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace wsf::disc
{

// A damaged or unsupported image, or a read that failed.
class Error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// A Wii partition's data as the game reads it: decrypted and without the hashes. That leaves 0x7C00 bytes for every
// 0x8000 bytes on the disc.
class PartitionData
{
public:
    virtual ~PartitionData() = default;

    // Reads `size` bytes at `offset` into `out`. Throws Error.
    virtual void Read(uint64_t offset, uint8_t* out, size_t size) = 0;
};

// Random access to a disc image's bytes.
class Image
{
public:
    virtual ~Image() = default;

    // The container's name for messages: "ISO", "GCZ", "CISO", "WBFS", "WDF", "WIA" or "RVZ".
    virtual std::string Format() const = 0;

    // Reads `size` bytes of the disc at `offset` into `out`. Bytes that the image doesn't store read as zeros. These
    // include scrubbed areas and anything past the end of the image. Throws Error.
    virtual void Read(uint64_t offset, uint8_t* out, size_t size) = 0;

    // Returns the data of the Wii partition whose data starts at disc offset `data_offset` if the container stores that
    // data apart from the rest of the disc. WIA and RVZ do this. For other containers it returns null. The partition's
    // data is then on the disc.
    virtual std::unique_ptr<PartitionData> OpenPartitionData(uint64_t data_offset);
};

// Opens an image file in any of the formats above. The format is detected from the file's contents. The name doesn't
// matter. A split image is opened by its first piece. WBFS continues in .wbf1 to .wbf9. The other formats append .1,
// .01 or .001 and onwards to the name. Wiimm's tools split images this way. Throws Error.
std::unique_ptr<Image> OpenImage(const std::filesystem::path& path);

} // namespace wsf::disc
