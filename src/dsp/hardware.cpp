// SPDX-License-Identifier: MIT

// The DSP's memories and hardware registers: the memory maps, the mailboxes, DMA, the accelerator, and the DSP side of
// the control register.

#include <algorithm>
#include <cstdint>

#include "common/bytes.h"
#include "common/common_types.h"
#include "dsp/coef_rom.h"
#include "dsp/dsp.h"
#include "dsp/internal.h"
#include "dsp/main_memory.h"

namespace wsf::dsp
{
namespace
{

// The numbers of the accelerator and the other hardware registers, as the low 8 bits of their addresses. The numbers
// for DMA and the mailboxes are in internal.h.
constexpr uint8_t kFormat = 0xd1;
constexpr uint8_t kAcdraw = 0xd3;
constexpr uint8_t kAcsah = 0xd4;
constexpr uint8_t kAcsal = 0xd5;
constexpr uint8_t kAceah = 0xd6;
constexpr uint8_t kAceal = 0xd7;
constexpr uint8_t kAccah = 0xd8;
constexpr uint8_t kAccal = 0xd9;
constexpr uint8_t kPredScale = 0xda;
constexpr uint8_t kYn1 = 0xdb;
constexpr uint8_t kYn2 = 0xdc;
constexpr uint8_t kAcdsamp = 0xdd;
constexpr uint8_t kGain = 0xde;
constexpr uint8_t kAcin = 0xdf;
constexpr uint8_t kAmdm = 0xef;
constexpr uint8_t kDirq = 0xfb;

// The init path copies its program from here.
constexpr PAddr kInitProgram = 0x01000000;
constexpr uint32_t kInitProgramSize = 0x1000;

// The init path's busy bit clears 130 time-base ticks after it starts.
constexpr uint64_t kInitBusyTicks = 130;

} // namespace

uint16_t Dsp::ReadDmem(uint16_t address)
{
    // The data memory map: DRAM, the coefficient ROM twice, and the hardware registers through the whole 0xF000-0xFFFF
    // range.
    if (address < kDramSize)
    {
        return dram_[address];
    }
    if (address < 0x2000)
    {
        return kCoefficientRom[address & 0x7ff];
    }
    if (address >= 0xf000)
    {
        return ReadHardware(static_cast<uint8_t>(address));
    }

    return 0;
}

void Dsp::WriteDmem(uint16_t address, uint16_t value)
{
    if (address < kDramSize)
    {
        dram_[address] = value;
    }
    else if (address >= 0xf000)
    {
        WriteHardware(static_cast<uint8_t>(address), value);
    }
}

uint16_t Dsp::ReadHardware(uint8_t reg)
{
    Accelerator& a = accelerator_;
    switch (reg)
    {
    case kFormat:
        return a.format;
    case kAcdraw:
        return AcceleratorRead(true);
    case kAcsah:
        return static_cast<uint16_t>(a.start >> 16);
    case kAcsal:
        return static_cast<uint16_t>(a.start);
    case kAceah:
        return static_cast<uint16_t>(a.end >> 16);
    case kAceal:
        return static_cast<uint16_t>(a.end);
    case kAccah:
        return static_cast<uint16_t>(a.current >> 16);
    case kAccal:
        return static_cast<uint16_t>(a.current);
    case kPredScale:
        return a.pred_scale;
    case kYn1:
        return a.yn1;
    case kYn2:
        return a.yn2;
    case kAcdsamp:
        return AcceleratorRead(false);
    case kGain:
        return a.gain;
    case kAcin:
        return a.input;
    case kDirq:
        return 0;
    case kDmbh:
        return MailboxReadHigh(mailbox_to_cpu_);
    case kDmbl:
        return MailboxReadLow(mailbox_to_cpu_);
    case kCmbh:
        return MailboxReadHigh(mailbox_to_dsp_);
    case kCmbl:
        return MailboxReadLow(mailbox_to_dsp_);
    default:
        return hardware_[reg];
    }
}

void Dsp::WriteHardware(uint8_t reg, uint16_t value)
{
    Accelerator& a = accelerator_;

    const auto high = [value](uint32_t old, uint32_t mask)
    {
        return ((uint32_t{value} << 16) | (old & 0xffff)) & mask;
    };

    const auto low = [value](uint32_t old, uint32_t mask)
    {
        return ((old & 0xffff0000) | value) & mask;
    };

    switch (reg)
    {
    case kDsbl:
        hardware_[kDsbl] = value;
        Dma();
        break;

    case kFormat:
        a.format = value;
        break;

    case kAcdraw:
        AcceleratorWrite(value);
        break;

    case kAcsah:
        a.start = high(a.start, 0x3fffffff);
        break;

    case kAcsal:
        a.start = low(a.start, 0x3fffffff);
        break;

    case kAceah:
        a.end = high(a.end, 0x3fffffff);
        break;

    case kAceal:
        a.end = low(a.end, 0x3fffffff);
        break;

    case kAccah:
        a.current = high(a.current, 0xbfffffff);
        break;

    case kAccal:
        a.current = low(a.current, 0xbfffffff);
        break;

    case kPredScale:
        a.pred_scale = value & 0x7f;
        break;

    case kYn1:
        a.yn1 = value;
        break;

    case kYn2:
        a.yn2 = value;
        a.reads_stopped = false;
        break;

    case kGain:
        a.gain = value;
        break;

    case kAcin:
        a.input = value;
        break;

    case kDirq:
        if (value & 1)
        {
            interrupt_request_ = true;
        }
        break;

    case kDmbh:
        MailboxWriteHigh(mailbox_to_cpu_, value);
        break;

    case kDmbl:
        MailboxWriteLow(mailbox_to_cpu_, value);
        break;

    case kCmbh:
        MailboxWriteHigh(mailbox_to_dsp_, value);
        break;

    case kCmbl:
        MailboxWriteLow(mailbox_to_dsp_, value);
        break;

    default:
        hardware_[reg] = value;
        break;
    }
}

void Dsp::Dma()
{
    // The whole transfer happens at once, between main memory and DRAM or IRAM. The busy bit is never seen set.
    const uint16_t control_bits = hardware_[kDscr];
    const uint32_t length = hardware_[kDsbl];
    hardware_[kDscr] = control_bits | 4;
    if (hardware_[kAmdm] == 0 && length <= 0x4000)
    {
        const uint32_t address = uint32_t{hardware_[kDsmah]} << 16 | hardware_[kDsmal];
        const uint32_t words = length / 2;
        const bool iram = (control_bits & 2) != 0;
        const bool to_main = (control_bits & 1) != 0;
        std::vector<uint16_t>& memory = iram ? iram_ : dram_;
        const uint32_t start = static_cast<uint16_t>(hardware_[kDspa] * 2) / 2u;
        uint8_t* host = nullptr;
        if (MainMemoryRange(address, words * 2, host))
        {
            if (!to_main)
            {
                for (uint32_t i = 0; i < words && start + i < memory.size(); i++)
                {
                    memory[start + i] = LoadBe16(host + i * 2);
                }

                if (iram)
                {
                    BuildLoopMap();
                }
            }
            else if (!iram)
            {
                for (uint32_t i = 0; i < words && start + i < memory.size(); i++)
                {
                    StoreBe16(host + i * 2, memory[start + i]);
                }
            }
        }
    }

    hardware_[kDscr] &= ~4;
    hardware_[kDsbl] = 0;
}

bool Dsp::MainMemoryRange(uint32_t address, uint32_t size, uint8_t*& host)
{
    // The address is masked to 30 bits. The whole range must lie in MEM1 or in MEM2.
    const uint32_t physical = address & 0x3fffffff;
    const bool in_mem1 = physical + size <= kMem1Base + kMem1Size;
    const bool in_mem2 = physical >= kMem2Base && physical - kMem2Base + size <= kMem2Size;
    if (!in_mem1 && !in_mem2)
    {
        return false;
    }

    host = memory_->At(physical, std::max<uint32_t>(size, 1));

    return host != nullptr;
}

uint16_t Dsp::AcceleratorMemory(uint32_t current, int size)
{
    // The raw value at the current address for an access size. It comes from MEM2 when bit 28 of the byte address is
    // set. Otherwise it comes from MEM1. Addresses outside MEM1 read as 0.
    uint32_t byte_address = 0;
    switch (size)
    {
    case 0:
        byte_address = current >> 1;
        break;
    case 1:
        byte_address = current;
        break;
    case 2:
        byte_address = current * 2;
        break;
    default:
        return 0;
    }

    const auto byte = [this](uint32_t address) -> uint8_t
    {
        const uint8_t* p = (address & 0x10000000) ? memory_->At(kMem2Base + (address & 0x03ffffff))
                                                  : memory_->At(address & 0x01ffffff);

        return p != nullptr ? *p : 0;
    };

    switch (size)
    {
    case 0:
        return (current & 1) ? byte(byte_address) & 0xf : byte(byte_address) >> 4;
    case 1:
        return byte(byte_address);
    default:
        return static_cast<uint16_t>(byte(byte_address) << 8 | byte(byte_address + 1));
    }
}

uint16_t Dsp::DecodeSample(uint16_t raw)
{
    // ADPCM, or PCM scaled by the gain. Both are predicted from the history with the coefficient pair that the
    // predictor selects.
    Accelerator& a = accelerator_;
    const int pair = a.pred_scale >> 4 & 7;
    const int32_t c1 = static_cast<int16_t>(hardware_[0xa0 + pair * 2]);
    const int32_t c2 = static_cast<int16_t>(hardware_[0xa1 + pair * 2]);
    const int32_t yn1 = static_cast<int16_t>(a.yn1);
    const int32_t yn2 = static_cast<int16_t>(a.yn2);
    int32_t sample = 0;
    if ((a.format >> 2 & 3) == 0)
    {
        int32_t x = raw & 0xf;
        if (x >= 8)
        {
            x -= 16;
        }

        const auto prediction = static_cast<int32_t>(static_cast<uint32_t>(0x400) + static_cast<uint32_t>(c1 * yn1) +
                                                     static_cast<uint32_t>(c2 * yn2));
        const int32_t value = x * (1 << (a.pred_scale & 0xf)) + (prediction >> 11);
        sample = std::clamp(value, -32768, 32767);
    }
    else
    {
        const int shift = (a.format >> 4 & 3) == 0 ? 11 : (a.format >> 4 & 3) == 2 ? 16 : 0;
        const int32_t value = ((static_cast<int32_t>(a.gain) * static_cast<int16_t>(raw)) >> shift) +
                              ((c1 * yn1) >> shift) + ((c2 * yn2) >> shift);
        sample = static_cast<int16_t>(value);
    }

    a.yn2 = a.yn1;
    a.yn1 = static_cast<uint16_t>(sample);

    return static_cast<uint16_t>(sample);
}

uint16_t Dsp::AcceleratorRead(bool raw)
{
    Accelerator& a = accelerator_;
    const int size = a.format & 3;
    if (raw)
    {
        // Returns the raw value, then advances the address. A read at the end wraps to the start and raises exception
        // 3.
        const uint16_t value = AcceleratorMemory(a.current, size);
        a.current = size == 3 ? (a.current & ~3u) | ((a.current + 1) & 3) : a.current + 1;
        if (a.current - 1 == a.end)
        {
            a.current = a.start;
            RaiseException(3);
        }

        a.current &= 0xbfffffff;

        return value;
    }

    // Returns a decoded sample and advances the address past frame headers. Reaching the end stops the reads.
    if (a.reads_stopped)
    {
        return 0;
    }

    const int decoding = a.format >> 2 & 3;
    const uint16_t sample = DecodeSample(decoding == 1 || decoding == 3 ? a.input : AcceleratorMemory(a.current, size));
    uint32_t step = 2;
    if (decoding == 0)
    {
        a.current++;
        if ((a.end & 0xf) == 0 && a.current == a.end)
        {
            a.current = a.start + 1;
        }
        else if ((a.end & 0xf) == 1 && a.current == a.end - 1)
        {
            a.current = a.start;
        }
        else if ((a.current & 0xf) == 0)
        {
            const uint8_t* header = nullptr;
            const uint32_t byte_address = a.current >> 1;
            header = (byte_address & 0x10000000) ? memory_->At(kMem2Base + (byte_address & 0x03ffffff))
                                                 : memory_->At(byte_address & 0x01ffffff);
            a.pred_scale = header != nullptr ? *header : 0;
            a.current += 2;
            step = 4;
        }
    }
    else if (decoding != 1)
    {
        a.current++;
    }

    if (a.current == a.end + step - 1)
    {
        a.current = a.start;
        a.reads_stopped = true;
        RaiseException(5);
    }

    a.current &= 0xbfffffff;

    return sample;
}

void Dsp::AcceleratorWrite(uint16_t value)
{
    // Writes happen only when bit 31 of the current address is set. They always go to MEM2 and raise exception 4 each
    // time.
    Accelerator& a = accelerator_;
    if (!(a.current & 0x80000000))
    {
        return;
    }

    const uint32_t offset = (a.current * 2) & 0x03ffffff;
    if (uint8_t* p = memory_->At(kMem2Base + offset, 2))
    {
        StoreBe16(p, value);
    }

    a.current++;
    RaiseException(4);
}

uint16_t Dsp::MailboxReadHigh(uint32_t mailbox)
{
    return static_cast<uint16_t>(mailbox >> 16);
}

uint16_t Dsp::MailboxReadLow(uint32_t& mailbox)
{
    mailbox &= ~kMailboxFull;

    return static_cast<uint16_t>(mailbox);
}

void Dsp::MailboxWriteHigh(uint32_t& mailbox, uint16_t value)
{
    mailbox = ((uint32_t{value} << 16) | (mailbox & 0xffff)) & ~kMailboxFull;
}

void Dsp::MailboxWriteLow(uint32_t& mailbox, uint16_t value)
{
    mailbox = (mailbox & 0xffff0000) | value | kMailboxFull;
}

uint16_t Dsp::CpuReadMailboxHigh(bool to_dsp) const
{
    return MailboxReadHigh(to_dsp ? mailbox_to_dsp_ : mailbox_to_cpu_);
}

uint16_t Dsp::CpuReadMailboxLow(bool to_dsp)
{
    return MailboxReadLow(to_dsp ? mailbox_to_dsp_ : mailbox_to_cpu_);
}

void Dsp::CpuWriteMailboxHigh(uint16_t value)
{
    MailboxWriteHigh(mailbox_to_dsp_, value);
}

void Dsp::CpuWriteMailboxLow(uint16_t value)
{
    MailboxWriteLow(mailbox_to_dsp_, value);
}

uint16_t Dsp::Control(uint64_t time_base)
{
    // Only these bits are kept on the DSP side.
    if ((control_ & control::kInitBusy) && time_base >= init_busy_until_)
    {
        control_ &= ~control::kInitBusy;
    }

    return control_ & (control::kReset | control::kInterrupt | control::kHalt | control::kInitBusy | control::kInit);
}

void Dsp::WriteControl(uint16_t value, uint64_t time_base)
{
    // The DSP side, in order: reset, the init path, the new value and the CPU's interrupt request.
    if (value & control::kReset)
    {
        Reset();
        value &= ~control::kReset;
    }

    if ((control_ & control::kInit) && !(value & control::kInit))
    {
        InitPath();
        value = (value & ~control::kInit) | control::kInitBusy;
        init_busy_until_ = time_base + kInitBusyTicks;
    }

    control_ = value;
    if (value & control::kInterrupt)
    {
        if (regs_.sr & (1u << 11))
        {
            RaiseException(7);
            control_ &= ~control::kInterrupt;
        }

        CheckExceptions();
    }
}

void Dsp::Reset()
{
    // Only the PC and the wrap registers change.
    regs_.pc = 0x8000;
    regs_.wr = {0xffff, 0xffff, 0xffff, 0xffff};
    BuildLoopMap();
}

void Dsp::InitPath()
{
    // Copy the program at 0x01000000 into the first half of IRAM and set the PC to 0.
    regs_.pc = 0;
    if (const uint8_t* source = memory_->At(kInitProgram, kInitProgramSize))
    {
        for (uint32_t i = 0; i < kInitProgramSize / 2; i++)
        {
            iram_[i] = LoadBe16(source + i * 2);
        }
    }

    BuildLoopMap();
}

} // namespace wsf::dsp
