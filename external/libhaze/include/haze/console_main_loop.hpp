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
#pragma once

#include <haze/event_reactor.hpp>
#include <haze/ptp_object_heap.hpp>
#include <haze/ptp_responder.hpp>
#include <haze/thread.hpp>

namespace haze {

    class ConsoleMainLoop final : EventConsumer {
        private:
            const Callback m_callback;
            const int m_prio;
            const int m_cpuid;
            const FsEntries m_entries;
            const u16 m_vid;
            const u16 m_pid;
            Thread m_thread{};
            UEvent m_cancel_event{};
            EventReactor m_event_reactor{};

        public:
            explicit ConsoleMainLoop(Callback callback, int prio, int cpuid, const FsEntries& entries, u16 vid = 0x057e, u16 pid = 0x201d)
            : m_callback{callback}, m_prio{prio}, m_cpuid{cpuid}, m_entries{entries}, m_vid{vid}, m_pid{pid} {
                /* Create cancel event. */
                ueventCreate(&m_cancel_event, false);

                /* Clear the event reactor. */
                m_event_reactor.SetResult(ResultSuccess());
                m_event_reactor.AddConsumer(this, waiterForUEvent(&m_cancel_event));

                /* Create and start thread. */
                sphaira::utils::CreateThread(&m_thread, thread_func, this, 1024*64);
                threadStart(&m_thread);
            }

            ~ConsoleMainLoop() {
                ueventSignal(&m_cancel_event);
                threadWaitForExit(&m_thread);
                threadClose(&m_thread);
            }

        public:
            void RunApplication() {
                /* Declare the object heap, to hold the database for an active session. */
                PtpObjectHeap ptp_object_heap;

                /* Configure the PTP responder. */
                PtpResponder ptp_responder{m_callback};
                ptp_responder.Initialize(std::addressof(m_event_reactor), std::addressof(ptp_object_heap), m_entries, m_vid, m_pid);

                /* Ensure we maintain a clean state on exit. */
                ON_SCOPE_EXIT {
                    /* Finalize the PTP responder. */
                    ptp_responder.Finalize();
                };

                /* Begin processing requests. */
                ptp_responder.LoopProcess();
            }

            static void thread_func(void* user) {
                static_cast<ConsoleMainLoop*>(user)->RunApplication();
            }

        private:
            void ProcessEvent() override {
                m_event_reactor.SetResult(haze::ResultStopRequested());
            };
    };

}
