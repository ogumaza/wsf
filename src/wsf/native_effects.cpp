// SPDX-License-Identifier: MIT

// Game mode's effects on WSF's models (see native_effects.h).

#include "wsf/native_effects.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "ntdlib/ppc/cpu.h"
#include "xsflib/bytes.h"

#include "wii/system.h"
#include "wsf/format.h"

namespace wsf
{
namespace
{

// The first instruction of each of the classes' functions: stwu r1, d(r1). It makes the function's stack frame. The
// displacement d is in the low 16 bits.
constexpr uint32_t kStackFrameMask = 0xffff0000;
constexpr uint32_t kStackFrame = 0x94210000;

// Reads the big-endian word of the game's memory at `address`. Returns false where the CPU can't read.
bool ReadWord(ntdlib::ppc::Cpu& cpu, uint32_t address, uint32_t& value)
{
    uint8_t bytes[4];
    if (!cpu.ReadVirtual(address, bytes, sizeof(bytes)))
    {
        return false;
    }

    value = xsflib::LoadBe32(bytes);

    return true;
}

// Writes the big-endian word `value` to the game's memory at `address`. Returns false where the CPU can't write.
bool WriteWord(ntdlib::ppc::Cpu& cpu, uint32_t address, uint32_t value)
{
    uint8_t bytes[4];
    xsflib::StoreBe32(bytes, value);

    return cpu.WriteVirtual(address, bytes, sizeof(bytes));
}

} // namespace

std::unique_ptr<NativeEffects> NativeEffects::Attach(wii::System& system)
{
    ntdlib::ppc::Cpu& cpu = system.Cpu();
    const auto word = [&cpu](uint32_t offset, uint32_t& value)
    {
        return ReadWord(cpu, DriverBlock::kAddress + offset, value);
    };

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t count = 0;
    uint32_t build = 0;
    if (!word(0, magic) || magic != DriverBlock::kMagic || !word(DriverBlock::kVersionOffset, version) ||
        version != DriverBlock::kVersion || !word(DriverBlock::kEffectCountOffset, count) || count == 0 ||
        count > DriverBlock::kMaxEffects || !word(DriverBlock::kAxfxBuildOffset, build) ||
        build > static_cast<uint32_t>(axfx::Build::kSeptember2008))
    {
        return nullptr;
    }

    std::vector<Class> classes;
    std::vector<Hook> hooks;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t effect = DriverBlock::kEffectsOffset + i * DriverBlock::kEffectSize;
        const uint32_t native = DriverBlock::kNativeOffset + i * DriverBlock::kNativeSize;
        Class c{};
        uint32_t type = 0;
        std::array<uint32_t, 4> functions{};
        if (!word(effect + DriverBlock::kEffectSetParamOffset, functions[0]) ||
            !word(effect + DriverBlock::kEffectWordsOffset, c.words) || !word(native, type) ||
            !word(native + 4, functions[1]) || !word(native + 8, functions[2]) || !word(native + 12, functions[3]))
        {
            return nullptr;
        }

        // A class that the models don't know, or a block from a ripper that didn't fill these words in, leaves the
        // effects to the game's code.
        c.type = static_cast<nw4r::EffectType>(type);
        const int words = nw4r::EffectParamWords(c.type);
        if (words == 0 || c.words != static_cast<uint32_t>(words))
        {
            return nullptr;
        }

        // Two effects of one class share its functions.
        bool known = false;
        for (const Class& other : classes)
        {
            known = known || other.type == c.type;
        }

        if (known)
        {
            continue;
        }

        classes.push_back(c);
        for (std::size_t f = 0; f < functions.size(); f++)
        {
            Hook hook{functions[f], classes.size() - 1, static_cast<Function>(f), 0};
            if (hook.address == 0 || !ReadWord(cpu, hook.address, hook.instruction) ||
                (hook.instruction & kStackFrameMask) != kStackFrame)
            {
                return nullptr;
            }

            for (const Hook& other : hooks)
            {
                if (other.address == hook.address)
                {
                    return nullptr;
                }
            }

            hooks.push_back(hook);
        }
    }

    for (std::size_t i = 0; i < hooks.size(); i++)
    {
        WriteWord(cpu, hooks[i].address, kHookOpcode | static_cast<uint32_t>(i));
    }

    return std::unique_ptr<NativeEffects>(
        new NativeEffects(system, static_cast<axfx::Build>(build), std::move(classes), std::move(hooks)));
}

NativeEffects::NativeEffects(wii::System& system, axfx::Build build, std::vector<Class> classes,
                             std::vector<Hook> hooks)
    : system_(system), build_(build), classes_(std::move(classes)), hooks_(std::move(hooks))
{
}

NativeEffects::~NativeEffects()
{
    for (const Hook& hook : hooks_)
    {
        WriteWord(system_.Cpu(), hook.address, hook.instruction);
    }
}

std::shared_ptr<const NativeEffects::Snapshot> NativeEffects::Save() const
{
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->objects = objects_;

    return snapshot;
}

void NativeEffects::Restore(const Snapshot& snapshot)
{
    objects_ = snapshot.objects;
}

nw4r::Effect& NativeEffects::Object(const Class& c, uint32_t object)
{
    return objects_.try_emplace(object, c.type, build_).first->second;
}

bool NativeEffects::OnStop()
{
    ntdlib::ppc::Cpu& cpu = system_.Cpu();
    ntdlib::ppc::State& state = cpu.state_;
    const uint32_t index = cpu.stop_opcode_ & ~kHookOpcode;
    if ((cpu.stop_opcode_ >> 26) != (kHookOpcode >> 26) || index >= hooks_.size() ||
        hooks_[index].address != cpu.stop_pc_ || state.pc != cpu.stop_pc_)
    {
        return false;
    }

    const Hook& hook = hooks_[index];
    const Class& c = classes_[hook.class_index];
    const uint32_t object = state.gpr[3];
    switch (hook.function)
    {
    case Function::kSetParam:
        {
            // SetParam(const Param&): the parameters' words.
            nw4r::EffectParams params{};
            for (uint32_t w = 0; w < c.words; w++)
            {
                if (!ReadWord(cpu, state.gpr[4] + 4 * w, params[w]))
                {
                    return false;
                }
            }

            Object(c, object).SetParam(params);
            break;
        }

    case Function::kStartUp:
        Object(c, object).StartUp();
        break;

    case Function::kShutdown:
        Object(c, object).Shutdown();
        break;

    case Function::kUpdateBuffer:
        // The model's frame replaces the game's code: execution goes on at the return address.
        if (UpdateBuffer(Object(c, object)))
        {
            state.pc = state.lr;
            frames_++;

            return true;
        }
        break;
    }

    return RunFirstInstruction(hook);
}

bool NativeEffects::RunFirstInstruction(const Hook& hook)
{
    // stwu r1, d(r1): stores r1 at r1 + d, and moves r1 there.
    ntdlib::ppc::Cpu& cpu = system_.Cpu();
    ntdlib::ppc::State& state = cpu.state_;
    const uint32_t frame = state.gpr[1] + static_cast<uint32_t>(static_cast<int16_t>(hook.instruction & 0xffff));
    if (!WriteWord(cpu, frame, state.gpr[1]))
    {
        return false;
    }

    state.gpr[1] = frame;
    state.pc = hook.address + 4;

    return true;
}

bool NativeEffects::UpdateBuffer(nw4r::Effect& effect)
{
    ntdlib::ppc::Cpu& cpu = system_.Cpu();
    const ntdlib::ppc::State& state = cpu.state_;
    if (state.gpr[4] != axfx::kChannelCount)
    {
        return false;
    }

    // The buffers' addresses, then each channel's samples: big-endian 32-bit integers.
    std::array<uint32_t, axfx::kChannelCount> buffers{};
    std::array<uint8_t, 4 * axfx::kSamplesPerFrame> bytes{};
    axfx::AuxFrame frame;
    for (std::size_t channel = 0; channel < buffers.size(); channel++)
    {
        if (!ReadWord(cpu, state.gpr[5] + static_cast<uint32_t>(4 * channel), buffers[channel]) ||
            !cpu.ReadVirtual(buffers[channel], bytes.data(), bytes.size()))
        {
            return false;
        }

        for (std::size_t i = 0; i < axfx::kSamplesPerFrame; i++)
        {
            frame.channels[channel][i] = static_cast<int32_t>(xsflib::LoadBe32(&bytes[4 * i]));
        }
    }

    effect.UpdateBuffer(frame);
    for (std::size_t channel = 0; channel < buffers.size(); channel++)
    {
        for (std::size_t i = 0; i < axfx::kSamplesPerFrame; i++)
        {
            xsflib::StoreBe32(&bytes[4 * i], static_cast<uint32_t>(frame.channels[channel][i]));
        }

        cpu.WriteVirtual(buffers[channel], bytes.data(), bytes.size());
    }

    return true;
}

} // namespace wsf
