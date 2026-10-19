// SPDX-License-Identifier: MIT

#include "wii/system.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

#include "ntdlib/dsp/memory.h"
#include "ntdlib/ppc/cpu.h"
#include "xsflib/bytes.h"

namespace wsf::wii
{
namespace
{

// The OS globals in low memory that the Wii's loader leaves after the RVL SDK's OSBootInfo, as cached addresses.
// ntdlib::flipper::low has the globals that the GameCube's IPL and apploader leave too.
namespace low
{

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
constexpr uint32_t kRequestedIosVersion = 0x80003188;

} // namespace low

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

    // The CPU: the BATs that map MEM2 cached at 0x90000000 and uncached at 0xd0000000, then MEM1's and the entry point
    // (see MachineBase::StartCpu).
    ntdlib::ppc::State& s = cpu_.state_;
    s.spr[ntdlib::ppc::spr::kHid4] = 0x82000000; // SBE: BATs 4-7 on
    SetBat(s, ntdlib::ppc::spr::kIbat4u, 0x90001fff, 0x10000002);
    SetBat(s, ntdlib::ppc::spr::kIbat4u + 8, 0x90001fff, 0x10000002);
    SetBat(s, ntdlib::ppc::spr::kIbat4u + 10, 0xd0001fff, 0x1000002a);
    StartCpu(entry);
}

void System::LoadLowMemory(uint32_t ios_version)
{
    const auto put32 = [this](uint32_t address, uint32_t value)
    {
        xsflib::StoreBe32(memory_.At(ntdlib::dsp::ToPhysical(address), 4), value);
    };

    // The part of the boot that the Wii shares with the GameCube (see MachineBase::LoadBootInfo). The header gives the
    // file system table's offset and size in units of 4 bytes.
    const auto header = LoadBootInfo({4, kMem1ArenaTop, 0x00000023, kBusClock, kCpuClock});

    put32(low::kMem1PhysicalSize, ntdlib::dsp::kMem1Size);
    put32(low::kMem1SimulatedSize, ntdlib::dsp::kMem1Size);
    put32(low::kMem1End, kMem1ArenaTop);
    put32(low::kMem1ArenaLo, 0);
    put32(low::kMem1ArenaHi, kMem1ArenaTop);
    put32(low::kMem2PhysicalSize, ntdlib::dsp::kMem2Size);
    put32(low::kMem2SimulatedSize, ntdlib::dsp::kMem2Size);
    put32(low::kMem2End, kIpcBufferHi);
    put32(low::kMem2ArenaLo, kMem2ArenaLo);
    put32(low::kMem2ArenaHi, kMem2ArenaHi);
    put32(low::kIpcBufferLo, kMem2ArenaHi);
    put32(low::kIpcBufferHi, kIpcBufferHi);
    put32(low::kHollywoodVersion, 0x00000011);
    put32(low::kIosVersion, ios_version << 16);
    // The IOS that the game's TMD requests. The OSInit of a later SDK stops the game with OSFatal unless the running
    // IOS is that one, at that revision or later. New Play Control! Donkey Kong Jungle Beat has such an OSInit.
    put32(low::kRequestedIosVersion, ios_version << 16);
    std::copy(header.begin(), header.begin() + 4, memory_.At(ntdlib::dsp::ToPhysical(low::kGameId), 4));
    put32(low::kApplicationType, 0x80000000);
}

System::Snapshot System::Save(const Snapshot* previous) const
{
    return {SaveDevices(previous ? &previous->devices : nullptr), ios_.Save(), *this};
}

void System::Restore(const Snapshot& snapshot)
{
    RestoreDevices(snapshot.devices);
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

System::IoBlock System::BlockOf(ntdlib::PAddr address)
{
    const uint32_t block = address & 0xffff0000;
    const uint32_t offset = address & 0xffff;
    if (block == 0x0c000000 || (block == 0x0d000000 && offset >= kDiscInterface && offset < kAudioInterfaceEnd))
    {
        return IoBlock::kLegacy;
    }

    if ((block == 0x0d000000 || block == 0x0d800000) && offset < 0x400)
    {
        return offset < 0x40 ? IoBlock::kIpc : IoBlock::kHollywood;
    }

    return IoBlock::kNone;
}

bool System::ReadIo(ntdlib::PAddr address, int size, uint32_t& value)
{
    // MachineBase handles the legacy blocks that the Wii shares with the GameCube. The legacy disc interface's block
    // and Hollywood's other registers only store what's written to them. An access that no model handles counts as
    // unknown.
    const uint32_t offset = address & 0xffff;
    value = 0;
    bool known = false;
    switch (BlockOf(address))
    {
    case IoBlock::kLegacy:
        if (const std::optional<bool> shared = ReadShared(offset, size, value))
        {
            known = *shared;
        }
        else if (offset >= kDiscInterface && offset < kSerialInterface)
        {
            value = di_.Read(offset & 0x3ff, size);
        }
        else
        {
            return false;
        }
        break;

    case IoBlock::kIpc:
        value = ios_.Read(offset);
        known = true;
        break;

    case IoBlock::kHollywood:
        value = hollywood_.Read(offset, size);
        break;

    case IoBlock::kNone:
        return false;
    }

    if (!known)
    {
        ReportUnknown(address, size, false, 0);
    }

    return true;
}

bool System::WriteIo(ntdlib::PAddr address, int size, uint32_t value)
{
    const uint32_t offset = address & 0xffff;
    bool known = false;
    switch (BlockOf(address))
    {
    case IoBlock::kLegacy:
        if (const std::optional<bool> shared = WriteShared(offset, size, value))
        {
            known = *shared;
        }
        else if (offset >= kDiscInterface && offset < kSerialInterface)
        {
            di_.Write(offset & 0x3ff, size, value);
        }
        else
        {
            return false;
        }
        break;

    case IoBlock::kIpc:
        ios_.Write(offset, value);
        known = true;
        break;

    case IoBlock::kHollywood:
        hollywood_.Write(offset, size, value);
        break;

    case IoBlock::kNone:
        return false;
    }

    FinishWrite(address, size, value, known);

    return true;
}

uint32_t System::InterruptCauses() const
{
    // The sources: EXI (4), DSP (6) and Hollywood (14), and the reset switch's state (16: not pressed).
    uint32_t causes = 1u << 16;
    causes |= exi_.InterruptPending() ? 1u << 4 : 0;
    causes |= dsp_interface_.InterruptPending() ? 1u << 6 : 0;
    causes |= ios_.InterruptPending() ? 1u << 14 : 0;

    return causes;
}

} // namespace wsf::wii
