// SPDX-License-Identifier: MIT

// wsfplay: renders .miniwsf files to WAV or FLAC at the audio interface's rate of 32000 Hz.
//
// usage: wsfplay [options] <file> [<file> ...]
//   -o, --output FILE  the output file, WAV or FLAC by its extension (default: the input's name with .wav, in the same
//                      folder). Only valid with a single input.
//   --length T         override the length tag (seconds, m:s or h:m:s)
//   --fade T           override the fade tag
//   --default T        the length when there's no length tag (default and fade: PlaybackOptions)
//   --info             print the tags and loaded files without rendering
//   --version          print the version
//
// The files are rendered one after another. A file that fails doesn't stop the rest. A .wsflib among the inputs is
// skipped. It's a library that the other files load. Files dropped on the program become inputs. xsflib's play tool
// does the work. This file gives it WSF's names, help and summary.

#include "xsflib/cli/play.h"

#include "wsf/player.h"

namespace
{

// The help's description of what the tool does.
constexpr char kDescription[] =
    "Renders each .miniwsf file to a WAV file next to it at 32000 Hz. The length and fade come\n"
    "from the file's tags. Libraries (.wsflib) are skipped because the .miniwsf files load them.\n"
    "You can also drop files on the program.\n";

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::PlayTool<wsf::Player> tool;
    tool.name = "wsfplay";
    tool.version = WSF_VERSION;
    tool.description = kDescription;
    tool.output_option = "write FILE instead. Its extension selects WAV or FLAC (one input only)";
    tool.info_option = "print the tags and the files that each input loads, without rendering";
    tool.library_extension = ".wsflib";
    tool.mini_extension = ".miniwsf";
    tool.a_file = "a WSF file";
    tool.describe_set = xsflib::cli::DescribeLoadedChunks<wsf::Player>;

    return xsflib::cli::RunPlayTool(tool, argc, argv);
}
