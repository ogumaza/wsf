// SPDX-License-Identifier: MIT

// The CPU side of AX, the audio library in the RVL SDK. The model follows the build that Wii Sports links (December
// 2006). The model covers the 96 voices and their priorities, the copy of the voices' parameter blocks into main memory
// each frame, the depop block and the command list that controls what the microcode mixes. Archive mode runs this model
// instead of the game's code. The model mixes in stereo. With no aux effects, its command list never mixes the aux
// buses.
//
// The model also follows the build of October 2006 (Build::kOctober2006). WarioWare: Smooth Moves links it. That build
// has 64 voices and a PB of another layout (see LoadPbOctober2006). The build counts other DSP cycles for each voice
// and numbers the commands otherwise. Comments give the addresses of the build's functions with "October 2006".

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <utility>

#include "ax/pb.h"
#include "ax/voice.h"
#include "common/common_types.h"
#include "dsp/main_memory.h"

namespace wsf::ax
{

// AX's buffers in main memory, at the physical addresses that Wii Sports uses (in the .bss and .data of its main.dol).
namespace layout
{

constexpr PAddr kCompressorTable = 0x003ab880; // 0xfc0 bytes
constexpr PAddr kCommandLists = 0x0042e280;    // two lists of 0x80 bytes
constexpr PAddr kRemoteBuffers = 0x0042e3c0;   // 4 rings of 180 samples
constexpr PAddr kSurroundBuffer = 0x0042e960;  // 0x300 bytes
constexpr PAddr kOutputBuffers = 0x0042ec60;   // 3 frames of 96 stereo samples
constexpr PAddr kStudio = 0x0042f180;          // 0x78 bytes
constexpr PAddr kPbs = 0x0042f200;             // 96 PBs
constexpr PAddr kItdBuffers = 0x00436a00;      // 96 buffers of 0x40 bytes
constexpr PAddr kUpdateBuffers = 0x00438200;   // October 2006: 64 buffers of 0x100 bytes, after the ITD buffers

constexpr uint32_t kCompressorTableSize = 0xfc0;
constexpr uint32_t kCommandListSize = 0x80;
constexpr uint32_t kRemoteBufferSamples = kRemoteSamplesPerFrame * 10;
constexpr uint32_t kOutputBufferSize = kSamplesPerFrame * 4;
constexpr uint32_t kItdBufferSize = 0x40;
constexpr uint32_t kUpdateBufferSize = 0x100;

} // namespace layout

// The input for one DSP frame: the command list to mail to the DSP, and the buffer that the list's OUTPUT command fills
// with 96 stereo samples (16-bit, big-endian, right then left).
struct Frame
{
    uint32_t command_list = 0; // the cached address that AX mails
    PAddr output = 0;
};

// A copy of a System is a snapshot of AX's state. Assigning the copy back to the original System restores that state.
// The voice links and callbacks in the copy still point at the voices of the original System and at their owners.
class System
{
public:
    explicit System(MainMemory& memory);

    // AXInit (main.dol 0x80104450), without booting the DSP. Sets up the voices, the depop block and the command list
    // state. Writes the compressor table to the address where the microcode reads it. Builds the first command list
    // (__AXOutInit, 0x80106068). The model then follows `build`.
    void Init(Build build = Build::kDecember2006);

    // The number of voices: 96, or 64 in the build of October 2006.
    uint32_t VoiceCount() const
    {
        return voice_count_;
    }

    // The PB of voice `index` in main memory, in the build's layout, and its replacement.
    Pb ReadPb(uint32_t index) const;
    void WritePb(uint32_t index, const Pb& pb);

    // AXAcquireVoice (0x80104740). Returns a free voice at `priority`. If no voice is free, steals the oldest voice of
    // the lowest priority below `priority` and runs that voice's callback first. Returns nullptr if every voice has
    // `priority` or higher.
    Voice* AcquireVoice(uint32_t priority, Voice::Callback callback);

    // AXFreeVoice (0x801046c4).
    void FreeVoice(Voice* voice);

    // AXSetVoicePriority (0x801048c0): moves the voice to the newest end of the stack for its new priority.
    void SetVoicePriority(Voice* voice, uint32_t priority);

    // AXSetMasterVolume (0x80105bd0): sets the volume of the OUTPUT command. The next command list built uses it.
    void SetMasterVolume(uint16_t volume);

    // AXRegisterCallback (0x80106260): registers a function that FinishFrame calls every frame before building the next
    // command list.
    void RegisterCallback(std::function<void()> callback)
    {
        frame_callback_ = std::move(callback);
    }

    // The first part of __AXOutNewFrame (0x80105c00). AX runs __AXOutNewFrame every frame from the audio interface
    // interrupt. This part copies the voices' changes into their PBs in main memory and drops voices that would push
    // the frame over its DSP cycle budget. It also updates the depop block and returns the command list built in the
    // previous frame.
    Frame StartFrame();

    // The rest of __AXOutNewFrame. It runs once the DSP has received the command list. Runs the callbacks of the voices
    // that StartFrame dropped, runs the frame callback and builds the next command list.
    void FinishFrame();

    // AX's estimate of the DSP cycles used by the last frame (__AXRecDspCycles).
    uint32_t DspCycles() const
    {
        return recorded_dsp_cycles_;
    }

private:
    // A depop accumulator and its entry in the depop block (AXSTUDIO). There is one for each of the 20 buses and
    // channels.
    struct Depop
    {
        int32_t sum = 0;   // __AXSpbAL and so on
        int32_t value = 0; // AXSTUDIO.L and so on
        int16_t delta = 0; // AXSTUDIO.dL and so on
    };

    void PushFreeStack(Voice* voice);
    Voice* PopFreeStack();
    void PushCallbackStack(Voice* voice);
    Voice* PopCallbackStack();
    void RemoveFromStack(Voice* voice);
    void PushStackHead(Voice* voice, uint32_t priority);
    Voice* PopStackFromBottom(uint32_t priority);

    PAddr PbAddress(uint32_t index) const;
    void SetPbDefault(Voice* voice);
    void SyncPbs(uint32_t base_cycles);
    uint32_t RunningCycles(const Pb& pb) const;
    void ServiceVpb(Voice* voice);
    void DumpVpb(Voice* voice);
    void DepopVoice(const Pb& pb);
    void PrintStudio();
    void ServiceCallbackStack();
    uint32_t CommandListAddress();
    void WriteCommand(uint16_t word);
    void WriteCommand32(uint32_t value);
    void NextFrame(PAddr surround, PAddr output, const std::array<PAddr, kRemoteCount>& remote);
    void NextFrameOctober2006(PAddr surround, PAddr output, const std::array<PAddr, kRemoteCount>& remote);

    MainMemory* memory_;
    Build build_ = Build::kDecember2006;
    uint32_t voice_count_ = kVoiceCount;
    std::function<void()> frame_callback_;
    std::array<Voice, kVoiceCount> voices_;

    // The microcode's copies of the PBs. SyncPbs reads them back from main memory before writing the PBs for the next
    // frame.
    std::array<Pb, kVoiceCount> dsp_pbs_;

    // The priority stacks: newest voice at the head, oldest at the tail. The free stack (priority 0) only uses its
    // head.
    std::array<Voice*, kPriorityMax + 1> stack_head_{};
    std::array<Voice*, kPriorityMax + 1> stack_tail_{};
    Voice* callback_stack_ = nullptr;

    uint32_t max_dsp_cycles_ = 0;
    uint32_t recorded_dsp_cycles_ = 0;
    std::array<Depop, 20> depop_{};

    // The command lists: AX writes one while the DSP reads the other.
    uint32_t command_list_position_ = 0;
    PAddr command_list_write_ = 0;
    uint32_t command_list_cycles_ = 0;
    std::array<PAddr, 2> list_output_{}; // the output buffer named in each list
    uint16_t master_volume_ = kMaxVolume;

    uint32_t output_frame_ = 0;
    int32_t remote_position_ = 0; // the microcode's write position in the Wii Remote rings
};

} // namespace wsf::ax
