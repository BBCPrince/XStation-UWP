// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
namespace Libraries::AvPlayer {
// An empty queue does not mean finished: a decoder may have popped its final
// packet and not published the resulting frame yet. Completion must be stable.
constexpr bool PlaybackHasWork(bool demux_eof, bool decoders_finished, std::size_t queued) {
    return !demux_eof || !decoders_finished || queued != 0;
}
} // namespace Libraries::AvPlayer
