/*
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "install/http_nsp.hpp"

#include <atomic>
#include <cstdio>
#include <switch.h>
#include <threads.h>
#include "data/buffered_placeholder_writer.hpp"
#include "util/title_util.hpp"
#include "util/error.hpp"
#include "util/debug.h"
#include "util/util.hpp"
#include "util/lang.hpp"
#include "ui/instPage.hpp"
#include "ui/MainApplication.hpp"

namespace inst::ui { extern MainApplication *mainApp; }

namespace tin::install::nsp
{
    namespace {
        std::string DeriveDisplayNameFromUrl(const std::string& url)
        {
            const std::size_t fragmentPos = url.find('#');
            if (fragmentPos != std::string::npos && fragmentPos + 1 < url.size()) {
                const std::string fragment = url.substr(fragmentPos + 1);
                std::string decoded = inst::util::formatUrlString("http://dummy/" + fragment);
                if (!decoded.empty())
                    return decoded;
            }

            std::string fromPath = inst::util::formatUrlString(url);
            if (!fromPath.empty())
                return fromPath;
            return "content";
        }

        std::string FormatOneDecimal(double value)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", value);
            return std::string(buf);
        }

        std::string FormatEta(std::uint64_t totalSeconds)
        {
            const std::uint64_t h = totalSeconds / 3600;
            const std::uint64_t m = (totalSeconds % 3600) / 60;
            const std::uint64_t s = totalSeconds % 60;
            char buf[32];
            if (h > 0) {
                std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                    static_cast<unsigned long long>(h),
                    static_cast<unsigned long long>(m),
                    static_cast<unsigned long long>(s));
            } else {
                std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                    static_cast<unsigned long long>(m),
                    static_cast<unsigned long long>(s));
            }
            return std::string(buf);
        }
    }

    bool stopThreadsHttpNsp;

    HTTPNSP::HTTPNSP(std::string url) :
        m_download(url), m_displayName(DeriveDisplayNameFromUrl(url))
    {

    }

    struct RetryConfirmState
    {
        std::atomic<bool> pending{false};
        std::atomic<bool> approved{false};
    };

    struct StreamFuncArgs
    {
        tin::network::HTTPDownload* download;
        tin::data::BufferedPlaceholderWriter* bufferedPlaceholderWriter;
        u64 pfs0Offset;
        u64 ncaSize;
        RetryConfirmState retryConfirm;
    };

    int CurlStreamFunc(void* in)
    {
        StreamFuncArgs* args = reinterpret_cast<StreamFuncArgs*>(in);
        try {
            auto streamFunc = [&](u8* streamBuf, size_t streamBufSize) -> size_t
            {
                if (inst::ui::instPage::isInstallCancelRequested())
                    return 0;
                while (true)
                {
                    if (inst::ui::instPage::isInstallCancelRequested())
                        return 0;
                    if (args->bufferedPlaceholderWriter->CanAppendData(streamBufSize))
                        break;
                    svcSleepThread(100000ULL); // 0.1ms yield to avoid busy-spin
                }

                args->bufferedPlaceholderWriter->AppendData(streamBuf, streamBufSize);
                return streamBufSize;
            };

            auto retryConfirmFunc = [&]() -> bool {
                args->retryConfirm.pending.store(true);
                while (args->retryConfirm.pending.load() && !stopThreadsHttpNsp)
                    svcSleepThread(50 * 1000 * 1000ULL);
                return !stopThreadsHttpNsp && args->retryConfirm.approved.load();
            };

            if (args->download->StreamDataRange(args->pfs0Offset, args->ncaSize, streamFunc, retryConfirmFunc) == 1)
                stopThreadsHttpNsp = true;
        }
        catch (...) {
            stopThreadsHttpNsp = true;
        }
        return 0;
    }

    int PlaceholderWriteFunc(void* in)
    {
        StreamFuncArgs* args = reinterpret_cast<StreamFuncArgs*>(in);
        try {
            while (!args->bufferedPlaceholderWriter->IsPlaceholderComplete() && !stopThreadsHttpNsp)
            {
                if (inst::ui::instPage::isInstallCancelRequested()) {
                    stopThreadsHttpNsp = true;
                    break;
                }
                if (args->bufferedPlaceholderWriter->CanWriteSegmentToPlaceholder())
                    args->bufferedPlaceholderWriter->WriteSegmentToPlaceholder();
                else
                    svcSleepThread(100000ULL); // 0.1ms yield to avoid busy-spin
            }
        }
        catch (...) {
            stopThreadsHttpNsp = true;
        }

        return 0;
    }

    void HTTPNSP::StreamToPlaceholder(std::shared_ptr<nx::ncm::ContentStorage>& contentStorage, NcmContentId placeholderId)
    {
        const PFS0FileEntry* fileEntry = this->GetFileEntryByNcaId(placeholderId);
        std::string ncaFileName = this->GetFileEntryName(fileEntry);
        const std::string displayFileName = ncaFileName.empty() ? m_displayName : ncaFileName;

        LOG_DEBUG("Retrieving %s\n", displayFileName.c_str());
        size_t ncaSize = fileEntry->fileSize;

        tin::data::BufferedPlaceholderWriter bufferedPlaceholderWriter(contentStorage, placeholderId, ncaSize);
        StreamFuncArgs args;
        args.download = &m_download;
        args.bufferedPlaceholderWriter = &bufferedPlaceholderWriter;
        args.pfs0Offset = this->GetDataOffset() + fileEntry->dataOffset;
        args.ncaSize = ncaSize;
        thrd_t curlThread;
        thrd_t writeThread;

        stopThreadsHttpNsp = false;
        thrd_create(&curlThread, CurlStreamFunc, &args);
        thrd_create(&writeThread, PlaceholderWriteFunc, &args);

        u64 freq = armGetSystemTickFreq();
        u64 startTime = armGetSystemTick();
        size_t startSizeBuffered = 0;
        double emaSpeed = 0.0;

        inst::ui::instPage::setInstBarPerc(0);
        while (!bufferedPlaceholderWriter.IsBufferDataComplete() && !stopThreadsHttpNsp)
        {
            if (inst::ui::instPage::isInstallCancelRequested()) {
                args.retryConfirm.approved.store(false);
                args.retryConfirm.pending.store(false);
                stopThreadsHttpNsp = true;
                break;
            }

            if (args.retryConfirm.pending.load())
            {
                int choice = inst::ui::mainApp->CreateShowDialog(
                    "inst.net.retry.title"_lang,
                    "inst.net.retry.desc"_lang,
                    {"inst.net.retry.yes"_lang, "inst.net.retry.no"_lang},
                    true);
                args.retryConfirm.approved.store(choice == 0);
                args.retryConfirm.pending.store(false);
            }
            u64 newTime = armGetSystemTick();

            if (newTime - startTime >= freq * 0.5)
            {
                size_t newSizeBuffered = bufferedPlaceholderWriter.GetSizeBuffered();
                double mbBuffered = (newSizeBuffered / (1024 * 1024)) - (startSizeBuffered / (1024 * 1024));
                double duration = ((double)(newTime - startTime) / (double)freq);
                double speed =  mbBuffered / duration;

                if (emaSpeed <= 0.0) {
                    emaSpeed = speed;
                } else {
                    emaSpeed = (emaSpeed * 0.7) + (speed * 0.3);
                }

                startTime = newTime;
                startSizeBuffered = newSizeBuffered;

                const double sizeBuffered = static_cast<double>(bufferedPlaceholderWriter.GetSizeBuffered());
                const double totalSize = static_cast<double>(bufferedPlaceholderWriter.GetTotalDataSize());
                int downloadProgress = (int)((sizeBuffered / totalSize) * 100.0);

                std::string etaText = "--:--";
                if (emaSpeed > 0.0 && totalSize > sizeBuffered) {
                    const double remainingMb = (totalSize - sizeBuffered) / (1024 * 1024);
                    const double etaSecondsF = remainingMb / emaSpeed;
                    if (etaSecondsF < 86400.0) {
                        etaText = FormatEta(static_cast<std::uint64_t>(etaSecondsF));
                    }
                }

                inst::ui::instPage::setInstInfoText("inst.info_page.downloading"_lang + inst::util::formatUrlString(displayFileName) + "inst.info_page.at"_lang + FormatOneDecimal(emaSpeed) + " MB/s");
                inst::ui::instPage::setInstBarPerc((double)downloadProgress);
                inst::ui::instPage::setProgressDetailText(
                    "Downloaded " + FormatOneDecimal(sizeBuffered / (1024 * 1024)) + " / " +
                    FormatOneDecimal(totalSize / (1024 * 1024)) + " MB (" +
                    std::to_string(downloadProgress) + "%) • ETA " + etaText
                );
            }
        }
        inst::ui::instPage::setInstBarPerc(100);
        inst::ui::instPage::setProgressDetailText("Downloaded 100% • Verifying and installing...");

        inst::ui::instPage::setInstInfoText("inst.info_page.top_info0"_lang + displayFileName + "...");
        inst::ui::instPage::setInstBarPerc(0);
        while (!bufferedPlaceholderWriter.IsPlaceholderComplete() && !stopThreadsHttpNsp)
        {
            if (inst::ui::instPage::isInstallCancelRequested()) {
                args.retryConfirm.approved.store(false);
                args.retryConfirm.pending.store(false);
                stopThreadsHttpNsp = true;
                break;
            }
            int installProgress = (int)(((double)bufferedPlaceholderWriter.GetSizeWrittenToPlaceholder() / (double)bufferedPlaceholderWriter.GetTotalDataSize()) * 100.0);

            inst::ui::instPage::setInstBarPerc((double)installProgress);
            inst::ui::instPage::setProgressDetailText(
                "Installing " + FormatOneDecimal((double)bufferedPlaceholderWriter.GetSizeWrittenToPlaceholder() / 1000000.0) + " / " +
                FormatOneDecimal((double)bufferedPlaceholderWriter.GetTotalDataSize() / 1000000.0) + " MB (" +
                std::to_string(installProgress) + "%)"
            );
        }
        inst::ui::instPage::setInstBarPerc(100);
        inst::ui::instPage::setProgressDetailText("Installing 100%");

        thrd_join(curlThread, NULL);
        thrd_join(writeThread, NULL);
        bufferedPlaceholderWriter.close();
        if (inst::ui::instPage::isInstallCancelRequested())
            THROW_FORMAT("Installation canceled.");
        if (stopThreadsHttpNsp) THROW_FORMAT(("inst.net.transfer_interput"_lang).c_str());
    }

    void HTTPNSP::BufferData(void* buf, off_t offset, size_t size)
    {
        m_download.BufferDataRange(buf, offset, size, nullptr);
    }
}
