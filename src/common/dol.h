// SPDX-License-Identifier: MIT

// A DOL, the executable format of GameCube and Wii games (main.dol). A DOL has up to 7 text and 11 data sections, each
// loaded at an address, plus a BSS range and an entry point.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wsf
{

class Dol
{
public:
    struct Section
    {
        uint32_t address = 0;
        std::span<const uint8_t> data;
    };

    // Reads a DOL's header. Returns nothing if `file` isn't a DOL. The sections point into `file`. The caller must keep
    // `file` alive while it uses the result.
    static std::optional<Dol> Parse(std::span<const uint8_t> file);

    const std::vector<Section>& Sections() const
    {
        return sections_;
    }

    uint32_t Entry() const
    {
        return entry_;
    }

    uint32_t BssAddress() const
    {
        return bss_address_;
    }

    uint32_t BssSize() const
    {
        return bss_size_;
    }

    // Returns the `size` bytes loaded at `address` if one section holds them all.
    std::optional<std::span<const uint8_t>> Read(uint32_t address, uint32_t size) const;

    // Returns the load address of `byte` (a pointer into the DOL's file). Returns nothing if no section contains
    // `byte`.
    std::optional<uint32_t> AddressOf(const uint8_t* byte) const;

private:
    std::vector<Section> sections_;
    uint32_t entry_ = 0;
    uint32_t bss_address_ = 0;
    uint32_t bss_size_ = 0;
};

} // namespace wsf
