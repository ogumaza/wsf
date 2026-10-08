// SPDX-License-Identifier: MIT

// A high-level model of IOS, the kernel of the Wii's I/O processor. The game's IPC library reaches IOS through
// Hollywood's IPC registers (physical 0x0D000000, mirrored at 0x0D800000). The PPC writes a request's physical address
// to PPCMSG and sets X1 in PPCCTRL. IOS acknowledges with Y2 and carries out the request. It replies by putting the
// address in ARMMSG and setting Y1. The PPC clears Y1 and sets X2 when it has taken the reply. Y1 and Y2 interrupt the
// PPC when IY1 and IY2 allow it. The interrupt goes through bit 30 of the PPC's Hollywood interrupt flags (0x30, with
// its mask at 0x34).
//
// A request is 0x20 bytes: the command (1 open, 2 close, 3 read, 4 write, 5 seek, 6 ioctl, 7 ioctlv), the result, the
// file descriptor, and the command's arguments, with physical addresses. A reply sets the command to 8. It moves the
// original command to where the descriptor was.
//
// The devices are those that the OS and the sound code open: the state machine (/dev/stm), the NAND file system
// (/dev/fs and its files, with the console settings), the disc drive (/dev/di) and ES (/dev/es). Opening anything else
// fails as if it were missing.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "dsp/main_memory.h"
#include "ppc/cpu.h"

namespace wsf::wii
{

class Disc;

// IOS's state apart from its open devices. A snapshot of the system copies it (see Ios::Save).
class IosState
{
protected:
    struct Request
    {
        PAddr address = 0;
        int32_t result = 0;
        uint64_t due = 0;
    };

    uint32_t ppc_message_ = 0;
    uint32_t ppc_control_ = 0;
    uint32_t arm_message_ = 0;
    uint32_t irq_flags_ = 0;
    uint32_t irq_mask_ = 0;

    // The acknowledgement waiting to be given, and the replies waiting to be sent, in order.
    uint64_t ack_due_ = UINT64_MAX;
    std::deque<Request> replies_;
    bool reply_out_ = false; // a reply the PPC hasn't released with X2

    // The NAND file system's files, by path.
    std::map<std::string, std::vector<uint8_t>> nand_files_;
};

class Ios : private IosState
{
public:
    // An open device, as a descriptor names it.
    class Device;

    // IOS's state, with a copy of each open device.
    struct Snapshot
    {
        IosState state;
        std::map<int32_t, std::shared_ptr<const Device>> descriptors;
    };

    Ios(MainMemory& memory, Disc& disc, ppc::Cpu& cpu);
    ~Ios();

    // A 32-bit register at `offset` from 0x0D000000.
    uint32_t Read(uint32_t offset) const;
    void Write(uint32_t offset, uint32_t value);

    // Whether the IPC interrupt is pending and enabled: the Hollywood line to the processor interface.
    bool InterruptPending() const;

    // The CPU cycle of the next acknowledgement or reply, and those due by `now`.
    uint64_t NextEvent() const;
    void Advance(uint64_t now);

    // IOS's state between runs of the CPU, and its restoration.
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

    // Called with a line of text for each request that no device handles. Useful when bringing up games.
    std::function<void(const std::string&)> on_unknown;

private:
    class Stm;
    class FileSystem;
    class NandFile;
    class DiscDrive;
    class Es;

    // Carries out the request at `address`. Returns false for one that stays pending, such as an event hook.
    bool Execute(PAddr address, int32_t& result, uint64_t& delay);

    int32_t Open(const std::string& path, uint32_t mode, uint64_t& delay);
    void Unknown(const std::string& what);
    void SetIrq();

    MainMemory& memory_;
    Disc& disc_;
    ppc::Cpu& cpu_;
    std::map<int32_t, std::unique_ptr<Device>> descriptors_;
};

} // namespace wsf::wii
