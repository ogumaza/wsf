// SPDX-License-Identifier: MIT

// The driver profiles of the JAudio games that WSF rips in game mode (see jaudio.h).

#include <optional>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "xsflib/sha1.h"

#include "rip/jaudio.h"
#include "rip/sound_libraries.h"

namespace wsf::rip
{

const std::vector<JAudioProfile>& KnownJAudioProfiles()
{
    // Addresses in the games' main.dol files.
    static const std::vector<JAudioProfile> kProfiles = {
        // The Legend of Zelda: Twilight Princess. The sizes and offsets are the game's: its machine start-up gives the
        // heaps and the ARAM manager, its audio set-up the audio heap, and its sound objects the analysis's offsets.
        // The game keeps its archive heap in MEM2's root heap. The audio set-up loads four wave groups itself.
        // As the game starts five pieces, Z2SeqMgr::bgmStart (0x80293634) mutes some children of their track 0. The
        // driver mutes the same children through the music manager (ntdlib's kTwilightPrincessStarts). The scenes'
        // starts, changeBgmStatus (0x80294254), changeSubBgmStatus (0x80295dd0) and the battle music's functions give
        // 17 pieces more states (ntdlib's kTwilightPrincessWiiStates). The driver sets their volumes the same way and
        // writes their status through Z2SoundStarter::setPortData.
        {
            .name = "The Legend of Zelda: Twilight Princess (USA, Rev 2)",
            .game_id = "RZDE01",
            .dol_sha1 = "5a99acb37b98e19682924502ee14a6d03d536c69",
            .main = 0x80008d04,
            .z2 =
                {
                    .frame_retraces = 2,
                    .max_heaps = 1,
                    .system_heap_size = 0x800000,
                    .aram_audio_size = 0xb00000,
                    .aram_graph_size = 0xffffffff,
                    .aram_priorities = 8 << 16 | 7 << 8 | 6,
                    .aram_buffer_size = 0x2000,
                    .command_heap_size = 0x1000,
                    .archive_heap_size = 0xf20c00,
                    .zelda_heap_margin = 0x10000,
                    .audio_heap_size = 0x169000,
                    .vi_init = 0,
                    .vi_wait_for_retrace = 0,
                    .get_current_thread = 0x803384c0,
                    .get_thread_priority = 0x80339754,
                    .create_root_heap = 0x802cd5bc,
                    .create_exp_heap = 0x802cd688,
                    .create_solid_heap = 0x802cf184,
                    .create_aram = 0x802d096c,
                    .set_aram_buffer = 0x802d296c,
                    .create_thread_switch = 0x802d00b8,
                    .create_command_heap = 0x800149f8,
                    .create_archive_heap = 0x80014a28,
                    .archive_heap_parent = 0x8052112c, // JKRHeap::sRootHeap2
                    .create_assert_heap = 0x80014930,
                    .create_zelda_heap = 0x800149c8,
                    .get_free_size = 0x802cd050,
                    .become_current_heap = 0x802cce18,
                    .system_heap = 0x80521120,
                    .alloc = 0x802cd4fc,
                    .thread_ctor = 0x802cfd98,
                    .create_disc_thread = 0x8001ad3c,
                    .audio_heap = 0x80520784,
                    .audio_setup = 0x8000973c,
                    .init_flag = 0x80520780,
                    .audio_frame = 0x802af680,
                    .load_se_wave = 0x8029cb7c,
                    .load_bgm_wave = 0x8029cbe8,
                    .wave_status = 0x8029caa8,
                    .audio_manager = 0x80431cac,
                    .start_sound = 0x802af854,
                    .erase_bgm_wave = 0x8029ca40,
                    .erase_se_wave = 0x8029c9d8,
                    .first_scene_wave_count = 2,
                    .first_scene_waves = {(1u << 16) | 0, (0u << 16) | 88},
                    .room_wave_count = 1,
                    .room_waves = {(0u << 16) | 0},
                    .music_manager = 0x80520798,    // JASGlobalInstance<Z2SeqMgr>::sInstance
                    .set_child_volume = 0x802977a4, // Z2SeqMgr::setChildTrackVolume
                    .set_port_data = 0x8028f360,    // Z2SoundStarter::setPortData
                    .get_port_data = 0x8028f3f0,    // Z2SoundStarter::getPortData
                },
            .sleep = 0x8025b100,      // JFWDisplay::threadSleep
            .reset_data = 0x805208d0, // mDoRst::mResetData
            .reset_flag = 0x12,
            .full_ring_count = 0x80520fc8, // JASDriver::finishDSPFrame's count (0x80281510)
            .ios_version = 9,
            .watch =
                {
                    .parse_command = 0x8027b080,
                    .jump_handler = 0x80279bf4,
                    // JASSeqParser's handler for 0xC8 (0x80279c0c) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x80279c38,
                    .sequence_vtable = 0x8040d934,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x8040d888, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8027d94c, 0x8027ddf4},
                    .wave_bank_table = 0x80521038,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0, 0 << 16 | 88},
            .muted_starts = ntdlib::jaudio::kTwilightPrincessStarts,
            .piece_states = ntdlib::jaudio::kTwilightPrincessWiiStates,
        },
        // Twilight Princess's first USA release (version 0). Its main.dol links Rev 2's code at other addresses. Each
        // address comes from Rev 2's code, matched with the linker's fields masked. The sizes are Rev 2's. Some of the
        // game's sound objects lay out their fields otherwise. None of the functions that the driver calls reads those
        // fields. Its bgmStart (0x80293e88) mutes the same children as Rev 2's. Runs of its changeBgmStatus
        // (0x80294a90) and changeSubBgmStatus (0x8029660c) on a CPU emulator gave the same volumes and ports as runs of
        // Rev 2's code, for every piece and status. Its battle and field functions give the same states.
        {
            .name = "The Legend of Zelda: Twilight Princess (USA)",
            .game_id = "RZDE01",
            .dol_sha1 = "0f626c1bfbd05979ed78b634cc85248342452fc2",
            .main = 0x80008cec,
            .z2 =
                {
                    .frame_retraces = 2,
                    .max_heaps = 1,
                    .system_heap_size = 0x800000,
                    .aram_audio_size = 0xb00000,
                    .aram_graph_size = 0xffffffff,
                    .aram_priorities = 8 << 16 | 7 << 8 | 6,
                    .aram_buffer_size = 0x2000,
                    .command_heap_size = 0x1000,
                    .archive_heap_size = 0xf20c00,
                    .zelda_heap_margin = 0x10000,
                    .audio_heap_size = 0x169000,
                    .vi_init = 0,
                    .vi_wait_for_retrace = 0,
                    .get_current_thread = 0x8034da88,
                    .get_thread_priority = 0x8034ed1c,
                    .create_root_heap = 0x802e2bec,
                    .create_exp_heap = 0x802e2cb8,
                    .create_solid_heap = 0x802e47b4,
                    .create_aram = 0x802e5f9c,
                    .set_aram_buffer = 0x802e7f9c,
                    .create_thread_switch = 0x802e56e8,
                    .create_command_heap = 0x80014948,
                    .create_archive_heap = 0x80014978,
                    .archive_heap_parent = 0x8053b134, // JKRHeap::sRootHeap2
                    .create_assert_heap = 0x80014880,
                    .create_zelda_heap = 0x80014918,
                    .get_free_size = 0x802e2680,
                    .become_current_heap = 0x802e2448,
                    .system_heap = 0x8053b128,
                    .alloc = 0x802e2b2c,
                    .thread_ctor = 0x802e53c8,
                    .create_disc_thread = 0x8001ac28,
                    .audio_heap = 0x8053a784,
                    .audio_setup = 0x800095cc,
                    .init_flag = 0x8053a780,
                    .audio_frame = 0x802afc6c,
                    .load_se_wave = 0x8029d258,
                    .load_bgm_wave = 0x8029d2c4,
                    .wave_status = 0x8029d184,
                    .audio_manager = 0x8044a6ac,
                    .start_sound = 0x802afe34,
                    .erase_bgm_wave = 0x8029d11c,
                    .erase_se_wave = 0x8029d0b4,
                    .first_scene_wave_count = 2,
                    .first_scene_waves = {(1u << 16) | 0, (0u << 16) | 88},
                    .room_wave_count = 1,
                    .room_waves = {(0u << 16) | 0},
                    .music_manager = 0x8053a798,    // JASGlobalInstance<Z2SeqMgr>::sInstance
                    .set_child_volume = 0x80297fcc, // Z2SeqMgr::setChildTrackVolume
                    .set_port_data = 0x8028fbec,    // Z2SoundStarter::setPortData
                    .get_port_data = 0x8028fc7c,    // Z2SoundStarter::getPortData
                },
            .sleep = 0x8025b9d8,      // JFWDisplay::threadSleep
            .reset_data = 0x8053a8c8, // mDoRst::mResetData
            .reset_flag = 0x12,
            .ios_version = 9,
            .watch =
                {
                    .parse_command = 0x8027b8e4,
                    .jump_handler = 0x8027a458,
                    // JASSeqParser's handler for 0xC8 (0x8027a470) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x8027a49c,
                    .sequence_vtable = 0x80422534,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x80422488, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8027e1b0, 0x8027e658},
                    .wave_bank_table = 0x8053b018,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0, 0 << 16 | 88},
            .muted_starts = ntdlib::jaudio::kTwilightPrincessStarts,
            .piece_states = ntdlib::jaudio::kTwilightPrincessWiiStates,
        },
        // Twilight Princess's European release. Its main.dol links Rev 2's code at other addresses. They were found as
        // for version 0. The sizes and the analysis's offsets are Rev 2's. Runs of its changeBgmStatus (0x802946bc) and
        // changeSubBgmStatus (0x80296238) on a CPU emulator gave the same volumes and ports as runs of Rev 2's code,
        // for every piece and status. Its bgmStart and its battle and field functions match Rev 2's code.
        {
            .name = "The Legend of Zelda: Twilight Princess (Europe)",
            .game_id = "RZDP01",
            .dol_sha1 = "a05fc0372eb427a5b5afc5e23c207aadbd10ebd1",
            .main = 0x80008d04,
            .z2 =
                {
                    .frame_retraces = 2,
                    .max_heaps = 1,
                    .system_heap_size = 0x800000,
                    .aram_audio_size = 0xb00000,
                    .aram_graph_size = 0xffffffff,
                    .aram_priorities = 8 << 16 | 7 << 8 | 6,
                    .aram_buffer_size = 0x2000,
                    .command_heap_size = 0x1000,
                    .archive_heap_size = 0xf20c00,
                    .zelda_heap_margin = 0x10000,
                    .audio_heap_size = 0x169000,
                    .vi_init = 0,
                    .vi_wait_for_retrace = 0,
                    .get_current_thread = 0x803388f0,
                    .get_thread_priority = 0x80339b84,
                    .create_root_heap = 0x802cd9ec,
                    .create_exp_heap = 0x802cdab8,
                    .create_solid_heap = 0x802cf5b4,
                    .create_aram = 0x802d0d9c,
                    .set_aram_buffer = 0x802d2d9c,
                    .create_thread_switch = 0x802d04e8,
                    .create_command_heap = 0x800149e8,
                    .create_archive_heap = 0x80014a18,
                    .archive_heap_parent = 0x805218ec, // JKRHeap::sRootHeap2
                    .create_assert_heap = 0x80014920,
                    .create_zelda_heap = 0x800149b8,
                    .get_free_size = 0x802cd480,
                    .become_current_heap = 0x802cd248,
                    .system_heap = 0x805218e0,
                    .alloc = 0x802cd92c,
                    .thread_ctor = 0x802d01c8,
                    .create_disc_thread = 0x8001ad2c,
                    .audio_heap = 0x80520f44,
                    .audio_setup = 0x80009704,
                    .init_flag = 0x80520f40,
                    .audio_frame = 0x802afab0,
                    .load_se_wave = 0x8029cfac,
                    .load_bgm_wave = 0x8029d018,
                    .wave_status = 0x8029ced8,
                    .audio_manager = 0x804325ac,
                    .start_sound = 0x802afc84,
                    .erase_bgm_wave = 0x8029ce70,
                    .erase_se_wave = 0x8029ce08,
                    .first_scene_wave_count = 2,
                    .first_scene_waves = {(1u << 16) | 0, (0u << 16) | 88},
                    .room_wave_count = 1,
                    .room_waves = {(0u << 16) | 0},
                    .music_manager = 0x80520f58,    // JASGlobalInstance<Z2SeqMgr>::sInstance
                    .set_child_volume = 0x80297c0c, // Z2SeqMgr::setChildTrackVolume
                    .set_port_data = 0x8028f7e0,    // Z2SoundStarter::setPortData
                    .get_port_data = 0x8028f870,    // Z2SoundStarter::getPortData
                },
            .sleep = 0x8025b568,      // JFWDisplay::threadSleep
            .reset_data = 0x80521090, // mDoRst::mResetData
            .reset_flag = 0x12,
            .ios_version = 9,
            .watch =
                {
                    .parse_command = 0x8027b4e8,
                    .jump_handler = 0x8027a05c,
                    // JASSeqParser's handler for 0xC8 (0x8027a074) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x8027a0a0,
                    .sequence_vtable = 0x8040e254,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x8040e1a8, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8027ddb4, 0x8027e25c},
                    .wave_bank_table = 0x805217f8,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0, 0 << 16 | 88},
            .muted_starts = ntdlib::jaudio::kTwilightPrincessStarts,
            .piece_states = ntdlib::jaudio::kTwilightPrincessWiiStates,
        },
        // Twilight Princess's Japanese release. Its main.dol links Rev 2's code at other addresses. They were found as
        // for version 0. Its machine start-up gives the archive heap 0xf0e800 bytes, not 0xf20c00. The other sizes and
        // the analysis's offsets are Rev 2's. Runs of its changeBgmStatus (0x80295d74) and changeSubBgmStatus
        // (0x802978f0) on a CPU emulator gave the same volumes and ports as runs of Rev 2's code, for every piece and
        // status. Its bgmStart and its battle and field functions match Rev 2's code.
        {
            .name = "The Legend of Zelda: Twilight Princess (Japan)",
            .game_id = "RZDJ01",
            .dol_sha1 = "1ab0f344e99132d19b2e174355cbc68b74109905",
            .main = 0x80008d04,
            .z2 =
                {
                    .frame_retraces = 2,
                    .max_heaps = 1,
                    .system_heap_size = 0x800000,
                    .aram_audio_size = 0xb00000,
                    .aram_graph_size = 0xffffffff,
                    .aram_priorities = 8 << 16 | 7 << 8 | 6,
                    .aram_buffer_size = 0x2000,
                    .command_heap_size = 0x1000,
                    .archive_heap_size = 0xf0e800,
                    .zelda_heap_margin = 0x10000,
                    .audio_heap_size = 0x169000,
                    .vi_init = 0,
                    .vi_wait_for_retrace = 0,
                    .get_current_thread = 0x80339f78,
                    .get_thread_priority = 0x8033b20c,
                    .create_root_heap = 0x802cf0dc,
                    .create_exp_heap = 0x802cf1a8,
                    .create_solid_heap = 0x802d0ca4,
                    .create_aram = 0x802d248c,
                    .set_aram_buffer = 0x802d448c,
                    .create_thread_switch = 0x802d1bd8,
                    .create_command_heap = 0x80014a74,
                    .create_archive_heap = 0x80014aa4,
                    .archive_heap_parent = 0x8051ef9c, // JKRHeap::sRootHeap2
                    .create_assert_heap = 0x800149ac,
                    .create_zelda_heap = 0x80014a44,
                    .get_free_size = 0x802ceb70,
                    .become_current_heap = 0x802ce938,
                    .system_heap = 0x8051ef90,
                    .alloc = 0x802cf01c,
                    .thread_ctor = 0x802d18b8,
                    .create_disc_thread = 0x8001adb8,
                    .audio_heap = 0x8051e604,
                    .audio_setup = 0x8000973c,
                    .init_flag = 0x8051e600,
                    .audio_frame = 0x802b11a0,
                    .load_se_wave = 0x8029e69c,
                    .load_bgm_wave = 0x8029e708,
                    .wave_status = 0x8029e5c8,
                    .audio_manager = 0x8042fb2c,
                    .start_sound = 0x802b1374,
                    .erase_bgm_wave = 0x8029e560,
                    .erase_se_wave = 0x8029e4f8,
                    .first_scene_wave_count = 2,
                    .first_scene_waves = {(1u << 16) | 0, (0u << 16) | 88},
                    .room_wave_count = 1,
                    .room_waves = {(0u << 16) | 0},
                    .music_manager = 0x8051e618,    // JASGlobalInstance<Z2SeqMgr>::sInstance
                    .set_child_volume = 0x802992c4, // Z2SeqMgr::setChildTrackVolume
                    .set_port_data = 0x80290e80,    // Z2SoundStarter::setPortData
                    .get_port_data = 0x80290f10,    // Z2SoundStarter::getPortData
                },
            .sleep = 0x8025cc1c,      // JFWDisplay::threadSleep
            .reset_data = 0x8051e750, // mDoRst::mResetData
            .reset_flag = 0x12,
            .ios_version = 9,
            .watch =
                {
                    .parse_command = 0x8027cb9c,
                    .jump_handler = 0x8027b710,
                    // JASSeqParser's handler for 0xC8 (0x8027b728) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x8027b754,
                    .sequence_vtable = 0x8040b7b4,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x8040b708, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8027f468, 0x8027f910},
                    .wave_bank_table = 0x8051eea8,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0, 0 << 16 | 88},
            .muted_starts = ntdlib::jaudio::kTwilightPrincessStarts,
            .piece_states = ntdlib::jaudio::kTwilightPrincessWiiStates,
        },
        {
            // A later build of Twilight Princess's engine. The game's main.dol has no JFWDisplay::threadSleep. The
            // driver's sleep stands in for that function, with `alarm`. The game's scene manager has no function that
            // loads or erases one wave group. The driver's functions stand in for those, with `wave_arcs`. The game
            // runs at 30 frames a second: each scene sets the display's tick rate to a 30th of a second (0x8010231c).
            // The audio set-up loads SE group 0 (4.8 MiB), BGM group 0 and the Home Button menu's group. When a
            // piece's BGM groups don't fit in the wave heap beside SE group 0, the driver erases SE group 0. The
            // game's Z2SeqMgr::bgmStart (0x80154aa4) mutes the same children of track 0 as Twilight Princess's
            // bgmStart. From 0x80155018 on, it calls changeBgmStatus (0x80155f3c) with 0 for the same four pieces. Its
            // field functions (0x8015b12c and 0x8015b238) work as in Twilight Princess. The game's scenes and the boss
            // d_a_b_tn give three pieces other states (ntdlib's kCrossbowTrainingStates).
            .name = "Link's Crossbow Training (USA, Rev 1)",
            .game_id = "RZPE01",
            .dol_sha1 = "2d3fe2c34ce4d2e470573846198ad93f07e4ac1e",
            .main = 0x800082d0,
            .z2 =
                {
                    .frame_retraces = 2,
                    .max_heaps = 1,
                    .system_heap_size = 0x800000,
                    .aram_audio_size = 0xb00000,
                    .aram_graph_size = 0xffffffff,
                    .aram_priorities = 8 << 16 | 7 << 8 | 6,
                    .aram_buffer_size = 0x2000,
                    .command_heap_size = 0x1000,
                    .archive_heap_size = 0x1620c00,
                    .zelda_heap_margin = 0x10000,
                    .audio_heap_size = 0x260800,
                    .vi_init = 0,
                    .vi_wait_for_retrace = 0,
                    .get_current_thread = 0x801da1a8,
                    .get_thread_priority = 0x801db43c,
                    .create_root_heap = 0x8017eda8,
                    .create_exp_heap = 0x8017ef20,
                    .create_solid_heap = 0x80180fec,
                    .create_aram = 0x801826c8,
                    .set_aram_buffer = 0x801844e8,
                    .create_thread_switch = 0x80181e38,
                    .create_command_heap = 0x800156ac,
                    .create_archive_heap = 0x800156dc,
                    .archive_heap_parent = 0x803e0eac, // JKRHeap::sRootHeap2
                    .create_assert_heap = 0x800155e4,
                    .create_zelda_heap = 0x8001567c,
                    .get_free_size = 0x8017d730,
                    .become_current_heap = 0x8017c834,
                    .system_heap = 0x803e0ea0,
                    .alloc = 0x8017e2d4,
                    .thread_ctor = 0x80181c70,
                    .create_disc_thread = 0x8001c098,
                    .audio_heap = 0x803e0694,
                    .audio_setup = 0x8000909c,
                    .init_flag = 0x803e0690,
                    .audio_frame = 0x8016f070,
                    .load_se_wave = 0,
                    .load_bgm_wave = 0,
                    .wave_status = 0,
                    .audio_manager = 0x802ed170,
                    .start_sound = 0x8016f288,
                    .erase_bgm_wave = 0,
                    .erase_se_wave = 0,
                    .first_scene_wave_count = 0,
                    .first_scene_waves = {},
                    .room_wave_count = 1,
                    .room_waves = {(0u << 16) | 0},
                    .music_manager = 0x803e06a8,    // JASGlobalInstance<Z2SeqMgr>::sInstance
                    .set_child_volume = 0x80159e00, // Z2SeqMgr::setChildTrackVolume
                    .set_port_data = 0x8014f5e8,    // Z2SoundStarter::setPortData
                },
            .sleep = 0,
            .reset_data = 0x803e07a8,
            .reset_flag = 0x12,
            .alarm =
                {
                    .set_alarm = 0x801d19ec,
                    .disable_interrupts = 0x801d68fc,
                    .restore_interrupts = 0x801d6924,
                    .suspend_thread = 0x801dafdc,
                    .resume_thread = 0x801dad44,
                },
            .wave_arcs =
                {
                    .section_heap = 0x803e0e20,
                    .bank_table = 0xf4,
                    .load = 0x801407bc,
                    .load_tail = 0x80140894,
                    .erase = 0x8014096c,
                },
            .full_ring_count = 0x803e0db0, // finishDSPFrame's count (0x80142f48)
            .ios_version = 21,
            .watch =
                {
                    .parse_command = 0x8013c0ec,
                    .jump_handler = 0x8013aca0,
                    // JASSeqParser's handler for 0xC8 takes the jump here, with the track in r30 and the arguments in
                    // r31.
                    .conditional_jump = 0x8013ad98,
                    .sequence_vtable = 0x802c61b8,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x802c6028,
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8013ead0, 0x8013f0f4},
                    .wave_bank_table = 0x803e0e20,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0},
            .muted_starts = ntdlib::jaudio::kTwilightPrincessStarts,
            .piece_states = ntdlib::jaudio::kCrossbowTrainingStates,
        },
        {
            // Link's Crossbow Training's European release. Its main.dol links Rev 1's code at other addresses.
            // Each address comes from Rev 1's code, matched with the linker's fields masked. The sizes and the
            // analysis's offsets are Rev 1's.
            .name = "Link's Crossbow Training (Europe)",
            .game_id = "RZPP01",
            .dol_sha1 = "30f6dbcf6d6f20398726180de41db3f0a132f0e5",
            .main = 0x800082d0,
            .z2 =
                {
                    .frame_retraces = 2,
                    .max_heaps = 1,
                    .system_heap_size = 0x800000,
                    .aram_audio_size = 0xb00000,
                    .aram_graph_size = 0xffffffff,
                    .aram_priorities = 8 << 16 | 7 << 8 | 6,
                    .aram_buffer_size = 0x2000,
                    .command_heap_size = 0x1000,
                    .archive_heap_size = 0x1620c00,
                    .zelda_heap_margin = 0x10000,
                    .audio_heap_size = 0x260800,
                    .vi_init = 0,
                    .vi_wait_for_retrace = 0,
                    .get_current_thread = 0x801da368,
                    .get_thread_priority = 0x801db5fc,
                    .create_root_heap = 0x8017ef68,
                    .create_exp_heap = 0x8017f0e0,
                    .create_solid_heap = 0x801811ac,
                    .create_aram = 0x80182888,
                    .set_aram_buffer = 0x801846a8,
                    .create_thread_switch = 0x80181ff8,
                    .create_command_heap = 0x80015658,
                    .create_archive_heap = 0x80015688,
                    .archive_heap_parent = 0x803e1aac, // JKRHeap::sRootHeap2
                    .create_assert_heap = 0x80015590,
                    .create_zelda_heap = 0x80015628,
                    .get_free_size = 0x8017d8f0,
                    .become_current_heap = 0x8017c9f4,
                    .system_heap = 0x803e1aa0,
                    .alloc = 0x8017e494,
                    .thread_ctor = 0x80181e30,
                    .create_disc_thread = 0x8001c044,
                    .audio_heap = 0x803e1294,
                    .audio_setup = 0x8000909c,
                    .init_flag = 0x803e1290,
                    .audio_frame = 0x8016f230,
                    .load_se_wave = 0,
                    .load_bgm_wave = 0,
                    .wave_status = 0,
                    .audio_manager = 0x802edd70,
                    .start_sound = 0x8016f448,
                    .erase_bgm_wave = 0,
                    .erase_se_wave = 0,
                    .first_scene_wave_count = 0,
                    .first_scene_waves = {},
                    .room_wave_count = 1,
                    .room_waves = {(0u << 16) | 0},
                    .music_manager = 0x803e12a8,    // JASGlobalInstance<Z2SeqMgr>::sInstance
                    .set_child_volume = 0x80159fc0, // Z2SeqMgr::setChildTrackVolume
                    .set_port_data = 0x8014f7a8,    // Z2SoundStarter::setPortData
                },
            .sleep = 0,
            .reset_data = 0x803e13a8,
            .reset_flag = 0x12,
            .alarm =
                {
                    .set_alarm = 0x801d1bac,
                    .disable_interrupts = 0x801d6abc,
                    .restore_interrupts = 0x801d6ae4,
                    .suspend_thread = 0x801db19c,
                    .resume_thread = 0x801daf04,
                },
            .wave_arcs =
                {
                    .section_heap = 0x803e1a20,
                    .bank_table = 0xf4,
                    .load = 0x80140984,
                    .load_tail = 0x80140a5c,
                    .erase = 0x80140b34,
                },
            .ios_version = 21,
            .watch =
                {
                    .parse_command = 0x8013c2b4,
                    .jump_handler = 0x8013ae68,
                    // JASSeqParser's handler for 0xC8 takes the jump here, with the track in r30 and the arguments in
                    // r31.
                    .conditional_jump = 0x8013af60,
                    .sequence_vtable = 0x802c6db8,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x802c6c28,
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8013ec98, 0x8013f2bc},
                    .wave_bank_table = 0x803e1a20,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0},
            .muted_starts = ntdlib::jaudio::kTwilightPrincessStarts,
            .piece_states = ntdlib::jaudio::kCrossbowTrainingStates,
        },
        {
            // Link's Crossbow Training's Japanese release. Its main.dol links Rev 1's code at other addresses.
            // They were found as for the European release. Its machine start-up gives the archive heap 0x160e800
            // bytes, not 0x1620c00. The other sizes and the analysis's offsets are Rev 1's.
            .name = "Link's Crossbow Training (Japan)",
            .game_id = "RZPJ01",
            .dol_sha1 = "85336539cbb66f4a33571ccc6888fed834505dec",
            .main = 0x800082d0,
            .z2 =
                {
                    .frame_retraces = 2,
                    .max_heaps = 1,
                    .system_heap_size = 0x800000,
                    .aram_audio_size = 0xb00000,
                    .aram_graph_size = 0xffffffff,
                    .aram_priorities = 8 << 16 | 7 << 8 | 6,
                    .aram_buffer_size = 0x2000,
                    .command_heap_size = 0x1000,
                    .archive_heap_size = 0x160e800,
                    .zelda_heap_margin = 0x10000,
                    .audio_heap_size = 0x260800,
                    .vi_init = 0,
                    .vi_wait_for_retrace = 0,
                    .get_current_thread = 0x801da4e8,
                    .get_thread_priority = 0x801db77c,
                    .create_root_heap = 0x8017f0e8,
                    .create_exp_heap = 0x8017f260,
                    .create_solid_heap = 0x8018132c,
                    .create_aram = 0x80182a08,
                    .set_aram_buffer = 0x80184828,
                    .create_thread_switch = 0x80182178,
                    .create_command_heap = 0x800156ac,
                    .create_archive_heap = 0x800156dc,
                    .archive_heap_parent = 0x803dca74, // JKRHeap::sRootHeap2
                    .create_assert_heap = 0x800155e4,
                    .create_zelda_heap = 0x8001567c,
                    .get_free_size = 0x8017da70,
                    .become_current_heap = 0x8017cb74,
                    .system_heap = 0x803dca68,
                    .alloc = 0x8017e614,
                    .thread_ctor = 0x80181fb0,
                    .create_disc_thread = 0x8001c098,
                    .audio_heap = 0x803dc254,
                    .audio_setup = 0x8000909c,
                    .init_flag = 0x803dc250,
                    .audio_frame = 0x8016f3b0,
                    .load_se_wave = 0,
                    .load_bgm_wave = 0,
                    .wave_status = 0,
                    .audio_manager = 0x802e8c30,
                    .start_sound = 0x8016f5c8,
                    .erase_bgm_wave = 0,
                    .erase_se_wave = 0,
                    .first_scene_wave_count = 0,
                    .first_scene_waves = {},
                    .room_wave_count = 1,
                    .room_waves = {(0u << 16) | 0},
                    .music_manager = 0x803dc268,    // JASGlobalInstance<Z2SeqMgr>::sInstance
                    .set_child_volume = 0x8015a140, // Z2SeqMgr::setChildTrackVolume
                    .set_port_data = 0x8014f928,    // Z2SoundStarter::setPortData
                },
            .sleep = 0,
            .reset_data = 0x803dc368,
            .reset_flag = 0x12,
            .alarm =
                {
                    .set_alarm = 0x801d1d2c,
                    .disable_interrupts = 0x801d6c3c,
                    .restore_interrupts = 0x801d6c64,
                    .suspend_thread = 0x801db31c,
                    .resume_thread = 0x801db084,
                },
            .wave_arcs =
                {
                    .section_heap = 0x803dc9e8,
                    .bank_table = 0xf4,
                    .load = 0x80140aec,
                    .load_tail = 0x80140bc4,
                    .erase = 0x80140c9c,
                },
            .ios_version = 21,
            .watch =
                {
                    .parse_command = 0x8013c41c,
                    .jump_handler = 0x8013afd0,
                    // JASSeqParser's handler for 0xC8 takes the jump here, with the track in r30 and the arguments in
                    // r31.
                    .conditional_jump = 0x8013b0c8,
                    .sequence_vtable = 0x802c1c98,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x802c1b08,
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8013ee00, 0x8013f424},
                    .wave_bank_table = 0x803dc9e8,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0},
            .muted_starts = ntdlib::jaudio::kTwilightPrincessStarts,
            .piece_states = ntdlib::jaudio::kCrossbowTrainingStates,
        },
        // As in the GameCube's Donkey Kong Jungle Beat, the sound library is JAudio2 of Twilight Princess's generation,
        // under the game's audio system. The start-up archive is compressed with Yaz0. The game loads it, decoded,
        // into its game heap. It mounts the sequence archive and the chord archive in its audio heap, and loads the
        // chord database there. Its 65 wave banks hold one group each. The sound library loads bank 2 with the
        // start-up archive, and the game loads the others per stage. The game's BGM player starts the music. It sets
        // each track of a BGM by tables in the game's code, and the driver starts music the same way. A chord effect
        // takes its keys from the chords of the music that plays. Its rip plays it over a stage's music with the
        // music's tracks off. The parameters follow driver/jungle_beat.c's Params from frame_ticks.
        {
            .name = "New Play Control! Donkey Kong Jungle Beat (USA)",
            .game_id = "R49E01",
            .dol_sha1 = "cdd123faa94ac8cff7c6c96c1de00bb42e5e3eeb",
            .main = 0x80226e00,
            .ios_version = 53,
            .watch =
                {
                    .parse_command = 0x802d0e60,
                    .jump_handler = 0x802cf880,
                    // JASSeqParser's handler for 0xC8 (0x802cf8a0) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x802cf8cc,
                    .idle_in_calls = true, // an effect's root track waits in a call until its child tracks end
                    .sequence_vtable = 0x804658c8,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x80465708, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x802d3c00, 0x802d4190},
                    .wave_bank_table = 0x805035a8,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0},
            .driver = JAudioDriver::kJungleBeat,
            .params =
                {
                    kFrameTicks,          // frame_ticks: the game runs at 60 frames a second
                    0xd80000,             // aram_audio_size
                    0xffffffff,           // aram_graph_size: the rest of the game's "ARAM"
                    8 << 16 | 7 << 8 | 6, // aram_priorities
                    0x18,                 // heaps_size
                    0x803299cc,           // DVDInit
                    0x80228700,           // create_root_heap: JKRExpHeap::createRoot(1, true)
                    0x8027df20,           // operator new
                    0x80228710,           // heaps_ctor: the game heap, the audio heap and the "ARAM" in MEM2
                    0x80502f18,           // heaps
                    0x8027d970,           // JKRHeap::becomeCurrentHeap
                    0x80503134,           // JKRHeap::sCurrentHeap
                    0x80281e40,           // JKRAram::create
                    0x80315974,           // OSSleepTicks
                    0x8027df90,           // operator delete
                    0x80328efc,           // DVDConvertPathToEntrynum
                    0x802885c0,           // JKRDvdRipper::loadToMainRAM
                    0x802843c0,           // JKRArchive::mount
                    0x8045b858,           // "/AudioRes/Donkey.szs"
                    0x8045b8d8,           // "/AudioRes/Seqs/JaiSeq.arc"
                    0x8045b958,           // "/AudioRes/Info/JaiChord.arc"
                    0x8045b9d8,           // "/AudioRes/Info/ChordSeDataBase.csd"
                    0x80265550,           // create_audio_system
                    0x80266cf0,           // the audio system's frame process
                    0x80502b88,           // speaker_manager: the Wii Remote speaker's manager
                    0x1060,               // speaker_offset: the audio system's pointer to it
                    0x805035a8,           // JASGlobalInstance<JAUSectionHeap>::sInstance
                    0xfc,                 // wave_banks_offset
                    0x802e1190,           // JAUSection::loadWaveArc with a mask of archives
                    0x4c,                 // wave_arc_state_offset
                    1,                    // music_section: music's sound IDs are 0x01xxxxxx
                    0xd18,                // music_player_offset: the audio system's BGM player
                    0x803ab6b0,           // music_tables: each BGM's tables of track states
                    0x803acc44,           // music_states: the second table's states
                    0x80279b20,           // set_music_state: the BGM player's function for a state of the second table
                    0xd10,                // chord_offset: the audio system's chord effects
                    164,                  // chord_records: the database's count says 218
                    0x8027c050,           // start_chord_effect
                    0xd14,                // stage_offset: the audio system's stage object
                    0x80277590,           // start_stage_music
                    0x80279270,           // set_track_states: the BGM player's function for a code for each track
                    0x818,                // rhythm_offset: the audio system's rhythm
                    0x80270260,           // rhythm_frame: the rhythm's frame process
                    0x50042,              // swapped_effect: JA_SE_IST_AUTO_COMP_ALTO_SAX
                    0x50063,              // swaps: JA_SE_IST_AUTO_COMP_E_PIANO_T
                    0x2c,                 // at stage 0x2c (TropicalCoast)
                    0x50062,              // JA_SE_IST_AUTO_COMP_E_PIANO
                    0x35,                 // at stage 0x35 (SwimRace)
                    0x70,                 // combo_effect: JA_SE_EAT_BIND_COMBO
                    0x802678b0,           // start_with_count: the audio system's start of a sound with a count
                    0,                    // music: written by the driver
                    0x802686b0,           // write_port: writes a port of a sound that plays
                    // counts and ports: effects whose sequences add a value at port 0xb to a key, each with the value
                    // that the game's code gives it first. The game starts the first five with a count, and
                    // start_with_count writes the count there. A game object writes the sixth's value after the start
                    // (0x8026e2b0, from 0x8026aea0).
                    // JA_SE_PAUSE_MENU_CLAP_PARAM: the opening ceremony's count + 1 (0x801cf75c). The count starts at
                    // 0 (0x801ced3c). With 0 at the port, the effect plays no note.
                    0x8e,
                    1,
                    // JA_SE_STAGE_RESULT_DEC: always 7 (0x801d53c0).
                    0xcc,
                    7,
                    // JA_SE_BLACK_FAIRY_REFLECT: (the reflections + 1) * 2 (0x8013c4a0). They start at 0 (0x8013a5b8).
                    0x169,
                    2,
                    // JA_SE_BEAT_POINT_ADD: the points added + 60 (0x80268124). They start at 0 (0x802680b4).
                    0x37,
                    60,
                    // JA_SE_MOOSHIN_CONTINUOUS_JUMP_H: a count of jumps, from 8 (0x802123dc). The count starts at 0
                    // (0x801e4db4) and rises by 1 after each jump's effect (0x801e5370).
                    0x18a,
                    8,
                    // JA_SE_OBJ_SOUND_BLOCK_DISAPPEAR: the entry of the table at 0x80500780 (8, 4, 1, 0) for the
                    // block's step (0x8026bf6c). The step starts at 0 (0x8026aa10) and goes on by 1 after each block
                    // (0x8026aae0). It goes back to 0 after 90 frames without one (0x8026aa80).
                    0x4000f,
                    8,
                    // The states that the game sets for its music during play (ntdlib::jaudio::kJungleBeatWiiStates).
                    0x80279cf0, // combo_state: the BGM player's state of the first table for a combo's count
                    0x80278150, // combo_threshold: the count in the stage row for the layer of a combo
                    30,         // combo_stage: MorningField, whose combos reach their layers at counts of 10, 45 and 80
                    0x802799d0, // flags_state: the BGM player's state of the first table for the player's flags
                    0x164,      // flags_offset
                    // The chord effects that objects play beside a stage's music (ntdlib's kJungleBeatWiiChordLayers).
                    // The table of chord layers follows. The objects that start an effect once do it through
                    // start_chord_effect.
                    0x8027c0b0, // keep_chord_effect: starts a record's effect, or keeps it playing, for an object
                                // that asks once a frame (0x802615f0)
                    0x80273f40, // chord_effect_playing: whether a record's effect plays
                    0x805012d8, // game_system: the variable that holds the game's system object (main, 0x80226ea4)
                    0x24,       // course_offset: the system object's course record (0x80227be8)
                    0x80230410, // course_ctor: the course record's constructor
                },
            .background_sound = 50, // the driver's `music`
            .track_parent_offset = 0x12c,
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/ChordSeDataBase.csd"},
            .layer_states = ntdlib::jaudio::kJungleBeatWiiStates,
            .chord_layers = ntdlib::jaudio::kJungleBeatWiiChordLayers,
        },
        // New Play Control! Pikmin (USA). The game's sound library is the early JAudio1 of the GameCube release. The
        // game calls the library through C functions (Jac, Jaf, Jaq and Jam). The game's code holds the sequence index.
        // The game plays a piece of music by the piece's number in the index. The piece plays in one of two slots, with
        // group 0 of the wave system of the same number. The disc has no map. Each function in main.dol was matched
        // with the function at the same place in the GameCube release's sound library. The sizes come from the game's
        // start-up, System::Initialise (0x800dc878). The parameters follow driver/pikmin.c's Params from frame_ticks.
        {
            .name = "New Play Control! Pikmin (USA)",
            .game_id = "R9IE01",
            .dol_sha1 = "551306334183a633d0fde434a930b98809433b70",
            .library = JAudioLibrary::kJac,
            .sound_archive = "pikmin1/US/dataDir/SndData/Banks/pikibank.bx",
            .sequence_archive = "pikmin1/US/dataDir/SndData/Seqs/pikiseq.arc",
            .sequence_index = 0x80316bc0,
            // The music of the cutscenes: demobgm, in parts. Jac_InitBgm (0x8001bf60) starts demobgm when the sound
            // library starts. A cutscene's entry in the table at 0x803165a4 gives the part to play. __Prepare_BGM
            // (0x8001e8a0) loads the part's group of wave system 13. Part 2 plays bank 14. Its waves are in the
            // diary's wave system, 14. Parts 3 and 4 play part 5's tracks without a wave group. No cutscene plays them.
            // The table and the parts hold the GameCube release's data.
            .parted_piece = 13,
            .parts = {{1, 13, 7}, {2, 14, 0}, {5, 13, 3}, {6, 13, 4}, {7, 13, 5}, {8, 13, 6}, {9, 13, 1}, {10, 13, 8}},
            // The game's catalogue of effects, and the first action of each type of event. The catalogue holds the
            // GameCube release's rows, with one more at row 309: the system's effect 41, "Change Piki". The table of
            // actions holds the GameCube's entries. ntdlib's lists give the effects that the game's code starts itself
            // and the ones that the ripper leaves out.
            .jac_effects =
                {
                    .table = 0x80341868,
                    .count = 332,
                    .event_offsets = 0x80315688,
                    .other_effects = ntdlib::jaudio::kPikminOtherEffects,
                    .left_out = ntdlib::jaudio::kPikminLeftOutEffects,
                    .sounding_event = ntdlib::jaudio::kPikminSoundingEvent,
                    .quiet_event = ntdlib::jaudio::kPikminQuietEvent,
                },
            // The game's set-up of its first scene of play loads groups 1 to 3 of wave system 0, the effects', and
            // group 0 of systems 14, 15 and 2 (from 0x8001d5a0).
            .effect_groups = {0 << 16 | 1, 0 << 16 | 2, 0 << 16 | 3, 14 << 16 | 0, 15 << 16 | 0, 2 << 16 | 0},
            .main = 0x80006de0,
            .ios_version = 33,
            .watch =
                {
                    .parse_command = 0x80016068, // the head of the loop over a track's commands in Jam_SeqmainNote
                    .jump_handler = 0x800147b0,  // Cmd_CallF, for the jump (0xC8) and the call (0xC4), at the jump
                    .command_track_register = 20,
                    .jump_track_register = 3,
                    .jump_target_register = 30,
                    .depth_offset = 8,
                    .relative_positions = true,
                    .wait_offset = 0x8c,
                    .note_wait_offset = 0,
                    .jaudio1 = true,
                    // The driver's word `sound` points at a word that holds the sound's root track.
                    .parameters_offset = 0,
                    .parameters_track_offset = 0,
                    .parent_offset = 0x40,
                    .children_offset = 0x44,
                },
            // Jac_Start loads group 0 of wave system 0. That system holds the effects.
            .static_waves = {0 << 16 | 0},
            .driver = JAudioDriver::kPikmin,
            .params =
                {
                    kFrameTicks, // frame_ticks: 60 polls a second. The sound library's threads play the music.
                    0x80000,     // audio_heap_size: as System::Initialise gives Jac_Start
                    0x20,        // audio_heap_align
                    0x80000,     // disc_buffer_size: the game's system object gives 0x2000 units of 64 bytes
                    0x90801600,  // aram_arena_lo: MEM2's arena from here, above the ARAM emulation's 8 MB
                    0x980000,    // aram_size: as System::Initialise gives Jac_Start
                    0x20,        // aram_align
                    0x802ff267,  // sound_path: "/pikmin1/US/dataDir/SndData/"
                    0,           // music_slot
                    4,           // music_handle: the slot's root sequence (Jac_PlayBgm plays slot n with handle n + 4)
                    0x30000,     // part_track: the name that demobgm's root track takes. Jac_StartDemo writes to it.
                    0x80058224,  // sleep_ticks: OSSleepTicks
                    0x8004e7e8,  // alloc_mem1: OSAllocFromMEM1ArenaLo
                    0x8004e7d8,  // set_mem2_arena_lo: OSSetMEM2ArenaLo
                    0x8004e81c,  // alloc_mem2: OSAllocFromMEM2ArenaLo
                    0x8001a4e0,  // set_aram_base: the sound library's setter. System::Initialise calls it.
                    0x8001a360,  // start: Jac_Start
                    0x8001a5e0,  // add_disc_buffer: Jac_AddDVDBuffer
                    0x8049e1a4,  // boot_ok: the flag that __BootSoundOK (0x8001a300) sets
                    0x8001c160,  // ready_bgm: Jac_ReadyBgm
                    0x80010580,  // collect_aram: Collect_AramMotherHeap
                    0x8000f3a0,  // set_wave_scene: WaveScene_Set
                    0x80009c60,  // check_pass: DVDT_CheckPass
                    0x8001c200,  // play_bgm: Jac_PlayBgm
                    0x8001f220,  // handle_to_sequence: Jaf_HandleToSeq
                    0x800133c0,  // get_track_handle: Jam_GetTrackHandle
                    0x80013040,  // write_port: Jam_WritePortAppDirect
                    0x8000f3c0,  // load_wave_scene: WaveScene_Load
                    0x80497450,  // scene: the game's scene, which the getter at 0x8001d1e0 returns
                    0x8049e198,  // menu: Jac_PlaySystemSe plays effect 7, which closes the menu, only while it's set
                    0x80019c00,  // play_system_effect: Jac_PlaySystemSe
                    0x8001bac0,  // play_player_effect: Jac_PlayOrimaSe
                    0x8001ac00,  // create_event: Jac_CreateEvent
                    0x8001aee0,  // play_event_action: Jac_PlayEventAction
                    0x8001aa40,  // update_camera: Jac_UpdateCamera
                    0x10009,     // system_track: the effects sequence's track for the system's effects
                    0x1000a,     // player_track: its track for the player's effects
                    0x20000,     // events_track: the events' root track. Its children play the events
                    0x8001b8c0,  // move_formation: Jac_Orima_Formation
                    0x10007,     // formation_track: the effects sequence's track for the formation
                },
        },
        // New Play Control! Pikmin 2 (USA), the GameCube's Pikmin 2 built for the Wii. Its sound library is a later
        // JAudio1, under the game's sound layer (PSSystem, PSGame and PSM). The start-up archive is an AAF. The game
        // starts its music by file name, through the objects of its sound scenes (driver/pikmin2.c). The sound table
        // lists its effects, and PSound.asn names them. Wave system 0 has 49 groups, one .aw each. wScene_0.aw holds
        // most effects and stays loaded, and a scene loads at most two others. Wave system 1 holds the Home Button
        // menu's sounds. The global scene loads it too. The game keeps its files in the disc's folder pikmin2. The
        // parameters follow driver/pikmin2.c's Params from frame_ticks to get_random. The table of pieces follows them.
        {
            .name = "New Play Control! Pikmin 2 (USA)",
            .game_id = "R92E01",
            .dol_sha1 = "d69f9083b7c47665a4f701099ec029176f2e3aeb",
            .library = JAudioLibrary::kAaf,
            .sound_archive = "pikmin2/AudioRes/PSound.aaf",
            .sequence_archive = "pikmin2/AudioRes/Seqs/Seq.arc",
            .sound_names = "pikmin2/AudioRes/PSound.asn",
            .main = 0x80497f08,
            .ios_version = 33,
            .watch =
                {
                    .parse_command = 0x8005025c, // the head of the loop over a track's commands in the sequence parser
                    .jump_handler = 0x8004ec34,  // the jump in the sequence parser's handler for 0xC8
                    .command_track_register = 30,
                    .jump_track_register = 31,
                    .jump_target_register = 25,
                    .reader_offset = 0xc,
                    .depth_offset = 0x18,
                    .interrupt_offset = 0x50, // JASSeqCtrl's saved position, +0x44 in the sequence control
                    .idle_in_calls = true,    // the root tracks poll the game once a beat in a call that never returns
                    .wait_offset = 0x14,
                    .wave_handle_functions = {0x8004c140, 0x8004d7a4}, // the two wave banks' getWaveHandle
                    .wave_bank_table = 0x8066b9e4,                     // JASWaveBankMgr::sWaveBank
                    .jaudio1 = true,
                    .parameters_offset = 0x48,
                    .parameters_inline = true,
                    .parameters_track_offset = 0x2c4,
                    .effects_sequence = 0x8066bb68, // JAInter::SeMgr::seHandle
                    .effect_slot_offset = 0x14,     // JAISound's track number, after its JSULink and its vtable
                    .parent_offset = 0x2f8,
                    .children_offset = 0x2fc,
                },
            .static_waves = {0 << 16 | 0, 1 << 16 | 0},
            .driver = JAudioDriver::kPikmin2,
            .params = Pikmin2Parameters({
                2 * kFrameTicks,      // frame_ticks: the game's areas and caves run at 30 frames a second
                1,                    // max_heaps
                0xa0000,              // system_heap_size
                0xc00000,             // aram_audio_size
                0x700000,             // aram_graph_size
                8 << 16 | 7 << 8 | 6, // aram_priorities
                0x7c,                 // thread_size
                0x200000,             // heap_margin
                0xd4800,              // file_heap_size: the game's heap for 2D resources
                0x1c,                 // factory_size
                0xc00000,             // sound_memory
                0x38,                 // scene_info_size
                17,                   // scene_type: the challenge mode's menu
                0x24,                 // battle_directors_size
                0xb8,                 // directed_bgm_size
                0x350,                // auto_bgm_size
                0x80117308,           // DVDInit
                0x80116cc0,           // DVDChangeDir
                0x80558eba,           // game_folder: "pikmin2"
                0x800f0a14,           // JFWDisplay::threadSleep
                0x8013539c,           // OSGetCurrentThread
                0x801338e8,           // OSInitMutex
                0x80084768,           // create_mem1_root_heap
                0x800846e8,           // JKRExpHeap::createRoot: MEM2's root heap
                0x800847e8,           // JKRExpHeap::create
                0x80089320,           // JKRSolidHeap::create
                0x80087d64,           // JKRHeap::getFreeSize
                0x80087c08,           // JKRHeap::becomeSystemHeap
                0x80087c18,           // JKRHeap::becomeCurrentHeap
                0x80087c28,           // JKRHeap::destroy
                0x800894f4,           // JKRSolidHeap::adjustSize
                0x80088948,           // operator new
                0x80089d04,           // JKRThread::JKRThread
                0x8007c4f8,           // JKRAram::create
                0x80086374,           // JKRFileCache::mount
                0x805593f9,           // audio_folder: "/pikmin2/AudioRes"
                0x8008759c,           // JKRFileLoader::getGlbResource
                0x8055940b,           // start_up_archive: "PSound.aaf"
                0x803b43a8,           // factory_ctor
                0x805ccaec,           // factory_vtable
                0x804f67e8,           // make_se
                0x803b43dc,           // new_sound_system
                0x8066cea0,           // scene_manager
                0x8066ce70,           // system_interface
                0x803b89d0,           // main_loop
                0x803b5628,           // new_bgm_seq
                0x804df7c8,           // new_main_bgm
                0x804df8b0,           // new_directed_bgm
                0x804e1994,           // new_auto_bgm
                0x804dfa78,           // new_game_scene
                0x803c0838,           // append_seq
                0x803c0718,           // adapt_child_scene
                0x803b4b24,           // scene_info_ctor
                0x803b2120,           // directed_bgm_ctor
                0x803bb1b8,           // auto_bgm_ctor
                0x804f5f70,           // battle_directors_ctor
                0x803c2390,           // adapt_directors
                0x803b2b84,           // start_jump_bgm
                0x8066ce80,           // conductor_heap
                0x803b85f8,           // play_system_se
                0x8066ca98,           // play_data
                0x8066ce98,           // rappas
                0x8066d368,           // big_boss
                0x803bce28,           // load_wave_scene: PSSystem::WaveScene::load
                1 << 16 | 0,          // menu_waves: the Home Button menu's wave group
                0x8004e508,           // get_random: JASPlayer::getRandomS32
            }),
            .files = {"pikmin2/AudioRes/Seqs/Seq.arc", "pikmin2/AudioRes/Conductor.arc", "pikmin2/AudioRes/Key.arc",
                      "pikmin2/user/Totaka/BgmList.txt", "pikmin2/user/Totaka/StreamList.txt",
                      "pikmin2/user/Totaka/trackMap_Cond_T.txt", "pikmin2/user/Totaka/trackMap_Seq_T.txt",
                      "pikmin2/user/Wakai/trackMap_Cond_W.txt", "pikmin2/user/Wakai/trackMap_Seq_W.txt"},
            // Three effects draw a random number from JASPlayer::getRandomS32 (0x8004e508) and end at once on some
            // draws. From the generator's first state, the first draw ends all three. Each one lists the draws before
            // the first draw on which it plays. Each plays the same sound on every draw that doesn't end it.
            .random_starts =
                {
                    {0x5829, 3}, // PSSE_EN_FROG_VOICE
                    {0x58fc, 1}, // PSSE_EN_OOPAN_BOUND2
                    {0x58fe, 1}, // PSSE_EN_OOPAN_VOICE_DOWN
                },
        },
        {
            // The game's start-up gives the sizes of ARAM and the heaps' object. The audio system loads wave bank 7
            // itself, and the scene manager loads banks 0 to 6, 8 and 11 before the first scene. The game loads the
            // others per stage and per scenario. Every bank of those holds one group.
            .name = "Super Mario Galaxy (USA)",
            .game_id = "RMGE01",
            .dol_sha1 = "9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd",
            .main = 0x80399510, // main's call of DVDInit, after main sets up the quantization registers
            .ios_version = 33,
            .watch =
                {
                    .parse_command = 0x8048dc98,
                    .jump_handler = 0x8048c7f4,
                    // JASSeqParser's handler for 0xC8 (0x8048c80c) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x8048c838,
                    .sequence_vtable = 0x805da424,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x805da368, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x80490564, 0x80490a8c},
                    .wave_bank_table = 0x806a2c28,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {7 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0, 2 << 16 | 0, 3 << 16 | 0, 4 << 16 | 0, 5 << 16 | 0,
                             6 << 16 | 0, 8 << 16 | 0, 11 << 16 | 0},
            .driver = JAudioDriver::kGalaxy,
            // driver/galaxy.c's block after the wave list, word by word.
            .params =
                {
                    kFrameTicks, // the game runs its audio frame process at 60 Hz
                    0xe00000,    // ARAM for the sound library
                    0xffffffff,  // ARAM for graphics: the rest
                    8 << 16 | 7 << 8 | 3,
                    0x28,       // HeapMemoryWatcher's size
                    0x804bfd58, // DVDInit
                    0x8039ed34, // HeapMemoryWatcher::createRootHeap
                    0x80409af8, // operator new
                    0x8039eee4, // HeapMemoryWatcher's constructor
                    0x806a245c, // SingletonHolder<HeapMemoryWatcher>::sInstance
                    0x0,        // the stationed heap in MEM1
                    0x24,       // the audio system's heap
                    0x8040963c, // JKRHeap::becomeCurrentHeap
                    0x8040d1b8, // JKRAram::create
                    0x804ac2b4, // OSSleepTicks
                    0x80409b40, // operator delete
                    0x804bef5c, // DVDConvertPathToEntrynum
                    0x804134ac, // JKRDvdRipper::loadToMainRAM
                    0x8040f528, // JKRArchive::mount
                    0x80551608, // "/AudioRes/SMR.szs"
                    0x8055161a, // "/AudioRes/Seqs/JaiSeq.arc"
                    0x80551634, // "/AudioRes/Info/JaiChord.arc"
                    0x80551650, // "/AudioRes/Info/JaiMe.arc"
                    0x80551669, // "/AudioRes/Info/JaiRemixSeq.arc"
                    0x80026158, // AudNewAudSystem
                    0x80026c34, // AudSystem::frameWork
                    0x132c,     // AudSystem's SpkSystem
                    0x9f8,      // AudSystem's AudSceneMgr
                    0x800316fc, // AudSceneMgr::isLoadDoneSystemInit
                    0x8003170c, // AudSceneMgr::loadStaticResource
                    0x800317a0, // AudSceneMgr::isLoadDoneStaticResource
                    0x806a2c28, // JASGlobalInstance<JAUSectionHeap>::sInstance
                    0x8049cfac, // JAUSection::loadWaveArc(bank, mask)
                    0x8049dadc, // JAUSectionHeap::isWaveLoaded
                    1,          // the section of music sound IDs
                    0x8002a72c, // AudWrap::startStageBgm
                    0x80026d54, // AudSystem::startSound
                    0x806a2de8, // THD_TIME_TO_DIMMING, VI's threshold
                    0x80027358, // AudSystem::initSceneVolume
                    0x9ec,      // AudSystem's AudSoundObject for system sound effects
                    0x8002b490, // AudSoundObject::setCutoffToPort
                    0x80031fc8, // AudSceneMgr::setPlayerModeLuigi
                    0,          // Luigi mode: set by a Luigi variant's .miniwsf
                },
            // AudSystem::startSound (0x80026d54) calls AudSystem::switchToLuigi (0x80027024). In Luigi mode, that
            // function writes 20 to port 0xc of a sound of group 1, the player's voices.
            .luigi_word = 42,
            .luigi_group = 0x0001,
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/JaiMe.arc",
                      "AudioRes/Info/JaiRemixSeq.arc"},
        },
        {
            // Super Mario Galaxy's European release. Its main.dol links the USA release's code, some of it at other
            // addresses. Each address comes from the USA release's code, matched with the linker's fields masked.
            // The sizes are the USA release's.
            .name = "Super Mario Galaxy (Europe)",
            .game_id = "RMGP01",
            .dol_sha1 = "9bd1daa520bd1f573b6a4b82e03336e9c51b511f",
            .main = 0x8039952c, // main's call of DVDInit, after main sets up the quantization registers
            .ios_version = 33,
            .watch =
                {
                    .parse_command = 0x8048dcb4,
                    .jump_handler = 0x8048c810,
                    // JASSeqParser's handler for 0xC8 (0x8048c828) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x8048c854,
                    .sequence_vtable = 0x805da424,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x805da368, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x80490580, 0x80490aa8},
                    .wave_bank_table = 0x806a2c28,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {7 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0, 2 << 16 | 0, 3 << 16 | 0, 4 << 16 | 0, 5 << 16 | 0,
                             6 << 16 | 0, 8 << 16 | 0, 11 << 16 | 0},
            .driver = JAudioDriver::kGalaxy,
            // driver/galaxy.c's block after the wave list, word by word.
            .params =
                {
                    kFrameTicks, // the game runs its audio frame process at 60 Hz
                    0xe00000,    // ARAM for the sound library
                    0xffffffff,  // ARAM for graphics: the rest
                    8 << 16 | 7 << 8 | 3,
                    0x28,       // HeapMemoryWatcher's size
                    0x804bfd58, // DVDInit
                    0x8039ed50, // HeapMemoryWatcher::createRootHeap
                    0x80409b14, // operator new
                    0x8039ef00, // HeapMemoryWatcher's constructor
                    0x806a245c, // SingletonHolder<HeapMemoryWatcher>::sInstance
                    0x0,        // the stationed heap in MEM1
                    0x24,       // the audio system's heap
                    0x80409658, // JKRHeap::becomeCurrentHeap
                    0x8040d1d4, // JKRAram::create
                    0x804ac2b4, // OSSleepTicks
                    0x80409b5c, // operator delete
                    0x804bef5c, // DVDConvertPathToEntrynum
                    0x804134c8, // JKRDvdRipper::loadToMainRAM
                    0x8040f544, // JKRArchive::mount
                    0x80551608, // "/AudioRes/SMR.szs"
                    0x8055161a, // "/AudioRes/Seqs/JaiSeq.arc"
                    0x80551634, // "/AudioRes/Info/JaiChord.arc"
                    0x80551650, // "/AudioRes/Info/JaiMe.arc"
                    0x80551669, // "/AudioRes/Info/JaiRemixSeq.arc"
                    0x80026158, // AudNewAudSystem
                    0x80026c34, // AudSystem::frameWork
                    0x132c,     // AudSystem's SpkSystem
                    0x9f8,      // AudSystem's AudSceneMgr
                    0x800316fc, // AudSceneMgr::isLoadDoneSystemInit
                    0x8003170c, // AudSceneMgr::loadStaticResource
                    0x800317a0, // AudSceneMgr::isLoadDoneStaticResource
                    0x806a2c28, // JASGlobalInstance<JAUSectionHeap>::sInstance
                    0x8049cfac, // JAUSection::loadWaveArc(bank, mask)
                    0x8049dadc, // JAUSectionHeap::isWaveLoaded
                    1,          // the section of music sound IDs
                    0x8002a72c, // AudWrap::startStageBgm
                    0x80026d54, // AudSystem::startSound
                    0x806a2de8, // THD_TIME_TO_DIMMING, VI's threshold
                    0x80027358, // AudSystem::initSceneVolume
                    0x9ec,      // AudSystem's AudSoundObject for system sound effects
                    0x8002b490, // AudSoundObject::setCutoffToPort
                    0x80031fc8, // AudSceneMgr::setPlayerModeLuigi
                    0,          // Luigi mode: set by a Luigi variant's .miniwsf
                },
            // AudSceneMgr::setPlayerModeLuigi and AudSystem::switchToLuigi are at the USA addresses. Their code is the
            // USA code, apart from the targets of switchToLuigi's calls.
            .luigi_word = 42,
            .luigi_group = 0x0001,
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/JaiMe.arc",
                      "AudioRes/Info/JaiRemixSeq.arc"},
        },
        {
            // Super Mario Galaxy's Japanese release. Its addresses were found as for the European release. The sizes
            // are the USA release's.
            .name = "Super Mario Galaxy (Japan)",
            .game_id = "RMGJ01",
            .dol_sha1 = "604d0b47d446caec0aab9ac370014f132e1b4016",
            .main = 0x80399510, // main's call of DVDInit, after main sets up the quantization registers
            .ios_version = 33,
            .watch =
                {
                    .parse_command = 0x8048dc94,
                    .jump_handler = 0x8048c7f0,
                    // JASSeqParser's handler for 0xC8 (0x8048c808) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x8048c834,
                    .sequence_vtable = 0x805da424,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x805da368, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x80490560, 0x80490a88},
                    .wave_bank_table = 0x806a2c28,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {7 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0, 2 << 16 | 0, 3 << 16 | 0, 4 << 16 | 0, 5 << 16 | 0,
                             6 << 16 | 0, 8 << 16 | 0, 11 << 16 | 0},
            .driver = JAudioDriver::kGalaxy,
            // driver/galaxy.c's block after the wave list, word by word.
            .params =
                {
                    kFrameTicks, // the game runs its audio frame process at 60 Hz
                    0xe00000,    // ARAM for the sound library
                    0xffffffff,  // ARAM for graphics: the rest
                    8 << 16 | 7 << 8 | 3,
                    0x28,       // HeapMemoryWatcher's size
                    0x804bfd38, // DVDInit
                    0x8039ed34, // HeapMemoryWatcher::createRootHeap
                    0x80409af4, // operator new
                    0x8039eee4, // HeapMemoryWatcher's constructor
                    0x806a245c, // SingletonHolder<HeapMemoryWatcher>::sInstance
                    0x0,        // the stationed heap in MEM1
                    0x24,       // the audio system's heap
                    0x80409638, // JKRHeap::becomeCurrentHeap
                    0x8040d1b4, // JKRAram::create
                    0x804ac294, // OSSleepTicks
                    0x80409b3c, // operator delete
                    0x804bef3c, // DVDConvertPathToEntrynum
                    0x804134a8, // JKRDvdRipper::loadToMainRAM
                    0x8040f524, // JKRArchive::mount
                    0x80551608, // "/AudioRes/SMR.szs"
                    0x8055161a, // "/AudioRes/Seqs/JaiSeq.arc"
                    0x80551634, // "/AudioRes/Info/JaiChord.arc"
                    0x80551650, // "/AudioRes/Info/JaiMe.arc"
                    0x80551669, // "/AudioRes/Info/JaiRemixSeq.arc"
                    0x80026158, // AudNewAudSystem
                    0x80026c34, // AudSystem::frameWork
                    0x132c,     // AudSystem's SpkSystem
                    0x9f8,      // AudSystem's AudSceneMgr
                    0x800316fc, // AudSceneMgr::isLoadDoneSystemInit
                    0x8003170c, // AudSceneMgr::loadStaticResource
                    0x800317a0, // AudSceneMgr::isLoadDoneStaticResource
                    0x806a2c28, // JASGlobalInstance<JAUSectionHeap>::sInstance
                    0x8049cf8c, // JAUSection::loadWaveArc(bank, mask)
                    0x8049dabc, // JAUSectionHeap::isWaveLoaded
                    1,          // the section of music sound IDs
                    0x8002a72c, // AudWrap::startStageBgm
                    0x80026d54, // AudSystem::startSound
                    0x806a2de8, // THD_TIME_TO_DIMMING, VI's threshold
                    0x80027358, // AudSystem::initSceneVolume
                    0x9ec,      // AudSystem's AudSoundObject for system sound effects
                    0x8002b490, // AudSoundObject::setCutoffToPort
                    0x80031fc8, // AudSceneMgr::setPlayerModeLuigi
                    0,          // Luigi mode: set by a Luigi variant's .miniwsf
                },
            // AudSceneMgr::setPlayerModeLuigi and AudSystem::switchToLuigi are at the USA addresses. Their code is the
            // USA code, apart from the targets of switchToLuigi's calls.
            .luigi_word = 42,
            .luigi_group = 0x0001,
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/JaiMe.arc",
                      "AudioRes/Info/JaiRemixSeq.arc"},
        },
        {
            // The game loads its sound files and its audio data tables through its file loader. The audio system
            // loads wave bank 7 itself. The driver loads no stationed waves: each rip lists the wave groups that its
            // sound uses.
            .name = "Super Mario Galaxy 2 (USA)",
            .game_id = "SB4E01",
            .dol_sha1 = "7d79810889824edd01c4ff02624713060a69818c",
            .main = 0x804b6410, // main's call of DVDInit, after main sets up the quantization registers
            .ios_version = 56,
            .watch =
                {
                    .parse_command = 0x8058a040,
                    .jump_handler = 0x80588a60,
                    // JASSeqParser's handler for 0xC8 (0x80588a80) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x80588aac,
                    .sequence_vtable = 0x8070bdb0,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x8070bd08, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8058cb00, 0x8058d0a0},
                    .wave_bank_table = 0x807d6568,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {7 << 16 | 0},
            .driver = JAudioDriver::kGalaxy2,
            // driver/galaxy2.c's block after the wave list, word by word.
            .params =
                {
                    kFrameTicks, // the game runs its audio frame process at 60 Hz
                    0xdb0000,    // ARAM for the sound library
                    0xffffffff,  // ARAM for graphics: the rest
                    8 << 16 | 7 << 8 | 3,
                    0x30,       // HeapMemoryWatcher's size
                    0x20000,    // FileRipper's buffer
                    0x3c,       // GameSystem's size
                    0x44,       // GameSystemObjHolder's size
                    0x34,       // AudSystemWrapper's size
                    0x21,       // the game's number for US English
                    0x805d1e10, // DVDInit
                    0x804bcda0, // HeapMemoryWatcher::createRootHeap
                    0x80501850, // operator new
                    0x804bcf90, // HeapMemoryWatcher's constructor
                    0x807d0dac, // SingletonHolder<HeapMemoryWatcher>::sInstance
                    0x0,        // the stationed heap in MEM1
                    0x4,        // the stationed heap in MEM2
                    0x2c,       // the audio system's heap
                    0x805012e0, // JKRHeap::becomeCurrentHeap
                    0x804b1f00, // FileRipper::setup
                    0x80505280, // JKRAram::create
                    0x804b90b0, // GameSystemObjHolder::initDvd
                    0x807d0da4, // SingletonHolder<GameSystem>::sInstance
                    0x20,       // GameSystem's GameSystemObjHolder
                    0x38,       // the holder's language
                    0x20,       // the holder's AudSystemWrapper
                    0x804ae050, // AudSystemWrapper's constructor
                    0x804ae0f0, // AudSystemWrapper::requestResourceForInitialize
                    0x804ae1c0, // AudSystemWrapper::createAudioSystem
                    0x805bdcc0, // OSSleepTicks
                    0x80078b20, // AudSystem::frameWork
                    0x9a8,      // AudSystem's SpkSystem
                    0x804ae340, // AudSystemWrapper::isLoadDoneWaveDataAtSystemInit
                    0x804ae370, // the wrapper's step after that bank loads
                    0x80089480, // the reader of BgmParam.arc
                    0x80083ce0, // the reader of MultiBgmInfo.arc
                    0x8008a120, // the reader of StageBgmInfo.arc
                    0x807d6568, // JASGlobalInstance<JAUSectionHeap>::sInstance
                    0x80599de0, // JAUSection::loadWaveArc(bank, mask)
                    0x8059a8f0, // JAUSectionHeap::isWaveLoaded
                    1,          // the section of music sound IDs
                    0x8007c8d0, // AudWrap::startStageBgm
                    0x80078cc0, // AudSystem::startSound
                    0x807d67a0, // THD_TIME_TO_DIMMING, VI's threshold
                    0x800793c0, // AudSystem::initSceneVolume
                    0x950,      // AudSystem's AudSoundObject for system sound effects
                    0x8007dbb0, // AudSoundObject::setCutoffToPort
                    0x95c,      // AudSystem's AudSceneMgr
                    0x800858d0, // AudSceneMgr::setPlayerModeLuigi
                    0,          // Luigi mode: set by a Luigi variant's .miniwsf
                },
            // AudSystem::startSound (0x80078cc0) calls AudSystem::switchToLuigi (0x80079000), as in Galaxy.
            .luigi_word = 49,
            .luigi_group = 0x0001,
            // The archives that the audio wrapper requests, then the three tables that the driver has the audio
            // system read. ActionSound.arc in the same folder is read only for the particle effects.
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/JaiMe.arc",
                      "AudioRes/Info/JaiRemixSeq.arc", "AudioRes/Info/SoundIdToInstList.arc",
                      "AudioRes/SpkRes/SpkRes.arc", "AudioRes/Info/BgmParam.arc", "AudioRes/Info/MultiBgmInfo.arc",
                      "AudioRes/Info/StageBgmInfo.arc"},
        },
        {
            // Super Mario Galaxy 2's European release. Its main.dol has the USA release's code at the same addresses.
            // Its data and variables lie elsewhere. The sizes are the USA release's. The language is English, one of
            // the disc's six. The disc holds no translated sound files.
            .name = "Super Mario Galaxy 2 (Europe)",
            .game_id = "SB4P01",
            .dol_sha1 = "1e4581ab2ace72dc3daa55a43fac110b7a4105fe",
            .main = 0x804b6410, // main's call of DVDInit, after main sets up the quantization registers
            .ios_version = 56,
            .watch =
                {
                    .parse_command = 0x8058a040,
                    .jump_handler = 0x80588a60,
                    // JASSeqParser's handler for 0xC8 (0x80588a80) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x80588aac,
                    .sequence_vtable = 0x807114b0,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x80711408, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8058cb00, 0x8058d0a0},
                    .wave_bank_table = 0x807dbc68,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {7 << 16 | 0},
            .driver = JAudioDriver::kGalaxy2,
            // driver/galaxy2.c's block after the wave list, word by word.
            .params =
                {
                    kFrameTicks, // the game runs its audio frame process at 60 Hz
                    0xdb0000,    // ARAM for the sound library
                    0xffffffff,  // ARAM for graphics: the rest
                    8 << 16 | 7 << 8 | 3,
                    0x30,       // HeapMemoryWatcher's size
                    0x20000,    // FileRipper's buffer
                    0x3c,       // GameSystem's size
                    0x44,       // GameSystemObjHolder's size
                    0x34,       // AudSystemWrapper's size
                    0x01,       // the game's number for English in Europe
                    0x805d1e10, // DVDInit
                    0x804bcda0, // HeapMemoryWatcher::createRootHeap
                    0x80501850, // operator new
                    0x804bcf90, // HeapMemoryWatcher's constructor
                    0x807d64ac, // SingletonHolder<HeapMemoryWatcher>::sInstance
                    0x0,        // the stationed heap in MEM1
                    0x4,        // the stationed heap in MEM2
                    0x2c,       // the audio system's heap
                    0x805012e0, // JKRHeap::becomeCurrentHeap
                    0x804b1f00, // FileRipper::setup
                    0x80505280, // JKRAram::create
                    0x804b90b0, // GameSystemObjHolder::initDvd
                    0x807d64a4, // SingletonHolder<GameSystem>::sInstance
                    0x20,       // GameSystem's GameSystemObjHolder
                    0x38,       // the holder's language
                    0x20,       // the holder's AudSystemWrapper
                    0x804ae050, // AudSystemWrapper's constructor
                    0x804ae0f0, // AudSystemWrapper::requestResourceForInitialize
                    0x804ae1c0, // AudSystemWrapper::createAudioSystem
                    0x805bdcc0, // OSSleepTicks
                    0x80078b20, // AudSystem::frameWork
                    0x9a8,      // AudSystem's SpkSystem
                    0x804ae340, // AudSystemWrapper::isLoadDoneWaveDataAtSystemInit
                    0x804ae370, // the wrapper's step after that bank loads
                    0x80089480, // the reader of BgmParam.arc
                    0x80083ce0, // the reader of MultiBgmInfo.arc
                    0x8008a120, // the reader of StageBgmInfo.arc
                    0x807dbc68, // JASGlobalInstance<JAUSectionHeap>::sInstance
                    0x80599de0, // JAUSection::loadWaveArc(bank, mask)
                    0x8059a8f0, // JAUSectionHeap::isWaveLoaded
                    1,          // the section of music sound IDs
                    0x8007c8d0, // AudWrap::startStageBgm
                    0x80078cc0, // AudSystem::startSound
                    0x807dbea0, // THD_TIME_TO_DIMMING, VI's threshold
                    0x800793c0, // AudSystem::initSceneVolume
                    0x950,      // AudSystem's AudSoundObject for system sound effects
                    0x8007dbb0, // AudSoundObject::setCutoffToPort
                    0x95c,      // AudSystem's AudSceneMgr
                    0x800858d0, // AudSceneMgr::setPlayerModeLuigi
                    0,          // Luigi mode: set by a Luigi variant's .miniwsf
                },
            // AudSceneMgr::setPlayerModeLuigi and AudSystem::switchToLuigi are the USA code at the USA addresses.
            .luigi_word = 49,
            .luigi_group = 0x0001,
            // The archives that the audio wrapper requests, then the three tables that the driver has the audio
            // system read. ActionSound.arc in the same folder is read only for the particle effects.
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/JaiMe.arc",
                      "AudioRes/Info/JaiRemixSeq.arc", "AudioRes/Info/SoundIdToInstList.arc",
                      "AudioRes/SpkRes/SpkRes.arc", "AudioRes/Info/BgmParam.arc", "AudioRes/Info/MultiBgmInfo.arc",
                      "AudioRes/Info/StageBgmInfo.arc"},
        },
        {
            // Super Mario Galaxy 2's Japanese release. Its addresses were found as for the European release. The
            // language is Japanese.
            .name = "Super Mario Galaxy 2 (Japan)",
            .game_id = "SB4J01",
            .dol_sha1 = "392529dcfb5b989d9005c1bf57aa7c5db5ed5b0a",
            .main = 0x804b6410, // main's call of DVDInit, after main sets up the quantization registers
            .ios_version = 56,
            .watch =
                {
                    .parse_command = 0x8058a040,
                    .jump_handler = 0x80588a60,
                    // JASSeqParser's handler for 0xC8 (0x80588a80) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x80588aac,
                    .sequence_vtable = 0x8070b590,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x8070b4e8, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8058cb00, 0x8058d0a0},
                    .wave_bank_table = 0x807d5d28,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {7 << 16 | 0},
            .driver = JAudioDriver::kGalaxy2,
            // driver/galaxy2.c's block after the wave list, word by word.
            .params =
                {
                    kFrameTicks, // the game runs its audio frame process at 60 Hz
                    0xdb0000,    // ARAM for the sound library
                    0xffffffff,  // ARAM for graphics: the rest
                    8 << 16 | 7 << 8 | 3,
                    0x30,       // HeapMemoryWatcher's size
                    0x20000,    // FileRipper's buffer
                    0x3c,       // GameSystem's size
                    0x44,       // GameSystemObjHolder's size
                    0x34,       // AudSystemWrapper's size
                    0x10,       // the game's number for Japanese
                    0x805d1e10, // DVDInit
                    0x804bcda0, // HeapMemoryWatcher::createRootHeap
                    0x80501850, // operator new
                    0x804bcf90, // HeapMemoryWatcher's constructor
                    0x807d056c, // SingletonHolder<HeapMemoryWatcher>::sInstance
                    0x0,        // the stationed heap in MEM1
                    0x4,        // the stationed heap in MEM2
                    0x2c,       // the audio system's heap
                    0x805012e0, // JKRHeap::becomeCurrentHeap
                    0x804b1f00, // FileRipper::setup
                    0x80505280, // JKRAram::create
                    0x804b90b0, // GameSystemObjHolder::initDvd
                    0x807d0564, // SingletonHolder<GameSystem>::sInstance
                    0x20,       // GameSystem's GameSystemObjHolder
                    0x38,       // the holder's language
                    0x20,       // the holder's AudSystemWrapper
                    0x804ae050, // AudSystemWrapper's constructor
                    0x804ae0f0, // AudSystemWrapper::requestResourceForInitialize
                    0x804ae1c0, // AudSystemWrapper::createAudioSystem
                    0x805bdcc0, // OSSleepTicks
                    0x80078b20, // AudSystem::frameWork
                    0x9a8,      // AudSystem's SpkSystem
                    0x804ae340, // AudSystemWrapper::isLoadDoneWaveDataAtSystemInit
                    0x804ae370, // the wrapper's step after that bank loads
                    0x80089480, // the reader of BgmParam.arc
                    0x80083ce0, // the reader of MultiBgmInfo.arc
                    0x8008a120, // the reader of StageBgmInfo.arc
                    0x807d5d28, // JASGlobalInstance<JAUSectionHeap>::sInstance
                    0x80599de0, // JAUSection::loadWaveArc(bank, mask)
                    0x8059a8f0, // JAUSectionHeap::isWaveLoaded
                    1,          // the section of music sound IDs
                    0x8007c8d0, // AudWrap::startStageBgm
                    0x80078cc0, // AudSystem::startSound
                    0x807d5f60, // THD_TIME_TO_DIMMING, VI's threshold
                    0x800793c0, // AudSystem::initSceneVolume
                    0x950,      // AudSystem's AudSoundObject for system sound effects
                    0x8007dbb0, // AudSoundObject::setCutoffToPort
                    0x95c,      // AudSystem's AudSceneMgr
                    0x800858d0, // AudSceneMgr::setPlayerModeLuigi
                    0,          // Luigi mode: set by a Luigi variant's .miniwsf
                },
            // AudSceneMgr::setPlayerModeLuigi and AudSystem::switchToLuigi are the USA code at the USA addresses.
            .luigi_word = 49,
            .luigi_group = 0x0001,
            // The archives that the audio wrapper requests, then the three tables that the driver has the audio
            // system read. ActionSound.arc in the same folder is read only for the particle effects.
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/JaiMe.arc",
                      "AudioRes/Info/JaiRemixSeq.arc", "AudioRes/Info/SoundIdToInstList.arc",
                      "AudioRes/SpkRes/SpkRes.arc", "AudioRes/Info/BgmParam.arc", "AudioRes/Info/MultiBgmInfo.arc",
                      "AudioRes/Info/StageBgmInfo.arc"},
        },
    };
    return kProfiles;
}

std::optional<JAudioProfile> FindJAudioProfile(const ntdlib::disc::GameFiles& game)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(game.dol));
    for (const JAudioProfile& profile : KnownJAudioProfiles())
    {
        if (game.GameId() == profile.game_id && sha1 == profile.dol_sha1)
        {
            return profile;
        }
    }

    return std::nullopt;
}

JAudioLibrary SoundLibraryOf(const JAudioProfile* profile)
{
    return profile ? profile->library : JAudioLibrary::kJAudio2;
}

} // namespace wsf::rip
