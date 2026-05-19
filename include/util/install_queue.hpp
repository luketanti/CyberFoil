#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "shopInstall.hpp"

namespace inst::queue {

    enum class QueueItemState {
        Pending,
        Installing,
        Success,
        Failed,
        Canceled
    };

    struct QueueItem {
        shopInstStuff::ShopItem shopItem;
        int storageId = 0;              // 0 = SD, 1 = NAND
        std::string sourceLabel;
        QueueItemState state = QueueItemState::Pending;
        double progress = 0.0;          // 0.0 - 100.0
        std::string statusText;
        std::string errorText;
    };

    // Snapshot of the queue state for UI consumption (thread-safe copy)
    struct QueueSnapshot {
        std::vector<QueueItem> items;
        std::size_t currentIndex = 0;
        bool workerRunning = false;
        std::size_t completedCount = 0;
        std::size_t failedCount = 0;
        std::size_t totalCount = 0;
    };

    class InstallQueue {
    public:
        static InstallQueue& Instance();

        // Add items to the queue. Starts the worker if not already running.
        void Enqueue(const std::vector<shopInstStuff::ShopItem>& items, int storageId, const std::string& sourceLabel);

        // Get a thread-safe snapshot of the current queue state for UI rendering.
        QueueSnapshot GetSnapshot() const;

        // Check if the worker is currently processing.
        bool IsRunning() const;

        // Check if there are pending or in-progress items.
        bool HasPending() const;

        // Get a short status string for overlay display (e.g. "Installing 2/5: Game Name")
        std::string GetStatusText() const;

        // Get progress of the current item (0.0 - 100.0), or -1 if idle.
        double GetCurrentProgress() const;

        // Cancel all pending items (current install finishes, rest are dropped).
        void CancelPending();

        // Clear completed/failed items from the queue.
        void ClearFinished();

        // Called by the install worker to update progress of the current item.
        // This is meant to be called from the worker thread.
        void ReportProgress(double percent, const std::string& statusText);

        // Flag that signals the UI has new data to display (polled by UI thread).
        bool ConsumeUiDirtyFlag();

        // Pop the next batch of pending items for installation (main-thread driven).
        // Returns true if a batch was found. Marks those items as Installing.
        bool PopNextBatch(std::vector<shopInstStuff::ShopItem>& outItems, int& outStorageId, std::string& outSourceLabel);

        // Mark the current installing batch as complete (success or failure).
        void MarkBatchComplete(bool success);

    private:
        InstallQueue() = default;
        ~InstallQueue();
        InstallQueue(const InstallQueue&) = delete;
        InstallQueue& operator=(const InstallQueue&) = delete;

        void EnsureWorkerRunning();
        void WorkerMain();
        void InstallSingleItem(QueueItem& item);

        mutable std::mutex m_mutex;
        std::deque<QueueItem> m_queue;
        std::size_t m_currentIndex = 0;
        std::size_t m_completedCount = 0;
        std::size_t m_failedCount = 0;
        std::thread m_workerThread;
        std::atomic<bool> m_workerRunning{false};
        std::atomic<bool> m_stopRequested{false};
        std::atomic<bool> m_uiDirty{false};
    };

} // namespace inst::queue
