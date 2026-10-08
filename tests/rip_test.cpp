// SPDX-License-Identifier: MIT

// Tests the ripper's choice of music (rip::IsMusic) on an archive of sounds and players that tests/rsar_files.h builds.

#include "rip/rip.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "check.h"
#include "rsar/formats.h"
#include "rsar_files.h"

using namespace wsf;
using namespace wsf::test;

int main()
{
    try
    {
        // The players: some named for music by a word in their names, one of them followed by digits. One sound effects
        // player also has MUSIC in its name. The others have unrelated names.
        const std::vector<const char*> players = {"PLAYER_BGM_MAIN", "MUSIC_SE_PLAYER", "SONG_PLAYER",   "PLAYER_SFX",
                                                  "PLAYER_FANFARE",  "PLAYER_BGM2",     "MUSICAL_PLAYER"};
        struct Case
        {
            TableSound sound;
            bool music;
        };

        const Case cases[] = {
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
        for (const Case& c : cases)
        {
            sounds.push_back(c.sound);
        }

        const Bytes bytes = MakeSoundTable(sounds, players);
        const rsar::SoundArchive archive = rsar::SoundArchive::Load(bytes);

        for (std::size_t i = 0; i < sounds.size(); ++i)
        {
            Check(rip::IsMusic(archive, static_cast<uint32_t>(i)) == cases[i].music,
                  std::string(sounds[i].name) + (cases[i].music ? " is music" : " isn't music"));
        }

        Check(!rip::IsMusic(archive, static_cast<uint32_t>(sounds.size())), "a sound the archive doesn't have");
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
