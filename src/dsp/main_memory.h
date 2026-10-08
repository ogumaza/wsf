// SPDX-License-Identifier: MIT

// The Wii's main memory: MEM1 (24 MiB at physical 0) and MEM2 (64 MiB at physical 0x10000000). The CPU and the DSP
// share it. AX keeps its parameter blocks, command lists and output buffers here. The DSP's accelerator reads wave data
// from it. In archive mode the NW4R SND model stores the sound archive's waves in MEM2.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>

#include "common/common_types.h"
#include "common/memory_image.h"

namespace wsf
{

constexpr PAddr kMem1Base = 0x00000000;
constexpr std::size_t kMem1Size = 24 * 1024 * 1024;
constexpr PAddr kMem2Base = 0x10000000;
constexpr std::size_t kMem2Size = 64 * 1024 * 1024;

// The CPU addresses memory through cached (0x80000000 and up) and uncached (0xc0000000 and up) mirrors of the physical
// address space (OSCachedToPhysical, OSPhysicalToCached).
constexpr PAddr ToPhysical(uint32_t address)
{
    return address & 0x1fffffff;
}

constexpr uint32_t ToCached(PAddr address)
{
    return address | 0x80000000;
}

class MainMemory
{
public:
    // The memory's contents, for a snapshot of the system that holds it.
    struct Snapshot
    {
        MemoryImage mem1;
        MemoryImage mem2;
        std::size_t mem2_used = 0;
    };

    // Both start zeroed. The memory comes from calloc. calloc leaves the zeroing to the system's lazy mapping of pages.
    // A model that touches little of the memory is cheap to create.
    MainMemory() : mem1_(Allocate(kMem1Size)), mem2_(Allocate(kMem2Size))
    {
    }

    // Takes a snapshot. If `previous` is given, the snapshot shares unchanged pages with it.
    Snapshot Save(const Snapshot* previous = nullptr) const
    {
        return {MemoryImage(mem1_.get(), kMem1Size, previous ? &previous->mem1 : nullptr),
                MemoryImage(mem2_.get(), kMem2Size, previous ? &previous->mem2 : nullptr), mem2_used_};
    }

    void Restore(const Snapshot& snapshot)
    {
        snapshot.mem1.Restore(mem1_.get());
        snapshot.mem2.Restore(mem2_.get());
        mem2_used_ = snapshot.mem2_used;
    }

    // The `size` bytes at physical address `address`, or nullptr if any of them is outside MEM1 and MEM2.
    uint8_t* At(PAddr address, std::size_t size = 1)
    {
        if (address < kMem1Base + kMem1Size)
        {
            return address + size <= kMem1Base + kMem1Size ? mem1_.get() + (address - kMem1Base) : nullptr;
        }
        if (address >= kMem2Base && address - kMem2Base + size <= kMem2Size)
        {
            return mem2_.get() + (address - kMem2Base);
        }

        return nullptr;
    }

    const uint8_t* At(PAddr address, std::size_t size = 1) const
    {
        return const_cast<MainMemory*>(this)->At(address, size);
    }

    // Copies `size` bytes into the next free 32-byte aligned space in MEM2 and returns their physical address. This is
    // bump allocation, for the waves that the NW4R SND model loads. Throws when MEM2 is full.
    PAddr Store(const void* data, std::size_t size)
    {
        const std::size_t offset = (mem2_used_ + 31) / 32 * 32;
        if (offset + size > kMem2Size)
        {
            throw std::runtime_error("MEM2 model exhausted");
        }

        std::memcpy(mem2_.get() + offset, data, size);
        mem2_used_ = offset + size;

        return kMem2Base + static_cast<PAddr>(offset);
    }

private:
    struct Free
    {
        void operator()(uint8_t* bytes) const
        {
            std::free(bytes);
        }
    };

    using Bytes = std::unique_ptr<uint8_t, Free>;

    static Bytes Allocate(std::size_t size)
    {
        Bytes bytes(static_cast<uint8_t*>(std::calloc(size, 1)));
        if (!bytes)
        {
            throw std::bad_alloc();
        }

        return bytes;
    }

    Bytes mem1_;
    Bytes mem2_;
    std::size_t mem2_used_ = 0;
};

} // namespace wsf
