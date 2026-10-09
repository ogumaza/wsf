// SPDX-License-Identifier: MIT

#include "nw4r/util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "nw4r/tables.h"

namespace wsf::nw4r
{
namespace
{

constexpr int kMicrotones = 256; // steps of a semitone in the pitch table
constexpr int kSemitones = 12;
constexpr float kMinDecibels = -90.4f;
constexpr float kMaxDecibels = 6.0f;
constexpr int kPanTableMax = 256; // the index of a pan table's last entry (the pan's full range spans the table)

const float* PanTable(PanCurve curve)
{
    switch (curve)
    {
    case PanCurve::kSinCos:
        return tables::kPanSinCos;
    case PanCurve::kLinear:
        return tables::kPanLinear;
    default:
        return tables::kPanSqrt;
    }
}

} // namespace

float CalcPitchRatio(int pitch)
{
    // Whole octaves, as a power of two, then the semitone and the 256th of one.
    int octave = 0;
    while (pitch < 0)
    {
        --octave;
        pitch += kSemitones * kMicrotones;
    }
    while (pitch >= kSemitones * kMicrotones)
    {
        ++octave;
        pitch -= kSemitones * kMicrotones;
    }

    float ratio = 1.0f;
    for (; octave > 0; --octave)
    {
        ratio *= 2.0f;
    }
    for (; octave < 0; ++octave)
    {
        ratio /= 2.0f;
    }

    const int note = pitch / kMicrotones;
    const int microtone = pitch % kMicrotones;
    if (note != 0)
    {
        ratio *= tables::kNote[note];
    }

    if (microtone != 0)
    {
        ratio *= tables::kPitch[microtone];
    }

    return ratio;
}

float CalcVolumeRatio(float db)
{
    db = std::clamp(db, kMinDecibels, kMaxDecibels);

    return tables::kDecibel[904 + static_cast<int>(10.0f * db)];
}

float CalcPanRatio(float pan, const PanInfo& info)
{
    // The pan from 0 (left) to 1 (right) selects the nearest of the table's 257 entries.
    pan = (1.0f + std::clamp(pan, -1.0f, 1.0f)) * 0.5f;
    const float* table = PanTable(info.curve);
    float ratio = table[static_cast<int>(0.5f + kPanTableMax * pan)];
    if (info.center_zero)
    {
        ratio /= table[kPanTableMax / 2];
    }

    return std::clamp(ratio, 0.0f, info.zero_clamp ? 1.0f : 2.0f);
}

int CalcLpfFreq(float scale)
{
    scale = std::clamp(scale, 0.0f, 1.0f);
    const double exponent = 10.0 * (static_cast<double>(scale) - 1.0);

    return static_cast<int>(32000.0 * std::pow(2.0, exponent));
}

std::optional<std::array<uint16_t, 5>> BiquadFilterCoefs(int type, float value)
{
    // The preset filters' coefficient callbacks (Wii Music, 0x800b3b7c, 0x800b3c04, 0x800b3c8c, 0x800b3d20 and
    // 0x800b3db4) take the row at the value times the last row's index. The product is rounded towards zero. The
    // band-passes scale value × (2 - value) instead.
    std::span<const uint16_t> table;
    bool band_pass = false;
    switch (type)
    {
    case 1:
        table = tables::kBiquadLpf;
        break;

    case 2:
        table = tables::kBiquadHpf;
        break;

    case 3:
        table = tables::kBiquadBpf512;
        band_pass = true;
        break;

    case 4:
        table = tables::kBiquadBpf1024;
        band_pass = true;
        break;

    case 5:
        table = tables::kBiquadBpf2048;
        band_pass = true;
        break;

    default:
        return std::nullopt;
    }

    const int last = static_cast<int>(table.size() / 5) - 1;
    const float scale = band_pass ? value * (2.0f - value) : value;
    const int row = std::clamp(static_cast<int>(static_cast<float>(last) * scale), 0, last) * 5;

    return std::array<uint16_t, 5>{table[row], table[row + 1], table[row + 2], table[row + 3], table[row + 4]};
}

SndBuild FindSndBuild(std::span<const uint8_t> dol)
{
    // The banner, then the build's date in the form of C's __DATE__: "Mar  8 2008".
    const std::string_view text(reinterpret_cast<const char*>(dol.data()), dol.size());
    const std::size_t banner = text.find("<< NW4R    - SND ");
    const std::size_t build = banner == std::string_view::npos ? banner : text.find("build: ", banner);
    if (build == std::string_view::npos || build - banner > 32 || build + 18 > text.size())
    {
        return {};
    }

    static constexpr std::string_view kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const std::string_view date = text.substr(build + 7, 11);
    const std::size_t month = kMonths.find(date.substr(0, 3));

    const auto digit = [&date](std::size_t i)
    {
        return date[i] == ' ' ? 0u : static_cast<uint32_t>(date[i] - '0');
    };

    if (month == std::string_view::npos || month % 3 != 0 ||
        !std::all_of(date.begin() + 7, date.end(), [](char c) { return c >= '0' && c <= '9'; }))
    {
        return {};
    }

    const uint32_t year = digit(7) * 1000 + digit(8) * 100 + digit(9) * 10 + digit(10);

    return {year * 10000 + static_cast<uint32_t>(month / 3 + 1) * 100 + digit(4) * 10 + digit(5)};
}

} // namespace wsf::nw4r
