#include <filesystem>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <stdexcept>
#include "ui/MainApplication.hpp"
#include "ui/instPage.hpp"
#include "util/util.hpp"
#include "util/config.hpp"
#include "util/offline_title_db.hpp"
#include <limits>
#include "mtp_install.hpp"
#include "mtp_server.hpp"
#include "ui/bottomHint.hpp"
#include <switch.h>

#include "util/theme.hpp"

#define COLOR(hex) inst::theme::Palette(hex)

namespace inst::ui {
    extern MainApplication *mainApp;
    static pu::ui::Layout::Ref lastLayoutBeforeInstall;
    static std::atomic<bool> g_installCancelRequested{false};
    static std::atomic<bool> g_installDimSessionActive{false};
    static std::atomic<bool> g_installDimStopRequested{false};
    static std::thread g_installDimThread;
    static std::mutex g_installDimMutex;
    static bool g_installDimLblReady = false;
    static bool g_installDimSavedBrightnessValid = false;
    static bool g_installDimIsDimmed = false;
    static bool g_installScreenOff = false;
    static bool g_installSavedAutoSleepDisabled = false;
    static bool g_installSavedAutoSleepValid = false;
    static float g_installDimSavedBrightness = 0.0f;
    static std::chrono::steady_clock::time_point g_installDimLastTouchAt;

    constexpr int kInstallIconSize = 256;
    constexpr int kInstallIconX = (1280 - kInstallIconSize) / 2;
    constexpr int kInstallIconY = 220;
    constexpr auto kInstallDimDelay = std::chrono::seconds(60);
    constexpr auto kInstallDimPollInterval = std::chrono::milliseconds(250);
    constexpr float kInstallDimBrightness = 0.2f;
    constexpr size_t kQueueVisibleRows = 6;

    static void FitQueueText(TextBlock::Ref text, const std::string& value, int width) {
        text->SetText(value);
        if (text->GetTextWidth() <= width) return;
        std::vector<size_t> cuts{0};
        for (size_t i = 1; i <= value.size(); ++i)
            if (i == value.size() || (static_cast<unsigned char>(value[i]) & 0xc0) != 0x80) cuts.push_back(i);
        size_t low = 0, high = cuts.size() - 1;
        while (low < high) {
            const size_t mid = (low + high + 1) / 2;
            text->SetText(value.substr(0, cuts[mid]) + "…");
            if (text->GetTextWidth() <= width) low = mid;
            else high = mid - 1;
        }
        text->SetText(value.substr(0, cuts[low]) + "…");
    }

    static void PositionInstallIcon() {
        const bool queue = mainApp->instpage->HasQueuePresentation();
        auto icon = mainApp->instpage->installIconImage;
        icon->SetX(queue ? 48 : kInstallIconX);
        icon->SetY(queue ? 278 : kInstallIconY);
        icon->SetWidth(queue ? 180 : kInstallIconSize);
        icon->SetHeight(queue ? 180 : kInstallIconSize);
    }

    static void RestoreInstallBrightnessLocked()
    {
        if (g_installScreenOff && g_installDimLblReady) {
            lblSwitchBacklightOn(0);
            g_installScreenOff = false;
        }
        if (!g_installDimLblReady || !g_installDimSavedBrightnessValid || !g_installDimIsDimmed)
            return;

        lblSetCurrentBrightnessSetting(g_installDimSavedBrightness);
        lblApplyCurrentBrightnessSettingToBacklight();
        g_installDimIsDimmed = false;
    }

    static void EnsureInstallDimmedLocked()
    {
        if (!g_installDimLblReady || !g_installDimSavedBrightnessValid || g_installDimIsDimmed || g_installScreenOff)
            return;

        const float targetBrightness = std::min(g_installDimSavedBrightness, kInstallDimBrightness);
        if (targetBrightness >= g_installDimSavedBrightness)
            return;

        if (R_SUCCEEDED(lblSetCurrentBrightnessSetting(targetBrightness)) &&
            R_SUCCEEDED(lblApplyCurrentBrightnessSettingToBacklight())) {
            g_installDimIsDimmed = true;
        }
    }

    static void StartInstallDimSession()
    {
        std::lock_guard<std::mutex> lock(g_installDimMutex);
        g_installDimLastTouchAt = std::chrono::steady_clock::now();

        if (g_installDimSessionActive)
            return;

        g_installDimStopRequested = false;
        g_installSavedAutoSleepValid = R_SUCCEEDED(appletIsAutoSleepDisabled(&g_installSavedAutoSleepDisabled));
        if (g_installSavedAutoSleepValid) appletSetAutoSleepDisabled(true);
        g_installDimLblReady = R_SUCCEEDED(lblInitialize());
        g_installDimSavedBrightnessValid = false;
        g_installDimIsDimmed = false;
        g_installDimSavedBrightness = 0.0f;

        if (g_installDimLblReady) {
            float currentBrightness = 0.0f;
            if (R_SUCCEEDED(lblGetCurrentBrightnessSetting(&currentBrightness))) {
                g_installDimSavedBrightness = currentBrightness;
                g_installDimSavedBrightnessValid = true;
            }
        }

        g_installDimSessionActive = true;
        g_installDimThread = std::thread([]() {
            while (!g_installDimStopRequested) {
                {
                    std::lock_guard<std::mutex> lock(g_installDimMutex);
                    if (!g_installDimSessionActive)
                        break;
                    if (std::chrono::steady_clock::now() - g_installDimLastTouchAt >= kInstallDimDelay)
                        EnsureInstallDimmedLocked();
                }
                std::this_thread::sleep_for(kInstallDimPollInterval);
            }
        });
    }

    static void StopInstallDimSession()
    {
        g_installDimStopRequested = true;
        if (g_installDimThread.joinable())
            g_installDimThread.join();

        std::lock_guard<std::mutex> lock(g_installDimMutex);
        RestoreInstallBrightnessLocked();
        if (g_installSavedAutoSleepValid) appletSetAutoSleepDisabled(g_installSavedAutoSleepDisabled);
        g_installSavedAutoSleepValid = false;
        if (g_installDimLblReady)
            lblExit();
        g_installDimLblReady = false;
        g_installDimSavedBrightnessValid = false;
        g_installDimSessionActive = false;
    }

    static void NotifyInstallTouchActivity()
    {
        std::lock_guard<std::mutex> lock(g_installDimMutex);
        if (!g_installDimSessionActive)
            return;

        g_installDimLastTouchAt = std::chrono::steady_clock::now();
        RestoreInstallBrightnessLocked();
    }

    instPage::instPage() : Layout::Layout() {
        inst::theme::ApplyBackground(*this, inst::config::oledMode, inst::config::appDir);
        const auto topColor = inst::theme::Color("header", inst::config::oledMode ? COLOR("#000000FF") : COLOR("#170909FF"));
        const auto infoColor = inst::theme::Color("panel", inst::config::oledMode ? COLOR("#000000FF") : COLOR("#17090980"));
        const auto botColor = inst::theme::Color("footer", inst::config::oledMode ? COLOR("#000000FF") : COLOR("#17090980"));
        this->topRect = Rectangle::New(0, 0, 1280, 74, topColor);
        this->infoRect = Rectangle::New(0, 75, 1280, 60, infoColor);
        this->botRect = Rectangle::New(0, 660, 1280, 60, botColor);
        if (inst::config::gayMode) {
            this->titleImage = Image::New(-113, -8, inst::theme::Asset("romfs:/images/logo.png"));
            this->appVersionText = TextBlock::New(367, 29, "v" + inst::config::appVersion + (inst::config::appGitMeta.empty() ? "" : ("\n" + inst::config::appGitMeta)), 22);
        }
        else {
            this->titleImage = Image::New(0, -8, inst::theme::Asset("romfs:/images/logo.png"));
            this->appVersionText = TextBlock::New(480, 29, "v" + inst::config::appVersion + (inst::config::appGitMeta.empty() ? "" : ("\n" + inst::config::appGitMeta)), 22);
        }
        // Keep external banners within the existing header geometry.
        this->titleImage->SetWidth(480);
        this->titleImage->SetHeight(93);
        this->appVersionText->SetColor(COLOR("#FFFFFFFF"));
        this->timeText = TextBlock::New(0, 18, "--:--", 22);
        this->timeText->SetColor(COLOR("#FFFFFFFF"));
        this->ipText = TextBlock::New(0, 26, "IP: --", 16);
        this->ipText->SetColor(COLOR("#FFFFFFFF"));
        this->sysLabelText = TextBlock::New(0, 6, "System Memory", 16);
        this->sysLabelText->SetColor(COLOR("#FFFFFFFF"));
        this->sysFreeText = TextBlock::New(0, 42, "Free --", 16);
        this->sysFreeText->SetColor(COLOR("#FFFFFFFF"));
        this->sdLabelText = TextBlock::New(0, 6, "microSD Card", 16);
        this->sdLabelText->SetColor(COLOR("#FFFFFFFF"));
        this->sdFreeText = TextBlock::New(0, 42, "Free --", 16);
        this->sdFreeText->SetColor(COLOR("#FFFFFFFF"));
        this->sysBarBack = Rectangle::New(0, 30, 180, 6, COLOR("#FFFFFF33"));
        this->sysBarFill = Rectangle::New(0, 30, 0, 6, COLOR("#FF4D4DFF"));
        this->sdBarBack = Rectangle::New(0, 30, 180, 6, COLOR("#FFFFFF33"));
        this->sdBarFill = Rectangle::New(0, 30, 0, 6, COLOR("#FF4D4DFF"));
        this->netIndicator = Rectangle::New(0, 0, 6, 6, COLOR("#FF3B30FF"), 3);
        this->wifiBar1 = Rectangle::New(0, 0, 4, 4, COLOR("#FFFFFF55"));
        this->wifiBar2 = Rectangle::New(0, 0, 4, 7, COLOR("#FFFFFF55"));
        this->wifiBar3 = Rectangle::New(0, 0, 4, 10, COLOR("#FFFFFF55"));
        this->batteryOutline = Rectangle::New(0, 0, 24, 12, COLOR("#FFFFFF66"));
        this->batteryFill = Rectangle::New(0, 0, 0, 10, COLOR("#4CD964FF"));
        this->batteryCap = Rectangle::New(0, 0, 3, 6, COLOR("#FFFFFF66"));
        this->pageInfoText = TextBlock::New(10, 89, "", 30);
        this->pageInfoText->SetColor(COLOR("#FFFFFFFF"));
        this->installInfoText = TextBlock::New(15, 568, "", 22);
        this->installInfoText->SetColor(COLOR("#FFFFFFFF"));
        this->installBar = pu::ui::elm::ProgressBar::New(10, 600, 850, 40, 100.0f);
        this->installBar->SetColor(COLOR("#222222FF"));
        this->installBar->SetProgressColor(COLOR("#FF4D4DFF"));
        this->hintText = TextBlock::New(0, 678, " Back", 20);
        this->hintText->SetColor(COLOR("#FFFFFFFF"));
        this->hintText->SetX(1280 - 10 - this->hintText->GetTextWidth());
        this->hintText->SetVisible(false);
        this->bottomHintSegments = BuildBottomHintSegments(this->hintText->GetText(), this->hintText->GetX(), 20);
        this->progressText = TextBlock::New(0, 340, "", 30);
        this->progressText->SetColor(COLOR("#FFFFFFFF"));
        this->progressText->SetVisible(false);
        this->progressDetailText = TextBlock::New(0, 646, "", 22);
        this->progressDetailText->SetColor(COLOR("#FFFFFFFF"));
        this->progressDetailText->SetVisible(false);
        // A theme's own mascot (or an empty path hiding it) wins over the legacy override file.
        if (!inst::theme::HasImage("romfs:/images/awoos/7d8a05cddfef6da4901b20d2698d5a71.png") &&
            std::filesystem::exists(inst::config::appDir + "/awoo_inst.png")) this->awooImage = Image::New(410, 190, inst::config::appDir + "/awoo_inst.png");
        else this->awooImage = Image::New(510, 166, inst::theme::Asset("romfs:/images/awoos/7d8a05cddfef6da4901b20d2698d5a71.png"));
        this->installIconImage = Image::New(kInstallIconX, kInstallIconY, inst::theme::Asset("romfs:/images/awoos/7d8a05cddfef6da4901b20d2698d5a71.png"));
        this->installIconImage->SetWidth(kInstallIconSize);
        this->installIconImage->SetHeight(kInstallIconSize);
        this->installIconImage->SetVisible(false);
        auto addQueueElement = [this](pu::ui::elm::Element::Ref element) {
            element->SetVisible(false); this->queueElements.push_back(element); this->Add(element);
        };
        addQueueElement(Rectangle::New(24, 152, 480, 486, COLOR("#17171CF5"), 14));
        addQueueElement(Rectangle::New(524, 152, 732, 486, COLOR("#17171CF5"), 14));
        addQueueElement(Rectangle::New(48, 176, 4, 18, COLOR("#FF4D4DFF"), 2));
        this->Add(this->topRect);
        this->Add(this->infoRect);
        this->Add(this->botRect);
        this->Add(this->titleImage);
        this->Add(this->appVersionText);
        this->Add(this->sysBarBack);
        this->Add(this->sysBarFill);
        this->Add(this->sdBarBack);
        this->Add(this->sdBarFill);
        this->Add(this->sysLabelText);
        this->Add(this->sysFreeText);
        this->Add(this->sdLabelText);
        this->Add(this->sdFreeText);
        this->Add(this->netIndicator);
        this->Add(this->wifiBar1);
        this->Add(this->wifiBar2);
        this->Add(this->wifiBar3);
        this->Add(this->batteryOutline);
        this->Add(this->batteryFill);
        this->Add(this->batteryCap);
        this->Add(this->timeText);
        this->Add(this->ipText);
        this->Add(this->pageInfoText);
        this->Add(this->installInfoText);
        this->Add(this->installBar);
        this->Add(this->progressText);
        this->Add(this->progressDetailText);
        this->Add(this->hintText);
        this->Add(this->awooImage);
        this->Add(this->installIconImage);
        auto queueLabel = [&](int x, int y, const std::string& value, int size, pu::ui::Color color) {
            auto label = TextBlock::New(x, y, value, size);
            label->SetColor(color); addQueueElement(label); return label;
        };
        this->activeQueueLabel = queueLabel(64, 173, "NOW INSTALLING", 18, COLOR("#FF7777FF"));
        this->activeQueueTitle = queueLabel(48, 216, "", 24, COLOR("#FFFFFFFF"));
        this->queueProgressLabel = queueLabel(264, 326, "0%", 52, COLOR("#FFFFFFFF"));
        queueLabel(264, 391, "CONTENT PROGRESS", 14, COLOR("#A8A8B8FF"));
        this->queueHeader = queueLabel(548, 171, "UP NEXT", 26, COLOR("#FFFFFFFF"));
        this->queuePageText = queueLabel(0, 177, "", 18, COLOR("#A8A8B8FF"));
        this->queueSummary = queueLabel(548, 609, "", 16, COLOR("#A8A8B8FF"));
        this->queueEmptyText = queueLabel(572, 343, "", 22, COLOR("#A8A8B8FF"));
        for (size_t i = 0; i < kQueueVisibleRows; ++i) {
            const int y = 220 + static_cast<int>(i) * 62;
            auto row = Rectangle::New(540, y, 700, 56, COLOR("#222228FF"), 8);
            addQueueElement(row); this->queueRows.push_back(row);
            this->queueRowNumbers.push_back(queueLabel(552, y + 18, "", 16, COLOR("#A8A8B8FF")));
            auto icon = Image::New(600, y + 6, inst::theme::Asset("romfs:/images/icons/title-placeholder.png"));
            icon->SetWidth(44); icon->SetHeight(44); addQueueElement(icon);
            this->queueRowIcons.push_back(icon); this->queueRowIconPaths.push_back("");
            this->queueRowEntryIds.push_back(std::numeric_limits<size_t>::max()); this->queueRowIconLoaded.push_back(false);
            this->queueRowTitles.push_back(queueLabel(660, y + 5, "", 21, COLOR("#FFFFFFFF")));
            this->queueRowDetails.push_back(queueLabel(660, y + 32, "", 14, COLOR("#A8A8B8FF")));
            this->queueRowActions.push_back(queueLabel(1132, y + 20, "", 14, COLOR("#FF9999FF")));
        }
        this->screenBlank = Rectangle::New(0, 0, 1280, 720, COLOR("#000000FF"));
        this->screenBlank->SetVisible(false); this->Add(this->screenBlank);
        if (inst::config::gayMode) this->awooImage->SetVisible(false);
    }

    void instPage::setTopInstInfoText(std::string text) {
        if (!mainApp->IsUiThread()) { mainApp->PostInstallUiUpdate("title", [=] { setTopInstInfoText(text); }); return; }
        mainApp->instpage->pageInfoText->SetText(text);
        mainApp->CallForRender();
    }

    void instPage::setInstInfoText(std::string text) {
        if (!mainApp->IsUiThread()) { mainApp->PostInstallUiUpdate("info", [=] { setInstInfoText(text); }); return; }
        auto page = mainApp->instpage;
        if (page->installActive && isInstallCancelRequested()) text = "Cancelling install...";
        if (page->HasQueuePresentation()) FitQueueText(page->installInfoText, text, 432);
        else page->installInfoText->SetText(text);
        mainApp->CallForRender();
    }

    void instPage::setInstBarPerc(double percent) {
        if (!mainApp->IsUiThread()) { mainApp->PostInstallUiUpdate("bar", [=] { setInstBarPerc(percent); }); return; }
        auto page = mainApp->instpage;
        page->installBar->SetVisible(true); page->installBar->SetProgress(percent);
        if (page->HasQueuePresentation()) page->queueProgressLabel->SetText(std::to_string(static_cast<int>(std::clamp(percent, 0.0, 100.0))) + "%");
        mainApp->CallForRender();
    }

    void instPage::setProgressDetailText(const std::string& text) {
        if (!mainApp->IsUiThread()) { mainApp->PostInstallUiUpdate("detail", [=] { setProgressDetailText(text); }); return; }
        auto page = mainApp->instpage;
        std::string detail = text;
        if (page->HasQueuePresentation()) {
            const auto split = detail.find(" • ");
            if (split != std::string::npos) detail.replace(split, std::string(" • ").size(), "\n");
            page->progressDetailText->SetText(detail); page->progressDetailText->SetX(48);
        } else {
            page->progressDetailText->SetText(detail);
            page->progressDetailText->SetX((1280 - page->progressDetailText->GetTextWidth()) / 2);
        }
        page->progressDetailText->SetVisible(true); mainApp->CallForRender();
    }

    void instPage::clearProgressDetailText() {
        if (!mainApp->IsUiThread()) { mainApp->PostInstallUiUpdate("detail", [] { clearProgressDetailText(); }); return; }
        mainApp->instpage->progressDetailText->SetVisible(false); mainApp->CallForRender();
    }

    void instPage::setInstallIconFromTitleId(u64 titleId) {
        if (!titleId) return;
        auto data = std::make_unique<NsApplicationControlData>();
        size_t read = 0;
        if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, titleId, data.get(), sizeof(*data), &read)) || read <= sizeof(data->nacp)) return;
        setInstallIconData(data->icon, static_cast<std::uint32_t>(read - sizeof(data->nacp)));
    }

    void instPage::setInstallIcon(const std::string& path) {
        if (!mainApp->IsUiThread()) { mainApp->RunOnUiThread([=] { setInstallIcon(path); }); return; }
        if (path.empty()) { clearInstallIcon(); return; }
        mainApp->instpage->installIconImage->SetImage(path); PositionInstallIcon();
        mainApp->instpage->installIconImage->SetVisible(true); mainApp->instpage->awooImage->SetVisible(false);
        mainApp->CallForRender();
    }

    void instPage::setInstallIconData(const void* data, std::uint32_t size) {
        if (!mainApp->IsUiThread()) {
            // Wait for the UI to consume the caller-owned image bytes.
            mainApp->RunOnUiThread([=] { setInstallIconData(data, size); }); return;
        }
        if (!data || !size) { clearInstallIcon(); return; }
        mainApp->instpage->installIconImage->SetJpegImage(const_cast<void*>(data), size); PositionInstallIcon();
        mainApp->instpage->installIconImage->SetVisible(true); mainApp->instpage->awooImage->SetVisible(false);
        mainApp->CallForRender();
    }

    void instPage::clearInstallIcon() {
        if (!mainApp->IsUiThread()) { mainApp->RunOnUiThread([] { clearInstallIcon(); }); return; }
        if (mainApp->instpage->HasQueuePresentation()) { setInstallIcon(inst::theme::Asset("romfs:/images/icons/title-placeholder.png")); return; }
        mainApp->instpage->installIconImage->SetVisible(false);
        mainApp->instpage->awooImage->SetVisible(!inst::config::gayMode); mainApp->CallForRender();
    }

    void instPage::loadMainMenu() {
        if (!mainApp->IsUiThread()) { mainApp->RunOnUiThread([] { loadMainMenu(); }); return; }
        finishInstall();
        if (mainApp->IsInstallWorkActive()) return;
        mainApp->LoadLayout(lastLayoutBeforeInstall && lastLayoutBeforeInstall != mainApp->instpage ? lastLayoutBeforeInstall : mainApp->mainPage);
    }

    void instPage::loadInstallScreen(bool render) {
        if (!mainApp->IsUiThread()) {
            // The pump opened the screen before launching the worker. Never
            // clear a cancellation that arrived during service initialization.
            mainApp->RunOnUiThread([=] { if (!mainApp->IsInstallWorkActive()) loadInstallScreen(render); }); return;
        }
        auto page = mainApp->instpage;
        auto layout = mainApp->GetCurrentLayout();
        if (layout && layout != page) lastLayoutBeforeInstall = layout;
        page->queueEntries.clear(); page->queueEntryIds.clear(); page->queueNames.clear(); page->queueBrowser = {}; page->queueBrowserOpen = false;
        for (auto& element : page->queueElements) element->SetVisible(false);
        page->pageInfoText->SetText(""); page->installInfoText->SetText("");
        page->installInfoText->SetX(15); page->installInfoText->SetY(568); page->installInfoText->SetFontSize(22);
        page->installBar->SetX(10); page->installBar->SetY(600); page->installBar->SetWidth(850); page->installBar->SetHeight(40);
        page->installBar->SetProgress(0); page->installBar->SetVisible(false);
        page->progressDetailText->SetY(646); page->progressDetailText->SetFontSize(22); page->progressDetailText->SetVisible(false);
        page->progressText->SetVisible(false); page->hintText->SetVisible(true);
        PositionInstallIcon(); page->installIconImage->SetVisible(false); page->awooImage->SetVisible(!inst::config::gayMode);
        page->touchWakeActive = false; page->suppressWakeTouch = false; page->queueHeldDirection = 0; page->queueRepeatAt = 0;
        page->installActive = true; page->screenBlank->SetVisible(false);
        clearInstallCancel(); StartInstallDimSession(); page->refreshQueue(); mainApp->LoadLayout(page);
        if (render) mainApp->CallForRender();
    }

    void instPage::requestInstallCancel() { g_installCancelRequested.store(true); }
    bool instPage::isInstallCancelRequested() { return g_installCancelRequested.load(); }
    void instPage::clearInstallCancel() { g_installCancelRequested.store(false); }

    void instPage::configureQueue(const std::vector<std::string>& names, const std::vector<size_t>& selected) {
        std::vector<QueueEntry> entries;
        for (const auto& name : names) entries.push_back({name, "", "Waiting to install", ""});
        configureQueue(entries, selected);
    }

    void instPage::configureQueue(const std::vector<QueueEntry>& entries, const std::vector<size_t>& selected, std::function<void()> browser) {
        if (!mainApp->IsUiThread()) { mainApp->RunOnUiThread([=] { configureQueue(entries, selected, browser); }); return; }
        auto page = mainApp->instpage;
        page->queueEntries = entries; page->queueNames.clear(); page->queueEntryIds.clear();
        std::fill(page->queueRowIconLoaded.begin(), page->queueRowIconLoaded.end(), false);
        std::fill(page->queueRowEntryIds.begin(), page->queueRowEntryIds.end(), std::numeric_limits<size_t>::max());
        page->queueIconPollAt = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            page->queueNames.push_back(entries[i].name);
            if (!entries[i].key.empty()) page->queueEntryIds.emplace(entries[i].key, i);
        }
        page->queueBrowser = std::move(browser); page->queueBrowserOpen = false;
        page->queue.reset(entries.size(), selected); page->queueSelection = 0; page->activeQueueName.clear();
        for (auto& element : page->queueElements) element->SetVisible(!entries.empty());
        page->installInfoText->SetX(48); page->installInfoText->SetY(491); page->installInfoText->SetFontSize(16);
        page->installBar->SetX(48); page->installBar->SetY(554); page->installBar->SetWidth(432); page->installBar->SetHeight(18);
        page->progressDetailText->SetX(48); page->progressDetailText->SetY(587); page->progressDetailText->SetFontSize(16);
        page->awooImage->SetVisible(false); setInstallIcon(inst::theme::Asset("romfs:/images/icons/title-placeholder.png")); page->refreshQueue();
    }

    bool instPage::canEnqueueTitle(const std::string& key) {
        if (!mainApp->IsUiThread()) return mainApp->RunOnUiThread([=] { return canEnqueueTitle(key); });
        auto page = mainApp->instpage;
        if (!page->installActive || isInstallCancelRequested() || key.empty()) return false;
        const auto found = page->queueEntryIds.find(key);
        return found != page->queueEntryIds.end() && page->queue.state(found->second) == inst::InstallQueue::State::Available;
    }

    bool instPage::enqueueTitle(const std::string& key) {
        return enqueueTitles({key}) != 0;
    }

    size_t instPage::enqueueTitles(const std::vector<std::string>& keys) {
        if (!mainApp->IsUiThread()) return mainApp->RunOnUiThread([=] { return enqueueTitles(keys); });
        auto page = mainApp->instpage;
        if (!page->installActive || isInstallCancelRequested()) return 0;
        size_t added = 0;
        for (const auto& key : keys) {
            const auto found = page->queueEntryIds.find(key);
            if (found != page->queueEntryIds.end() && page->queue.add(found->second)) ++added;
        }
        if (added) page->refreshQueue();
        return added;
    }

    void instPage::showQueue() {
        if (!mainApp->IsUiThread()) { mainApp->RunOnUiThread([] { showQueue(); }); return; }
        mainApp->remoteinstPage->endQueueAddMode();
        mainApp->sdinstPage->endQueueAddMode(); mainApp->hddinstPage->endQueueAddMode();
        mainApp->instpage->queueBrowserOpen = false;
        mainApp->instpage->refreshQueue(); mainApp->LoadLayout(mainApp->instpage);
    }

    std::optional<size_t> instPage::nextQueuedItem() {
        if (!mainApp->IsUiThread()) {
            for (;;) {
                auto state = mainApp->RunOnUiThread([] { return std::make_pair(nextQueuedItem(), mainApp->instpage->queueBrowserOpen); });
                if (state.first || !state.second) return state.first;
                svcSleepThread(50000000);
            }
        }
        if (isInstallCancelRequested()) throw std::runtime_error("Installation canceled.");
        auto page = mainApp->instpage; auto id = page->queue.next();
        if (id) { page->activeQueueName = page->queueNames[*id]; page->refreshQueue(); }
        else if (!page->queueBrowserOpen) page->refreshQueue();
        mainApp->CallForRender(); return id;
    }

    void instPage::finishQueuedItem() {
        if (!mainApp->IsUiThread()) { mainApp->RunOnUiThread([] { finishQueuedItem(); }); return; }
        mainApp->instpage->queue.finish(); mainApp->instpage->activeQueueName.clear(); mainApp->instpage->refreshQueue();
    }

    void instPage::finishInstall() {
        if (!mainApp->IsUiThread()) { mainApp->RunOnUiThread([] { finishInstall(); }); return; }
        auto page = mainApp->instpage;
        if (page->queueBrowserOpen) showQueue();
        page->queueBrowser = {}; page->installActive = false; page->screenBlank->SetVisible(false); StopInstallDimSession();
        page->hintText->SetText(" Back"); page->hintText->SetX(1280 - 10 - page->hintText->GetTextWidth());
        page->bottomHintSegments = BuildBottomHintSegments(" Back", page->hintText->GetX(), 20);
    }

    void instPage::refreshQueue() {
        if (queueNames.empty()) hintText->SetText(" Screen off     Cancel");
        else {
            const auto pending = queue.snapshot(); const size_t count = pending.size();
            queueSelection = count ? std::min(queueSelection, count - 1) : 0;
            const size_t first = queueSelection / kQueueVisibleRows * kQueueVisibleRows;
            FitQueueText(activeQueueTitle, activeQueueName.empty() ? "Waiting for the next title" : activeQueueName, 432);
            activeQueueLabel->SetText(activeQueueName.empty() && queueBrowserOpen ? "WAITING FOR TITLES" : "NOW INSTALLING");
            queueSummary->SetText(std::to_string(count) + " waiting   •   " + std::to_string(queue.completedCount()) + " completed");
            queuePageText->SetText(count ? std::to_string(first + 1) + "–" + std::to_string(std::min(count, first + kQueueVisibleRows)) + " of " + std::to_string(count) : "Queue clear");
            queuePageText->SetX(1232 - queuePageText->GetTextWidth()); queueEmptyText->SetVisible(count == 0);
            queueEmptyText->SetText(queueBrowser ? "No titles waiting\nAdd more from the shop." : "No titles waiting\nThe current install continues.");
            for (size_t row = 0; row < kQueueVisibleRows; ++row) {
                const bool visible = first + row < count;
                queueRows[row]->SetVisible(visible); queueRowNumbers[row]->SetVisible(visible); queueRowTitles[row]->SetVisible(visible);
                queueRowDetails[row]->SetVisible(visible); queueRowActions[row]->SetVisible(visible); queueRowIcons[row]->SetVisible(visible);
                if (!visible) continue;
                const size_t position = first + row; const auto& entry = queueEntries[pending[position]];
                const bool selected = position == queueSelection;
                queueRows[row]->SetColor(selected ? inst::theme::Color("highlight", COLOR("#542329FF")) : COLOR("#222228FF"));
                queueRowTitles[row]->SetColor(selected ? inst::theme::Color("selected_text", COLOR("#FFFFFFFF")) : COLOR("#FFFFFFFF"));
                queueRowNumbers[row]->SetText(std::to_string(position + 1)); FitQueueText(queueRowTitles[row], entry.name, 454);
                FitQueueText(queueRowDetails[row], (position == 0 ? "Next to install  •  " : "Waiting  •  ") + entry.detail, 454);
                queueRowActions[row]->SetText(selected ? " Remove" : "");
                loadQueueIcon(row, pending[position]);
            }
            hintText->SetText(queueBrowser ? " Remove     Add titles     Screen off     Cancel" : " Remove     Screen off     Cancel");
        }
        hintText->SetX(1280 - 10 - hintText->GetTextWidth()); bottomHintSegments = BuildBottomHintSegments(hintText->GetText(), hintText->GetX(), 20);
    }

    void instPage::loadQueueIcon(size_t row, size_t entryId) {
        if (queueRowEntryIds[row] == entryId && queueRowIconLoaded[row]) return;
        if (queueRowEntryIds[row] != entryId) {
            queueRowEntryIds[row] = entryId; queueRowIconLoaded[row] = false; queueRowIconPaths[row].clear();
        }
        const auto& entry = queueEntries[entryId];
        auto image = queueRowIcons[row];
        std::vector<std::uint8_t> data;
        if (entry.iconTitleId && inst::offline::TryGetIconData(entry.iconTitleId, data) && !data.empty()) {
            image->SetJpegImage(data.data(), static_cast<s32>(data.size()));
            queueRowIconLoaded[row] = image->IsImageValid();
            if (!queueRowIconLoaded[row]) queueRowIconPaths[row].clear();
        }
        if (!queueRowIconLoaded[row]) {
            std::string path = entry.iconPath;
            if (!path.empty() && !std::filesystem::exists(path)) {
                const auto legacy = std::filesystem::path(inst::config::legacyShopIconsDir) / std::filesystem::path(path).filename();
                path = std::filesystem::exists(legacy) ? legacy.string() : "";
            }
            if (!path.empty()) {
                image->SetImage(path); queueRowIconLoaded[row] = image->IsImageValid();
                if (queueRowIconLoaded[row]) queueRowIconPaths[row] = path;
                else queueRowIconPaths[row].clear();
            }
        }
        if (!queueRowIconLoaded[row]) {
            const std::string placeholder = inst::theme::Asset("romfs:/images/icons/title-placeholder.png");
            if (queueRowIconPaths[row] != placeholder) { image->SetImage(placeholder); queueRowIconPaths[row] = placeholder; }
            mainApp->remoteinstPage->queueInstallIcon(entry.key, entry.iconUrl, entry.iconPath);
        }
        // Loading resets Image's dimensions to the source texture size.
        image->SetWidth(44); image->SetHeight(44);
    }

    void instPage::refreshQueueIcons() {
        const auto pending = queue.snapshot();
        const size_t first = queueSelection / kQueueVisibleRows * kQueueVisibleRows;
        for (size_t row = 0; row < kQueueVisibleRows && first + row < pending.size(); ++row) loadQueueIcon(row, pending[first + row]);
    }

    void instPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
        if (screenBlank->IsVisible()) {
            if (Down || !Pos.IsEmpty()) {
                screenBlank->SetVisible(false); NotifyInstallTouchActivity(); touchWakeActive = !Pos.IsEmpty();
                suppressWakeTouch = !Pos.IsEmpty(); bottomHintTouch = {};
            }
            return;
        }
        if (suppressWakeTouch) { if (Pos.IsEmpty()) suppressWakeTouch = false; return; }
        if (HasQueuePresentation() && armGetSystemTick() >= queueIconPollAt) {
            queueIconPollAt = armGetSystemTick() + armGetSystemTickFreq();
            refreshQueueIcons();
        }
        if (Down) NotifyInstallTouchActivity();
        const bool freshTouch = !Pos.IsEmpty() && !touchWakeActive;
        if (!Pos.IsEmpty()) { if (!touchWakeActive) { touchWakeActive = true; NotifyInstallTouchActivity(); } }
        else touchWakeActive = false;
        if (installActive && !isInstallCancelRequested() && HasQueuePresentation() && freshTouch && Pos.X >= 540 && Pos.X < 1240 && Pos.Y >= 220 && Pos.Y < 592) {
            const size_t row = static_cast<size_t>((Pos.Y - 220) / 62);
            const size_t position = queueSelection / kQueueVisibleRows * kQueueVisibleRows + row;
            if ((Pos.Y - 220) % 62 < 56 && position < queue.snapshot().size()) { queueSelection = position; refreshQueue(); }
        }
        int tap = 0;
        if (DetectBottomHintTap(Pos, bottomHintTouch, 668, 52, tap)) Down |= FindBottomHintButton(bottomHintSegments, tap);
        // Use the same queue movement and repeat timing for all directional controls.
        const auto queueDirections = [](u64 buttons) {
            if (buttons & (HidNpadButton_StickLUp | HidNpadButton_StickRUp)) buttons |= HidNpadButton_Up;
            if (buttons & (HidNpadButton_StickLDown | HidNpadButton_StickRDown)) buttons |= HidNpadButton_Down;
            if (buttons & (HidNpadButton_StickLLeft | HidNpadButton_StickRLeft)) buttons |= HidNpadButton_Left;
            if (buttons & (HidNpadButton_StickLRight | HidNpadButton_StickRRight)) buttons |= HidNpadButton_Right;
            return buttons;
        };
        Down = queueDirections(Down);
        Held = queueDirections(Held);
        const u64 directions = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right;
        const u64 held = installActive ? Held & directions : 0, now = armGetSystemTick(), frequency = armGetSystemTickFreq();
        if (held != queueHeldDirection) { queueHeldDirection = held; queueRepeatAt = now + frequency * 35 / 100; }
        else if (held && !(Down & directions) && now >= queueRepeatAt) { Down |= held; queueRepeatAt = now + frequency / 10; }
        inst::util::playNavigationClickIfNeeded(Down);
        if (installActive && (Down & HidNpadButton_X)) {
            screenBlank->SetVisible(true); std::lock_guard<std::mutex> lock(g_installDimMutex);
            if (g_installDimLblReady && R_SUCCEEDED(lblSwitchBacklightOff(0))) g_installScreenOff = true;
            return;
        }
        if (installActive && !isInstallCancelRequested() && !queueNames.empty() && (Down & (directions | HidNpadButton_A | HidNpadButton_Y))) {
            if ((Down & HidNpadButton_Y) && queueBrowser) { queueBrowserOpen = true; queueBrowser(); return; }
            const auto pending = queue.snapshot(); const auto count = pending.size();
            if ((Down & HidNpadButton_Down) && queueSelection + 1 < count) ++queueSelection;
            if ((Down & HidNpadButton_Up) && queueSelection) --queueSelection;
            if ((Down & HidNpadButton_Right) && count) queueSelection = std::min(queueSelection + kQueueVisibleRows, count - 1);
            if (Down & HidNpadButton_Left) queueSelection = queueSelection >= kQueueVisibleRows ? queueSelection - kQueueVisibleRows : 0;
            if ((Down & HidNpadButton_A) && queueSelection < count) queue.remove(pending[queueSelection]);
            refreshQueue();
        }
        if (Down & HidNpadButton_B) {
            if (installActive) {
                requestInstallCancel();
                if (inst::mtp::IsStreamInstallActive()) { inst::mtp::CancelStreamInstall(); finishInstall(); }
                setInstInfoText("Cancelling install..."); return;
            }
            if (!hintText->IsVisible()) { requestInstallCancel(); setInstInfoText("Cancelling install..."); return; }
            if (inst::mtp::IsInstallServerRunning()) inst::mtp::StopInstallServer();
            loadMainMenu();
        }
    }
}

