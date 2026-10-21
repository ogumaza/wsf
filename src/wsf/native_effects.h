// SPDX-License-Identifier: MIT

// Game mode's effects on WSF's models (docs/wsf.md, "Effects"). The game-mode driver makes the game's effect objects,
// and the game's AxManager appends them and runs them in each AX frame. Each object belongs to one of NW4R SND's effect
// classes. The game's code calls the class's SetParam, StartUp and Shutdown, and its UpdateBuffer in each frame.
// UpdateBuffer runs AXFX's callback. The callback takes almost all of the time that the effects cost.
//
// NativeEffects keeps a model of each object (nw4r::Effect, the model that archive mode plays). It replaces the first
// instruction of each of the classes' four functions with an instruction that the CPU doesn't run (kHookOpcode). The
// CPU stops there. The player then calls OnStop. On SetParam, StartUp and Shutdown, OnStop makes the same call on the
// model, does what the replaced instruction did, and lets the game's code go on. On UpdateBuffer, it runs the model on
// the frame and returns to the caller in place of the game's code. The game's memory then differs only in the replaced
// instructions, and in what the game's AXFX code alone reads: its delay lines and their positions.
//
// A watch on the CPU (ntdlib::ppc::Cpu::Watch) would follow the same calls without a change to the game's code. It
// checks every instruction, though, and that costs more than the models save.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "axfx/axfx.h"
#include "nw4r/effect_setup.h"
#include "nw4r/effects.h"

namespace wsf
{
namespace wii
{

class System;

} // namespace wii

class NativeEffects
{
public:
    // The models' state, for a snapshot of the player.
    struct Snapshot
    {
        std::map<uint32_t, nw4r::Effect> objects;
    };

    // The instruction that replaces the first of each function: primary opcode 1, with the number of the function in
    // the low bits. Broadway has no instruction with that opcode.
    static constexpr uint32_t kHookOpcode = 0x04000000;

    // Reads the effects from the block of NW4R SND's driver in `system`'s memory (DriverBlock), and replaces the first
    // instruction of each of their classes' functions. Returns nullptr, and changes nothing, if the memory holds no
    // such block, a block without effects, or a function that doesn't start as the classes' functions do.
    static std::unique_ptr<NativeEffects> Attach(wii::System& system);

    NativeEffects(const NativeEffects&) = delete;
    NativeEffects& operator=(const NativeEffects&) = delete;

    // Puts the replaced instructions back.
    ~NativeEffects();

    // Called when the CPU has stopped at an instruction that it doesn't run. Returns true if the instruction is one of
    // the replacements, after the call that it stands for. The CPU can then go on. Returns false otherwise.
    bool OnStop();

    std::shared_ptr<const Snapshot> Save() const;
    void Restore(const Snapshot& snapshot);

    // The frames that the models have run in place of the game's code.
    uint64_t Frames() const
    {
        return frames_;
    }

private:
    // An effect class: its type, and the number of words in its parameters.
    struct Class
    {
        nw4r::EffectType type;
        uint32_t words;
    };

    // The four functions of a class.
    enum class Function
    {
        kSetParam,
        kStartUp,
        kShutdown,
        kUpdateBuffer,
    };

    // A replaced instruction: the function's address, the class, and the instruction.
    struct Hook
    {
        uint32_t address;
        std::size_t class_index;
        Function function;
        uint32_t instruction;
    };

    NativeEffects(wii::System& system, axfx::Build build, std::vector<Class> classes, std::vector<Hook> hooks);

    // Does what the replaced first instruction of `hook`'s function does, and goes on at the second.
    bool RunFirstInstruction(const Hook& hook);

    // The model of the game's object at `object`, made on its first call.
    nw4r::Effect& Object(const Class& c, uint32_t object);

    // UpdateBuffer(int channels, void** buffers, ...): runs the model on the frame in the buffers. Returns false, and
    // leaves the frame as it is, if the frame isn't three channels in memory that the CPU can read and write.
    bool UpdateBuffer(nw4r::Effect& effect);

    wii::System& system_;
    axfx::Build build_;
    std::vector<Class> classes_;
    std::vector<Hook> hooks_;
    std::map<uint32_t, nw4r::Effect> objects_; // by the address of the game's object
    uint64_t frames_ = 0;
};

} // namespace wsf
