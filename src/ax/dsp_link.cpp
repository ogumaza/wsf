// SPDX-License-Identifier: MIT

#include "ax/dsp_link.h"

#include <cstdint>
#include <span>

#include "dsp/dsp.h"

namespace wsf::ax
{
namespace
{

// The mails between AX and its microcode (DSPUCode in the SDK's DSP library).
constexpr uint32_t kDspInit = 0xdcd10000;
constexpr uint32_t kDspYield = 0xdcd10002;
constexpr uint32_t kDspRequest = 0xdcd10004;
constexpr uint32_t kMailContinue = 0xcdd10003;
constexpr uint32_t kCommandListMail = 0xbabe0000;

// The most DSP steps to wait for a mail. A frame takes about 30,000 at most.
constexpr uint64_t kStepLimit = 20'000'000;

// The DSP runs in batches. The mailboxes are checked between batches. Steps that run after a mail only spin in the
// microcode loop that waits for the next mail. They change no state.
constexpr uint64_t kBatchSteps = 256;

} // namespace

bool DspLink::Start(std::span<const uint16_t> microcode, uint32_t address)
{
    dsp_->StartMicrocode(microcode, address, 0x0010);
    uint32_t mail = 0;

    return WaitForMail(mail) && mail == kDspInit;
}

bool DspLink::RunFrame(uint32_t command_list, uint32_t size)
{
    if (!SendMail(kCommandListMail | size) || !SendMail(command_list))
    {
        return false;
    }

    // The DSP library passes a request to AX's request callback (__AXDSPRequestCallback, 0x80105f74). That callback
    // does nothing.
    uint32_t mail = 0;
    do
    {
        if (!WaitForMail(mail))
        {
            return false;
        }
    } while (mail == kDspRequest);

    return mail == kDspYield && SendMail(kMailContinue);
}

bool DspLink::SendMail(uint32_t mail)
{
    dsp_->CpuWriteMailboxHigh(static_cast<uint16_t>(mail >> 16));
    dsp_->CpuWriteMailboxLow(static_cast<uint16_t>(mail));
    for (uint64_t steps = 0; dsp_->MailboxToDsp() & 0x80000000; steps += kBatchSteps)
    {
        if (steps >= kStepLimit || dsp_->Halted())
        {
            return false;
        }

        dsp_->Run(kBatchSteps);
    }

    return true;
}

bool DspLink::WaitForMail(uint32_t& mail)
{
    for (uint64_t steps = 0; !(dsp_->MailboxToCpu() & 0x80000000); steps += kBatchSteps)
    {
        if (steps >= kStepLimit || dsp_->Halted())
        {
            return false;
        }

        dsp_->Run(kBatchSteps);
    }

    // This code takes the place of the DSP library's interrupt handler. The microcode raises that interrupt when it
    // sends a mail. The handler reads the high half of the mailbox before the low half. Reading the low half empties
    // the mailbox.
    const uint16_t high = dsp_->CpuReadMailboxHigh(false);
    const uint16_t low = dsp_->CpuReadMailboxLow(false);
    mail = static_cast<uint32_t>(high) << 16 | low;
    dsp_->TakeInterruptRequest();

    return true;
}

} // namespace wsf::ax
