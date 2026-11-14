// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace BbSampler {
/// bbport (BB_IP_SAMPLE=<delay s>[,<duration s>]): samples the calling thread's instruction
/// pointer once the delay has passed; the first call decides the thread.
/// `who`: "command" or "recorder"; only the thread named by BB_IP_SAMPLE_THREAD (default
/// "command") is sampled.
void Register(const char* who = "command");
} // namespace BbSampler
