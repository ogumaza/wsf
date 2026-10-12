# Third-party notices

WSF is under the MIT licence (`LICENSE`). Its dependencies have the following licences.

| Component | Licence | Use in WSF |
|---|---|---|
| xsflib, Copyright (c) 2026 the xsflib authors | MIT (`externals/xsflib/LICENSE`) | In `externals/xsflib`, a submodule, compiled into every WSF program: the PSF container, playback, the length analysis and the tools' frames. |
| ntdlib, Copyright (c) 2026 the ntdlib authors | MIT (`externals/ntdlib/LICENSE`) | In `externals/ntdlib`, a submodule, compiled into every WSF program: the disc reader, the Broadway CPU, the DSP, the hardware that the GameCube and the Wii share, the sound archive readers, and the channels, sequence player and tables of NintendoWare's sound libraries. Its notices (`externals/ntdlib/THIRD-PARTY-NOTICES.md`) describe the reference material behind its code. |
| Data from Unicode: JIS0208.TXT, © 2015 Unicode®, Inc. | Unicode License v3 (`Unicode-3.0`) | In ntdlib's `src/disc/jis0208.h`, compiled into `wsfrip`. The table decodes the Shift-JIS text of Wii discs: the names in their file systems and the titles of Japanese discs. ntdlib's notices give its source. |
| seqlib, Copyright (c) 2026 the seqlib authors | MIT (`externals/xsflib/externals/seqlib/LICENSE`) | In `externals/xsflib/externals/seqlib`, a submodule of xsflib. Only the tests use it: their checks come from it, through `xsflib::testing`. |
| [zlib](https://zlib.net), Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler | zlib | The PSF container, and GCZ disc images. The installed copy is used if there is one. Otherwise v1.3.1 is downloaded when configuring. |
| [Zstandard](https://github.com/facebook/zstd), Copyright (c) Meta Platforms, Inc. and affiliates | BSD-3-Clause | RVZ disc images. The installed copy is used if there is one. Otherwise v1.5.7 is downloaded when configuring. |
| [liblzma](https://tukaani.org/xz/) from XZ Utils | 0BSD | LZMA and LZMA2 in WIA and RVZ disc images. The installed copy is used if there is one. Otherwise v5.8.1 is downloaded when configuring. |
| [bzip2](https://sourceware.org/bzip2/), Copyright (C) 1996-2019 Julian R Seward | bzip2 (BSD-style) | bzip2 in WIA and RVZ disc images. The installed copy is used if there is one. Otherwise 1.0.8 is downloaded when configuring. |
| [libFLAC](https://xiph.org/flac/), Copyright the Xiph.Org Foundation | BSD-3-Clause | FLAC output in `wsfplay`. The installed copy is used if there is one. Otherwise 1.5.0 is downloaded when configuring. |
| [foobar2000 SDK](https://www.foobar2000.org/SDK), Copyright Peter Pawlowski | BSD-style (the SDK's `sdk-license.txt`, and `pfc-license.txt` for its pfc library) | Only for `plugins/foobar2000`. Not included here. The SDK is supplied when building the component. |

Binary distributions must include the MIT notices of WSF, xsflib and ntdlib, the Unicode License v3's notice when
ntdlib's disc reader is linked in (as in `wsfrip`), the Zstandard notice when Zstandard is linked in, and the libFLAC
notice when libFLAC is linked in. Shipping `LICENSE` and this file does that. The licence texts are at the end of this
file. The licences of zlib, liblzma and bzip2, and those of the foobar2000 SDK and its pfc library, require no notice
in binaries. seqlib's checks go only into tests.

## Reference material (no code taken)

- [Dolphin](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later) is the reference for how the Wii behaves. No code
  is taken from it. Where WSF follows Dolphin's behaviour, the code was written from a description of that behaviour.
  The writer hadn't read Dolphin's source for that part. Dolphin's documentation of the WIA and RVZ formats
  (`docs/WiaAndRvz.md`) describes those formats and the generator of the padding data on GameCube and Wii discs.
- [Wiimm's ISO Tools](https://wit.wiimm.de) document the WDF and CISO formats.
- [WiiBrew](https://wiibrew.org) documents the Wii's disc format.

## Game data

Nothing in this repository licenses game data. Rips (`.wsflib`) contain a game's code and sound archive. These belong to
their copyright holders.

ntdlib's `include/ntdlib/nwsnd/tables.h` holds numeric tables that NW4R SND uses: powers of two, decibel levels, pan
curves, the envelope's attack multipliers, the LFO's sine, the low-pass filter's cutoffs and the coefficients of the
biquad filter's presets.
`src/ax/tables.h` holds the gains of AX's output compressor. ntdlib's `include/ntdlib/dsp/coef_rom.h` holds the DSP's
coefficient ROM. Every GameCube and Wii has the same ROM. It holds the 4-tap filters that AX's resampler reads: 128
phases for each of three cutoffs. The player uses the exact values to reproduce the console's output.

## Licence texts

### xsflib

```
MIT License

Copyright (c) 2026 the xsflib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### ntdlib

```
MIT License

Copyright (c) 2026 the ntdlib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Zstandard

```
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### libFLAC

```
Copyright (C) 2000-2009  Josh Coalson
Copyright (C) 2011-2025  Xiph.Org Foundation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.Org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### zlib

```
Copyright notice:

 (C) 1995-2022 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```

### liblzma (0BSD)

```
Permission to use, copy, modify, and/or distribute this
software for any purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

### bzip2

```

--------------------------------------------------------------------------

This program, "bzip2", the associated library "libbzip2", and all
documentation, are copyright (C) 1996-2019 Julian R Seward.  All
rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. The origin of this software must not be misrepresented; you must 
   not claim that you wrote the original software.  If you use this 
   software in a product, an acknowledgment in the product 
   documentation would be appreciated but is not required.

3. Altered source versions must be plainly marked as such, and must
   not be misrepresented as being the original software.

4. The name of the author may not be used to endorse or promote 
   products derived from this software without specific prior written 
   permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Julian Seward, jseward@acm.org
bzip2/libbzip2 version 1.0.8 of 13 July 2019

--------------------------------------------------------------------------
```

### Unicode License v3

```
UNICODE LICENSE V3

COPYRIGHT AND PERMISSION NOTICE

Copyright © 1991-2026 Unicode, Inc.

NOTICE TO USER: Carefully read the following legal agreement. BY
DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.

Permission is hereby granted, free of charge, to any person obtaining a
copy of data files and any associated documentation (the "Data Files") or
software and any associated documentation (the "Software") to deal in the
Data Files or Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, and/or sell
copies of the Data Files or Software, and to permit persons to whom the
Data Files or Software are furnished to do so, provided that either (a)
this copyright and permission notice appear with all copies of the Data
Files or Software, or (b) this copyright and permission notice appear in
associated Documentation.

THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
THIRD PARTY RIGHTS.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
FILES OR SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall
not be used in advertising or otherwise to promote the sale, use or other
dealings in these Data Files or Software without prior written
authorization of the copyright holder.
```
