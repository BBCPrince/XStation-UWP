// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace BbSampler {
/// bbport (BB_IP_SAMPLE=<delay s>[,<duration s>]): samples the calling thread's instruction
/// pointer once the delay has passed; the first call decides the thread.
/// `who`: "command" or "recorder"; only the thread named by BB_IP_SAMPLE_THREAD (default
/// "command") is sampled.
void Register(const char* who = "command");
/// Frame time (BB_FRAME_STATS flip path). BB_IP_SAMPLE_STALL_MS=<ms>: sampling starts after the
/// delay only once a frame at least that long was seen (Detroit hallway stall, 2026-11-10).
void NoteFrame(double ms);
} // namespace BbSampler
