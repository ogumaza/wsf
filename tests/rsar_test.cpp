// SPDX-License-Identifier: MIT

// RSAR reader tests on small files built here. The files are a version 1.1 bank with a WAVE block and a version 1.2
// bank with an RWAR, with instruments that exercise every way the bank reader chooses a region. Many instruments share
// references. The version 1.1 bank is also tested marked as version 1.0. The game ignores the regions' volumes in such
// a bank. There's also a sequence with labels, and an archive that holds them in a group, whole and truncated. Archives
// of the 3DS and later consoles, and little-endian files, must be rejected with a message.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "check.h"
#include "common/bytes.h"
#include "rsar/binary.h"
#include "rsar/formats.h"
#include "rsar_files.h"

namespace
{

using namespace wsf;
using namespace wsf::test;

// The original key of the region a note plays, or -1 for none.
int KeyOf(const rsar::Bank& bank, int program, int key, int velocity)
{
    const rsar::Region* region = bank.Find(program, key, velocity);

    return region ? region->original_key : -1;
}

void CheckInstruments(const std::string& name, const rsar::Bank& bank)
{
    Check(bank.InstrumentCount() == 70, name + ": instruments");
    Check(KeyOf(bank, 0, 10, 10) == 60 && KeyOf(bank, 0, 127, 127) == 60, name + ": a direct region");
    Check(KeyOf(bank, 1, 0, 64) == 40 && KeyOf(bank, 1, 63, 64) == 40 && KeyOf(bank, 1, 64, 64) == 80,
          name + ": a range table by key");
    Check(KeyOf(bank, 2, 59, 64) == -1 && KeyOf(bank, 2, 63, 64) == -1, name + ": an index table's bounds");
    Check(bank.Find(2, 61, 63)->wave_index == 0 && bank.Find(2, 61, 64)->wave_index == 1,
          name + ": an index table, then a range table by velocity");
    Check(!bank.Find(3, 60, 64) && !bank.Find(4, 60, 64) && !bank.Find(5, 60, 64),
          name + ": nothing, an unknown type and a negative wave index play nothing");
    Check(KeyOf(bank, 69, 64, 64) == 80 && !bank.Find(70, 60, 64) && !bank.Find(-1, 60, 64),
          name + ": shared references, and programs out of range");
}

void CheckWaves(const std::string& name, const rsar::Bank& bank, uint8_t first)
{
    Check(bank.Waves().size() == 2, name + ": the number of waves");
    for (size_t i = 0; i < bank.Waves().size() && i < 2; ++i)
    {
        const rsar::Wave& wave = bank.Waves()[i];
        Check(wave.format == rsar::WaveFormat::kAdpcm && wave.loop && wave.sample_rate == 32000 &&
                  wave.loop_start == 2 && wave.loop_end == 30 && wave.channels.size() == 1,
              name + ": wave " + std::to_string(i) + "'s info");
        if (wave.channels.empty())
        {
            continue;
        }

        const rsar::WaveChannel& channel = wave.channels[0];
        Check(channel.adpcm.pred_scale == first + i && channel.data.size() >= 0x40 && channel.data[0] == first + i,
              name + ": wave " + std::to_string(i) + "'s samples and predictor/scale (" +
                  std::to_string(channel.adpcm.pred_scale) + ", " + std::to_string(channel.data.size()) +
                  " bytes, first " + std::to_string(channel.data.empty() ? -1 : channel.data[0]) + ")");
        Check(channel.adpcm.coefs[0] == 0x400 && channel.volumes[0] == 0x01000000,
              name + ": wave " + std::to_string(i) + "'s coefficients and volume (" +
                  std::to_string(channel.adpcm.coefs[0]) + ", " + std::to_string(channel.volumes[0]) + ")");
    }
}

// Checks that loading `bytes` as an archive fails with a message containing `words`.
void CheckRefused(const std::string& name, const Bytes& bytes, const std::string& words)
{
    try
    {
        rsar::SoundArchive::Load(bytes);
        Check(false, name + ": loaded");
    }
    catch (const rsar::FormatError& e)
    {
        Check(std::string(e.what()).find(words) != std::string::npos, name + ": the message \"" + e.what() + "\"");
    }
}

} // namespace

int main()
{
    try
    {
        // The banks' waves point into the bytes they're read from. Those bytes must outlive the banks.
        const Bytes old_file = MakeOldBank();
        const Bytes old_waves = MakeOldWaveData();
        const rsar::Bank old_bank = rsar::Bank::Parse(old_file, old_waves);
        CheckInstruments("version 1.1 bank", old_bank);
        CheckWaves("version 1.1 bank", old_bank, 0x21);

        // A bank before version 1.1 plays its regions at full volume, whatever volume they hold (100 here).
        Bytes first_file = MakeOldBank();
        StoreBe16(&first_file[6], 0x0100);
        const rsar::Bank first_bank = rsar::Bank::Parse(first_file, old_waves);
        const rsar::Region* first_region = first_bank.Find(0, 60, 64);
        const rsar::Region* old_region = old_bank.Find(0, 60, 64);

        Check(first_region != nullptr && first_region->volume == 127 && first_region->tune == 1.0f,
              "a version 1.0 bank's regions take full volume");
        Check(old_region != nullptr && old_region->volume == 100, "a version 1.1 bank's regions take their volume");

        const Bytes new_file = MakeNewBank();
        const Bytes rwar = MakeRwar();
        const rsar::Bank new_bank = rsar::Bank::Parse(new_file, rwar);
        CheckInstruments("version 1.2 bank", new_bank);
        CheckWaves("version 1.2 bank", new_bank, 0x31);

        Bytes commands(16);
        for (size_t i = 0; i < commands.size(); ++i)
        {
            commands[i] = static_cast<uint8_t>(0x2C + i);
        }

        const Bytes sequence_bytes = MakeSequence(commands);
        const rsar::Sequence sequence = rsar::Sequence::Parse(sequence_bytes);
        Check(sequence.data.size() == 16 && sequence.labels.size() == 2 && sequence.labels[1].name == "SEQ_BB" &&
                  sequence.labels[1].offset == 8,
              "a sequence's commands and labels");

        const Bytes bytes = MakeArchive(sequence_bytes, 8, 0);
        const rsar::SoundArchive archive = rsar::SoundArchive::Load(bytes);
        Check(archive.Version() == 0x0104 && archive.Sounds().size() == 1 && archive.Banks().size() == 1 &&
                  !archive.Truncated(),
              "an archive's tables");
        const rsar::SoundInfo& sound = archive.Sounds()[0];
        Check(sound.name == "SEQ_TEST" && sound.type == rsar::SoundType::kSequence && sound.volume == 90 &&
                  sound.pan_mode == 0 && sound.pan_curve == 3 && sound.data_offset == 8 &&
                  sound.channel_priority == 70 && sound.release_priority_fix && sound.bank_id == 0,
              "a sequence sound's info");
        Check(archive.Banks()[0].name == "BANK_TEST" && archive.FindSound("SEQ_TEST") == 0u &&
                  !archive.FindSound("SEQ_OTHER"),
              "names");
        const auto sequence_file = archive.File(0);
        const auto bank_file = archive.File(1);
        Check(sequence_file && rsar::Sequence::Parse(sequence_file->file).labels.size() == 2 &&
                  sequence_file->wave_data.empty(),
              "the sequence from the group");
        Check(bank_file && rsar::Bank::Parse(bank_file->file, bank_file->wave_data).Waves().size() == 2,
              "the bank and its RWAR from the group");

        // Cut short in the bank's wave data. The sequence is still there. The bank isn't.
        const Bytes cut(bytes.begin(), bytes.end() - 100);
        const rsar::SoundArchive truncated = rsar::SoundArchive::Load(cut);
        Check(truncated.Truncated() && truncated.File(0) && !truncated.File(1), "a truncated archive");

        const Bytes table_bytes =
            MakeSoundTable({{"SEQ_FIELD", 1, 1}, {"STRM_TITLE", 2, 0}}, {"PLAYER_BGM", "PLAYER_SE"});
        const rsar::SoundArchive table = rsar::SoundArchive::Load(table_bytes);
        Check(table.Players().size() == 2 && table.Players()[0].name == "PLAYER_BGM" &&
                  table.Players()[1].name == "PLAYER_SE" && table.Players()[1].sound_count == 2,
              "an archive's players");
        Check(table.Sounds().size() == 2 && table.Sounds()[0].name == "SEQ_FIELD" && table.Sounds()[0].player_id == 1 &&
                  table.Sounds()[1].type == rsar::SoundType::kStream,
              "the sounds' players");

        Bytes ctr = bytes;
        std::memcpy(ctr.data(), "CSAR", 4);
        CheckRefused("a 3DS archive", ctr, "3DS");
        std::memcpy(ctr.data(), "FSAR", 4);
        CheckRefused("a Wii U or Switch archive", ctr, "Wii U or Switch");
        Bytes little = bytes;
        StoreBe16(&little[4], 0xFFFE);
        CheckRefused("a little-endian archive", little, "big-endian");
        CheckRefused("something else", Bytes(64, 'x'), "RSAR");
    }
    catch (const std::exception& e)
    {
        Check(false, std::string("unexpected error: ") + e.what());
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
