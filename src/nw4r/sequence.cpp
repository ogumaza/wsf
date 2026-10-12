// SPDX-License-Identifier: MIT

#include "nw4r/sequence.h"

#include "ntdlib/nwsnd/sequence_impl.h"

#include "nw4r/engine.h"

template class ntdlib::nwsnd::SequenceTrack<wsf::nw4r::SequencePolicy>;
template class ntdlib::nwsnd::SequenceSoundPlayer<wsf::nw4r::SequencePolicy>;
