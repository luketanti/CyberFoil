/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <haze.hpp>
#include <haze/console_main_loop.hpp>
#include <mutex>

namespace haze {
namespace {

std::mutex g_mutex;
std::unique_ptr<haze::ConsoleMainLoop> g_haze{};

} // namespace

bool Initialize(Callback callback, int prio, int cpuid, const FsEntries& entries, u16 vid, u16 pid) {
    std::scoped_lock lock{g_mutex};
    if (g_haze) {
        return false;
    }

    if (entries.empty()) {
        return false;
    }

    /* Load device firmware version and serial number. */
    HAZE_R_ABORT_UNLESS(haze::LoadDeviceProperties());

    g_haze = std::make_unique<haze::ConsoleMainLoop>(callback, prio, cpuid, entries, vid, pid);

    return true;
}

void Exit() {
    std::scoped_lock lock{g_mutex};
    if (!g_haze) {
        return;
    }

    /* this will block until thread exit. */
    g_haze.reset();
}

} // namespace haze
