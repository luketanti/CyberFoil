#pragma once

#include <switch.h>
#include <vapours/results.hpp>

namespace sphaira::utils {

using Result = ams::Result;

inline Result CreateThread(Thread *t, ThreadFunc entry, void *arg, size_t stack_sz = 1024*128, int prio = 0x3B) {
    u64 core_mask = 0;
    R_TRY(svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0));
    R_TRY(threadCreate(t, entry, arg, nullptr, stack_sz, prio, -2));
    R_TRY(svcSetThreadCoreMask(t->handle, -1, core_mask));
    R_SUCCEED();
}

} // namespace sphaira::utils
