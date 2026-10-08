// SPDX-License-Identifier: MIT

#include "nw4r/engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "ax/microcode.h"
#include "ax/pb.h"
#include "ax/system.h"
#include "common/bytes.h"
#include "dsp/main_memory.h"
#include "nw4r/sequence.h"
#include "rsar/formats.h"

namespace wsf::nw4r
{
namespace
{

ax::SampleFormat ToAxFormat(rsar::WaveFormat format)
{
    switch (format)
    {
    case rsar::WaveFormat::kPcm16:
        return ax::SampleFormat::kPcm16;
    case rsar::WaveFormat::kPcm8:
        return ax::SampleFormat::kPcm8;
    default:
        return ax::SampleFormat::kAdpcm;
    }
}

// A sample's index from a PB address, counting from address 0 in the format's units. ADPCM indices skip the two header
// nibbles of each 16-nibble frame.
uint64_t SampleAt(uint32_t address, ax::SampleFormat format)
{
    if (format == ax::SampleFormat::kAdpcm)
    {
        return uint64_t{address / 16} * 14 + std::max<uint32_t>(address % 16, 2) - 2;
    }

    return address;
}

uint32_t AddressOf(uint64_t sample, ax::SampleFormat format)
{
    if (format == ax::SampleFormat::kAdpcm)
    {
        return static_cast<uint32_t>(sample / 14 * 16 + sample % 14 + 2);
    }

    return static_cast<uint32_t>(sample);
}

// Does the microcode's work on a running voice's position over a frame, without the DSP. The voice advances by its
// pitch over the frame's samples and wraps to its loop. A voice that doesn't loop stops at its loop address, in the
// zero buffer, when it passes its end. Nothing is mixed.
void AdvanceWithoutDsp(ax::Pb& pb)
{
    const auto format = static_cast<ax::SampleFormat>(pb.addr.format);
    const uint64_t step =
        uint64_t{ax::Join(pb.src.ratio_hi, pb.src.ratio_lo)} * ax::kSamplesPerFrame + pb.src.current_address_frac;
    pb.src.current_address_frac = static_cast<uint16_t>(step);
    uint64_t sample = SampleAt(ax::Join(pb.addr.current_address_hi, pb.addr.current_address_lo), format) + (step >> 16);
    const uint64_t end = SampleAt(ax::Join(pb.addr.end_address_hi, pb.addr.end_address_lo), format);
    if (sample > end)
    {
        const uint64_t loop = SampleAt(ax::Join(pb.addr.loop_address_hi, pb.addr.loop_address_lo), format);
        if (pb.addr.loop_flag && loop <= end)
        {
            sample = loop + (sample - end - 1) % (end - loop + 1);
        }
        else
        {
            sample = loop;
            pb.state = static_cast<uint16_t>(ax::VoiceState::kStop);
        }
    }

    const uint32_t address = AddressOf(sample, format);
    pb.addr.current_address_hi = static_cast<uint16_t>(address >> 16);
    pb.addr.current_address_lo = static_cast<uint16_t>(address);
}

} // namespace

Engine::Engine(MainMemory& memory, ax::System& ax, const rsar::SoundArchive& archive, SndBuild build)
    : memory_(memory), ax_(ax), archive_(archive), ax_voices_(*this), voices_(*this), channels_(*this), build_(build)
{
    std::memset(memory_.At(kZeroBuffer, kZeroBufferSize), 0, kZeroBufferSize);
    ax_.RegisterCallback([this] { voices_.NotifyVoiceUpdate(); });
}

Engine::~Engine()
{
    ax_.RegisterCallback(nullptr);
}

SeqPlayer* Engine::StartSequence(uint32_t index)
{
    // SoundArchivePlayer's PrepareSeqImpl: the sequence file and its bank must be in the archive.
    if (index >= archive_.Sounds().size())
    {
        return nullptr;
    }

    const rsar::SoundInfo& sound = archive_.Sounds()[index];
    if (sound.type != rsar::SoundType::kSequence)
    {
        return nullptr;
    }

    const std::optional<rsar::FileData> file = archive_.File(sound.file_id);
    const LoadedBank* bank = GetBank(sound.bank_id);
    if (!file || bank == nullptr)
    {
        return nullptr;
    }

    sequences_.push_back(std::make_unique<rsar::Sequence>(rsar::Sequence::Parse(file->file)));
    players_.push_back(std::make_unique<SeqPlayer>(*this, sound, *sequences_.back(), bank));
    SeqPlayer* player = players_.back().get();
    player->Start();

    return player;
}

Channel* Engine::NoteOn(const LoadedBank& bank, const NoteOnInfo& info)
{
    // BankFileReader::ReadInstInfo and ReadWaveParam, then a channel for the wave's channels at the note's priority.
    const rsar::Region* region = bank.bank.Find(info.program, info.key, info.velocity);
    if (region == nullptr || static_cast<size_t>(region->wave_index) >= bank.bank.Waves().size())
    {
        return nullptr;
    }

    const WaveData wave = ReadWaveParam(bank, bank.bank.Waves()[region->wave_index]);
    Channel* channel = Channel::AllocChannel(*this, std::min(wave.channel_count, 2), info.priority, info.callback);
    if (channel == nullptr)
    {
        return nullptr;
    }

    // The note's volume is the velocity squared times the region's volume. Its pan is the region's pan plus the track's
    // initial pan.
    channel->SetKey(info.key);
    channel->SetOriginalKey(region->original_key);
    float init_volume = static_cast<float>(info.velocity) / 127.0f;
    init_volume *= init_volume;
    init_volume *= static_cast<float>(region->volume) / 127.0f;
    channel->SetInitVolume(init_volume);
    channel->SetTune(region->tune);
    channel->SetAttack(region->attack);
    channel->SetDecay(region->decay);
    channel->SetSustain(region->sustain);
    channel->SetRelease(region->release);
    if (build_.Since2008())
    {
        // Bank::NoteOn sets the region's hold, note-off type and key group too (Mario Kart Wii, 0x8008d41c and
        // 0x8008d490).
        channel->SetHold(region->hold);
        channel->SetIgnoreNoteOff(region->note_off_type == 1);
        channel->SetKeyGroup(region->alternate_assign);
    }

    float init_pan = static_cast<float>(region->pan - 64) / 63.0f;
    init_pan += static_cast<float>(info.init_pan) / 63.0f;
    channel->SetInitPan(init_pan);
    channel->SetInitSurroundPan(0.0f);
    channel->Start(wave, info.length, 0);

    return channel;
}

void Engine::FrameProcess()
{
    ax_voices_.FreeAllReservedAxVoice();

    // AxManager::Update (0x800315ac). The master volume and the volume for a reset are both 1. The output volume
    // therefore stays at its full 0x8000.
    ax_.SetMasterVolume(ax::kMaxVolume);

    // The players that have started, in the order they started (SoundThread's player callbacks).
    for (const std::unique_ptr<SeqPlayer>& player : players_)
    {
        player->Update();
    }

    channels_.UpdateAllChannel();
    random_.Next();
    voices_.UpdateAllVoices();
}

bool Engine::IsBusy() const
{
    if (channels_.ActiveCount() > 0)
    {
        return true;
    }

    return std::any_of(players_.begin(), players_.end(), [](const auto& player) { return player->IsActive(); });
}

Engine::Snapshot Engine::Save() const
{
    Snapshot snapshot{ax_voices_.Save(), voices_.Save(), channels_.Save(), random_, global_variables_, {},
                      banks_.size()};
    for (const std::unique_ptr<SeqPlayer>& player : players_)
    {
        snapshot.players.push_back(player->Save());
    }

    return snapshot;
}

void Engine::Restore(const Snapshot& snapshot)
{
    if (snapshot.players.size() != players_.size() || snapshot.banks != banks_.size())
    {
        throw std::runtime_error("the engine has started a sound since the snapshot");
    }

    ax_voices_.Restore(snapshot.ax_voices);
    voices_.Restore(snapshot.voices);
    channels_.Restore(snapshot.channels);
    random_ = snapshot.random;
    global_variables_ = snapshot.global_variables;
    for (std::size_t i = 0; i < players_.size(); i++)
    {
        players_[i]->Restore(snapshot.players[i]);
    }
}

const LoadedBank* Engine::GetBank(uint32_t bank_id)
{
    const auto found = banks_.find(bank_id);
    if (found != banks_.end())
    {
        return found->second.get();
    }

    if (bank_id >= archive_.Banks().size())
    {
        return nullptr;
    }

    const std::optional<rsar::FileData> file = archive_.File(archive_.Banks()[bank_id].file_id);
    if (!file)
    {
        return nullptr;
    }

    auto bank =
        std::make_unique<LoadedBank>(LoadedBank{rsar::Bank::Parse(file->file, file->wave_data), file->wave_data});
    bank->wave_address = memory_.Store(file->wave_data.data(), file->wave_data.size());

    return banks_.emplace(bank_id, std::move(bank)).first->second.get();
}

WaveData Engine::ReadWaveParam(const LoadedBank& bank, const rsar::Wave& wave) const
{
    // The loop's addresses become sample counts from the wave's start. The end is one past the last sample played.
    const ax::SampleFormat format = ToAxFormat(wave.format);
    WaveData data;
    data.format = wave.format;
    data.loop = wave.loop;
    data.channel_count = static_cast<int>(wave.channels.size());
    data.sample_rate = wave.sample_rate;
    data.loop_start = AxVoice::SampleByDspAddress(0, wave.loop_start, format);
    data.loop_end = AxVoice::SampleByDspAddress(0, wave.loop_end, format) + 1;
    for (size_t i = 0; i < std::min<size_t>(wave.channels.size(), data.channels.size()); i++)
    {
        const rsar::WaveChannel& channel = wave.channels[i];
        data.channels[i].data = bank.wave_address + static_cast<PAddr>(channel.data.data() - bank.wave_data.data());
        data.channels[i].adpcm = channel.adpcm;
    }

    return data;
}

bool ArchiveModel::Start(const ax::Microcode& microcode, const rsar::SoundArchive& archive)
{
    // AXInit boots the DSP with AX's microcode. Then NW4R SND's AxManager sets up its side.
    ax.Init();
    if (!link.Start(microcode.words, microcode.address))
    {
        return false;
    }

    engine.emplace(memory, ax, archive, build);

    return true;
}

bool ArchiveModel::RunFrame(std::vector<int16_t>& output)
{
    // __AXOutNewFrame: the PBs and the command list go to the microcode. The microcode mixes the frame into an output
    // buffer.
    const ax::Frame frame = ax.StartFrame();
    if (!link.RunFrame(frame.command_list, ax::layout::kCommandListSize))
    {
        return false;
    }

    // The microcode writes each stereo sample right first. A voice that only mixes into AX's left bus comes out in the
    // second half.
    const uint8_t* samples = memory.At(frame.output, ax::layout::kOutputBufferSize);
    for (int i = 0; i < ax::kSamplesPerFrame; i++)
    {
        output.push_back(static_cast<int16_t>(LoadBe16(samples + i * 4 + 2)));
        output.push_back(static_cast<int16_t>(LoadBe16(samples + i * 4)));
    }

    // The rest of AX's frame, then NW4R SND's sound thread.
    ax.FinishFrame();
    engine->FrameProcess();

    return true;
}

void ArchiveModel::StartWithoutDsp(const rsar::SoundArchive& archive)
{
    ax.Init();
    engine.emplace(memory, ax, archive, build);
}

void ArchiveModel::RunFrameWithoutDsp()
{
    // AX's frame. In place of the microcode, only its work on the voices' positions is done.
    ax.StartFrame();
    for (uint32_t i = 0; i < ax::kVoiceCount; i++)
    {
        uint8_t* bytes = memory.At(ax::layout::kPbs + i * static_cast<PAddr>(ax::kPbSize), ax::kPbSize);
        ax::Pb pb = ax::LoadPb(bytes);
        if (pb.state == static_cast<uint16_t>(ax::VoiceState::kRun))
        {
            AdvanceWithoutDsp(pb);
            ax::StorePb(pb, bytes);
        }
    }

    ax.FinishFrame();
    engine->FrameProcess();
}

ArchiveModel::Snapshot ArchiveModel::Save(const Snapshot* previous) const
{
    return {memory.Save(previous ? &previous->memory : nullptr), dsp, ax, engine->Save()};
}

void ArchiveModel::Restore(const Snapshot& snapshot)
{
    engine->Restore(snapshot.engine);
    memory.Restore(snapshot.memory);
    dsp = snapshot.dsp;
    ax = snapshot.ax;
}

} // namespace wsf::nw4r
