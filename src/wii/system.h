// SPDX-License-Identifier: MIT

// The Wii as game mode emulates it: Broadway running the game's code (including its OS), main memory, the DSP running
// AX's microcode, and the parts of Hollywood that the OS and the sound code use. A Disc provides the data of the game's
// partition.
//
// The system boots the game's main.dol from its entry point. It starts in the state that the system menu's loader
// leaves: the OS globals in low memory (see WiiBrew's "Memory map"), the BATs that map MEM1 and MEM2, and the disc's
// file system table at the top of MEM1.

#pragma once

#include <cstdint>

#include "ntdlib/dsp/dsp.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/flipper/dsp_interface.h"
#include "ntdlib/flipper/exi.h"
#include "ntdlib/flipper/machine.h"
#include "ntdlib/flipper/registers.h"
#include "ntdlib/ppc/cpu.h"
#include "ntdlib/types.h"

#include "wii/ios.h"

namespace wsf::wii
{

// The DSP interface and EXI that the Wii shares with the GameCube (ntdlib::flipper), on the Wii's memory.
using DspInterface = ntdlib::flipper::DspInterface<ntdlib::dsp::WiiMemory>;
using Exi = ntdlib::flipper::Exi<ntdlib::dsp::WiiMemory>;

// The registers that System models itself. A snapshot of the system copies them (see System::Save). The base class
// holds the blocks that the Wii shares with the GameCube. SystemRegisters gives it the Wii's rates for the audio
// interface. The legacy disc interface's block and Hollywood's registers only store what's written to them.
class SystemRegisters : public ntdlib::flipper::SharedRegisters
{
public:
    SystemRegisters() : SharedRegisters(ntdlib::flipper::kWiiAudioInterface)
    {
    }

protected:
    ntdlib::flipper::RegisterFile<0x400> di_{};
    ntdlib::flipper::RegisterFile<0x400> hollywood_{};
};

class System final : public ntdlib::flipper::MachineBase<System>, private SystemRegisters
{
public:
    // The whole system's state between runs: main memory, the CPU, the DSP and the devices. It can only be restored to
    // the system it came from.
    struct Snapshot
    {
        ntdlib::flipper::DeviceSnapshot<ntdlib::dsp::WiiMemory> devices;
        Ios::Snapshot ios;
        SystemRegisters registers;
    };

    // The CPU's clock, and the bus clock that times the hardware.
    static constexpr uint32_t kCpuClock = 729'000'000;
    static constexpr uint32_t kBusClock = 243'000'000;

    explicit System(Disc& disc);

    // Puts the system, with the game's code in memory, in the state in which the system menu's loader starts the game
    // at `entry`. That covers the OS globals, the disc's file system table and the CPU's registers. `ios_version` is
    // the IOS that the game runs on (21 for IOS21). Throws std::runtime_error if the disc's header or file system table
    // can't be read.
    void Start(uint32_t entry, uint32_t ios_version);

    // Takes a snapshot between runs, and restores one. A snapshot shares its unchanged memory pages with `previous`.
    Snapshot Save(const Snapshot* previous = nullptr) const;
    void Restore(const Snapshot& snapshot);

    ntdlib::ppc::Cpu& Cpu()
    {
        return cpu_;
    }

    ntdlib::dsp::WiiMemory& Memory()
    {
        return memory_;
    }

    DspInterface& DspRegisters()
    {
        return dsp_interface_;
    }

    // ppc::Bus. MachineBase provides RamPage.
    bool ReadIo(ntdlib::PAddr address, int size, uint32_t& value) override;
    bool WriteIo(ntdlib::PAddr address, int size, uint32_t value) override;

private:
    friend class ntdlib::flipper::MachineBase<System>;

    void LoadLowMemory(uint32_t ios_version);

    // The interrupt causes of the processor interface (physical 0x0C003000).
    uint32_t InterruptCauses() const;

    // The next device event, and the device events due by now.
    uint64_t NextEvent() const;
    void Advance();

    Disc& disc_;
    ntdlib::dsp::WiiMemory memory_;
    ntdlib::ppc::Cpu cpu_{*this};
    ntdlib::dsp::Dsp<ntdlib::dsp::WiiMemory> dsp_{memory_};
    DspInterface dsp_interface_{dsp_, memory_, cpu_, ntdlib::flipper::kWiiDspInterface};
    Exi exi_{memory_, ntdlib::flipper::kWiiExi};
    Ios ios_{memory_, disc_, cpu_};
};

} // namespace wsf::wii
