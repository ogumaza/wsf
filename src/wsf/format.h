// SPDX-License-Identifier: MIT

// The WSF file format (docs/wsf.md): PSF container, program chunks (MEM, FILE and SND), process and archive
// descriptors, tags.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/program.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/set.h"

#include "nw4r/arrangement.h"
#include "nw4r/effect_setup.h"

namespace wsf
{

// PSF version byte of WSF. See docs/wsf.md ("Version byte") before changing it.
constexpr uint8_t kPsfVersionWsf = 0x27;

// The sample rate of the audio interface's DMA: 32,000 Hz exactly. Players report this rate. The `length` and `fade`
// tags count at it (docs/wsf.md, "Tags").
constexpr int kPlayerSampleRate = 32'000;

// Descriptor flags. Version 1 requires bit 0 (DSP required) and rejects all other bits.
constexpr uint32_t kFlagDspRequired = xsflib::psf::kDescriptorFlagDspRequired;

// Output modes: 0 mono, 1 stereo, 2 surround. Version 1 plays stereo only.
constexpr uint32_t kOutputStereo = xsflib::psf::kDescriptorOutputStereo;

// The PSF container's tags, and the reader that loads a set's files (xsflib::psf).
using Tags = xsflib::psf::Tags;
using FileReader = xsflib::psf::FileReader;

// The tags that arrange a sequence in archive mode (docs/wsf.md, "Tags"): the tracks that play, the tempo ratio and the
// variables that the sequence starts with. Game mode ignores them.
constexpr char kTracksTag[] = "wsf_tracks";
constexpr char kTempoTag[] = "wsf_tempo";
constexpr char kVariablesTag[] = "wsf_var";

// Parses a wsf_tracks tag: one or more track numbers from 0 to 15 and ranges such as 8-14, separated by commas or white
// space, line breaks included. Each number is decimal. Returns the tracks as bits, bit N for track N. Returns nullopt
// if any part is malformed or out of range, or if there's no track.
std::optional<uint16_t> ParseTracks(std::string_view text);

// The wsf_tracks tag for `tracks`: the tracks in increasing order, separated by ", ", with each run of three or more as
// a range ("0, 1, 8-14").
std::string FormatTracks(uint16_t tracks);

// Parses a wsf_tempo tag: a decimal number from 0.25 to 4, such as 1.1 or 0.9, with '.' or ',' before its decimals.
// White space around it is ignored. Returns the ratio rounded to single precision, or nullopt if the text is malformed
// or out of range.
std::optional<float> ParseTempo(std::string_view text);

// The arrangement that a set's tags give. A tag that's missing, malformed or out of range changes nothing.
nw4r::Arrangement ArrangementFromTags(const Tags& tags);

// The process descriptor: the reserved area of a game-mode set (docs/wsf.md, "Process descriptor").
struct ProcessDescriptor
{
    static constexpr uint32_t kMagic = 0x50465357; // "WSFP"
    static constexpr uint32_t kVersion = 1;

    static std::optional<ProcessDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = kVersion;
    uint32_t entry = 0x80004000; // main.dol's entry point
    uint32_t status_address = 0; // the driver's status word, or 0
    uint32_t ios_version = 0;    // the IOS that the game's TMD names (21 for IOS21)
    uint32_t flags = kFlagDspRequired;
};

// Descriptor of an archive-mode set (docs/wsf.md, "Archive mode"). WSF's model of the SDK's sound player (nw4r::snd and
// AX) plays the sound archive on the game's AX microcode. The game's code isn't used.
struct ArchiveDescriptor
{
    static constexpr uint32_t kMagic = 0x41465357; // "WSFA"
    static constexpr uint32_t kVersion = 1;

    static std::optional<ArchiveDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = kVersion;
    uint32_t flags = kFlagDspRequired;
    uint32_t output_mode = kOutputStereo;
    uint32_t microcode_address = 0; // where the game loads the microcode from
    std::string archive_path;       // FILE chunk holding the sound archive (.brsar)
    std::string microcode_path;     // FILE chunk holding AX's microcode (4,096 big-endian words)
    uint32_t snd_build = 0;         // the game's build of NW4R SND, by its date (YYYYMMDD), or 0 if unknown

    // The effects on the aux buses and the sends that the game sets on sounds (docs/wsf.md, "Effects"), after the
    // build. A descriptor without them gives none. Parse skips an effect of a type that it doesn't know.
    std::vector<nw4r::EffectSetup> effects;
    std::vector<nw4r::SoundSend> sends;
};

// The builder of a program's MEM, FILE and SND chunks (xsflib::psf).
using xsflib::psf::ProgramBuilder;

// The parameter block of WSF's game-mode driver (docs/wsf.md, "Driver"). A game-mode set loads it at kAddress. Like the
// rest of the Wii's memory, its words are big-endian.
struct DriverBlock
{
    static constexpr uint32_t kAddress = 0x81700010;
    static constexpr uint32_t kMagic = 0x57534644; // "WSFD"
    static constexpr uint32_t kVersion = 1;

    // The JAudio2 driver's block (driver/jaudio.c). NW4R SND's driver keeps version 1. A player of WSF 1.1.0 accepts
    // version 1 alone.
    static constexpr uint32_t kJAudioVersion = 2;

    static constexpr uint32_t kVersionOffset = 0x04;
    static constexpr uint32_t kStatusOffset = 0x08;
    static constexpr uint32_t kSoundOffset = 0x0c;

    // The effects in NW4R SND's driver's block: their number, and 4 effects of 8 words each (the class's SetParam is
    // the fifth word, and the number of words in its parameters the sixth). Then the build of AXFX, and 4 effects of 4
    // words each: the class (nw4r::EffectType), and its StartUp, Shutdown and UpdateBuffer. The player's models of the
    // effects read them (see wsf/native_effects.h).
    static constexpr uint32_t kEffectCountOffset = 0xdc;
    static constexpr uint32_t kEffectsOffset = 0xec;
    static constexpr uint32_t kEffectSize = 32;
    static constexpr uint32_t kEffectSetParamOffset = 0x10;
    static constexpr uint32_t kEffectWordsOffset = 0x14;
    static constexpr uint32_t kAxfxBuildOffset = 0x26c;
    static constexpr uint32_t kNativeOffset = 0x270;
    static constexpr uint32_t kNativeSize = 16;
    static constexpr uint32_t kMaxEffects = 4;
};

// What xsflib::psf's set loader requires of WSF: its descriptors, version byte, name and driver block.
struct SetFormat
{
    using ProcessDescriptor = wsf::ProcessDescriptor;
    using ArchiveDescriptor = wsf::ArchiveDescriptor;

    static constexpr uint8_t kPsfVersion = kPsfVersionWsf;
    static constexpr const char* kName = "WSF";
    static constexpr xsflib::psf::DriverBlockLayout kDriverBlock{.address = DriverBlock::kAddress,
                                                                 .magic = DriverBlock::kMagic,
                                                                 .version = DriverBlock::kVersion,
                                                                 .newest_version = DriverBlock::kJAudioVersion,
                                                                 .version_offset = DriverBlock::kVersionOffset,
                                                                 .big_endian = true};
};

// A complete set: the chunks of a file and its libraries in loading order, the effective descriptor (exactly one of
// `descriptor` and `archive`) and the tags of the top-level file.
using LoadedSet = xsflib::psf::LoadedSet<SetFormat>;

// Loads a WSF/miniWSF file with its _lib/_libN chain. Paths in _lib tags are relative to the directory of the file that
// names them: that file's path up to the last of `separators`. Returns an error for a set that a version 1 player can't
// play. That's a set whose descriptor fails its Check(), or a game-mode set whose driver block has a version other than
// 1 (NW4R SND's driver) and 2 (JAudio2's).
inline std::optional<std::string> LoadSet(const std::string& path, const FileReader& reader, LoadedSet& out,
                                          std::string_view separators = "/\\")
{
    return xsflib::psf::LoadSet(path, reader, out, separators);
}

} // namespace wsf
