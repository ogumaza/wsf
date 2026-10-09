// SPDX-License-Identifier: MIT

#include "wii/system.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/bytes.h"
#include "dsp/main_memory.h"
#include "ppc/cpu.h"

namespace wsf::wii
{
namespace
{

constexpr uint32_t kPageSize = 0x1000;

// The OS globals in low memory (the RVL SDK's OSBootInfo and the Wii's memory layout after it), as cached addresses.
namespace low
{

constexpr uint32_t kBootMagic = 0x80000020;
constexpr uint32_t kBootVersion = 0x80000024;
constexpr uint32_t kMemorySize = 0x80000028;
constexpr uint32_t kConsoleType = 0x8000002c;
constexpr uint32_t kArenaLo = 0x80000030;
constexpr uint32_t kArenaHi = 0x80000034;
constexpr uint32_t kFst = 0x80000038;
constexpr uint32_t kFstMaxSize = 0x8000003c;
constexpr uint32_t kSimulatedMemorySize = 0x800000f0;
constexpr uint32_t kBusClock = 0x800000f8;
constexpr uint32_t kCpuClock = 0x800000fc;
constexpr uint32_t kMem1PhysicalSize = 0x80003100;
constexpr uint32_t kMem1SimulatedSize = 0x80003104;
constexpr uint32_t kMem1End = 0x80003108;
constexpr uint32_t kMem1ArenaLo = 0x8000310c;
constexpr uint32_t kMem1ArenaHi = 0x80003110;
constexpr uint32_t kMem2PhysicalSize = 0x80003118;
constexpr uint32_t kMem2SimulatedSize = 0x8000311c;
constexpr uint32_t kMem2End = 0x80003120;
constexpr uint32_t kMem2ArenaLo = 0x80003124;
constexpr uint32_t kMem2ArenaHi = 0x80003128;
constexpr uint32_t kIpcBufferLo = 0x80003130;
constexpr uint32_t kIpcBufferHi = 0x80003134;
constexpr uint32_t kHollywoodVersion = 0x80003138;
constexpr uint32_t kIosVersion = 0x80003140;
constexpr uint32_t kGameId = 0x80003180;
constexpr uint32_t kApplicationType = 0x80003184;

} // namespace low

// The disc header's offsets: the file system table's offset and size, each a byte count shifted right by 2.
constexpr uint32_t kHeaderFstOffset = 0x424;
constexpr uint32_t kHeaderFstSize = 0x428;

// The top of the OS's MEM1 arena: above it are WSF's driver (at 0x81700000) and the disc's file system table.
constexpr uint32_t kMem1ArenaTop = 0x81700000;

// The arenas that the loader leaves: in MEM1 below the driver, and in MEM2 below IOS's area.
constexpr uint32_t kMem2ArenaLo = 0x90000800;
constexpr uint32_t kMem2ArenaHi = 0x933e0000;
constexpr uint32_t kIpcBufferHi = 0x93400000;

} // namespace

System::System(Disc& disc) : disc_(disc)
{
}

void System::Start(uint32_t entry, uint32_t ios_version)
{
    LoadLowMemory(ios_version);

    // The CPU: translation on through the BATs that map MEM1 and MEM2 cached (0x80000000, 0x90000000) and uncached
    // (0xc0000000, 0xd0000000), floating point on, and the entry point.
    ppc::State& s = cpu_.state_;
    s.msr = ppc::msr::kIr | ppc::msr::kDr | ppc::msr::kRi | ppc::msr::kFp;
    s.spr[ppc::spr::kHid4] = 0x82000000; // SBE: BATs 4-7 on

    const auto set_bat = [&s](int base, uint32_t upper, uint32_t lower)
    {
        s.spr[base] = upper;
        s.spr[base + 1] = lower;
    };

    set_bat(ppc::spr::kIbat0u, 0x80001fff, 0x00000002);
    set_bat(ppc::spr::kIbat0u + 8, 0x80001fff, 0x00000002);
    set_bat(ppc::spr::kIbat0u + 10, 0xc0001fff, 0x0000002a);
    set_bat(ppc::spr::kIbat4u, 0x90001fff, 0x10000002);
    set_bat(ppc::spr::kIbat4u + 8, 0x90001fff, 0x10000002);
    set_bat(ppc::spr::kIbat4u + 10, 0xd0001fff, 0x1000002a);
    cpu_.InvalidateTranslation();
    s.pc = entry;
}

void System::LoadLowMemory(uint32_t ios_version)
{
    const auto put32 = [this](uint32_t address, uint32_t value)
    {
        StoreBe32(memory_.At(ToPhysical(address), 4), value);
    };

    // The disc header's first 32 bytes: the game's ID, its version, and the Wii disc magic.
    std::array<uint8_t, 0x440> header{};
    if (!disc_.Read(0, header.data(), header.size()))
    {
        throw std::runtime_error("the disc's header can't be read");
    }

    std::copy(header.begin(), header.begin() + 0x20, memory_.At(0, 0x20));

    // The file system table, at the top of MEM1. The DVD library looks files up there.
    const uint32_t fst_offset = LoadBe32(&header[kHeaderFstOffset]) * 4;
    const uint32_t fst_size = LoadBe32(&header[kHeaderFstSize]) * 4;
    const uint32_t fst_address = (ToCached(kMem1Size) - fst_size) & ~0x1fu;
    std::vector<uint8_t> fst(fst_size);
    if (fst_size > 0x100000 || !disc_.Read(fst_offset, fst.data(), fst.size()))
    {
        throw std::runtime_error("the disc's file system table can't be read");
    }

    if (fst_address < kMem1ArenaTop + 0x10000)
    {
        throw std::runtime_error("the disc's file system table is too large");
    }

    std::copy(fst.begin(), fst.end(), memory_.At(ToPhysical(fst_address), fst.size()));
    const uint32_t arena_hi = kMem1ArenaTop;

    put32(low::kBootMagic, 0x0d15ea5e);
    put32(low::kBootVersion, 1);
    put32(low::kMemorySize, kMem1Size);
    put32(low::kConsoleType, 0x00000023);
    put32(low::kArenaLo, 0);
    put32(low::kArenaHi, arena_hi);
    put32(low::kFst, fst_address);
    put32(low::kFstMaxSize, fst_size);
    put32(low::kSimulatedMemorySize, kMem1Size);
    put32(low::kBusClock, kBusClock);
    put32(low::kCpuClock, kCpuClock);
    put32(low::kMem1PhysicalSize, kMem1Size);
    put32(low::kMem1SimulatedSize, kMem1Size);
    put32(low::kMem1End, arena_hi);
    put32(low::kMem1ArenaLo, 0);
    put32(low::kMem1ArenaHi, arena_hi);
    put32(low::kMem2PhysicalSize, kMem2Size);
    put32(low::kMem2SimulatedSize, kMem2Size);
    put32(low::kMem2End, kIpcBufferHi);
    put32(low::kMem2ArenaLo, kMem2ArenaLo);
    put32(low::kMem2ArenaHi, kMem2ArenaHi);
    put32(low::kIpcBufferLo, kMem2ArenaHi);
    put32(low::kIpcBufferHi, kIpcBufferHi);
    put32(low::kHollywoodVersion, 0x00000011);
    put32(low::kIosVersion, ios_version << 16);
    std::copy(header.begin(), header.begin() + 4, memory_.At(ToPhysical(low::kGameId), 4));
    put32(low::kApplicationType, 0x80000000);
}

ppc::StopReason System::Run(uint64_t cycles)
{
    // The CPU runs up to the next device event. Then the event happens and the interrupt line is updated.
    const uint64_t end = cpu_.Cycles() + cycles;
    while (cpu_.Cycles() < end)
    {
        const uint64_t until = std::min(end, std::max(NextEvent(), cpu_.Cycles() + 1));
        const ppc::StopReason reason = cpu_.Run(until - cpu_.Cycles());
        Advance();
        if (reason != ppc::StopReason::kBudget)
        {
            return reason;
        }
    }

    return ppc::StopReason::kBudget;
}

System::Snapshot System::Save(const Snapshot* previous) const
{
    return {memory_.Save(previous ? &previous->memory : nullptr),
            cpu_.Save(),
            dsp_,
            dsp_interface_.Save(),
            exi_.Save(),
            ios_.Save(),
            *this};
}

void System::Restore(const Snapshot& snapshot)
{
    memory_.Restore(snapshot.memory);
    cpu_.Restore(snapshot.cpu);
    dsp_ = snapshot.dsp;
    dsp_interface_.Restore(snapshot.dsp_interface);
    exi_.Restore(snapshot.exi);
    ios_.Restore(snapshot.ios);
    SystemRegisters::operator=(snapshot.registers);
    UpdateInterrupt();
}

uint64_t System::NextEvent() const
{
    return std::min(dsp_interface_.NextEvent(), ios_.NextEvent());
}

void System::Advance()
{
    const uint64_t now = cpu_.Cycles();
    dsp_interface_.Advance(now);
    ios_.Advance(now);
    UpdateInterrupt();
}

uint8_t* System::RamPage(PAddr page)
{
    return memory_.At(page, kPageSize);
}

bool System::ReadIo(PAddr address, int size, uint32_t& value)
{
    // The legacy blocks at 0x0C00xxxx, the Wii's at 0x0D00xxxx, and Hollywood's registers (also at 0x0D80xxxx).
    const uint32_t block = address & 0xffff0000;
    const uint32_t offset = address & 0xffff;
    bool known = true;
    value = 0;
    if (block == 0x0c000000 && offset < 0x1000)
    {
        value = cp_.Read(offset & 0xff, size);
        known = false;
    }
    else if (block == 0x0c000000 && offset < 0x2000)
    {
        value = pe_.Read(offset & 0xff, size);
        known = false;
    }
    else if (block == 0x0c000000 && offset < 0x3000)
    {
        value = vi_.Read(offset & 0xff, size);
        known = false;
    }
    else if (block == 0x0c000000 && offset < 0x4000)
    {
        value = ReadPi(offset & 0xff);
    }
    else if (block == 0x0c000000 && offset < 0x5000)
    {
        value = mi_.Read(offset & 0xff, size);
        known = false;
    }
    else if (block == 0x0c000000 && offset < 0x6000)
    {
        // 16-bit registers. A 32-bit read takes the high half first.
        const uint32_t reg = offset & 0x1fe;
        value = size == 4 ? uint32_t{dsp_interface_.Read(reg)} << 16 | dsp_interface_.Read(reg + 2)
                          : dsp_interface_.Read(reg);
    }
    else if ((block == 0x0c000000 || block == 0x0d000000) && offset >= 0x6000 && offset < 0x7000)
    {
        const uint32_t local = offset & 0xfff;
        if (local >= 0xc00)
        {
            value = ReadAi(local & 0x1f);
        }
        else if (local >= 0x800)
        {
            value = exi_.Read(local & 0x3f);
        }
        else
        {
            value = di_si_.Read(local, size);
            known = false;
        }
    }
    else if ((block == 0x0d000000 || block == 0x0d800000) && offset < 0x400)
    {
        if (offset < 0x40)
        {
            value = ios_.Read(offset);
        }
        else
        {
            value = hollywood_.Read(offset, size);
            known = false;
        }
    }
    else
    {
        return false;
    }

    if (!known)
    {
        ReportUnknown(address, size, false, 0);
    }

    return true;
}

bool System::WriteIo(PAddr address, int size, uint32_t value)
{
    const uint32_t block = address & 0xffff0000;
    const uint32_t offset = address & 0xffff;
    bool known = true;
    if (block == 0x0c000000 && offset < 0x1000)
    {
        cp_.Write(offset & 0xff, size, value);
        known = false;
    }
    else if (block == 0x0c000000 && offset < 0x2000)
    {
        pe_.Write(offset & 0xff, size, value);
        known = false;
    }
    else if (block == 0x0c000000 && offset < 0x3000)
    {
        vi_.Write(offset & 0xff, size, value);
        known = false;
    }
    else if (block == 0x0c000000 && offset < 0x4000)
    {
        WritePi(offset & 0xff, value);
    }
    else if (block == 0x0c000000 && offset < 0x5000)
    {
        mi_.Write(offset & 0xff, size, value);
        known = false;
    }
    else if (block == 0x0c000000 && offset < 0x6000)
    {
        const uint32_t reg = offset & 0x1fe;
        if (size == 4)
        {
            dsp_interface_.Write(reg, static_cast<uint16_t>(value >> 16));
            dsp_interface_.Write(reg + 2, static_cast<uint16_t>(value));
        }
        else
        {
            dsp_interface_.Write(reg, static_cast<uint16_t>(value));
        }
    }
    else if ((block == 0x0c000000 || block == 0x0d000000) && offset >= 0x6000 && offset < 0x7000)
    {
        const uint32_t local = offset & 0xfff;
        if (local >= 0xc00)
        {
            WriteAi(local & 0x1f, value);
        }
        else if (local >= 0x800)
        {
            exi_.Write(local & 0x3f, value);
        }
        else if (local >= 0x400)
        {
            WriteSi(local, size, value);
            known = false;
        }
        else
        {
            di_si_.Write(local, size, value);
            known = false;
        }
    }
    else if ((block == 0x0d000000 || block == 0x0d800000) && offset < 0x400)
    {
        if (offset < 0x40)
        {
            ios_.Write(offset, value);
        }
        else
        {
            hollywood_.Write(offset, size, value);
            known = false;
        }
    }
    else
    {
        return false;
    }

    if (!known)
    {
        ReportUnknown(address, size, true, value);
    }

    // A write may have scheduled an event, or raised or cleared an interrupt.
    cpu_.LimitRun(NextEvent());
    UpdateInterrupt();

    return true;
}

uint32_t System::ReadPi(uint32_t offset)
{
    switch (offset)
    {
    case 0x00:
        return InterruptCauses();
    case 0x04:
        return pi_mask_;
    default:
        return pi_.Read(offset, 4);
    }
}

void System::WritePi(uint32_t offset, uint32_t value)
{
    switch (offset)
    {
    case 0x00: // the causes clear at their sources
        break;

    case 0x04:
        pi_mask_ = value;
        break;

    default:
        pi_.Write(offset, 4, value);
        ReportUnknown(0x0c003000 + offset, 4, true, value);
        break;
    }
}

uint32_t System::InterruptCauses() const
{
    // The sources: SI (3), EXI (4), DSP (6) and Hollywood (14), and the reset switch's state (16: not pressed).
    uint32_t causes = 1u << 16;
    causes |= exi_.InterruptPending() ? 1u << 4 : 0;
    causes |= dsp_interface_.InterruptPending() ? 1u << 6 : 0;
    causes |= ios_.InterruptPending() ? 1u << 14 : 0;

    return causes;
}

void System::UpdateInterrupt()
{
    cpu_.SetExternalInterrupt((InterruptCauses() & pi_mask_ & 0xffff) != 0);
}

uint32_t System::ReadAi(uint32_t offset)
{
    switch (offset)
    {
    case 0x00:
        return ai_control_;

    case 0x04:
        return ai_volume_;

    case 0x08:
        AdvanceSampleCounter();
        return ai_samples_;

    case 0x0c:
        return ai_interrupt_timing_;

    default:
        return 0;
    }
}

void System::WriteAi(uint32_t offset, uint32_t value)
{
    // AICR's bits: PSTAT (0) runs the sample counter. AISFR (1) selects its rate (48 kHz when set). AIINT (3) clears
    // when written with 1. SCRESET (5) resets the counter and reads back 0. AIDFR (6) selects the DMA's rate (32 kHz
    // when set).
    constexpr uint32_t kPlay = 1u << 0;
    constexpr uint32_t kRate48k = 1u << 1;
    const auto period = [](uint32_t control)
    {
        return static_cast<double>(kCpuClock) / ((control & kRate48k) ? 48'000.0 : 32'000.0);
    };

    switch (offset)
    {
    case 0x00:
        {
            AdvanceSampleCounter();
            const uint32_t old = ai_control_;
            ai_control_ = (ai_control_ & 0x08 & ~value) | (value & ~0x28u);
            if (value & 0x20)
            {
                ai_samples_ = 0;
            }

            if ((ai_control_ & kPlay) && (!(old & kPlay) || ((ai_control_ ^ old) & kRate48k)))
            {
                ai_period_ = period(ai_control_);
                ai_next_count_ = static_cast<double>(cpu_.Cycles()) + ai_period_;
            }

            dsp_interface_.SetDmaRate32k((value & 0x40) != 0);
            break;
        }

    case 0x04:
        ai_volume_ = value;
        break;

    case 0x08:
        AdvanceSampleCounter();
        ai_samples_ = value & 0x7fffffff;
        break;

    case 0x0c:
        ai_interrupt_timing_ = value;
        break;

    default:
        break;
    }
}

void System::AdvanceSampleCounter()
{
    const auto now = static_cast<double>(cpu_.Cycles());
    if (!(ai_control_ & 1) || now < ai_next_count_)
    {
        return;
    }

    const double counts = std::floor((now - ai_next_count_) / ai_period_) + 1.0;
    ai_samples_ = (ai_samples_ + static_cast<uint32_t>(counts)) & 0x7fffffff;
    ai_next_count_ += counts * ai_period_;
}

void System::WriteSi(uint32_t offset, int size, uint32_t value)
{
    // SICOMCSR (0x434): a transfer started with TSTART finishes at once, with its completion flag set.
    if (offset == 0x434 && (value & 1))
    {
        value = (value & ~1u) | 0x80000000;
    }
    di_si_.Write(offset, size, value);
}

template <std::size_t N>
uint32_t SystemRegisters::RegisterFile<N>::Read(uint32_t offset, int size) const
{
    uint32_t value = 0;
    for (int i = 0; i < size; i++)
    {
        value = value << 8 | (offset + i < N ? bytes[offset + i] : 0);
    }

    return value;
}

template <std::size_t N>
void SystemRegisters::RegisterFile<N>::Write(uint32_t offset, int size, uint32_t value)
{
    for (int i = size - 1; i >= 0; i--, value >>= 8)
    {
        if (offset + i < N)
        {
            bytes[offset + i] = static_cast<uint8_t>(value);
        }
    }
}

void System::ReportUnknown(PAddr address, int size, bool write, uint32_t value)
{
    if (!on_unknown_io || !reported_.insert(address).second)
    {
        return;
    }

    char line[80];
    if (write)
    {
        std::snprintf(line, sizeof(line), "write%d %08x = %08x at pc %08x", size * 8, address, value, cpu_.stop_pc_);
    }
    else
    {
        std::snprintf(line, sizeof(line), "read%d %08x at pc %08x", size * 8, address, cpu_.stop_pc_);
    }
    on_unknown_io(line);
}

} // namespace wsf::wii
