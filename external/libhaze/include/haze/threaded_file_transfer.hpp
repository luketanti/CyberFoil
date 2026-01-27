#pragma once

#include <switch.h>
#include <vapours/results.hpp>
#include <functional>

namespace sphaira::thread {

using Result = ams::Result;

enum class Mode {
    // default, always multi-thread.
    MultiThreaded,
    // always single-thread.
    SingleThreaded,
    // check buffer size, if smaller, single thread.
    SingleThreadedIfSmaller,
};

using ReadCallback = std::function<Result(void* data, s64 off, s64 size, u64* bytes_read)>;
using WriteCallback = std::function<Result(const void* data, s64 off, s64 size)>;

// reads data from rfunc into wfunc.
Result Transfer(s64 size, const ReadCallback& rfunc, const WriteCallback& wfunc, Mode mode = Mode::MultiThreaded);

} // namespace sphaira::thread
