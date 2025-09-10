// Lightweight debug hook for internal decode steps.
#pragma once
namespace lora { namespace debug {
inline thread_local int last_fail_step = 0; // set by RX helpers on failure paths
inline void set_fail(int code) { last_fail_step = code; }
} } // namespace lora::debug

