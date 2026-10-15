// SPDX-License-Identifier: MIT

// Tests the ripper's choice of music (rip::IsMusic) on an archive of sounds and players that
// ntdlib/testing/rsar_files.h builds. Also tests the length analysis on sequences in which one track plays while
// another polls or counts a variable or marks beats for the game, and on a sequence in an arrangement. TestLibraryNames
// checks the names of libraries in a folder that holds earlier rips (rip::LibraryName). TestJac reads a Jac game's
// bank file and sequence index, made up in the formats of Pikmin's files. It also checks a piece that plays in parts.
// TestPikmin2Sounds lists Pikmin 2's sounds from files made up in the game's formats.

#include "rip/rip.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/jaudio_files.h"
#include "ntdlib/testing/rsar_files.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"
#include "xsflib/testing/check.h"

#include "nw4r/arrangement.h"
#include "nw4r/rsar_options.h"
#include "nw4r/util.h"

using xsflib::testing::Check;

using namespace wsf;
using ntdlib::testing::rsar_files::Bytes;
using ntdlib::testing::rsar_files::MakeArchive;
using ntdlib::testing::rsar_files::MakeSequence;
using ntdlib::testing::rsar_files::MakeSoundTable;
using ntdlib::testing::rsar_files::TableSound;

namespace fs = std::filesystem;

namespace
{

// The length analysis of a sequence of `commands` in `arrangement`, with the tracks in the mask `tracks`, in an archive
// that ntdlib/testing/rsar_files.h builds. The archive's waves loop unless `loop` is false, and play at `sample_rate`
// Hz. The model can't drive an empty microcode. The analyzer then runs the model alone. A frame then has sound while
// any note plays.
std::optional<rip::Timing> Measure(const Bytes& commands, uint32_t tracks, bool loop = true,
                                   uint16_t sample_rate = 32000, const nw4r::Arrangement& arrangement = {})
{
    const rip::LengthAnalyzer analyzer(MakeArchive(MakeSequence(commands), 0, tracks, loop, sample_rate),
                                       ntdlib::Microcode{}, nw4r::SndBuild{20070608}, arrangement);
    return analyzer.Analyze(0);
}

// Checks that a sequence sounds and that its length and fade are as given.
void CheckTiming(const std::string& name, const std::optional<rip::Timing>& timing, long long min_ms, long long max_ms,
                 long long fade_ms, bool silent = false)
{
    const std::string got = timing ? std::to_string(timing->length_ms) + " ms, fade " +
                                         std::to_string(timing->fade_ms) + " ms" + (timing->silent ? ", silent" : "")
                                   : "none";
    Check(timing && timing->length_ms >= min_ms && timing->length_ms <= max_ms && timing->fade_ms == fade_ms &&
              timing->silent == silent,
          name + " (" + got + ")");
}

void TestLengths()
{
    // Each track that plays sets its notes' envelope first (D1 7F D2 7F D3 7F, 6 bytes): a full sustain at once and the
    // fastest release. The test bank's regions leave both slow. The tempo is 96 ticks a second.
    //
    // In the first three sequences, track 0 opens track 1 at 22 and then polls variable 0 once a tick. It compares the
    // variable with 1 (F0 90), would jump to its end at 21 if they were equal (A2 89), waits a tick and jumps back to
    // 5. Variable 0 stays at its default of -1.
    const Bytes kPoll = {0x88, 1,    0x00, 0x00, 22,   0xF0, 0x90, 0x00, 0x00, 0x01, 0xA2,
                         0x89, 0x00, 0x00, 21,   0x80, 1,    0x89, 0x00, 0x00, 5,    0xFF};
    const auto with_poll = [&kPoll](const Bytes& track1)
    {
        Bytes commands = kPoll;
        commands.insert(commands.end(), {0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F});
        commands.insert(commands.end(), track1.begin(), track1.end());

        return commands;
    };

    // After the envelope, track 1's commands start at 28. First, track 1 plays a note of 24 ticks over and over. Next,
    // it rests for 48 ticks and then ties a note of a tick on program 1 to itself over and over (from 34). The note
    // sounds on, and nothing changes. Last, it plays a note of 12 ticks, rests for 48 ticks, plays the note again and
    // ends.
    const auto music = Measure(with_poll({60, 127, 24, 0x89, 0x00, 0x00, 28, 0xFF}), 3);
    const auto hold = Measure(with_poll({0x80, 48, 0x81, 1, 0xC8, 1, 60, 127, 1, 0x89, 0x00, 0x00, 34, 0xFF}), 3);
    const auto end = Measure(with_poll({60, 127, 12, 0x80, 48, 60, 127, 12, 0xFF}), 3);

    // Track 0 sets the track's volume to 100 at the start of each pass (at 5) and otherwise polls as above. Only its
    // first pass changes the volume. Track 1, at 24, plays a note of 24 ticks over and over (from 30).
    const auto move = Measure({0x88, 1,    0x00, 0x00, 24,   0xC1, 100,  0xF0, 0x90, 0x00, 0x00, 0x01, 0xA2,
                               0x89, 0x00, 0x00, 23,   0x80, 1,    0x89, 0x00, 0x00, 5,    0xFF, 0xD1, 0x7F,
                               0xD2, 0x7F, 0xD3, 0x7F, 60,   127,  24,   0x89, 0x00, 0x00, 30,   0xFF},
                              3);

    // Track 0 sets variable 1 to 1 and then to 2, 24 ticks apart, over and over. These are beats for the game. No track
    // reads them. Track 1, at 24, holds a note on program 1.
    const auto beats = Measure(
        {0x88, 1,    0x00, 0x00, 24,   0xF0, 0x80, 0x01, 0x00, 0x01, 0x80, 24,   0xF0, 0x80, 0x01, 0x00, 0x02, 0x80, 24,
         0x89, 0x00, 0x00, 5,    0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0x81, 1,    60,   127,  0xCB, 0x00, 0xFF},
        3);

    // Track 0 counts in variable 2, once a tick, from -1 to 30 (F0 81 adds, F0 91 compares). Then track 0 jumps to 26
    // and holds a note on program 1, two semitones up. Track 1, at 39, holds a note on program 1 from the start.
    const auto count = Measure(
        {0x88, 1,    0x00, 0x00, 39,   0xF0, 0x81, 0x02, 0x00, 0x01, 0xF0, 0x91, 0x02, 0x00, 30,   0xA2, 0x89, 0x00,
         0x00, 26,   0x80, 1,    0x89, 0x00, 0x00, 5,    0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0x81, 1,    62,   127,
         0xCB, 0x00, 0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0x81, 1,    60,   127,  0xCB, 0x00, 0xFF},
        3);

    // Track 0 only polls, and its passes don't count. The sequence loops with track 1: two loops of 24 ticks.
    CheckTiming("track 1's loops count while track 0 polls", music, 490, 510, 10000);

    // The tied note starts at 500 ms. Nothing changes after that, and its wave loops twice within a few frames.
    CheckTiming("a still poll and a tied note settle into a loop", hold, 505, 540, 10000);

    // The second note ends at 750 ms, and 500 ms of tail follow. Track 0's polls during the rest aren't loops.
    CheckTiming("a sequence ends while track 0 polls", end, 1240, 1265, 0);

    // Track 0's first pass changes its volume, and counts as a loop. Then track 1's loops take over, and the count
    // starts again: two loops of 24 ticks.
    CheckTiming("the count starts again on another track's loop", move, 490, 510, 10000);

    // No track reads the beats. Track 0's first pass ends at 500 ms. The sequence is steady from then on, and the held
    // note's wave has long since looped twice.
    CheckTiming("beats that no track reads don't stop a steady loop", beats, 490, 515, 10000);

    // Track 0 isn't still while it counts. Its held note starts after 32 ticks (333 ms), and the sequence is steady
    // soon after.
    CheckTiming("a counting track isn't still", count, 335, 365, 10000);

    // Track 0 opens tracks 1 and 2, then waits 48 ticks and jumps back, over and over. Track 1, at 17, sets its volume
    // to 100 and to 90, 12 ticks apart, and jumps back. Track 2, at 30, plays a note of 24 ticks over and over (from
    // 36).
    const auto quiet =
        Measure({0x88, 1,    0x00, 0x00, 17,   0x88, 2,    0x00, 0x00, 30,   0x80, 48,   0x89, 0x00, 0x00,
                 10,   0xFF, 0xC1, 100,  0x80, 12,   0xC1, 90,   0x80, 12,   0x89, 0x00, 0x00, 17,   0xFF,
                 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60,   127,  24,   0x89, 0x00, 0x00, 36,   0xFF},
                7);

    // Track 0 plays a note of 24 ticks, then ties the note to itself over and over (from 11). The wave doesn't loop,
    // and at 30 Hz it ends during the third pass. The first tie changes the note's volume, and later ones change
    // nothing. After the note ends, the next pass starts it again.
    const auto restart =
        Measure({0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0xC8, 1, 60, 127, 24, 60, 127, 24, 0x89, 0x00, 0x00, 11, 0xFF}, 1,
                false, 30);

    // Track 0 plays a note of 48 ticks without waiting for it (C7 00), then waits a tick and jumps back, over and over
    // (from 11).
    const auto ending =
        Measure({0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0xC7, 0x00, 60, 127, 48, 0x80, 1, 0x89, 0x00, 0x00, 11, 0xFF}, 1);

    // Track 0's passes are idle. Track 1's passes change its volume but play no notes. Neither keeps track 2 from being
    // the main track. Track 2's loops at 48 and 72 ticks count. At 24 ticks, track 0 had made no pass yet.
    CheckTiming("a track that plays no notes doesn't keep a later one from the loops", quiet, 740, 760, 10000);

    // The first tie's pass is the first loop. The note ends at about 0.9 s. A track whose passes play notes isn't stuck
    // then, and the pass that starts the note again is the second loop.
    CheckTiming("a still track that plays notes isn't stuck", restart, 1235, 1265, 10000);

    // The note runs out at 500 ms, and 500 ms of tail follow. Its track polls, but isn't settled while the note has
    // length left.
    CheckTiming("a still track's note that runs out ends the sequence", ending, 990, 1010, 0);
}

// Tests the length analysis on sequences that sound late or never.
void TestSilence()
{
    // Track 0 opens track 1 at 18, then sets its volume to 100 and to 90, a tick apart, and jumps back to 5, over and
    // over. Track 1 rests for 48 ticks and then holds a note on program 1.
    const auto late =
        Measure({0x88, 1,    0x00, 0x00, 18,   0xC1, 100,  0x80, 1,  0xC1, 90, 0x80, 1,   0x89, 0x00, 0x00, 5,
                 0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0x80, 48, 0x81, 1,  60,   127, 0xCB, 0x00, 0xFF},
                3);

    // Track 0 waits a tick and jumps back, over and over, and plays nothing.
    const auto silent = Measure({0x80, 1, 0x89, 0x00, 0x00, 0, 0xFF}, 1);

    // Track 0's passes change its volume, but the 24 passes before the note starts at 500 ms don't count. Two passes
    // after it do.
    CheckTiming("loops before the first sound don't count", late, 500, 550, 10000);

    // A sequence that never sounds gets the tail alone, and the ripper skips it.
    CheckTiming("a sequence that never sounds is silent", silent, 500, 500, 0, true);
}

// Tests the length analysis on sequences whose track 0 never continues while track 1 loops. Track 1 doesn't own the
// loop while track 0 can still continue.
void TestTracksThatWaitForEver()
{
    // Track 0 opens track 1 at 17 and plays a note of length 0 on program 1 with note wait on. The note never ends.
    // Track 1 plays a note of 24 ticks over and over (from 23).
    const auto endless =
        Measure({0x88, 1,    0x00, 0x00, 17,   0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0x81, 1,    60, 127, 0,
                 0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 62,   127,  24,   0x89, 0x00, 0x00, 23, 0xFF},
                3);

    // The same, with a held note of 9,600 ticks on track 0. Track 1 starts at 18 and loops from 24.
    const auto held =
        Measure({0x88, 1,    0x00, 0x00, 18,   0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0x81, 1,    60,   127, 0xCB,
                 0x00, 0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 62,   127,  24,   0x89, 0x00, 0x00, 24,  0xFF},
                3);

    // The note of length 0 again, but on program 0. Program 0 plays the wave at its pitch. No wave loops, and at 30 Hz
    // the note's wave ends after 0.87 s. Track 0 then ends.
    const auto ends = Measure({0x88, 1,    0x00, 0x00, 17,   0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0x81, 0,    60, 127, 0,
                               0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 62,   127,  24,   0x89, 0x00, 0x00, 23, 0xFF},
                              3, false, 30);

    // Track 0 plays a note of 48 ticks on program 1 with note wait off. Then it turns note wait on and plays a note of
    // length 0 on program 5. Program 5 plays nothing. Track 0 waits for the first note to end. Track 1 starts at 26 and
    // loops from 32.
    const auto finite = Measure({0x88, 1,    0x00, 0x00, 26,   0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0xC7, 0x00, 0x81,
                                 1,    60,   127,  48,   0xC7, 0x01, 0x81, 5,    60,   127,  0,    0xFF, 0xD1, 0x7F,
                                 0xD2, 0x7F, 0xD3, 0x7F, 62,   127,  24,   0x89, 0x00, 0x00, 32,   0xFF},
                                3);

    // Track 0 plays a note of length 0 on program 1 with note wait off, and goes on. It sets its volume to 100 and to
    // 90, 18 ticks apart, and jumps back to 18, over and over. Track 1 starts at 31 and loops from 37.
    const auto playing =
        Measure({0x88, 1,    0x00, 0x00, 31,   0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0xC7, 0x00, 0x81, 1,
                 60,   127,  0,    0xC1, 100,  0x80, 18,   0xC1, 90,   0x80, 18,   0x89, 0x00, 0x00, 18,
                 0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 62,   127,  24,   0x89, 0x00, 0x00, 37,   0xFF},
                3);

    // Track 1's loops count from the start: two loops of 24 ticks.
    CheckTiming("a track on a note that never ends doesn't keep a later one from the loops", endless, 490, 510, 10000);
    CheckTiming("a track that waits out a held note doesn't keep a later one from the loops", held, 490, 510, 10000);

    // Track 1's loops count once track 0 has ended: at 1,000 and 1,250 ms.
    CheckTiming("a track whose notes end keeps a later one from the loops until then", ends, 1240, 1265, 10000);

    // The first note ends at 500 ms, and track 0 with it. Track 1's loops count from then: at 750 and 1,000 ms.
    CheckTiming("a track that waits on a note with length keeps a later one from the loops", finite, 990, 1010, 10000);

    // Track 0 doesn't wait for its note, and the sequence loops with track 0: two loops of 36 ticks.
    CheckTiming("a track that doesn't wait for its endless note owns the loops", playing, 740, 760, 10000);
}

// Tests the length analysis on sequences in which a later track holds a note of length 0 and only changes it.
void TestSwells()
{
    // Track 0 opens track 1 at 12, then waits a tick and jumps back, over and over (from 5). Track 1 plays a note of
    // length 0 on program 1 without waiting for it. Then it loops forever (D4 00 to FC). Each pass sets its volume to
    // 100 and to 90, 12 ticks apart, and plays no note.
    const auto swell = Measure(
        {0x88, 1, 0x00, 0x00, 12, 0x80, 1, 0x89, 0x00, 0x00, 5,  0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0xC7, 0x00,
         0x81, 1, 60,   127,  0,  0xD4, 0, 0xC1, 100,  0x80, 12, 0xC1, 90,   0x80, 12,   0xFC, 0xFF},
        3);

    // Track 0 opens tracks 1 and 2, then waits 48 ticks and jumps back, over and over (from 10). Track 1, at 17, plays
    // a note of length 0 on program 1 without waiting for it. Then it sets its volume to 100 and to 90, 12 ticks apart,
    // and jumps back to 30. Track 2, at 43, plays a note of 24 ticks over and over (from 49).
    const auto pad = Measure({0x88, 1,    0x00, 0x00, 17,   0x88, 2,    0x00, 0x00, 43,   0x80, 48,  0x89, 0x00, 0x00,
                              10,   0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0xC7, 0x00, 0x81, 1,   60,   127,  0,
                              0xC1, 100,  0x80, 12,   0xC1, 90,   0x80, 12,   0x89, 0x00, 0x00, 30,  0xFF, 0xD1, 0x7F,
                              0xD2, 0x7F, 0xD3, 0x7F, 60,   127,  24,   0x89, 0x00, 0x00, 49,   0xFF},
                             7);

    // Track 0's passes are idle. Track 1's first pass played its note. Its second pass plays no note and only changes
    // the volume of the note of length 0. Two loops of 24 ticks.
    CheckTiming("a track that only swells its endless note loops", swell, 490, 510, 10000);

    // Track 2 plays notes. Track 1's swells therefore don't count. Track 2's loops at 48 and 72 ticks do. At 24 ticks,
    // track 0 had made no pass yet.
    CheckTiming("a swell doesn't take the loops from a track that plays notes", pad, 740, 760, 10000);
}

// Tests the length analysis on a sequence in which a later track's loop holds an inner loop.
void TestInnerLoops()
{
    // Track 0 opens track 1 at 6 and ends. Track 1 sets variable 0 to 0 (at 14) and plays a note of 12 ticks on program
    // 1. Then it counts 12 ticks in variable 0 and jumps back to 22 after each tick (F0 81 adds, F0 91 compares, A2 89
    // leaves the count at 43). At 43 it jumps back to 14, over and over.
    const auto counted =
        Measure({0x88, 1,    0x00, 0x00, 6,    0xFF, 0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0x81, 1,    0xF0, 0x80,
                 0x00, 0x00, 0x00, 62,   127,  12,   0x80, 1,    0xF0, 0x81, 0x00, 0x00, 0x01, 0xF0, 0x91, 0x00,
                 0x00, 12,   0xA2, 0x89, 0x00, 0x00, 43,   0x89, 0x00, 0x00, 22,   0x89, 0x00, 0x00, 14,   0xFF},
                3);

    // The jumps back of the count play no note. Each pass of the outer loop still plays one. Two loops of 24 ticks.
    CheckTiming("an inner loop doesn't hide the notes of the loop around it", counted, 490, 510, 10000);
}

void TestArrangement()
{
    // Track 0 plays a note of 24 ticks over and over (from 6). Two loops take 500 ms, and half as long at twice the
    // tempo. The fade stays the same.
    const Bytes kLoop = {0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127, 24, 0x89, 0x00, 0x00, 6, 0xFF};
    CheckTiming("two loops of 24 ticks", Measure(kLoop, 1), 490, 510, 10000);
    CheckTiming("two loops of 24 ticks at twice the tempo", Measure(kLoop, 1, true, 32000, {.tempo_ratio = 2.0f}), 240,
                260, 10000);
}

// Tests the ripper's choice of music (rip::IsMusic).
void TestIsMusic()
{
    // The players: some named for music by a word in their names, one of them followed by digits. One sound effects
    // player also has MUSIC in its name. The others have unrelated names.
    const std::vector<const char*> kPlayers = {"PLAYER_BGM_MAIN", "MUSIC_SE_PLAYER", "SONG_PLAYER",   "PLAYER_SFX",
                                               "PLAYER_FANFARE",  "PLAYER_BGM2",     "MUSICAL_PLAYER"};

    // The sounds, with their names, types and players, and whether each is music.
    struct Case
    {
        TableSound sound;
        bool music;
    };

    const Case kCases[] = {
        {{"SEQ_FIELD", 1, 0}, true},       // on a BGM player
        {{"SE_CURSOR", 1, 1}, false},      // on a player whose name has SE
        {{"SEQ_ODE", 1, 2}, true},         // on a SONG player
        {{"SEQ_JUNGLE_BGM1", 1, 3}, true}, // its label contains BGM
        {{"SEQ_WHISTLE", 1, 3}, false},    // a sound effect
        {{"STRM_RACE", 2, 0}, false},      // a stream, which isn't ripped
        {{"SEQ_GOAL", 1, 4}, true},        // on a FANFARE player
        {{"SEQ_BOSS", 1, 5}, true},        // on a BGM2 player
        {{"SEQ_TUNE", 1, 6}, false},       // MUSICAL isn't MUSIC
        {{"seq_bgm_menu", 1, 3}, true},    // labels are matched ignoring case
        {{"SEQ_NOWHERE", 1, 9}, false},    // on a player the archive doesn't have
    };
    std::vector<TableSound> sounds;
    for (const Case& c : kCases)
    {
        sounds.push_back(c.sound);
    }

    const Bytes bytes = MakeSoundTable(sounds, kPlayers);
    const ntdlib::rsar::SoundArchive archive = ntdlib::rsar::SoundArchive::Load(bytes, wsf::nw4r::kRsarOptions);

    for (std::size_t i = 0; i < sounds.size(); ++i)
    {
        Check(rip::IsMusic(archive, static_cast<uint32_t>(i)) == kCases[i].music,
              std::string(sounds[i].name) + (kCases[i].music ? " is music" : " isn't music"));
    }

    Check(!rip::IsMusic(archive, static_cast<uint32_t>(sounds.size())), "a sound the archive doesn't have");
}

// The name that rip::LibraryName gives `library` in `folder`, from a registry that holds no names yet. Each run of the
// ripper starts with such a registry.
std::string NameInFolder(const fs::path& folder, const std::string& base, const std::vector<uint8_t>& library)
{
    xsflib::rip::FileNames names(".wsflib", ".miniwsf");
    return rip::LibraryName(folder, names, base, library);
}

// Tests the names of libraries in a folder that holds earlier rips (rip::LibraryName).
void TestLibraryNames()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path folder = fs::temp_directory_path() / ("wsf_rip_test_" + std::to_string(stamp));
    fs::create_directories(folder);

    // The earlier rips: two libraries of 3 bytes, one of over 1 MiB and a folder with a library's name. FileHolds
    // compares the large one in blocks.
    std::vector<uint8_t> large((std::size_t{1} << 20) + 1, 7);
    const bool written = xsflib::rip::WriteFile(folder / "Game.wsflib", {1, 2, 3}) &&
                         xsflib::rip::WriteFile(folder / "Game_2.wsflib", {1, 2, 4}) &&
                         xsflib::rip::WriteFile(folder / "Large.wsflib", large);
    fs::create_directories(folder / "Folder.wsflib");

    const std::string free_name = NameInFolder(folder, "New", {1});
    const std::string same = NameInFolder(folder, "Game", {1, 2, 3});
    const std::string same_as_second = NameInFolder(folder, "Game", {1, 2, 4});
    const std::string other = NameInFolder(folder, "Game", {1, 2, 5});
    const std::string shorter = NameInFolder(folder, "Game", {1, 2});
    const std::string large_same = NameInFolder(folder, "Large", large);
    large.back() = 8;
    const std::string large_other = NameInFolder(folder, "Large", large);
    const std::string over_folder = NameInFolder(folder, "Folder", {1});

    Check(written, "the earlier rips are written");
    Check(free_name == "New.wsflib", "a free name is taken");
    Check(same == "Game.wsflib", "a file with the same bytes is replaced");
    Check(same_as_second == "Game_2.wsflib", "the first name whose file holds the same bytes is taken");
    Check(other == "Game_3.wsflib", "files of the same size with other bytes are passed over");
    Check(shorter == "Game_3.wsflib", "longer files are passed over");
    Check(large_same == "Large.wsflib", "a file over 1 MiB with the same bytes is replaced");
    Check(large_other == "Large_2.wsflib", "a file over 1 MiB that differs in its last byte is passed over");
    Check(over_folder == "Folder_2.wsflib", "a folder with the name is passed over");

    std::error_code ec;
    fs::remove_all(folder, ec);
}

// Appends `value` to `data`, big-endian.
void Put32(std::vector<uint8_t>& data, uint32_t value)
{
    uint8_t word[4];
    xsflib::StoreBe32(word, value);
    data.insert(data.end(), word, word + 4);
}

// Whether `read` throws xsflib::FormatError.
template <typename Read>
bool ThrowsFormatError(const Read& read)
{
    try
    {
        read();
    }
    catch (const xsflib::FormatError&)
    {
        return true;
    }

    return false;
}

// Tests the readers of a Jac game's bank file and sequence index (rip::ReadJacWaveSystems, rip::ReadJacSounds).
void TestJac()
{
    // A bank file with three wave systems' entries: none for system 0, and a wave system for each of 1 and 2. It has no
    // banks.
    const std::vector<uint8_t> one = ntdlib::testing::jaudio_files::WaveSystemFile(1, 2, "one_0.aw");
    const std::vector<uint8_t> two = ntdlib::testing::jaudio_files::WaveSystemFile(2, 1, "two_0.aw");
    constexpr uint32_t kList = 0x10, kSystems = 3;
    const uint32_t first = kList + kSystems * 8;
    std::vector<uint8_t> banks;
    for (const uint32_t word : {kList, kSystems, first, 0u, 0u, 0u, first, uint32_t(one.size()),
                                first + uint32_t(one.size()), uint32_t(two.size())})
    {
        Put32(banks, word);
    }
    banks.insert(banks.end(), one.begin(), one.end());
    banks.insert(banks.end(), two.begin(), two.end());

    const std::vector<ntdlib::jaudio::WaveSystem> systems = rip::ReadJacWaveSystems(banks);
    Check(systems.size() == 2 && systems[0].number == 1 && systems[1].number == 2,
          "a bank file's wave systems take their places in its list as their numbers");
    Check(systems.size() == 2 && systems[0].groups.size() == 1 && systems[0].groups[0].archive == "one_0.aw" &&
              systems[0].waves.size() == 2 && systems[1].groups.size() == 1 &&
              systems[1].groups[0].archive == "two_0.aw",
          "a bank file's wave systems name their wave archives and hold their waves");

    std::vector<uint8_t> too_many = banks;
    xsflib::StoreBe32(&too_many[4], 0x10000000);
    Check(ThrowsFormatError([&] { rip::ReadJacWaveSystems(too_many); }),
          "a bank file that lists more wave systems than it can hold is refused");
    Check(ThrowsFormatError([&] { rip::ReadJacWaveSystems(std::vector<uint8_t>(6)); }),
          "a bank file shorter than its header is refused");

    // A main.dol with one section at 0x80300000. The section holds a sequence index of six files: the effects, the
    // system's events, a piece, an empty file, a piece whose name has no extension, and a piece whose name fills its
    // 14 bytes.
    constexpr uint32_t kSection = 0x80300000, kIndex = kSection + 0x20;
    std::vector<uint8_t> dol(0x100);
    xsflib::StoreBe32(&dol[0x1c], 0x100);    // the first data section's file offset
    xsflib::StoreBe32(&dol[0x64], kSection); // its address
    std::vector<uint8_t> section(0x20);
    const auto add_file = [&section](const std::string& name, uint32_t offset, uint32_t size)
    {
        std::vector<uint8_t> entry(0x20);
        std::copy(name.begin(), name.end(), entry.begin());
        xsflib::StoreBe32(&entry[0x18], offset);
        xsflib::StoreBe32(&entry[0x1c], size);
        section.insert(section.end(), entry.begin(), entry.end());
    };
    const std::string header = "BARC----";
    section.insert(section.end(), header.begin(), header.end());
    Put32(section, 0);
    Put32(section, 6);
    const std::string archive = "test.arc";
    section.insert(section.end(), archive.begin(), archive.end());
    section.resize(0x20 + 0x20);
    add_file("se.jam", 0, 0x100);
    add_file("sysevent.jam", 0x100, 0x100);
    add_file("song.jam", 0x200, 0x40);
    add_file("dummy", 0, 0);
    add_file("tune", 0x240, 0x20);
    add_file("fourteen.chars", 0x260, 0x20);
    xsflib::StoreBe32(&dol[0xac], static_cast<uint32_t>(section.size())); // the first data section's size
    dol.insert(dol.end(), section.begin(), section.end());

    rip::JAudioProfile profile{};
    profile.library = rip::JAudioLibrary::kJac;
    profile.sequence_index = kIndex;
    const std::vector<rip::JAudioSound> sounds = rip::ReadJacSounds(dol, profile);
    Check(sounds.size() == 3, "the pieces are the files from number 2 on that aren't empty");
    Check(sounds.size() == 3 && sounds[0].id == 0x80000002 && sounds[0].label == "song" && sounds[1].id == 0x80000004 &&
              sounds[1].label == "tune" && sounds[2].id == 0x80000005 && sounds[2].label == "fourteen",
          "a piece's ID holds the file's number, and the label is the file's name without the extension");
    Check(sounds.size() == 3 && sounds[1].waves == std::vector<uint32_t>{4u << 16} && sounds[1].music &&
              !sounds[1].stream,
          "a piece is music, and plays group 0 of the wave system with its number");

    // The piece "tune" plays in two parts. Part 1 plays group 2 of wave system 4. Part 3 plays group 0 of wave
    // system 5.
    profile.parted_piece = 4;
    profile.parts = {{.number = 1, .system = 4, .group = 2}, {.number = 3, .system = 5, .group = 0}};
    const std::vector<rip::JAudioSound> parts = rip::ReadJacSounds(dol, profile);
    Check(parts.size() == 4 && parts[1].id == 0x80000104 && parts[1].label == "tune-1" && parts[2].id == 0x80000304 &&
              parts[2].label == "tune-3",
          "each part of a piece in parts is a sound with the part's number in the ID's second byte");
    Check(parts.size() == 4 && parts[1].waves == std::vector<uint32_t>{4u << 16 | 2} &&
              parts[2].waves == std::vector<uint32_t>{5u << 16} && parts[1].music && parts[2].music,
          "a part is music and plays the wave group of the part's entry in the profile");
    Check(parts.size() == 4 && parts[0].id == 0x80000002 && parts[3].id == 0x80000005,
          "the other pieces are a sound each");
    profile.parted_piece = 0;
    profile.parts.clear();

    // The index holds the count of files at offset 0xc. Seven files run past the end of the section.
    std::vector<uint8_t> short_index = dol;
    xsflib::StoreBe32(&short_index[0x100 + 0x20 + 0xc], 7);
    Check(ThrowsFormatError([&] { rip::ReadJacSounds(short_index, profile); }),
          "an index whose files run past the end of its section is refused");

    profile.sequence_index = kSection;
    Check(ThrowsFormatError([&] { rip::ReadJacSounds(dol, profile); }), "an index without its magic is refused");
    profile.sequence_index = 0x80400000;
    Check(ThrowsFormatError([&] { rip::ReadJacSounds(dol, profile); }), "an index outside main.dol is refused");
    Check(ThrowsFormatError([&] { rip::ReadJacSounds(std::vector<uint8_t>(0x10), profile); }),
          "a main.dol that isn't a DOL is refused");
}

// Tests the list of Pikmin 2's sounds (rip::ReadPikmin2Sounds) and the watch of its AutoBgm pieces (rip::WatchOf), with
// New Play Control! Pikmin 2's profile. The start-up archive has the effects sequence, another sequence, two effects of
// category 0 and one of category 6. The sequence archive holds the effects sequence and the game's 48 pieces. The name
// table names the second effect and the third, and a sequence.
void TestPikmin2Sounds()
{
    const std::vector<rip::JAudioProfile>& profiles = rip::KnownJAudioProfiles();
    const auto profile = std::find_if(profiles.begin(), profiles.end(),
                                      [](const rip::JAudioProfile& p) { return p.game_id == "R92E01"; });
    if (!Check(profile != profiles.end() && profile->driver == rip::JAudioDriver::kPikmin2,
               "New Play Control! Pikmin 2 has a profile for Pikmin 2's driver"))
    {
        return;
    }

    namespace files = ntdlib::testing::jaudio_files;
    const std::vector<uint8_t> aaf = files::AafFile({{16, 127, 0}, {16, 127, 1}, {0}, {0}, {6}}, {"wScene_0.aw"});

    const char* const kPieces[] = {
        "2pbattle",      "a_iron",      "a_zawinul01", "battle_t",        "book",         "caveboss",
        "caveconc",      "caveglass",   "cavekeyget",  "cavemetal",       "caverelax",    "cavesoil",
        "cavetile",      "cavetsumiki", "comp_result", "c_menu",          "c_result",     "ff_annihi",
        "ff_doped",      "ff_doping",   "ff_keyget",   "ff_playerdown",   "ff_treasureget", "forest",
        "f_result",      "hiscore",     "id_test",     "kuro_post",       "kuro_pre",     "last",
        "l_boss",        "m_boss",      "new_00",      "new_01",          "new_02",       "new_03",
        "new_04",        "new_05",      "new_06",      "new_07",          "new_08",       "n_tutorial",
        "n_tutorial_1stday", "result",  "u_result",    "worldmap",        "worldmap_intro", "yakushima",
    };
    std::vector<files::RarcFileSpec> archive_files = {{0, "se.bms", {0xc1}}};
    for (const char* piece : kPieces)
    {
        archive_files.push_back({static_cast<uint16_t>(archive_files.size()), std::string(piece) + ".bms", {0xff}});
    }

    // The name table: its header, the 18 categories' records, then the records of a sequence and two effects. The
    // game's table gives a category 6 effect's ID with bit 11.
    std::vector<uint8_t> names(0x10 + 18 * 0x20);
    const auto add_name = [&names](const std::string& name, uint32_t id)
    {
        std::vector<uint8_t> record(0x20);
        std::copy(name.begin(), name.end(), record.begin());
        xsflib::StoreBe32(&record[0x1c], id);
        names.insert(names.end(), record.begin(), record.end());
    };
    add_name("PSSQ_TEST", 0x80010001);
    add_name("PSSE_SECOND", 0x0001);
    add_name("HOMESE_TEST", 0x6800);

    const std::vector<rip::JAudioSound> sounds = rip::ReadPikmin2Sounds(aaf, files::RarcFile(archive_files), names,
                                                                        *profile);
    if (!Check(sounds.size() == 48 + 3, "the pieces come first, then the effects, without the table's sequences"))
    {
        return;
    }

    Check(sounds[3].id == 0x80000003 && sounds[3].label == "battle_t" && sounds[3].music && sounds[3].section == "BGM",
          "a piece's ID holds its number in the driver's table, and its label is its file's name");
    Check(sounds[3].waves == std::vector<uint32_t>{0, 6} && sounds[31].waves == std::vector<uint32_t>{0},
          "a piece plays group 0 and the group of its scene");
    Check(sounds[0].waves.empty(), "a piece that the game never plays has no groups. The ripper finds them");
    Check(sounds[48].id == 0x800 && sounds[48].label == "se0-0000" && sounds[49].id == 0x801 &&
              sounds[49].label == "PSSE_SECOND" && sounds[50].id == 0x6800 && sounds[50].label == "HOMESE_TEST",
          "the name table names an effect, and an effect without a name is named by its category and index");
    Check(!sounds[48].music && sounds[48].section == "SE" && sounds[48].waves == profile->static_waves,
          "an effect plays in the groups that the global scene loads");

    const std::vector<uint8_t> short_names(names.begin(), names.begin() + 0x10 + 17 * 0x20);
    Check(
        ThrowsFormatError([&] { rip::ReadPikmin2Sounds(aaf, files::RarcFile(archive_files), short_names, *profile); }),
        "a name table shorter than its categories' records is refused");
    archive_files.pop_back();
    Check(ThrowsFormatError([&] { rip::ReadPikmin2Sounds(aaf, files::RarcFile(archive_files), names, *profile); }),
          "a sequence archive without a piece is refused");

    // The pieces whose conductors pick what the tracks play never repeat.
    Check(!rip::WatchOf(*profile, 0x80000006).idle_in_calls && !rip::WatchOf(*profile, 0x80000001).idle_in_calls,
          "the root track of an AutoBgm piece doesn't idle in its polling routine");
    Check(rip::WatchOf(*profile, 0x80000003).idle_in_calls && rip::WatchOf(*profile, 0x806).idle_in_calls,
          "the root tracks of the other pieces and the effects idle in calls");
}

} // namespace

int main()
{
    try
    {
        TestLengths();
        TestSilence();
        TestTracksThatWaitForEver();
        TestSwells();
        TestInnerLoops();
        TestArrangement();
        TestIsMusic();
        TestLibraryNames();
        TestJac();
        TestPikmin2Sounds();
    }
    catch (const std::exception& e)
    {
        Check(false, std::string("unexpected error: ") + e.what());
    }

    return xsflib::testing::Summary("rip");
}
