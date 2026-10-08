// SPDX-License-Identifier: MIT

// foo_input_wsf: foobar2000 input component for WSF (.miniwsf). Modelled on kode54's PSF-family inputs
// (foo_input_vio2sf, foo_input_ncsf). PSF tags are shown as metadata (game as album, year as date). Length and fade
// come from the tags, or from the defaults in Advanced preferences > Decoding > WSF decoder. The tags can be edited.
//
// The player core (src/wsf) does the emulation. Game mode runs the game's sound code on an emulated Wii. Archive mode
// runs WSF's model of the SDK's sound player. Both modes run the game's AX microcode on the emulated DSP. While
// playing, the component renders ahead on a worker thread and buffers the audio (see src/wsf/render_ahead.h). Seeks
// within the buffer are instant. Seeks further ahead wait for rendering to reach the target. Tracks longer than the
// buffer also take snapshots every few seconds. Seeking back before the buffered audio restores the latest snapshot
// before the target and renders from there. With rendering ahead turned off, decode_run renders each block itself.
// Every track then takes snapshots.
//
// Build: see plugins/foobar2000/README.md (clang-cl or MSVC on Windows, or Apple Clang on macOS, with the foobar2000
// SDK).

#include <SDK/foobar2000.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/ascii.h"
#include "wsf/format.h"
#include "wsf/playback.h"
#include "wsf/render_ahead.h"

namespace
{

using wsf::Playback;
using wsf::PlaybackOptions;
using wsf::Player;
using wsf::RenderAhead;
using wsf::Tags;

// {A34DC260-7833-4B54-8DCC-750144AD1B92}
constexpr GUID kInputGuid = {0xa34dc260, 0x7833, 0x4b54, {0x8d, 0xcc, 0x75, 0x01, 0x44, 0xad, 0x1b, 0x92}};

// Advanced preferences entries, under Decoding > WSF decoder.
constexpr GUID kBranchGuid = {0x314b08dd, 0x9806, 0x4128, {0x83, 0xf2, 0x39, 0x6f, 0xc3, 0x3e, 0x1c, 0xf5}};
constexpr GUID kLengthGuid = {0x37d0ea69, 0x4a53, 0x46ac, {0x99, 0xad, 0xbd, 0x74, 0xcd, 0x89, 0x0e, 0x66}};
constexpr GUID kFadeGuid = {0xd3112fc5, 0x414e, 0x4352, {0xb7, 0x86, 0xdb, 0xc9, 0x2d, 0xc1, 0xf2, 0xe6}};
constexpr GUID kEndlessGuid = {0xd98bb8a9, 0x9afb, 0x4392, {0xb9, 0xbe, 0x9e, 0x9e, 0x07, 0xf7, 0x2f, 0x2f}};
constexpr GUID kRenderAheadGuid = {0xa705e71b, 0xaced, 0x468f, {0xaf, 0x69, 0x5a, 0xbd, 0xe2, 0xe7, 0x23, 0xbc}};

advconfig_branch_factory cfg_branch("WSF decoder", kBranchGuid, advconfig_entry::guid_branch_decoding, 0);
advconfig_integer_factory cfg_default_length("Default length for files without a length tag (seconds)", kLengthGuid,
                                             kBranchGuid, 0, PlaybackOptions::kDefaultLengthMs / 1000, 1, 24 * 3600);
advconfig_integer_factory cfg_default_fade("Default fade for files without a length tag (seconds)", kFadeGuid,
                                           kBranchGuid, 1, PlaybackOptions::kDefaultFadeMs / 1000, 0, 3600);
advconfig_checkbox_factory cfg_endless("Play endlessly (ignore lengths during playback only)", kEndlessGuid,
                                       kBranchGuid, 2, false);
advconfig_checkbox_factory cfg_render_ahead("Render ahead during playback (quicker seeking; uses more CPU and memory)",
                                            kRenderAheadGuid, kBranchGuid, 3, true);

constexpr std::size_t kBlockFrames = 1024;      // frames decoded per decode_run call
constexpr t_filesize kMaxFileSize = 256u << 20; // largest file read into memory

// PSF tags that aren't metadata. Editing the tags leaves these unchanged.
bool IsSystemTag(const std::string& name)
{
    return name.starts_with("_lib") || name == "length" || name == "fade" || name == "volume" || name == "utf8";
}

// The foobar2000 field name for a PSF tag name.
std::string FieldFromTag(const std::string& tag)
{
    if (tag == "game")
    {
        return "album";
    }
    if (tag == "year")
    {
        return "date";
    }

    return tag;
}

// The PSF tag name for a foobar2000 field name.
std::string TagFromField(const std::string& field)
{
    if (field == "album")
    {
        return "game";
    }
    if (field == "date")
    {
        return "year";
    }

    return field;
}

// PSF tags are UTF-8 when the file has a utf8 tag. Without one, text that isn't valid UTF-8 is read as the system code
// page on Windows and as Windows-1252 elsewhere. pfc converts "ANSI" text the same way outside Windows.
pfc::string8 TagText(const std::string& value, bool utf8)
{
    if (utf8 || pfc::is_valid_utf8(value.c_str()))
    {
        return pfc::string8(value.c_str());
    }

    return pfc::string8(pfc::stringcvt::string_utf8_from_ansi(value.c_str()));
}

void ReadWhole(file::ptr& f, std::vector<uint8_t>& out, abort_callback& abort)
{
    f->reopen(abort);
    const t_filesize size = f->get_size_ex(abort);
    if (size > kMaxFileSize)
    {
        pfc::throw_exception_with_message<exception_io_data>("WSF file too large");
    }

    out.resize(static_cast<std::size_t>(size));
    if (!out.empty())
    {
        f->read_object(out.data(), out.size(), abort);
    }
}

class InputWsf : public input_stubs
{
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort)
    {
        path_ = p_path;
        file_ = p_filehint;
        input_open_file_helper(file_, p_path, p_reason, p_abort);
        ReadWhole(file_, data_, p_abort);

        wsf::PsfFile psf;
        if (auto err = wsf::ParsePsf(data_, psf, false))
        {
            throw exception_io_unsupported_format();
        }

        if (psf.version != wsf::kPsfVersionWsf)
        {
            throw exception_io_unsupported_format();
        }

        tags_ = std::move(psf.tags);
    }

    void get_info(file_info& p_info, abort_callback& /*p_abort*/)
    {
        const bool utf8 = tags_.count("utf8") != 0;
        replaygain_info rg = p_info.get_replaygain();
        bool have_rg = false;
        for (const auto& [name, value] : tags_)
        {
            if (IsSystemTag(name))
            {
                continue;
            }

            if (rg.set_from_meta(name.c_str(), value.c_str()))
            {
                have_rg = true;
                continue;
            }

            const std::string field = FieldFromTag(name);

            // A multi-line value (repeated tag lines) becomes one field value per line.
            std::size_t start = 0;
            while (true)
            {
                const std::size_t nl = value.find('\n', start);
                const std::string line = value.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                p_info.meta_add(field.c_str(), TagText(line, utf8));
                if (nl == std::string::npos)
                {
                    break;
                }

                start = nl + 1;
            }
        }
        if (have_rg)
        {
            p_info.set_replaygain(rg);
        }

        const wsf::TrackLength length = GetLength();
        p_info.set_length(static_cast<double>(length.play_ms + length.fade_ms) / 1000.0);
        p_info.info_set_int("samplerate", Playback::kSampleRate);
        p_info.info_set_int("channels", 2);
        p_info.info_set_int("bitspersample", 16);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "WSF");
        if (auto it = tags_.find("volume"); it != tags_.end())
        {
            p_info.info_set("wsf_volume", it->second.c_str());
        }
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& a)
    {
        return file_->get_stats2_(f, a);
    }

    void decode_initialize(unsigned p_flags, abort_callback& p_abort)
    {
        // foobar2000 may restart decoding with the same input. Stop the previous worker thread first.
        ahead_.reset();
        playback_.reset();

        // The file and its libraries, read through foobar2000's file system.
        const wsf::FileReader reader = [&](const std::string& path, std::vector<uint8_t>& out) -> bool
        {
            if (path == path_)
            {
                out = data_;
                return true;
            }

            try
            {
                file::ptr f;
                filesystem::g_open_read(f, path.c_str(), p_abort);
                ReadWhole(f, out, p_abort);
                return true;
            }
            catch (const exception_aborted&)
            {
                throw;
            }
            catch (const std::exception&)
            {
                return false;
            }
        };
        // A file in an archive has a path like unpack://zip|<length>|<the archive's path>|<its path in the archive>.
        // Its libraries are therefore also found after the last |.
        const bool in_archive = std::string_view(path_).starts_with("unpack://");
        wsf::LoadedSet set;
        if (auto err = wsf::LoadSet(path_, reader, set, in_archive ? "/\\|" : "/\\"))
        {
            pfc::throw_exception_with_message<exception_io_data>(err->c_str());
        }

        // Enable endless playback, snapshots and rendering ahead only during playback. Conversion and scanning need a
        // finite length and never seek.
        const bool playing = (p_flags & input_flag_playback) != 0;
        PlaybackOptions options = DefaultOptions();
        options.endless = cfg_endless.get() && playing && !(p_flags & input_flag_no_looping);
        options.snapshots = playing;

        playback_ = std::make_unique<Playback>();
        if (!playback_->Open(std::move(set), options))
        {
            pfc::throw_exception_with_message<exception_io_data>(playback_->Error().c_str());
        }

        if (!playback_->Start())
        {
            pfc::throw_exception_with_message<exception_io_data>(playback_->Error().c_str());
        }

        if (playing && cfg_render_ahead.get())
        {
            ahead_ = std::make_unique<RenderAhead>(std::move(playback_));
        }

        buffer_.resize(2 * kBlockFrames);
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort)
    {
        p_abort.check();
        const std::size_t n = ahead_ ? ahead_->Read(buffer_.data(), kBlockFrames, [&] { return p_abort.is_aborting(); })
                                     : playback_->Render(buffer_.data(), kBlockFrames);
        if (n == 0)
        {
            p_abort.check();
            std::string error;
            if (ahead_)
            {
                error = ahead_->Error();
            }
            else if (playback_->GetPlayer().GetState() == Player::State::kError)
            {
                error = playback_->Error();
            }

            if (!error.empty())
            {
                pfc::throw_exception_with_message<exception_io_data>(error.c_str());
            }

            return false;
        }

        p_chunk.set_data_fixedpoint(buffer_.data(), n * 2 * sizeof(int16_t), Playback::kSampleRate, 2, 16,
                                    audio_chunk::channel_config_stereo);

        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort)
    {
        const t_uint64 target = audio_math::time_to_samples(p_seconds, Playback::kSampleRate);
        if (ahead_)
        {
            ahead_->Seek(target); // decode_run waits for the audio there
            return;
        }

        if (!playback_->Seek(target, [&] { return p_abort.is_aborting(); }))
        {
            p_abort.check();
            pfc::throw_exception_with_message<exception_io_data>(playback_->Error().c_str());
        }
    }

    bool decode_can_seek()
    {
        return true;
    }

    bool decode_get_dynamic_info(file_info& /*p_out*/, double& /*p_timestamp_delta*/)
    {
        return false;
    }

    bool decode_get_dynamic_info_track(file_info& /*p_out*/, double& /*p_timestamp_delta*/)
    {
        return false;
    }

    void decode_on_idle(abort_callback& p_abort)
    {
        file_->on_idle(p_abort);
    }

    // Rewrites the tag area with the edited tags. The program and the system tags (_lib, length, fade and volume) stay
    // as they are.
    void retag(const file_info& p_info, abort_callback& p_abort)
    {
        Tags tags;
        for (const auto& [name, value] : tags_)
        {
            if (IsSystemTag(name))
            {
                tags[name] = value;
            }
        }

        for (t_size i = 0; i < p_info.meta_get_count(); i++)
        {
            const std::string tag = TagFromField(wsf::AsciiLower(p_info.meta_enum_name(i)));
            if (tag.empty() || IsSystemTag(tag) || tag.find('=') != std::string::npos)
            {
                continue;
            }

            std::string value;
            for (t_size j = 0; j < p_info.meta_enum_value_count(i); j++)
            {
                if (j)
                {
                    value += '\n';
                }
                value += p_info.meta_enum_value(i, j);
            }

            tags[tag] = value;
        }

        const replaygain_info rg = p_info.get_replaygain();
        replaygain_info::t_text_buffer buf;
        if (rg.format_album_gain(buf))
        {
            tags["replaygain_album_gain"] = buf;
        }

        if (rg.format_album_peak(buf))
        {
            tags["replaygain_album_peak"] = buf;
        }

        if (rg.format_track_gain(buf))
        {
            tags["replaygain_track_gain"] = buf;
        }

        if (rg.format_track_peak(buf))
        {
            tags["replaygain_track_peak"] = buf;
        }

        tags["utf8"] = "1";
        WriteTags(tags, p_abort);
    }

    void remove_tags(abort_callback& p_abort)
    {
        Tags tags;
        for (const auto& [name, value] : tags_)
        {
            if (IsSystemTag(name))
            {
                tags[name] = value;
            }
        }

        WriteTags(tags, p_abort);
    }

    static bool g_is_our_content_type(const char* /*p_content_type*/)
    {
        return false;
    }

    // Only .miniwsf files play. A .wsflib holds no sound to play by itself. WSF has no standalone .wsf file.
    static bool g_is_our_path(const char* /*p_path*/, const char* p_extension)
    {
        return stricmp_utf8(p_extension, "miniwsf") == 0;
    }

    static const char* g_get_name()
    {
        return "WSF decoder";
    }

    static GUID g_get_guid()
    {
        return kInputGuid;
    }

private:
    // The length and fade that Advanced preferences give files without a length tag.
    static PlaybackOptions DefaultOptions()
    {
        PlaybackOptions options;
        options.default_length_ms = static_cast<long long>(cfg_default_length.get()) * 1000;
        options.default_fade_ms = static_cast<long long>(cfg_default_fade.get()) * 1000;

        return options;
    }

    // The file's tagged length and fade, or the defaults. Playback uses these values.
    wsf::TrackLength GetLength() const
    {
        const auto length = tags_.find("length");
        const auto fade = tags_.find("fade");
        const long long length_ms = length != tags_.end() ? wsf::ParseTime(length->second) : -1;
        const long long fade_ms = fade != tags_.end() ? wsf::ParseTime(fade->second) : -1;
        return wsf::ResolveLength(length_ms, fade_ms, DefaultOptions());
    }

    void WriteTags(const Tags& tags, abort_callback& p_abort)
    {
        const auto offset = wsf::TagAreaOffset(data_);
        if (!offset)
        {
            pfc::throw_exception_with_message<exception_io_data>("corrupt WSF file");
        }

        const std::string text = wsf::FormatTagArea(tags);
        file_->seek(*offset, p_abort);
        file_->set_eof(p_abort);
        if (!text.empty())
        {
            file_->write_object(text.data(), text.size(), p_abort);
        }

        // Keep our copy current for a following get_info.
        data_.resize(*offset);
        data_.insert(data_.end(), text.begin(), text.end());
        tags_ = tags;
    }

    std::string path_;
    service_ptr_t<file> file_;
    std::vector<uint8_t> data_; // the file as read (its program is small for a .miniwsf)
    Tags tags_;
    std::unique_ptr<Playback> playback_; // when decode_run renders the blocks itself
    std::unique_ptr<RenderAhead> ahead_; // owns the Playback while rendering ahead
    std::vector<int16_t> buffer_;
};

input_singletrack_factory_t<InputWsf> g_input_wsf_factory;

} // namespace

DECLARE_FILE_TYPE("WSF files", "*.MINIWSF");

DECLARE_COMPONENT_VERSION("WSF Decoder", WSF_VERSION,
                          "Plays WSF (Nintendo Wii sequenced music) rips. The game's AX microcode runs on an emulated "
                          "DSP. In game mode, the game's sound code drives it on an emulated Wii. In archive mode, "
                          "WSF's model of the SDK's sound player drives it.\n\n"
                          "MIT licence. Uses zlib.");

VALIDATE_COMPONENT_FILENAME("foo_input_wsf.dll");
