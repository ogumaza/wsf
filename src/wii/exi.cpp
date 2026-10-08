// SPDX-License-Identifier: MIT

#include "wii/exi.h"

#include <algorithm>
#include <cstdint>

#include "common/bytes.h"
#include "dsp/main_memory.h"

namespace wsf::wii
{
namespace
{

// CSR's bits: the interrupt flags (a write of 1 clears them) with their masks, and the selected device.
namespace csr
{

constexpr uint32_t kExiInterruptMask = 1u << 0;
constexpr uint32_t kExiInterrupt = 1u << 1;
constexpr uint32_t kTransferInterruptMask = 1u << 2;
constexpr uint32_t kTransferInterrupt = 1u << 3;
constexpr uint32_t kSelectShift = 7;
constexpr uint32_t kSelect = 7u << kSelectShift;
constexpr uint32_t kExtInterruptMask = 1u << 10;
constexpr uint32_t kExtInterrupt = 1u << 11;
constexpr uint32_t kFlags = kExiInterrupt | kTransferInterrupt | kExtInterrupt;

} // namespace csr

// The control register's bits.
constexpr uint32_t kStart = 1u << 0;
constexpr uint32_t kDma = 1u << 1;

// The RTC/SRAM device's commands, in the first word of a transfer: bit 31 for a write, then the address of the clock or
// of a byte of SRAM. SRAM addresses count in steps of 0x40.
constexpr uint32_t kCommandWrite = 0x80000000;
constexpr uint32_t kRtcAddress = 0x20000000;
constexpr uint32_t kSramAddress = 0x20000100;

// The console settings, all at their defaults, with the checksums that the OS verifies. The checksums are the 16-bit
// sums of the words at 0x0C-0x13 and of their complements.
std::array<uint8_t, 64> DefaultSram()
{
    std::array<uint8_t, 64> sram{};
    uint16_t sum = 0;
    uint16_t inverse_sum = 0;
    for (int at = 0x0c; at < 0x14; at += 2)
    {
        const uint16_t word = LoadBe16(&sram[at]);
        sum = static_cast<uint16_t>(sum + word);
        inverse_sum = static_cast<uint16_t>(inverse_sum + static_cast<uint16_t>(~word));
    }
    StoreBe16(&sram[0], sum);
    StoreBe16(&sram[2], inverse_sum);

    return sram;
}

} // namespace

Exi::Exi(MainMemory& memory) : memory_(memory)
{
    sram_ = DefaultSram();
}

uint32_t Exi::Read(uint32_t offset) const
{
    const uint32_t index = offset / 0x14;
    if (index >= channels_.size())
    {
        return 0;
    }

    const Channel& channel = channels_[index];
    switch (offset % 0x14)
    {
    case 0x00:
        return channel.status;
    case 0x04:
        return channel.dma_address;
    case 0x08:
        return channel.dma_length;
    case 0x0c:
        return channel.control;
    default:
        return channel.data;
    }
}

void Exi::Write(uint32_t offset, uint32_t value)
{
    const uint32_t index = offset / 0x14;
    if (index >= channels_.size())
    {
        return;
    }

    Channel& channel = channels_[index];
    switch (offset % 0x14)
    {
    case 0x00:
        {
            // A change of device starts the RTC/SRAM device's next transfer afresh.
            const bool device_one = index == 0 && (value & csr::kSelect) == (2u << csr::kSelectShift);
            if (index == 0 && device_one != selected_)
            {
                selected_ = device_one;
                have_command_ = false;
                command_ = 0;
                position_ = 0;
            }

            channel.status = (channel.status & csr::kFlags & ~value) | (value & ~csr::kFlags);
            break;
        }
    case 0x04:
        channel.dma_address = value & 0x03ffffe0;
        break;
    case 0x08:
        channel.dma_length = value & 0x03ffffe0;
        break;
    case 0x0c:
        channel.control = value;
        if (value & kStart)
        {
            Transfer(channel, static_cast<int>(index));
        }

        break;
    default:
        channel.data = value;
        break;
    }
}

bool Exi::InterruptPending() const
{
    return std::any_of(channels_.begin(), channels_.end(),
                       [](const Channel& c)
                       {
                           return ((c.status & csr::kExiInterrupt) && (c.status & csr::kExiInterruptMask)) ||
                                  ((c.status & csr::kTransferInterrupt) && (c.status & csr::kTransferInterruptMask)) ||
                                  ((c.status & csr::kExtInterrupt) && (c.status & csr::kExtInterruptMask));
                       });
}

void Exi::Transfer(Channel& channel, int index)
{
    // Only device 1 on channel 0 responds. On the other channels and devices, no data moves.
    const bool device = index == 0 && selected_;
    const uint32_t direction = channel.control >> 2 & 3; // 0 read, 1 write, 2 both
    if (channel.control & kDma)
    {
        uint8_t* bytes = memory_.At(channel.dma_address, channel.dma_length);
        if (device && bytes != nullptr)
        {
            DeviceTransfer(bytes, channel.dma_length, direction == 1);
        }
    }
    else
    {
        // Immediate: up to 4 bytes of the data register, its high bytes first.
        const uint32_t size = (channel.control >> 4 & 3) + 1;
        uint8_t bytes[4];
        StoreBe32(bytes, channel.data);
        if (device)
        {
            DeviceTransfer(bytes, size, direction == 1);
        }
        if (direction != 1)
        {
            channel.data = LoadBe32(bytes);
        }
    }

    channel.control &= ~kStart;
    channel.status |= csr::kTransferInterrupt;
}

void Exi::DeviceTransfer(uint8_t* bytes, uint32_t size, bool write)
{
    // The first four bytes written after the device is selected are its command.
    uint32_t at = 0;
    if (!have_command_)
    {
        for (; at < size && position_ < 4; at++, position_++)
        {
            command_ = command_ << 8 | bytes[at];
        }
        if (position_ < 4)
        {
            return;
        }

        have_command_ = true;
        position_ = 0;
    }

    // Then the data: the clock's four bytes, or SRAM from the address in the command.
    const uint32_t address = command_ & ~kCommandWrite;
    const bool device_write = (command_ & kCommandWrite) != 0;
    for (; at < size; at++, position_++)
    {
        if (address == kRtcAddress)
        {
            uint8_t rtc[4];
            StoreBe32(rtc, rtc_);
            if (device_write && write && position_ < 4)
            {
                rtc[position_] = bytes[at];
                rtc_ = LoadBe32(rtc);
            }
            else if (!write)
            {
                bytes[at] = position_ < 4 ? rtc[position_] : 0;
            }
        }
        else if (address >= kSramAddress)
        {
            const uint32_t offset = (address - kSramAddress) / 0x40 + position_;
            if (device_write && write && offset < sram_.size())
            {
                sram_[offset] = bytes[at];
            }
            else if (!write)
            {
                bytes[at] = offset < sram_.size() ? sram_[offset] : 0;
            }
        }
    }
}

} // namespace wsf::wii
