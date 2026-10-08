// SPDX-License-Identifier: MIT

// The Wii as game mode emulates it: Broadway running the game's code (including its OS), main memory, the DSP running
// AX's microcode, and the parts of Hollywood that the OS and the sound code use. A Disc provides the data of the game's
// partition.
//
// The system boots the game's main.dol from its entry point. It starts in the state that the system menu's loader
// leaves: the OS globals in low memory (see WiiBrew's "Memory map"), the BATs that map MEM1 and MEM2, and the disc's
// file system table at the top of MEM1.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "dsp/dsp.h"
#include "dsp/main_memory.h"
#include "ppc/cpu.h"
#include "wii/dsp_interface.h"
#include "wii/exi.h"
#include "wii/ios.h"

namespace wsf::wii
{

// The game partition's data, as the disc drive reads it.
class Disc
{
public:
    virtual ~Disc() = default;

    // Reads `size` bytes at byte `offset` of the partition's data. Returns false if any of them is missing.
    virtual bool Read(uint64_t offset, void* out, std::size_t size) = 0;
};

// The registers that System models itself. A snapshot of the system copies them (see System::Save).
class SystemRegisters
{
protected:
    // Registers that only store what's written to them, big-endian. They cover the blocks that the system doesn't model
    // further.
    template <std::size_t N>
    struct RegisterFile
    {
        std::array<uint8_t, N> bytes{};

        uint32_t Read(uint32_t offset, int size) const;
        void Write(uint32_t offset, int size, uint32_t value);
    };

    uint32_t pi_mask_ = 0;
    uint32_t ai_control_ = 0;
    uint32_t ai_volume_ = 0;
    uint32_t ai_interrupt_timing_ = 0;
    uint32_t ai_samples_ = 0;
    double ai_next_count_ = 0.0; // the CPU cycle of the counter's next count
    double ai_period_ = 0.0;     // and the cycles between counts
    RegisterFile<0x100> pi_{};
    RegisterFile<0x100> vi_{};
    RegisterFile<0x100> mi_{};
    RegisterFile<0x100> cp_{};
    RegisterFile<0x100> pe_{};
    RegisterFile<0x400> di_si_{};
    RegisterFile<0x400> hollywood_{};
};

class System final : public ppc::Bus, private SystemRegisters
{
public:
    // The CPU's clock, and the bus clock that times the hardware.
    static constexpr uint32_t kCpuClock = 729'000'000;
    static constexpr uint32_t kBusClock = 243'000'000;

    // The whole system's state between runs: main memory, the CPU, the DSP and the devices. It can only be restored to
    // the system it came from.
    struct Snapshot
    {
        MainMemory::Snapshot memory;
        ppc::Cpu::Snapshot cpu;
        dsp::Dsp dsp;
        DspInterface::Snapshot dsp_interface;
        Exi::Snapshot exi;
        Ios::Snapshot ios;
        SystemRegisters registers;
    };

    explicit System(Disc& disc);

    // Loads main.dol's sections, with its BSS cleared, and returns its entry point. Throws std::runtime_error for a DOL
    // that can't be loaded.
    uint32_t LoadDol(std::span<const uint8_t> dol);

    // Puts the system, with the game's code in memory, in the state in which the system menu's loader starts the game
    // at `entry`. That covers the OS globals, the disc's file system table and the CPU's registers. `ios_version` is
    // the IOS that the game runs on (21 for IOS21). Throws std::runtime_error if the disc's header or file system table
    // can't be read.
    void Start(uint32_t entry, uint32_t ios_version);

    // LoadDol, then Start on IOS `ios_version`.
    void Boot(std::span<const uint8_t> dol, uint32_t ios_version = 21)
    {
        Start(LoadDol(dol), ios_version);
    }

    // Runs for `cycles` CPU cycles, or until the CPU stops. Returns why it stopped.
    ppc::StopReason Run(uint64_t cycles);

    // Takes a snapshot between runs, and restores one. A snapshot shares its unchanged memory pages with `previous`.
    Snapshot Save(const Snapshot* previous = nullptr) const;
    void Restore(const Snapshot& snapshot);

    ppc::Cpu& Cpu()
    {
        return cpu_;
    }

    MainMemory& Memory()
    {
        return memory_;
    }

    dsp::Dsp& Dsp()
    {
        return dsp_;
    }

    DspInterface& DspRegisters()
    {
        return dsp_interface_;
    }

    Ios& IosModel()
    {
        return ios_;
    }

    // Called with a line of text the first time each unmodelled hardware register is accessed. Useful when bringing up
    // games.
    std::function<void(const std::string&)> on_unknown_io;

    // ppc::Bus.
    uint8_t* RamPage(PAddr page) override;
    bool ReadIo(PAddr address, int size, uint32_t& value) override;
    bool WriteIo(PAddr address, int size, uint32_t value) override;

private:
    void LoadLowMemory(uint32_t ios_version);

    // The processor interface's registers (physical 0x0C003000) and its interrupt causes.
    uint32_t ReadPi(uint32_t offset);
    void WritePi(uint32_t offset, uint32_t value);
    uint32_t InterruptCauses() const;
    void UpdateInterrupt();

    // The audio interface's registers (physical 0x0D006C00). The sample counter (for disc streaming) counts at the
    // streaming rate while playback is on. Counting starts a period after playback starts or the rate changes. The OS
    // times the counter at boot.
    uint32_t ReadAi(uint32_t offset);
    void WriteAi(uint32_t offset, uint32_t value);
    void AdvanceSampleCounter();

    // The serial interface's registers (physical 0x0D006400). Nothing is connected. Transfers finish at once.
    void WriteSi(uint32_t offset, int size, uint32_t value);

    // The next device event, and the device events due by now.
    uint64_t NextEvent() const;
    void Advance();

    void ReportUnknown(PAddr address, int size, bool write, uint32_t value);

    Disc& disc_;
    MainMemory memory_;
    ppc::Cpu cpu_{*this};
    dsp::Dsp dsp_{memory_};
    DspInterface dsp_interface_{dsp_, memory_, cpu_};
    Exi exi_{memory_};
    Ios ios_{memory_, disc_, cpu_};
    std::set<PAddr> reported_;
};

} // namespace wsf::wii
