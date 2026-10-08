// SPDX-License-Identifier: MIT

// Bounds-checked big-endian reads for the NintendoWare for Revolution sound formats, and their data references.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

#include "common/bytes.h"

namespace wsf::rsar
{

// A damaged file, or one of another format.
class FormatError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Reads big-endian values from a byte span. A read past the end throws FormatError.
class Reader
{
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data)
    {
    }

    size_t Size() const
    {
        return data_.size();
    }

    std::span<const uint8_t> Data() const
    {
        return data_;
    }

    std::span<const uint8_t> Sub(size_t offset, size_t size) const
    {
        Check(offset, size);

        return data_.subspan(offset, size);
    }

    uint8_t U8(size_t offset) const
    {
        Check(offset, 1);

        return data_[offset];
    }

    int8_t S8(size_t offset) const
    {
        return static_cast<int8_t>(U8(offset));
    }

    uint16_t U16(size_t offset) const
    {
        Check(offset, 2);

        return LoadBe16(data_.data() + offset);
    }

    int16_t S16(size_t offset) const
    {
        return static_cast<int16_t>(U16(offset));
    }

    uint32_t U32(size_t offset) const
    {
        Check(offset, 4);

        return LoadBe32(data_.data() + offset);
    }

    int32_t S32(size_t offset) const
    {
        return static_cast<int32_t>(U32(offset));
    }

    float F32(size_t offset) const
    {
        return std::bit_cast<float>(U32(offset));
    }

    // A string that ends with a zero byte or at max_size.
    std::string CString(size_t offset, size_t max_size) const
    {
        Check(offset, 0);
        std::string text;
        while (offset < data_.size() && data_[offset] && text.size() < max_size)
        {
            text.push_back(static_cast<char>(data_[offset++]));
        }

        return text;
    }

    bool Magic(size_t offset, const char* magic) const
    {
        return offset <= data_.size() && data_.size() - offset >= 4 &&
               std::memcmp(data_.data() + offset, magic, 4) == 0;
    }

private:
    void Check(size_t offset, size_t size) const
    {
        if (offset > data_.size() || size > data_.size() - offset)
        {
            throw FormatError("a read at offset " + std::to_string(offset) + " runs past the end of the data");
        }
    }

    std::span<const uint8_t> data_;
};

// A data reference. It records whether its value is an address or an offset from a base that the containing structure
// defines. Only data loaded into memory has addresses. An address means nothing here. It also records the type of the
// data it points to, for references that can point to several types.
struct DataRef
{
    // Whether it points anywhere: an offset reference with a value. A null reference is all zeros.
    bool IsOffset() const
    {
        return ref_type == 1;
    }

    uint8_t ref_type = 0;
    uint8_t data_type = 0;
    uint32_t value = 0;
};

inline DataRef ReadRef(const Reader& reader, size_t offset)
{
    return DataRef{reader.U8(offset), reader.U8(offset + 1), reader.U32(offset + 4)};
}

} // namespace wsf::rsar
