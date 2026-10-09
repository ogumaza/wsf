// SPDX-License-Identifier: MIT

// WSF player core (see player.h).

#include "wsf/player.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/bytes.h"
#include "dsp/main_memory.h"
#include "ppc/cpu.h"
#include "wii/file_disc.h"
#include "wii/system.h"
#include "wsf/archive_player.h"

namespace wsf
{
namespace
{

// Emulated time run per step while producing output: 1 ms of CPU cycles.
constexpr uint64_t kStepCycles = wii::System::kCpuClock / 1000;

// Longest time allowed from boot until the driver reports that the sound plays.
constexpr uint64_t kBootTimeout = uint64_t{10} * wii::System::kCpuClock;

// The partition header, stored in the set as a FILE chunk. The disc starts with it.
constexpr char kHeaderPath[] = "sys/boot.bin";

} // namespace

struct Player::Snapshot
{
    const Player* owner = nullptr;
    uint64_t run = 0;
    State state = State::kIdle;
    uint64_t finished_frame = 0;
    uint64_t dropped = 0;         // samples produced before `pending`
    std::vector<int16_t> pending; // samples produced but not handed out yet
    std::shared_ptr<ArchivePlayer::Snapshot> archive;
    std::shared_ptr<wii::System::Snapshot> system;
};

Player::Player() = default;

Player::~Player() = default;

bool Player::Fail(const std::string& message)
{
    error_ = message;
    state_ = State::kError;

    return false;
}

bool Player::Load(const std::string& path, const FileReader& reader)
{
    LoadedSet set;
    if (auto err = LoadSet(path, reader, set))
    {
        return Fail(*err);
    }

    return Load(std::move(set));
}

bool Player::Load(LoadedSet set)
{
    set_ = std::move(set);
    system_.reset();
    disc_.reset();
    archive_.reset();
    state_ = State::kIdle;
    error_.clear();

    if (!set_.descriptor && !set_.archive)
    {
        return Fail("no process or archive descriptor");
    }

    return true;
}

uint32_t Player::DriverStatus() const
{
    if (archive_)
    {
        return archive_->Busy() ? 1 : 2;
    }

    const uint32_t address = set_.descriptor ? set_.descriptor->status_address : 0;
    if (!address || !system_)
    {
        return 1; // no status word: assume playing
    }

    const uint8_t* word = system_->Memory().At(ToPhysical(address), 4);

    return word != nullptr ? LoadBe32(word) : 1;
}

std::vector<int16_t>& Player::Output()
{
    return archive_ ? archive_->output_ : system_output_;
}

bool Player::Start()
{
    // Emulation failures arrive as exceptions.
    try
    {
        return StartImpl();
    }
    catch (const std::exception& e)
    {
        return Fail(std::string("emulation failed: ") + e.what());
    }
}

bool Player::StartImpl()
{
    run_++;
    consumed_ = 0;
    dropped_ = 0;
    error_.clear();
    if (set_.archive)
    {
        system_.reset();
        disc_.reset();
        archive_ = std::make_unique<ArchivePlayer>();
        if (auto err = archive_->Start(set_))
        {
            return Fail(*err);
        }

        state_ = State::kPlaying;
        return true;
    }

    archive_.reset();
    if (!set_.descriptor)
    {
        return Fail("nothing loaded");
    }

    // The disc: the partition's header and the set's files. A FILE chunk replaces an earlier one with the same path.
    std::map<std::string, const std::vector<uint8_t>*> files;
    for (const auto& f : set_.files)
    {
        files[f.path] = &f.data;
    }

    std::vector<uint8_t> header(0x440, 0);
    StoreBe32(&header[0x18], 0x5d1c9ea3); // the Wii disc magic
    std::vector<std::pair<std::string, std::span<const uint8_t>>> disc_files;
    for (const auto& [path, data] : files)
    {
        if (path == kHeaderPath)
        {
            header = *data;
        }
        else
        {
            disc_files.emplace_back(path, *data);
        }
    }

    system_.reset();
    disc_ = std::make_unique<wii::FileDisc>(std::move(header), disc_files);
    system_ = std::make_unique<wii::System>(*disc_);

    // The set's memory, then the boot.
    for (const auto& m : set_.memory)
    {
        uint8_t* to = system_->Memory().At(ToPhysical(m.address), m.data.size());
        if (to == nullptr)
        {
            return Fail("a MEM chunk lies outside the Wii's memory");
        }

        std::copy(m.data.begin(), m.data.end(), to);
    }

    const ProcessDescriptor& d = *set_.descriptor;
    system_->Start(d.entry, d.ios_version);
    system_output_.clear();
    system_->DspRegisters().on_audio = [this](std::span<const uint8_t, 32> block)
    {
        // Each sample is right, then left.
        for (int i = 0; i < 8; i++)
        {
            system_output_.push_back(static_cast<int16_t>(LoadBe16(&block[i * 4 + 2])));
            system_output_.push_back(static_cast<int16_t>(LoadBe16(&block[i * 4])));
        }
    };

    // Run until the driver reports that the sound plays. Discard the silence before that point.
    while (true)
    {
        const uint32_t status = DriverStatus();
        if (status & 0x80000000u)
        {
            return Fail("driver error at step " + std::to_string(status & 0xff));
        }

        if (status == 1 || status == 2)
        {
            break;
        }

        if (system_->Cpu().Cycles() > kBootTimeout)
        {
            return Fail("timed out waiting for the sound to start");
        }

        if (!Step())
        {
            return false;
        }

        system_output_.clear();
    }

    state_ = State::kPlaying;

    return true;
}

bool Player::Step()
{
    if (archive_)
    {
        try
        {
            if (!archive_->RunFrame())
            {
                return Fail("AX's microcode stopped responding");
            }
        }
        catch (const std::exception& e)
        {
            return Fail(std::string("emulation failed: ") + e.what());
        }

        return true;
    }

    try
    {
        const ppc::StopReason reason = system_->Run(kStepCycles);
        if (reason == ppc::StopReason::kFault)
        {
            return Fail("the emulated program accessed memory that isn't there");
        }
        if (reason == ppc::StopReason::kIllegal)
        {
            return Fail("the emulated program ran an instruction that the CPU doesn't support");
        }
    }
    catch (const std::exception& e)
    {
        return Fail(std::string("emulation failed: ") + e.what());
    }

    return true;
}

std::size_t Player::Render(int16_t* out, std::size_t frames)
{
    if (state_ != State::kPlaying && state_ != State::kFinished)
    {
        std::fill(out, out + frames * 2, int16_t{0});
        return 0;
    }

    auto& buf = Output();
    while ((buf.size() - consumed_) / 2 < frames)
    {
        if (!Step())
        {
            std::fill(out, out + frames * 2, int16_t{0});
            return 0;
        }

        if (state_ == State::kPlaying && DriverStatus() == 2)
        {
            state_ = State::kFinished;
            finished_frame_ = (dropped_ + buf.size()) / 2;
        }
    }

    std::copy(buf.begin() + static_cast<std::ptrdiff_t>(consumed_),
              buf.begin() + static_cast<std::ptrdiff_t>(consumed_ + frames * 2), out);
    consumed_ += frames * 2;

    // Drop consumed samples now and then to bound memory.
    if (consumed_ > (1u << 20))
    {
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed_));
        dropped_ += consumed_;
        consumed_ = 0;
    }

    return frames;
}

std::shared_ptr<const Player::Snapshot> Player::Save(const Snapshot* previous)
{
    if ((state_ != State::kPlaying && state_ != State::kFinished) || (!archive_ && !system_))
    {
        return nullptr;
    }

    if (previous && (previous->owner != this || previous->run != run_))
    {
        previous = nullptr;
    }

    auto snapshot = std::make_shared<Snapshot>();
    snapshot->owner = this;
    snapshot->run = run_;
    snapshot->state = state_;
    snapshot->finished_frame = finished_frame_;
    const std::vector<int16_t>& buf = Output();
    snapshot->pending.assign(buf.begin() + static_cast<std::ptrdiff_t>(consumed_), buf.end());
    snapshot->dropped = dropped_ + consumed_;
    try
    {
        if (archive_)
        {
            snapshot->archive = archive_->Save(previous ? previous->archive.get() : nullptr);
        }
        else
        {
            snapshot->system =
                std::make_shared<wii::System::Snapshot>(system_->Save(previous ? previous->system.get() : nullptr));
        }
    }
    catch (const std::exception&)
    {
        // Saving only reads the emulation's state. Playback can continue without the snapshot.
        return nullptr;
    }

    return snapshot;
}

bool Player::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner != this || snapshot.run != run_ || (archive_ ? !snapshot.archive : !system_ || !snapshot.system))
    {
        return false;
    }

    try
    {
        if (archive_)
        {
            archive_->Restore(*snapshot.archive);
        }
        else
        {
            system_->Restore(*snapshot.system);
        }
    }
    catch (const std::exception& e)
    {
        return Fail(std::string("restoring the emulation's state failed: ") + e.what());
    }

    Output() = snapshot.pending;
    consumed_ = 0;
    dropped_ = snapshot.dropped;
    finished_frame_ = snapshot.finished_frame;
    state_ = snapshot.state;
    error_.clear();

    return true;
}

long long Player::LengthMs() const
{
    auto it = set_.tags.find("length");
    return it == set_.tags.end() ? -1 : ParseTime(it->second);
}

long long Player::FadeMs() const
{
    auto it = set_.tags.find("fade");
    return it == set_.tags.end() ? -1 : ParseTime(it->second);
}

double Player::Volume() const
{
    auto it = set_.tags.find("volume");
    if (it == set_.tags.end())
    {
        return 1.0;
    }

    // Text after the number, as in "0.5dB", is ignored. So is a value that isn't a finite number, such as 1e999. It
    // would turn every sample into nonsense.
    const std::optional<DecimalNumber> volume = ReadDecimal(it->second);
    return volume && std::isfinite(volume->value) ? volume->value : 1.0;
}

} // namespace wsf
