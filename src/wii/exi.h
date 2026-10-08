// SPDX-License-Identifier: MIT

// The external interface (EXI), at physical 0x0D006800: three channels of serial transfers, immediate or by DMA. The
// only device is the one that the OS reads at boot: device 1 on channel 0. It holds the real-time clock and the 64-byte
// SRAM of console settings. The memory card slots and the other devices are empty. Transfers finish at once.

#pragma once

#include <array>
#include <cstdint>

#include "dsp/main_memory.h"

namespace wsf::wii
{

// The channels' registers and the device's state. A snapshot of the system copies them (see Exi::Save).
class ExiState
{
protected:
    struct Channel
    {
        uint32_t status = 0; // CSR
        uint32_t dma_address = 0;
        uint32_t dma_length = 0;
        uint32_t control = 0;
        uint32_t data = 0;
    };

    std::array<Channel, 3> channels_{};

    // The RTC/SRAM device: the command of the transfer in progress, and where its data goes.
    bool selected_ = false;
    bool have_command_ = false;
    uint32_t command_ = 0;
    uint32_t position_ = 0;
    std::array<uint8_t, 64> sram_{};
    uint32_t rtc_ = 0;
};

class Exi : private ExiState
{
public:
    // The interface's state as Save captures it.
    using Snapshot = ExiState;

    explicit Exi(MainMemory& memory);

    // A 32-bit register at `offset` from 0x0D006800.
    uint32_t Read(uint32_t offset) const;
    void Write(uint32_t offset, uint32_t value);

    // Whether a channel's transfer-complete or external interrupt is pending and enabled: the EXI line to the processor
    // interface.
    bool InterruptPending() const;

    // The interface's state between runs of the CPU, and its restoration.
    Snapshot Save() const
    {
        return *this;
    }

    void Restore(const Snapshot& snapshot)
    {
        ExiState::operator=(snapshot);
    }

private:
    // The RTC/SRAM device's side of a transfer of `size` bytes. Returns the bytes that the device sends back for a
    // read, given the bytes it received.
    void DeviceTransfer(uint8_t* bytes, uint32_t size, bool write);
    void Transfer(Channel& channel, int index);

    MainMemory& memory_;
};

} // namespace wsf::wii
