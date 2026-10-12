// SPDX-License-Identifier: MIT

// WSF player core (see player.h).

#include "wsf/player.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/header.h"
#include "ntdlib/flipper/file_disc.h"
#include "ntdlib/flipper/game_source.h"
#include "ntdlib/ppc/cpu.h"
#include "xsflib/player/modes.h"

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

struct Player::SourceSnapshot : xsflib::player::ModeSnapshot<ArchivePlayer::Snapshot, wii::System::Snapshot>
{
};

Player::Player() = default;

Player::~Player() = default;

void Player::Unload()
{
    system_.reset();
    disc_.reset();
    archive_.reset();
}

bool Player::Playable() const
{
    return set_.descriptor || set_.archive;
}

uint32_t Player::DriverStatus() const
{
    if (archive_)
    {
        return archive_->Busy() ? 1 : 2;
    }

    if (!system_)
    {
        return 1;
    }

    return ntdlib::flipper::DriverStatus(system_->Memory(), set_.descriptor ? set_.descriptor->status_address : 0);
}

std::vector<int16_t>& Player::Output()
{
    return archive_ ? archive_->Output() : system_output_;
}

uint64_t Player::EmulatedCycles() const
{
    return system_->Cpu().Cycles();
}

bool Player::StartSource()
{
    if (set_.archive)
    {
        system_.reset();
        disc_.reset();
        archive_ = std::make_unique<ArchivePlayer>();
        if (auto err = archive_->Start(set_))
        {
            return Fail(*err);
        }

        return true;
    }

    archive_.reset();

    // The disc: the partition's header and the set's files.
    system_.reset();
    disc_ = ntdlib::flipper::DiscFromFiles(set_.files, kHeaderPath, ntdlib::disc::kWiiMagicOffset,
                                           ntdlib::disc::kWiiMagic, 4);
    system_ = std::make_unique<wii::System>(*disc_);

    // The set's memory, then the boot.
    if (!ntdlib::flipper::LoadMemChunks(system_->Memory(), set_.memory))
    {
        return Fail("a MEM chunk lies outside the Wii's memory");
    }

    const ProcessDescriptor& d = *set_.descriptor;
    system_->Start(d.entry, d.ios_version);
    system_output_.clear();
    system_->DspRegisters().on_audio = [this](std::span<const uint8_t, 32> block)
    {
        ntdlib::flipper::AppendAudioBlock(system_output_, block);
    };

    // Run until the driver reports that the sound plays. Discard the silence before that point.
    return RunUntilSoundPlays(kBootTimeout);
}

bool Player::RunStep()
{
    if (archive_)
    {
        if (!archive_->RunFrame())
        {
            return Fail("AX's microcode stopped responding");
        }

        return true;
    }

    if (const auto message = ntdlib::flipper::StopMessage(system_->Run(kStepCycles)))
    {
        return Fail(*message);
    }

    return true;
}

bool Player::Finished() const
{
    return DriverStatus() == 2;
}

std::shared_ptr<const Player::SourceSnapshot> Player::SaveSource(const SourceSnapshot* previous)
{
    return xsflib::player::SaveModes(archive_.get(), system_.get(), previous);
}

bool Player::CanRestore(const SourceSnapshot* source) const
{
    return xsflib::player::CanRestoreModes(archive_.get(), system_.get(), source);
}

void Player::RestoreSource(const SourceSnapshot& source)
{
    xsflib::player::RestoreModes(archive_.get(), system_.get(), source);
}

} // namespace wsf
