// SPDX-License-Identifier: MIT

#include "wii/ios.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/disc/header.h"
#include "ntdlib/dsp/memory.h"
#include "xsflib/bytes.h"

namespace wsf::wii
{
namespace
{

// The registers, by their offsets from 0x0D000000: the PPC's request, the PPC's control, IOS's reply, and the PPC's
// Hollywood interrupts, flagged and enabled.
constexpr uint32_t kPpcMsg = 0x00;
constexpr uint32_t kPpcCtrl = 0x04;
constexpr uint32_t kArmMsg = 0x08;
constexpr uint32_t kPpcIrqFlag = 0x30;
constexpr uint32_t kPpcIrqMask = 0x34;

// PPCCTRL's bits.
constexpr uint32_t kX1 = 1u << 0;  // the PPC sends a request
constexpr uint32_t kY2 = 1u << 1;  // IOS has taken it
constexpr uint32_t kY1 = 1u << 2;  // IOS has replied
constexpr uint32_t kX2 = 1u << 3;  // the PPC has taken the reply
constexpr uint32_t kIy1 = 1u << 4; // Y1 interrupts
constexpr uint32_t kIy2 = 1u << 5; // Y2 interrupts

// The IPC interrupt's bit among the PPC's Hollywood interrupts.
constexpr uint32_t kIpcIrq = 1u << 30;

// The commands, and the replies' command.
enum Command : uint32_t
{
    kOpen = 1,
    kClose = 2,
    kRead = 3,
    kWrite = 4,
    kSeek = 5,
    kIoctl = 6,
    kIoctlv = 7,
    kReply = 8,
};

// IOS's errors.
constexpr int32_t kInvalid = -4;
constexpr int32_t kNoDevice = -6;
constexpr int32_t kFsNotFound = -106;
constexpr int32_t kFsExists = -105;

// The size of the whole disc. The drive reports a single-layer disc.
constexpr uint64_t kDiscSize = 0x118240000;

// The drive's errors in the form that RequestError gives: the drive's state (0, ready) in the top byte, then the sense
// key and the additional sense code.
constexpr uint32_t kDriveInvalidCommand = 0x052000;
constexpr uint32_t kDriveOutOfRange = 0x052100;

// How long IOS takes, in CPU cycles, to acknowledge a request and to carry it out.
constexpr uint64_t kAckDelay = 1'000;
constexpr uint64_t kReplyDelay = 20'000;

// A buffer that a request names: a physical address and a size. The size may be 0.
std::span<uint8_t> Buffer(ntdlib::dsp::WiiMemory& memory, uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return {};
    }

    uint8_t* bytes = memory.At(ntdlib::dsp::ToPhysical(address), size);
    return bytes != nullptr ? std::span<uint8_t>(bytes, size) : std::span<uint8_t>();
}

// The console's settings file, /shared2/sys/SYSCONF: "SCv0", the item count and each item's offset, then the items.
// Each item has a type and name-length byte, the name and the value. The file is padded to 0x4000 bytes with "SCed" at
// the end.
std::vector<uint8_t> MakeSysconf()
{
    struct Item
    {
        const char* name;
        uint8_t type; // 0x60 byte, 0x80 short, 0xa0 long, 0xe0 bool
        uint32_t value;
    };

    static constexpr Item kItems[] = {
        {"IPL.SND", 0x60, 1},  // stereo
        {"IPL.LNG", 0x60, 1},  // English
        {"IPL.AR", 0x60, 0},   // 4:3
        {"IPL.E60", 0x60, 1},  // EuRGB60
        {"IPL.PGS", 0x60, 0},  // no progressive scan
        {"IPL.SSV", 0x60, 1},  // screen saver
        {"IPL.CB", 0xa0, 0},   // RTC counter bias
        {"IPL.IDL", 0x60, 0},  // no standby connection
        {"BT.SENS", 0xa0, 3},  // sensor bar sensitivity
        {"BT.BAR", 0x60, 0},   // sensor bar below the TV
        {"BT.SPKV", 0x60, 88}, // remote speaker volume
        {"BT.MOT", 0x60, 1},   // rumble
    };

    std::vector<uint8_t> file(0x4000, 0);
    std::memcpy(file.data(), "SCv0", 4);
    constexpr uint16_t kCount = sizeof(kItems) / sizeof(kItems[0]);
    xsflib::StoreBe16(&file[4], kCount);

    uint32_t at = 6 + (kCount + 1) * 2;
    for (uint16_t i = 0; i < kCount; i++)
    {
        const Item& item = kItems[i];
        const auto name_length = static_cast<uint8_t>(std::strlen(item.name));
        xsflib::StoreBe16(&file[6 + i * 2], static_cast<uint16_t>(at));
        file[at++] = static_cast<uint8_t>(item.type | (name_length - 1));
        std::memcpy(&file[at], item.name, name_length);
        at += name_length;

        if (item.type == 0xa0)
        {
            xsflib::StoreBe32(&file[at], item.value);
            at += 4;
        }
        else if (item.type == 0x80)
        {
            xsflib::StoreBe16(&file[at], static_cast<uint16_t>(item.value));
            at += 2;
        }
        else
        {
            file[at++] = static_cast<uint8_t>(item.value);
        }
    }

    // The offset table's last entry gives the end of the items. "SCed" marks the end of the file.
    xsflib::StoreBe16(&file[6 + kCount * 2], static_cast<uint16_t>(at));
    std::memcpy(&file[0x3ffc], "SCed", 4);

    return file;
}

} // namespace

// A device or file that a descriptor refers to. Each operation sets the result and returns whether IOS replies. An
// operation that the device doesn't support fails.
class Ios::Device
{
public:
    explicit Device(Ios& ios, std::string name) : ios_(ios), name_(std::move(name))
    {
    }

    virtual ~Device() = default;

    // A copy of the device, open in the same state, for a snapshot.
    virtual std::unique_ptr<Device> Clone() const = 0;

    virtual bool Read(std::span<uint8_t> buffer, int32_t& result)
    {
        static_cast<void>(buffer);
        result = kInvalid;

        return true;
    }

    virtual bool Write(std::span<const uint8_t> buffer, int32_t& result)
    {
        static_cast<void>(buffer);
        result = kInvalid;

        return true;
    }

    virtual bool Seek(int32_t offset, int32_t whence, int32_t& result)
    {
        static_cast<void>(offset);
        static_cast<void>(whence);
        result = kInvalid;

        return true;
    }

    virtual bool Ioctl(uint32_t number, std::span<const uint8_t> in, std::span<uint8_t> out, int32_t& result)
    {
        static_cast<void>(number);
        static_cast<void>(in);
        static_cast<void>(out);
        result = kInvalid;

        return true;
    }

    virtual bool Ioctlv(uint32_t number, const std::vector<std::span<uint8_t>>& in,
                        const std::vector<std::span<uint8_t>>& io, int32_t& result)
    {
        static_cast<void>(number);
        static_cast<void>(in);
        static_cast<void>(io);
        result = kInvalid;

        return true;
    }

protected:
    Ios& ios_;
    std::string name_;
};

// The state machine: power, the reset button and the LED. The event hook replies only when such an event happens. That
// never happens here.
class Ios::Stm : public Ios::Device
{
public:
    using Device::Device;

    std::unique_ptr<Device> Clone() const override
    {
        return std::make_unique<Stm>(*this);
    }

    bool Ioctl(uint32_t number, std::span<const uint8_t> in, std::span<uint8_t> out, int32_t& result) override
    {
        static_cast<void>(in);
        static_cast<void>(out);
        result = 0;

        return number != 0x1000;
    }
};

// The NAND file system's operations on its files.
class Ios::FileSystem : public Ios::Device
{
public:
    using Device::Device;

    std::unique_ptr<Device> Clone() const override
    {
        return std::make_unique<FileSystem>(*this);
    }

    bool Ioctl(uint32_t number, std::span<const uint8_t> in, std::span<uint8_t> out, int32_t& result) override
    {
        // The attribute block's path is at 6; a path alone is 64 bytes.
        const auto path_at = [&in](std::size_t offset)
        {
            const std::size_t end = std::min<std::size_t>(in.size(), offset + 64);
            std::string path;
            for (std::size_t i = offset; i < end && in[i] != 0; i++)
            {
                path += static_cast<char>(in[i]);
            }

            return path;
        };

        result = 0;
        switch (number)
        {
        case 0x03: // CreateDir
            return true;

        case 0x06: // GetAttr
            {
                const std::string path = path_at(0);
                if (!ios_.nand_files_.count(path))
                {
                    result = kFsNotFound;
                }

                return true;
            }

        case 0x07: // Delete
            ios_.nand_files_.erase(path_at(0));
            return true;

        case 0x09: // CreateFile
            {
                const std::string path = path_at(6);
                if (ios_.nand_files_.count(path))
                {
                    result = kFsExists;
                }
                else
                {
                    ios_.nand_files_[path];
                }

                return true;
            }

        default:
            return Device::Ioctl(number, in, out, result);
        }
    }
};

// An open file of the NAND file system.
class Ios::NandFile : public Ios::Device
{
public:
    using Device::Device;

    std::unique_ptr<Device> Clone() const override
    {
        return std::make_unique<NandFile>(*this);
    }

    bool Read(std::span<uint8_t> buffer, int32_t& result) override
    {
        const std::vector<uint8_t>& data = Data();
        const std::size_t count = std::min(buffer.size(), data.size() - std::min(position_, data.size()));
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(position_), count, buffer.begin());
        position_ += count;
        result = static_cast<int32_t>(count);

        return true;
    }

    bool Write(std::span<const uint8_t> buffer, int32_t& result) override
    {
        std::vector<uint8_t>& data = Data();
        if (data.size() < position_ + buffer.size())
        {
            data.resize(position_ + buffer.size());
        }

        std::copy(buffer.begin(), buffer.end(), data.begin() + static_cast<std::ptrdiff_t>(position_));
        position_ += buffer.size();
        result = static_cast<int32_t>(buffer.size());

        return true;
    }

    bool Seek(int32_t offset, int32_t whence, int32_t& result) override
    {
        const auto size = static_cast<int64_t>(Data().size());
        const int64_t base = whence == 0 ? 0 : whence == 1 ? static_cast<int64_t>(position_) : size;
        const int64_t to = base + offset;
        if (to < 0 || to > size)
        {
            result = kInvalid;

            return true;
        }

        position_ = static_cast<std::size_t>(to);
        result = static_cast<int32_t>(to);

        return true;
    }

    bool Ioctl(uint32_t number, std::span<const uint8_t> in, std::span<uint8_t> out, int32_t& result) override
    {
        // GetFileStats: the length and the position.
        if (number == 0x0b && out.size() >= 8)
        {
            xsflib::StoreBe32(&out[0], static_cast<uint32_t>(Data().size()));
            xsflib::StoreBe32(&out[4], static_cast<uint32_t>(position_));
            result = 0;

            return true;
        }

        return Device::Ioctl(number, in, out, result);
    }

private:
    // The file's data, looked up by its path (the device's name) on each access. After a snapshot is restored, this
    // finds the restored file system's file. A file deleted while open comes back empty.
    std::vector<uint8_t>& Data()
    {
        return ios_.nand_files_[name_];
    }

    std::size_t position_ = 0;
};

// The disc drive, with the game's disc inserted and its game partition open. The system menu leaves the drive in this
// state.
class Ios::DiscDrive : public Ios::Device
{
public:
    using Device::Device;

    std::unique_ptr<Device> Clone() const override
    {
        return std::make_unique<DiscDrive>(*this);
    }

    // The SDK's start-up code checks the drive with requests that a retail drive rejects: an unencrypted read past the
    // disc's end and a ReportKey. A rejected request returns 2. RequestError then gives the error.
    bool Ioctl(uint32_t number, std::span<const uint8_t> in, std::span<uint8_t> out, int32_t& result) override
    {
        result = 1; // the drive's success
        switch (number)
        {
        case 0x12: // Inquiry: the drive's revision and date
            if (out.size() >= 8)
            {
                std::fill(out.begin(), out.end(), uint8_t{0});
                xsflib::StoreBe32(&out[0], 0x00000002);
                xsflib::StoreBe32(&out[4], 0x20060526);
            }

            return true;

        case 0x70: // ReadDiskID: the header's first 32 bytes
            if (!ios_.disc_.Read(0, out.data(), std::min<std::size_t>(out.size(), 0x20)))
            {
                result = 2;
            }

            return true;

        case 0x71: // Read: length, then the partition offset >> 2
            {
                const uint32_t length = in.size() >= 8 ? xsflib::LoadBe32(&in[4]) : 0;
                const uint64_t offset = in.size() >= 12 ? uint64_t{xsflib::LoadBe32(&in[8])} * 4 : 0;
                if (length > out.size() || !ios_.disc_.Read(offset, out.data(), length))
                {
                    result = 2;
                }

                return true;
            }

        case 0x8d: // UnencryptedRead: length, then the disc offset >> 2, outside the partition
            {
                // The disc's header is the partition's header. The rest of the disc outside the partition reads as
                // zeros.
                const uint32_t length = in.size() >= 8 ? xsflib::LoadBe32(&in[4]) : 0;
                const uint64_t offset = in.size() >= 12 ? uint64_t{xsflib::LoadBe32(&in[8])} * 4 : 0;
                if (length > out.size())
                {
                    result = 2;

                    return true;
                }

                if (offset + length > kDiscSize)
                {
                    error_ = kDriveOutOfRange;
                    result = 2;

                    return true;
                }

                std::fill(out.begin(), out.begin() + length, uint8_t{0});
                if (offset < ntdlib::disc::kHeaderSize)
                {
                    const auto size =
                        static_cast<std::size_t>(std::min<uint64_t>(length, ntdlib::disc::kHeaderSize - offset));
                    ios_.disc_.Read(offset, out.data(), size);
                }

                return true;
            }

        case 0x88: // GetCoverStatus: closed, with a disc
            if (out.size() >= 4)
            {
                xsflib::StoreBe32(&out[0], 2);
            }

            return true;

        case 0xa4: // ReportKey: a Wii's drive rejects it
            error_ = kDriveInvalidCommand;
            result = 2;

            return true;

        case 0xe0: // RequestError: the last request's error, if it was rejected
            if (out.size() >= 4)
            {
                xsflib::StoreBe32(&out[0], error_);
            }

            error_ = 0;

            return true;

        case 0x86: // ClearCoverInterrupt
        case 0x8a: // Reset
        case 0x8e: // EnableDvdVideo
        case 0xe3: // StopMotor
            return true;

        default:
            return Device::Ioctl(number, in, out, result);
        }
    }

private:
    uint32_t error_ = 0; // the last rejected request's error, for RequestError
};

// ES. The running title is the game.
class Ios::Es : public Ios::Device
{
public:
    using Device::Device;

    std::unique_ptr<Device> Clone() const override
    {
        return std::make_unique<Es>(*this);
    }

    bool Ioctlv(uint32_t number, const std::vector<std::span<uint8_t>>& in, const std::vector<std::span<uint8_t>>& io,
                int32_t& result) override
    {
        // GetTitleID: the title ID of a disc game, 00010000 followed by its four-letter code.
        if (number == 0x20 && io.size() == 1 && io[0].size() >= 8)
        {
            uint8_t header[4] = {};
            ios_.disc_.Read(0, header, sizeof(header));
            xsflib::StoreBe32(&io[0][0], 0x00010000);
            std::copy(std::begin(header), std::end(header), &io[0][4]);
            result = 0;

            return true;
        }

        return Device::Ioctlv(number, in, io, result);
    }
};

Ios::Ios(ntdlib::dsp::WiiMemory& memory, Disc& disc, ntdlib::ppc::Cpu& cpu) : memory_(memory), disc_(disc), cpu_(cpu)
{
    nand_files_["/shared2/sys/SYSCONF"] = MakeSysconf();
}

Ios::~Ios() = default;

Ios::Snapshot Ios::Save() const
{
    Snapshot snapshot{*this, {}};
    for (const auto& [fd, device] : descriptors_)
    {
        snapshot.descriptors.emplace(fd, device->Clone());
    }

    return snapshot;
}

void Ios::Restore(const Snapshot& snapshot)
{
    IosState::operator=(snapshot.state);
    descriptors_.clear();
    for (const auto& [fd, device] : snapshot.descriptors)
    {
        descriptors_.emplace(fd, device->Clone());
    }
}

uint32_t Ios::Read(uint32_t offset) const
{
    switch (offset)
    {
    case kPpcMsg:
        return ppc_message_;
    case kPpcCtrl:
        return ppc_control_;
    case kArmMsg:
        return arm_message_;
    case kPpcIrqFlag:
        return irq_flags_;
    case kPpcIrqMask:
        return irq_mask_;
    default:
        return 0;
    }
}

void Ios::Write(uint32_t offset, uint32_t value)
{
    switch (offset)
    {
    case kPpcMsg:
        ppc_message_ = value;
        break;

    case kPpcCtrl:
        {
            // Writing 1 to Y1 or Y2 clears it. X2 releases the reply. X1 sends the request in PPCMSG.
            ppc_control_ = (ppc_control_ & (kY1 | kY2) & ~value) | (value & (kIy1 | kIy2));
            if (value & kX2)
            {
                reply_out_ = false;
            }

            if (value & kX1)
            {
                int32_t result = 0;
                if (Execute(ppc_message_, result))
                {
                    replies_.push_back({ppc_message_, result, cpu_.Cycles() + kReplyDelay});
                }

                ack_due_ = cpu_.Cycles() + kAckDelay;
            }

            cpu_.LimitRun(NextEvent());
            break;
        }

    case kPpcIrqFlag:
        irq_flags_ &= ~value;
        break;

    case kPpcIrqMask:
        irq_mask_ = value;
        break;

    default:
        break;
    }
}

bool Ios::InterruptPending() const
{
    return (irq_flags_ & irq_mask_) != 0;
}

uint64_t Ios::NextEvent() const
{
    uint64_t next = ack_due_;
    if (CanSendReply())
    {
        next = std::min(next, replies_.front().due);
    }

    return next;
}

void Ios::Advance(uint64_t now)
{
    if (now >= ack_due_)
    {
        ack_due_ = UINT64_MAX;
        ppc_control_ |= kY2;
        SetIrq();
    }

    // The next reply. It goes out once the PPC has taken the last one.
    if (CanSendReply() && now >= replies_.front().due)
    {
        const Request reply = replies_.front();
        replies_.pop_front();
        uint8_t* request = memory_.At(reply.address, 0x20);
        if (request != nullptr)
        {
            const uint32_t command = xsflib::LoadBe32(&request[0]);
            xsflib::StoreBe32(&request[0], kReply);
            xsflib::StoreBe32(&request[4], static_cast<uint32_t>(reply.result));
            xsflib::StoreBe32(&request[8], command);
        }

        arm_message_ = reply.address;
        ppc_control_ |= kY1;
        reply_out_ = true;
        SetIrq();
    }
}

bool Ios::CanSendReply() const
{
    return !replies_.empty() && !reply_out_ && !(ppc_control_ & kY1);
}

void Ios::SetIrq()
{
    if (((ppc_control_ & kY1) && (ppc_control_ & kIy1)) || ((ppc_control_ & kY2) && (ppc_control_ & kIy2)))
    {
        irq_flags_ |= kIpcIrq;
    }
}

bool Ios::Execute(ntdlib::PAddr address, int32_t& result)
{
    const uint8_t* request = memory_.At(address, 0x20);
    if (request == nullptr)
    {
        result = kInvalid;

        return true;
    }

    const uint32_t command = xsflib::LoadBe32(&request[0]);
    const auto fd = static_cast<int32_t>(xsflib::LoadBe32(&request[8]));

    const auto arg = [request](int i)
    {
        return xsflib::LoadBe32(&request[0x0c + i * 4]);
    };

    if (command == kOpen)
    {
        std::string path;
        for (uint32_t at = arg(0); path.size() < 64; at++)
        {
            const uint8_t* c = memory_.At(ntdlib::dsp::ToPhysical(at), 1);
            if (c == nullptr || *c == 0)
            {
                break;
            }

            path += static_cast<char>(*c);
        }

        result = Open(path);

        return true;
    }

    const auto it = descriptors_.find(fd);
    if (it == descriptors_.end())
    {
        result = kInvalid;

        return true;
    }

    Device& device = *it->second;
    switch (command)
    {
    case kClose:
        descriptors_.erase(it);
        result = 0;
        return true;

    case kRead:
        return device.Read(Buffer(memory_, arg(0), arg(1)), result);

    case kWrite:
        return device.Write(Buffer(memory_, arg(0), arg(1)), result);

    case kSeek:
        return device.Seek(static_cast<int32_t>(arg(0)), static_cast<int32_t>(arg(1)), result);

    case kIoctl:
        return device.Ioctl(arg(0), Buffer(memory_, arg(1), arg(2)), Buffer(memory_, arg(3), arg(4)), result);

    case kIoctlv:
        {
            // The vectors: the inputs, then the buffers that IOS writes.
            std::vector<std::span<uint8_t>> in;
            std::vector<std::span<uint8_t>> io;
            const uint8_t* vectors = memory_.At(ntdlib::dsp::ToPhysical(arg(3)), (arg(1) + arg(2)) * 8);
            for (uint32_t i = 0; vectors != nullptr && i < arg(1) + arg(2); i++)
            {
                const std::span<uint8_t> buffer =
                    Buffer(memory_, xsflib::LoadBe32(&vectors[i * 8]), xsflib::LoadBe32(&vectors[i * 8 + 4]));
                (i < arg(1) ? in : io).push_back(buffer);
            }

            return device.Ioctlv(arg(0), in, io, result);
        }

    default:
        result = kInvalid;
        return true;
    }
}

int32_t Ios::Open(const std::string& path)
{
    std::unique_ptr<Device> device;
    if (path == "/dev/stm/immediate" || path == "/dev/stm/eventhook")
    {
        device = std::make_unique<Stm>(*this, path);
    }
    else if (path == "/dev/fs")
    {
        device = std::make_unique<FileSystem>(*this, path);
    }
    else if (path == "/dev/di")
    {
        device = std::make_unique<DiscDrive>(*this, path);
    }
    else if (path == "/dev/es")
    {
        device = std::make_unique<Es>(*this, path);
    }
    else if (!path.starts_with("/dev/"))
    {
        const auto file = nand_files_.find(path);
        if (file == nand_files_.end())
        {
            return kFsNotFound;
        }

        device = std::make_unique<NandFile>(*this, path);
    }
    else
    {
        return kNoDevice;
    }

    // The lowest free descriptor.
    int32_t fd = 0;
    while (descriptors_.count(fd))
    {
        fd++;
    }

    descriptors_[fd] = std::move(device);

    return fd;
}

} // namespace wsf::wii
