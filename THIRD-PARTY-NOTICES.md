# Third-party notices

WSF is under the MIT licence (`LICENSE`). Its dependencies have the following licences.

| Component | Licence | Use in WSF |
|---|---|---|
| [zlib](https://zlib.net), Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler | zlib | The PSF container, and GCZ disc images. The installed copy is used if there is one. Otherwise v1.3.1 is downloaded when configuring. |
| [Zstandard](https://github.com/facebook/zstd), Copyright (c) Meta Platforms, Inc. and affiliates | BSD-3-Clause | RVZ disc images. The installed copy is used if there is one. Otherwise v1.5.7 is downloaded when configuring. |
| [liblzma](https://tukaani.org/xz/) from XZ Utils | 0BSD | LZMA and LZMA2 in WIA and RVZ disc images. The installed copy is used if there is one. Otherwise v5.8.1 is downloaded when configuring. |
| [bzip2](https://sourceware.org/bzip2/), Copyright (C) 1996-2019 Julian R Seward | bzip2 (BSD-style) | bzip2 in WIA and RVZ disc images. The installed copy is used if there is one. Otherwise 1.0.8 is downloaded when configuring. |
| [libFLAC](https://xiph.org/flac/), Copyright the Xiph.Org Foundation | BSD-3-Clause | FLAC output in `wsfplay`. The installed copy is used if there is one. Otherwise 1.5.0 is downloaded when configuring. |
| [foobar2000 SDK](https://www.foobar2000.org/SDK), Copyright Peter Pawlowski | BSD-style (the SDK's `sdk-license.txt`, and `pfc-license.txt` for its pfc library) | Only for `plugins/foobar2000`. Not included here. The SDK is supplied when building the component. |

Binary distributions must include WSF's MIT notice, the Zstandard notice when Zstandard is linked in, and the libFLAC
notice when libFLAC is linked in. Shipping `LICENSE` and this file does that. The licence texts are at the end of this
file. The licences of zlib, liblzma and bzip2, and those of the foobar2000 SDK and its pfc library, require no notice in
binaries.

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

`src/nw4r/tables.h` holds the numeric tables that NW4R SND uses: powers of two, decibel levels, pan curves, the
envelope's attack multipliers, the LFO's sine, the Wii Remote's filter coefficients and the coefficients of the biquad
filter's presets. `src/ax/tables.h` holds the gains of AX's output compressor. `src/dsp/coef_rom.h` holds the DSP's
coefficient ROM. Every GameCube and Wii has the same ROM. It holds the 4-tap filters that AX's resampler reads: 128
phases for each of three cutoffs. The player uses the exact values to reproduce the console's output.

## Licence texts

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
