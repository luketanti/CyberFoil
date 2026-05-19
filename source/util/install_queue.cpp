#include "util/install_queue.hpp"
#include "util/util.hpp"
#include "util/config.hpp"
#include "shopInstall.hpp"
#include <algorithm>
#include <cstdio>

namespace inst::queue {

    InstallQueue& InstallQueue::Instance() {
        static InstallQueue instance;
        return instance;
    }

    InstallQueue::~InstallQueue() {
        m_stopRequested.store(true);
        if (m_workerThread.joinable())
            m_workerThread.join();
    }

    void InstallQueue::Enqueue(const std::vector<shopInstStuff::ShopItem>& items, int storageId, const std::string& sourceLabel) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& item : items) {
            QueueItem qi;
            qi.shopItem = item;
            qi.storageId = storageId;
            qi.sourceLabel = sourceLabel;
            qi.state = QueueItemState::Pending;
            qi.statusText = "Queued";
            m_queue.push_back(std::move(qi));
        }
        m_uiDirty.store(true);
    }

    QueueSnapshot InstallQueue::GetSnapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        QueueSnapshot snap;
        snap.items.assign(m_queue.begin(), m_queue.end());
        snap.currentIndex = m_currentIndex;
        snap.workerRunning = m_workerRunning.load();
        snap.completedCount = m_completedCount;
        snap.failedCount = m_failedCount;
        snap.totalCount = m_queue.size();
        return snap;
    }

    bool InstallQueue::IsRunning() const {
        return m_workerRunning.load();
    }

    bool InstallQueue::HasPending() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& item : m_queue) {
            if (item.state == QueueItemState::Pending || item.state == QueueItemState::Installing)
                return true;
        }
        return false;
    }

    std::string InstallQueue::GetStatusText() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
            return "";

        std::size_t pendingAndInstalling = 0;
        std::string currentName;
        for (const auto& q : m_queue) {
            if (q.state == QueueItemState::Installing) {
                currentName = q.shopItem.name;
                pendingAndInstalling++;
            } else if (q.state == QueueItemState::Pending) {
                pendingAndInstalling++;
            }
        }

        if (!currentName.empty()) {
            if (currentName.size() > 28)
                currentName = currentName.substr(0, 25) + "...";
            return "Installing " + std::to_string(m_completedCount + 1) + "/" +
                   std::to_string(m_completedCount + pendingAndInstalling) + ": " + currentName;
        }

        if (pendingAndInstalling > 0)
            return "Queue: " + std::to_string(pendingAndInstalling) + " pending";

        if (m_completedCount > 0 && m_failedCount == 0)
            return "Done (" + std::to_string(m_completedCount) + " installed)";

        if (m_failedCount > 0)
            return "Done (" + std::to_string(m_completedCount) + " ok, " + std::to_string(m_failedCount) + " failed)";

        return "";
    }

    double InstallQueue::GetCurrentProgress() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& item : m_queue) {
            if (item.state == QueueItemState::Installing)
                return item.progress;
        }
        return -1.0;
    }

    void InstallQueue::CancelPending() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& item : m_queue) {
            if (item.state == QueueItemState::Pending) {
                item.state = QueueItemState::Canceled;
                item.statusText = "Canceled";
            }
        }
        m_uiDirty.store(true);
    }

    void InstallQueue::ClearFinished() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.erase(
            std::remove_if(m_queue.begin(), m_queue.end(), [](const QueueItem& item) {
                return item.state == QueueItemState::Success ||
                       item.state == QueueItemState::Failed ||
                       item.state == QueueItemState::Canceled;
            }),
            m_queue.end()
        );
        m_completedCount = 0;
        m_failedCount = 0;
        m_currentIndex = 0;
        m_uiDirty.store(true);
    }

    void InstallQueue::ReportProgress(double percent, const std::string& statusText) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& item : m_queue) {
            if (item.state == QueueItemState::Installing) {
                item.progress = percent;
                if (!statusText.empty())
                    item.statusText = statusText;
                break;
            }
        }
        m_uiDirty.store(true);
    }

    bool InstallQueue::ConsumeUiDirtyFlag() {
        return m_uiDirty.exchange(false);
    }

    bool InstallQueue::PopNextBatch(std::vector<shopInstStuff::ShopItem>& outItems, int& outStorageId, std::string& outSourceLabel) {
        std::lock_guard<std::mutex> lock(m_mutex);
        outItems.clear();
        bool foundFirst = false;

        for (auto& item : m_queue) {
            if (item.state != QueueItemState::Pending)
                continue;
            if (!foundFirst) {
                outStorageId = item.storageId;
                outSourceLabel = item.sourceLabel;
                foundFirst = true;
            }
            if (item.storageId == outStorageId && item.sourceLabel == outSourceLabel) {
                item.state = QueueItemState::Installing;
                item.statusText = "Installing...";
                outItems.push_back(item.shopItem);
            } else {
                break;
            }
        }

        if (!outItems.empty()) {
            m_workerRunning.store(true);
            m_uiDirty.store(true);
        }
        return !outItems.empty();
    }

    void InstallQueue::MarkBatchComplete(bool success) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& item : m_queue) {
            if (item.state == QueueItemState::Installing) {
                if (success) {
                    item.state = QueueItemState::Success;
                    item.statusText = "Installed";
                    item.progress = 100.0;
                    m_completedCount++;
                } else {
                    item.state = QueueItemState::Failed;
                    item.statusText = "Failed";
                    m_failedCount++;
                }
            }
        }
        m_workerRunning.store(false);
        m_uiDirty.store(true);
    }

    void InstallQueue::EnsureWorkerRunning() {
        // Not used in main-thread-driven approach
    }

    void InstallQueue::WorkerMain() {
        // Not used in main-thread-driven approach
    }

    void InstallQueue::InstallSingleItem(QueueItem& item) {
        (void)item;
    }

} // namespace inst::queue
