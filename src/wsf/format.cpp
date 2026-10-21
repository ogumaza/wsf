// SPDX-License-Identifier: MIT

// The WSF file format (see format.h and docs/wsf.md).

#include "wsf/format.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/streaming.h"
#include "xsflib/text.h"

#include "ax/pb.h"

namespace wsf
{
namespace
{

// The most sounds in one of a descriptor's lists: an effect's sounds, the sends or the silenced tracks.
constexpr uint32_t kMaxListSounds = 0x10000;

// Each of ReadEffectSetups, ReadSends and ReadTracks reads its part of the effects at `pos` (see
// ArchiveDescriptor::Serialize) and moves `pos` past it. Data that ends before the part has none of it. Each returns
// false if the part runs past the end of the data, or holds more than a descriptor can.

// The effects. An effect of an unknown type, or whose size doesn't fit its type, and an effect whose bus isn't A, B or
// C are skipped.
bool ReadEffectSetups(const std::vector<uint8_t>& data, std::size_t& pos, std::vector<nw4r::EffectSetup>& out)
{
    static constexpr uint32_t kMaxEffects = 16;
    static constexpr uint32_t kMaxPresets = 64;
    if (pos + 4 > data.size())
    {
        return true;
    }

    const uint32_t count = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    if (count > kMaxEffects)
    {
        return false;
    }

    for (uint32_t e = 0; e < count; e++)
    {
        if (pos + 12 > data.size())
        {
            return false;
        }

        nw4r::EffectSetup effect;
        effect.bus = data[pos];
        effect.type = static_cast<nw4r::EffectType>(data[pos + 1]);
        const uint32_t words = data[pos + 2];
        const uint32_t presets = xsflib::LoadLe32(&data[pos + 4]);
        const uint32_t sounds = xsflib::LoadLe32(&data[pos + 8]);
        pos += 12;
        if (presets > kMaxPresets || sounds > kMaxListSounds ||
            pos + 4ull * words * std::max(presets, 1u) + 4ull * sounds > data.size())
        {
            return false;
        }

        const auto read_params = [&]()
        {
            nw4r::EffectParams params{};
            for (uint32_t i = 0; i < words; i++)
            {
                if (i < params.size())
                {
                    params[i] = xsflib::LoadLe32(&data[pos]);
                }

                pos += 4;
            }

            return params;
        };

        if (presets == 0)
        {
            effect.params = read_params();
        }

        for (uint32_t i = 0; i < presets; i++)
        {
            effect.presets.push_back(read_params());
        }

        for (uint32_t i = 0; i < sounds; i++, pos += 4)
        {
            effect.sounds.push_back(xsflib::LoadLe32(&data[pos]));
        }

        const int expected = nw4r::EffectParamWords(effect.type);
        if (expected != 0 && words == static_cast<uint32_t>(expected) && effect.bus < ax::kAuxBusCount)
        {
            out.push_back(std::move(effect));
        }
    }

    return true;
}

// The sends that the game sets on sounds. A send whose bus isn't A, B or C is skipped.
bool ReadSends(const std::vector<uint8_t>& data, std::size_t& pos, std::vector<nw4r::SoundSend>& sends)
{
    if (pos + 4 > data.size())
    {
        return true;
    }

    const uint32_t send_count = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    if (send_count > kMaxListSounds || pos + 12ull * send_count > data.size())
    {
        return false;
    }

    for (uint32_t i = 0; i < send_count; i++, pos += 12)
    {
        const uint32_t bus = xsflib::LoadLe32(&data[pos + 4]);
        if (bus < ax::kAuxBusCount)
        {
            sends.push_back({xsflib::LoadLe32(&data[pos]), static_cast<int>(bus),
                             std::bit_cast<float>(xsflib::LoadLe32(&data[pos + 8]))});
        }
    }

    return true;
}

// The tracks that the game silences on sounds. Tracks beyond track 15 are skipped.
bool ReadTracks(const std::vector<uint8_t>& data, std::size_t& pos, std::vector<nw4r::SoundTracks>& tracks)
{
    if (pos + 4 > data.size())
    {
        return true;
    }

    const uint32_t track_count = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    if (track_count > kMaxListSounds || pos + 8ull * track_count > data.size())
    {
        return false;
    }

    for (uint32_t i = 0; i < track_count; i++, pos += 8)
    {
        const uint32_t played = xsflib::LoadLe32(&data[pos + 4]);
        if (played <= nw4r::kAllTracks)
        {
            tracks.push_back({xsflib::LoadLe32(&data[pos]), static_cast<uint16_t>(played)});
        }
    }

    return true;
}

// Reads the effects, the sends and the tracks from `data` at `pos`. Returns false if one of the parts is damaged.
bool ParseEffects(const std::vector<uint8_t>& data, std::size_t pos, nw4r::ArchiveEffects& effects)
{
    return ReadEffectSetups(data, pos, effects.setups) && ReadSends(data, pos, effects.sends) &&
           ReadTracks(data, pos, effects.tracks);
}

} // namespace

std::optional<uint16_t> ParseTracks(std::string_view text)
{
    constexpr std::string_view kSeparators = ", \t\r\n";
    constexpr xsflib::IntegerRules kTrackRules{.base = 10, .minus = false};

    uint32_t tracks = 0;
    std::size_t at = 0;
    while ((at = text.find_first_not_of(kSeparators, at)) != std::string_view::npos)
    {
        const std::size_t end = std::min(text.find_first_of(kSeparators, at), text.size());
        const std::string_view part = text.substr(at, end - at);
        at = end;

        // A track, or a range of tracks: "8" or "8-14".
        const std::size_t dash = part.find('-');
        const std::optional<int64_t> first = xsflib::ParseInteger(part.substr(0, dash), kTrackRules);
        const std::optional<int64_t> last =
            dash == std::string_view::npos ? first : xsflib::ParseInteger(part.substr(dash + 1), kTrackRules);
        if (!first || !last || *first > *last || *last > 15)
        {
            return std::nullopt;
        }

        for (int64_t track = *first; track <= *last; track++)
        {
            tracks |= 1u << track;
        }
    }

    if (tracks == 0)
    {
        return std::nullopt;
    }

    return static_cast<uint16_t>(tracks);
}

std::string FormatTracks(uint16_t tracks)
{
    std::string text;
    const auto add = [&text](const std::string& part)
    {
        text += (text.empty() ? "" : ", ") + part;
    };

    int track = 0;
    while (track < 16)
    {
        if ((tracks >> track & 1) == 0)
        {
            track++;
            continue;
        }

        int last = track;
        while (last < 15 && (tracks >> (last + 1) & 1) != 0)
        {
            last++;
        }

        if (last - track >= 2)
        {
            add(std::to_string(track) + "-" + std::to_string(last));
        }
        else
        {
            for (int t = track; t <= last; t++)
            {
                add(std::to_string(t));
            }
        }

        track = last + 1;
    }

    return text;
}

std::optional<float> ParseTempo(std::string_view text)
{
    constexpr std::string_view kSpace = " \t\r\n";

    const std::size_t from = text.find_first_not_of(kSpace);
    if (from == std::string_view::npos)
    {
        return std::nullopt;
    }

    text = text.substr(from, text.find_last_not_of(kSpace) + 1 - from);
    const std::optional<xsflib::DecimalNumber> number = xsflib::ReadDecimal(text);
    if (!number || number->length != text.size() || !(number->value >= 0.25 && number->value <= 4.0))
    {
        return std::nullopt;
    }

    return static_cast<float>(number->value);
}

nw4r::Arrangement ArrangementFromTags(const Tags& tags, uint16_t tracks)
{
    nw4r::Arrangement arrangement;
    arrangement.tracks = tracks;
    if (const auto it = tags.find(kTracksTag); it != tags.end())
    {
        arrangement.tracks = ParseTracks(it->second).value_or(arrangement.tracks);
    }

    if (const auto it = tags.find(kTempoTag); it != tags.end())
    {
        arrangement.tempo_ratio = ParseTempo(it->second).value_or(arrangement.tempo_ratio);
    }

    if (const auto it = tags.find(kVariablesTag); it != tags.end())
    {
        arrangement.variables = xsflib::psf::ParseVariables(it->second).value_or(arrangement.variables);
    }

    return arrangement;
}

std::vector<uint8_t> ProcessDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    xsflib::psf::Put32(v, kMagic);
    xsflib::psf::Put32(v, version);
    xsflib::psf::Put32(v, entry);
    xsflib::psf::Put32(v, status_address);
    xsflib::psf::Put32(v, ios_version);
    xsflib::psf::Put32(v, flags);

    return v;
}

std::optional<ProcessDescriptor> ProcessDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 24 || xsflib::LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ProcessDescriptor d;
    d.version = xsflib::LoadLe32(&data[4]);
    d.entry = xsflib::LoadLe32(&data[8]);
    d.status_address = xsflib::LoadLe32(&data[12]);
    d.ios_version = xsflib::LoadLe32(&data[16]);
    d.flags = xsflib::LoadLe32(&data[20]);

    return d;
}

std::optional<std::string> ProcessDescriptor::Check() const
{
    if (auto err = xsflib::psf::CheckDescriptorVersion("process", version, kVersion))
    {
        return err;
    }

    return xsflib::psf::CheckDescriptorFlags("process", flags, kFlagDspRequired);
}

std::vector<uint8_t> ArchiveDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    xsflib::psf::Put32(v, kMagic);
    xsflib::psf::Put32(v, version);
    xsflib::psf::Put32(v, flags);
    xsflib::psf::Put32(v, output_mode);
    xsflib::psf::Put32(v, microcode_address);

    xsflib::psf::PutPath(v, archive_path);
    xsflib::psf::PutPath(v, microcode_path);
    xsflib::psf::PadTo4(v);
    xsflib::psf::Put32(v, snd_build);

    // The effects: their count, then each one's bus, type, size in words, number of presets and number of sounds, then
    // its parameter words and its sounds. Then the sends: their count, then each one's sound, bus and send. Then the
    // tracks: their count, then each sound and the tracks that it plays.
    if (!effects.setups.empty() || !effects.sends.empty() || !effects.tracks.empty())
    {
        xsflib::psf::Put32(v, static_cast<uint32_t>(effects.setups.size()));
        for (const nw4r::EffectSetup& effect : effects.setups)
        {
            const int words = nw4r::EffectParamWords(effect.type);
            v.push_back(static_cast<uint8_t>(effect.bus));
            v.push_back(static_cast<uint8_t>(effect.type));
            v.push_back(static_cast<uint8_t>(words));
            v.push_back(0);
            xsflib::psf::Put32(v, static_cast<uint32_t>(effect.presets.size()));
            xsflib::psf::Put32(v, static_cast<uint32_t>(effect.sounds.size()));

            const auto put_params = [&](const nw4r::EffectParams& params)
            {
                for (int i = 0; i < words; i++)
                {
                    xsflib::psf::Put32(v, params[static_cast<std::size_t>(i)]);
                }
            };

            if (effect.presets.empty())
            {
                put_params(effect.params);
            }

            for (const nw4r::EffectParams& preset : effect.presets)
            {
                put_params(preset);
            }

            for (const uint32_t sound : effect.sounds)
            {
                xsflib::psf::Put32(v, sound);
            }
        }

        xsflib::psf::Put32(v, static_cast<uint32_t>(effects.sends.size()));
        for (const nw4r::SoundSend& send : effects.sends)
        {
            xsflib::psf::Put32(v, send.sound);
            xsflib::psf::Put32(v, static_cast<uint32_t>(send.bus));
            xsflib::psf::Put32(v, std::bit_cast<uint32_t>(send.send));
        }
    }

    if (!effects.tracks.empty())
    {
        xsflib::psf::Put32(v, static_cast<uint32_t>(effects.tracks.size()));
        for (const nw4r::SoundTracks& played : effects.tracks)
        {
            xsflib::psf::Put32(v, played.sound);
            xsflib::psf::Put32(v, played.tracks);
        }
    }

    return v;
}

std::optional<ArchiveDescriptor> ArchiveDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 24 || xsflib::LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ArchiveDescriptor d;
    d.version = xsflib::LoadLe32(&data[4]);
    d.flags = xsflib::LoadLe32(&data[8]);
    d.output_mode = xsflib::LoadLe32(&data[12]);
    d.microcode_address = xsflib::LoadLe32(&data[16]);

    std::size_t pos = 20;
    const std::optional<std::string> archive = xsflib::psf::ReadPath(data, pos);
    const std::optional<std::string> microcode = archive ? xsflib::psf::ReadPath(data, pos) : std::nullopt;
    if (!microcode)
    {
        return std::nullopt;
    }

    d.archive_path = *archive;
    d.microcode_path = *microcode;

    // The build of NW4R SND follows the paths' padding. A descriptor that ends before the build gives none.
    pos = (pos + 3) & ~std::size_t{3};
    if (pos + 4 <= data.size())
    {
        d.snd_build = xsflib::LoadLe32(&data[pos]);
        pos += 4;
    }

    if (!ParseEffects(data, pos, d.effects))
    {
        return std::nullopt;
    }

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    // Version 2 has version 1's flags and output modes.
    if (version != kVersion && version != kListVersion)
    {
        return "archive descriptor version " + std::to_string(version) + "; this player supports versions 1 and 2";
    }

    if (auto err = xsflib::psf::CheckDescriptorFlags("archive", flags, kFlagDspRequired))
    {
        return err;
    }

    return xsflib::psf::CheckDescriptorOutputMode("archive", output_mode);
}

std::vector<uint32_t> SoundsToPlay(const LoadedSet& set)
{
    if (!set.archive || set.sounds.empty())
    {
        return {};
    }

    if (set.archive->version == ArchiveDescriptor::kVersion)
    {
        return {set.sounds.front()};
    }

    return set.sounds;
}

} // namespace wsf
