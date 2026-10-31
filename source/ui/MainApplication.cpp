#include "ui/MainApplication.hpp"
#include "util/lang.hpp"
#include "util/config.hpp"
#include "util/util.hpp"
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <thread>
#include "mtp_install.hpp"
#include "mtp_server.hpp"
#include "switch.h"

#include "util/theme.hpp"

#define COLOR(hex) inst::theme::Palette(hex)

namespace inst::ui {
    MainApplication *mainApp;

    static std::string WrapForTextBlock(const pu::ui::elm::TextBlock::Ref& block, const std::string& text, int max_width) {
        if (!block || text.empty()) return text;

        std::stringstream input(text);
        std::string paragraph;
        std::string wrapped;

        while (std::getline(input, paragraph)) {
            if (paragraph.empty()) {
                wrapped += "\n";
                continue;
            }

            std::stringstream words(paragraph);
            std::string word;
            std::string line;
            while (words >> word) {
                std::string candidate = line.empty() ? word : (line + " " + word);
                block->SetText(candidate);
                if (block->GetTextWidth() <= max_width) {
                    line = candidate;
                } else {
                    if (!wrapped.empty() && wrapped.back() != '\n') wrapped += "\n";
                    wrapped += line.empty() ? word : line;
                    line = line.empty() ? "" : word;
                    if (line == word) {
                        block->SetText(line);
                        if (block->GetTextWidth() > max_width) {
                            // Single overlong token: place as-is on its own line.
                            wrapped += "\n" + line;
                            line.clear();
                        }
                    }
                }
            }
            if (!line.empty()) {
                if (!wrapped.empty() && wrapped.back() != '\n') wrapped += "\n";
                wrapped += line;
            }
            wrapped += "\n";
        }

        if (!wrapped.empty() && wrapped.back() == '\n') wrapped.pop_back();
        return wrapped;
    }

    static std::string FormatOneDecimal(double value) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f", value);
        std::string out = buf;
        if (out.find('.') != std::string::npos) {
            while (!out.empty() && out.back() == '0') out.pop_back();
            if (!out.empty() && out.back() == '.') out.pop_back();
        }
        return out;
    }

    static std::string FormatBinarySize(std::uint64_t bytes) {
        const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
        if (mib >= 1024.0) {
            const double gib = mib / 1024.0;
            return FormatOneDecimal(gib) + " GiB";
        }
        return FormatOneDecimal(mib) + " MiB";
    }

    bool MainApplication::CallForRender() {
        // The install pump owns the frame cadence. Progress producers must
        // neither render from workers nor recursively poll input from callbacks.
        if (!IsUiThread() || IsInstallWorkActive()) return true;
        return pu::ui::Application::CallForRender();
    }

    int MainApplication::CreateShowDialog(const std::string& title, const std::string& content,
        std::vector<std::string> options, bool useLastOptionAsCancel, const std::string& icon) {
        return RunOnUiThread([=, this] {
            return pu::ui::Application::CreateShowDialog(title, content, options, useLastOptionAsCancel, icon);
        });
    }

    void MainApplication::RunInstallWork(std::function<void()> work) {
        if (!IsUiThread()) throw std::logic_error("Install pump must start on the UI thread");
        if (installWorkActive.exchange(true)) return;
        instPage::loadInstallScreen(false);
        std::atomic<bool> done{false};
        std::exception_ptr failure;
        std::thread worker;
        try {
            worker = std::thread([&] {
                try { work(); }
                catch (...) { failure = std::current_exception(); }
                done.store(true, std::memory_order_release);
            });
        } catch (...) {
            installWorkActive = false;
            instPage::loadMainMenu();
            throw;
        }
        // Rendering polls input every frame even while the worker waits for
        // HTTP headers, reads local content, writes NCAs, or rolls back a cancel.
        while (!done.load(std::memory_order_acquire))
            pu::ui::Application::CallForRender();
        worker.join();
        uiDispatcher.drain();
        installWorkActive = false;
        instPage::loadMainMenu();
        if (failure) {
            try { std::rethrow_exception(failure); }
            catch (const std::exception& e) { CreateShowDialog("Installation failed", e.what(), {"OK"}, true); }
            catch (...) { CreateShowDialog("Installation failed", "Unexpected installation error.", {"OK"}, true); }
        }
    }

    void MainApplication::Close() {
        // The refresh worker owns the remote model until its UI pump joins it.
        if (this->remoteinstPage && this->remoteinstPage->isLibraryRefreshActive()) return;
        if (IsInstallWorkActive()) {
            instPage::requestInstallCancel();
            return; // Renderer and services must outlive the installation worker.
        }
        if (this->optionspage)
            this->optionspage->stopDiscoveryWorker();
        if (this->remoteinstPage)
            this->remoteinstPage->stopIconDownloadWorker();
        pu::ui::Application::Close();
    }

    void MainApplication::RefreshInputDevice(bool force) {
        if (!IsUiThread()) return;
        const AppletFocusState focus = appletGetFocusState();
        const bool regainedFocus = (focus != this->lastFocusState) && (focus == AppletFocusState_InFocus);
        this->lastFocusState = focus;

        if (focus != AppletFocusState_InFocus)
            return;

        if (force || regainedFocus || !padIsConnected(&this->input_pad)) {
            padConfigureInput(8, HidNpadStyleSet_NpadStandard);
            padInitializeAny(&this->input_pad);
        }
    }

    void MainApplication::CreateLayoutStep(int step) {
        switch (step) {
        case 0:
            this->mainPage = MainPage::New();
            this->mainPage->SetOnInput(std::bind(&MainPage::onInput, this->mainPage.get(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
            break;
        case 1:
            this->netinstPage = netInstPage::New();
            this->netinstPage->SetOnInput(std::bind(&netInstPage::onInput, this->netinstPage.get(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
            break;
        case 2:
            this->remoteinstPage = remoteInstPage::New();
            this->remoteinstPage->SetOnInput(std::bind(&remoteInstPage::onInput, this->remoteinstPage.get(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
            break;
        case 3:
            this->sdinstPage = sdInstPage::New();
            this->sdinstPage->SetOnInput(std::bind(&sdInstPage::onInput, this->sdinstPage.get(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
            break;
        case 4:
            this->usbinstPage = usbInstPage::New();
            this->usbinstPage->SetOnInput(std::bind(&usbInstPage::onInput, this->usbinstPage.get(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
            break;
        case 5:
            this->hddinstPage = hddInstPage::New();
            this->hddinstPage->SetOnInput(std::bind(&hddInstPage::onInput, this->hddinstPage.get(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
            break;
        case 6:
            this->instpage = instPage::New();
            this->instpage->SetOnInput(std::bind(&instPage::onInput, this->instpage.get(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
            break;
        case 7:
            this->optionspage = optionsPage::New();
            this->optionspage->SetOnInput(std::bind(&optionsPage::onInput, this->optionspage.get(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
            break;
        }
    }

    void MainApplication::RecreateLayouts() {
        if (!IsUiThread()) throw std::logic_error("Layouts must be recreated on the UI thread");
        if (this->optionspage) this->optionspage->stopDiscoveryWorker();
        if (this->remoteinstPage) this->remoteinstPage->stopIconDownloadWorker();
        for (int step = 0; step < 8; ++step) this->CreateLayoutStep(step);
    }

    void MainApplication::ShowThemeLoading() {
        this->themeLoadingLayout = pu::ui::Layout::New();
        this->themeLoadingLayout->SetBackgroundColor(pu::ui::Color::FromHex("#10151FFF"));
        auto label = pu::ui::elm::TextBlock::New(0, 388, "options.theme_loading"_lang, 28);
        label->SetColor(pu::ui::Color::FromHex("#FFFFFFFF"));
        label->SetX((1280 - label->GetTextWidth()) / 2);
        this->themeLoadingLayout->Add(label);
        this->themeLoadingDots.clear();
        this->themeLoadingFrame = 0;
        for (int i = 0; i < 12; ++i) {
            const double angle = i * 6.283185307179586 / 12.0;
            auto dot = pu::ui::elm::Rectangle::New(
                634 + static_cast<int>(std::sin(angle) * 38),
                304 - static_cast<int>(std::cos(angle) * 38),
                12, 12, pu::ui::Color::FromHex("#FFFFFF33"), 6);
            this->themeLoadingDots.push_back(dot);
            this->themeLoadingLayout->Add(dot);
        }
        auto track = pu::ui::elm::Rectangle::New(440, 452, 400, 6, pu::ui::Color::FromHex("#FFFFFF33"), 3);
        this->themeLoadingProgress = pu::ui::elm::Rectangle::New(440, 452, 0, 6, pu::ui::Color::FromHex("#FFFFFFFF"), 3);
        this->themeLoadingLayout->Add(track);
        this->themeLoadingLayout->Add(this->themeLoadingProgress);
        this->themeLoadingLayout->AddThread([this] {
            // Advance between loading stages; no rendering or input runs on a worker.
            const unsigned head = this->themeLoadingFrame++ % 12;
            for (unsigned i = 0; i < this->themeLoadingDots.size(); ++i) {
                const unsigned distance = (head + 12 - i) % 12;
                const u8 alpha = distance < 5 ? static_cast<u8>(255 - distance * 40) : 45;
                this->themeLoadingDots[i]->SetColor(pu::ui::Color(255, 255, 255, alpha));
            }
        });
        this->LoadLayout(this->themeLoadingLayout);
    }

    void MainApplication::RequestThemeReload() {
        if (!IsUiThread()) throw std::logic_error("Theme selection must run on the UI thread");
        this->themeReloadPending = true;
    }

    void MainApplication::ApplyPendingTheme() {
        if (this->remoteinstPage && this->remoteinstPage->isLibraryRefreshActive()) return;
        if (IsInstallWorkActive() || inst::mtp::IsStreamInstallActive()) return;
        if (this->themeReloadStage < 0) {
            if (!this->themeReloadPending) return;
            this->themeReloadPending = false;
            this->ShowThemeLoading();
            this->themeReloadStage = 0;
            return; // Present the loading screen before any blocking work.
        }
        if (this->themeReloadStage == 0) {
            if (this->optionspage) this->optionspage->stopDiscoveryWorker();
            if (this->remoteinstPage) this->remoteinstPage->stopIconDownloadWorker();
            inst::theme::Load(inst::config::appDir, inst::config::themeFile);
        } else if (this->themeReloadStage <= 8) {
            // Create one layout per frame so the spinner can animate between stages.
            this->CreateLayoutStep(this->themeReloadStage - 1);
        } else {
            this->optionspage->ShowThemePicker();
            this->LoadLayout(this->optionspage);
            this->themeReloadStage = -1;
            this->themeLoadingLayout.reset();
            this->themeLoadingDots.clear();
            this->themeLoadingProgress.reset();
            return;
        }
        ++this->themeReloadStage;
        this->themeLoadingProgress->SetWidth(400 * this->themeReloadStage / 9);
    }

    void MainApplication::OnLoad() {
        mainApp = this;
        this->RefreshInputDevice(true);

        Language::Load();
        inst::theme::Load(inst::config::appDir, inst::config::themeFile);

        this->RecreateLayouts();
        this->LoadLayout(this->mainPage);

        this->AddThread([this]() {
            this->uiDispatcher.drain();
            this->ApplyPendingTheme();
            this->RefreshInputDevice();
        });

        this->AddThread([this]() {
            static bool last_active = false;
            static bool last_server_running = false;
            static std::string last_name;
            static bool icon_set = false;
            static bool complete_notified = false;
            static auto last_time = std::chrono::steady_clock::now();
            static std::uint64_t last_bytes = 0;
            static double ema_rate = 0.0;
            static int spinner_idx = 0;
            static const char kSpinner[4] = {'|', '/', '-', '\\'};
            static auto last_ui_text_update = std::chrono::steady_clock::now();

            const bool active = inst::mtp::IsStreamInstallActive();
            const bool server_running = inst::mtp::IsInstallServerRunning();
            constexpr int kRightIconSize = 220;
            constexpr int kRightIconX = 1280 - 60 - kRightIconSize;
            constexpr int kRightIconY = 220;

            if (server_running && !active && !last_server_running) {
                this->LoadLayout(this->instpage);
                this->instpage->pageInfoText->SetText("inst.mtp.waiting.title"_lang);
                this->instpage->installInfoText->SetText("inst.mtp.waiting.desc"_lang + std::string("\n\n") + "inst.mtp.waiting.hint"_lang);
                this->instpage->installInfoText->SetX(15);
                this->instpage->installInfoText->SetY(568);
                this->instpage->installBar->SetVisible(false);
                this->instpage->installBar->SetProgress(0);
                this->instpage->installIconImage->SetVisible(false);
                this->instpage->awooImage->SetVisible(!inst::config::gayMode);
                this->instpage->hintText->SetVisible(true);
                this->instpage->progressText->SetVisible(false);
                this->instpage->progressDetailText->SetVisible(false);
                icon_set = false;
            }

            if (active && !last_active) {
                instPage::loadInstallScreen(false);
                last_name = inst::mtp::GetStreamInstallName();
                if (last_name.empty()) {
                    last_name = "MTP Install";
                }
                complete_notified = false;
                last_time = std::chrono::steady_clock::now();
                last_bytes = 0;
                ema_rate = 0.0;
                spinner_idx = 0;
                last_ui_text_update = std::chrono::steady_clock::now() - std::chrono::milliseconds(1000);
                this->LoadLayout(this->instpage);
                this->instpage->pageInfoText->SetText("inst.info_page.top_info0"_lang + last_name + " (MTP)");
                this->instpage->installInfoText->SetText("inst.info_page.preparing"_lang);
                this->instpage->installInfoText->SetX(15);
                this->instpage->installInfoText->SetY(568);
                this->instpage->installBar->SetVisible(true);
                this->instpage->installBar->SetProgress(0);
                this->instpage->installIconImage->SetVisible(false);
                this->instpage->awooImage->SetVisible(!inst::config::gayMode);
                this->instpage->hintText->SetVisible(true);
                this->instpage->progressText->SetVisible(true);
                this->instpage->progressDetailText->SetVisible(true);
                icon_set = false;
            }

            if (active) {
                std::uint64_t received = 0;
                std::uint64_t total = 0;
                inst::mtp::GetStreamInstallProgress(&received, &total);
                if (total > 0) {
                    const double percent = (double)received / (double)total * 100.0;
                    this->instpage->installBar->SetVisible(true);
                    this->instpage->installBar->SetProgress(percent);
                    if (!icon_set) {
                        this->instpage->installInfoText->SetText("inst.info_page.downloading"_lang + last_name);
                    }

                    const auto now = std::chrono::steady_clock::now();
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();
                    if (elapsed >= 1000) {
                        const auto delta = received - last_bytes;
                        const double rate = (elapsed > 0) ? (double)delta / ((double)elapsed / 1000.0) : 0.0;
                        if (rate > 0.0) {
                            if (ema_rate <= 0.0) {
                                ema_rate = rate;
                            } else {
                                ema_rate = (ema_rate * 0.7) + (rate * 0.3);
                            }
                        }
                        last_bytes = received;
                        last_time = now;
                    }

                    std::string eta_text = "Calculating...";
                    if (ema_rate > 0.0 && received < total) {
                        const auto remaining = total - received;
                        const auto seconds = static_cast<std::uint64_t>(remaining / ema_rate);
                        const auto h = seconds / 3600;
                        const auto m = (seconds % 3600) / 60;
                        const auto s = seconds % 60;
                        if (h > 0) {
                            eta_text = std::to_string(h) + ":" + (m < 10 ? "0" : "") + std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);
                        } else {
                            eta_text = std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);
                        }
                        eta_text = eta_text + " remaining";
                    }

                    std::string speed_text;
                    if (ema_rate > 0.0) {
                        const double mbps = ema_rate / (1024.0 * 1024.0);
                        speed_text = FormatOneDecimal(mbps) + " MiB/s";
                    } else {
                        speed_text = "-- MiB/s";
                    }

                    std::string format_text;
                    const auto dot = last_name.find_last_of('.');
                    if (dot != std::string::npos) {
                        format_text = last_name.substr(dot + 1);
                        std::transform(format_text.begin(), format_text.end(), format_text.begin(), ::toupper);
                    }

                    const auto ui_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ui_text_update).count();
                    if (ui_elapsed >= 250 || received >= total) {
                        const int pct = static_cast<int>(percent + 0.5);
                        std::string progress_text = std::string(1, kSpinner[spinner_idx]) + " " + std::to_string(pct) + "% • " + eta_text + " • " + speed_text;
                        spinner_idx = (spinner_idx + 1) % 4;
                        if (!format_text.empty()) {
                            progress_text += " • " + format_text;
                        }
                        this->instpage->progressText->SetText(progress_text);
                        this->instpage->progressText->SetX((1280 - this->instpage->progressText->GetTextWidth()) / 2);
                        this->instpage->progressText->SetVisible(true);

                        this->instpage->progressDetailText->SetText(
                            "Copied " + FormatBinarySize(received) + " / " + FormatBinarySize(total)
                        );
                        this->instpage->progressDetailText->SetX((1280 - this->instpage->progressDetailText->GetTextWidth()) / 2);
                        this->instpage->progressDetailText->SetVisible(true);
                        last_ui_text_update = now;
                    }
                }

                if (!icon_set) {
                    std::uint64_t title_id = 0;
                    if (inst::mtp::GetStreamInstallTitleId(&title_id) && title_id != 0) {
                        NsApplicationControlData appControlData{};
                        size_t sizeRead = 0;
                        Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, title_id, &appControlData, sizeof(NsApplicationControlData), &sizeRead);
                        if (R_SUCCEEDED(rc) && sizeRead > sizeof(appControlData.nacp)) {
                            const size_t iconSize = sizeRead - sizeof(appControlData.nacp);
                            if (iconSize > 0) {
                                this->instpage->installIconImage->SetJpegImage(appControlData.icon, iconSize);
                                this->instpage->installIconImage->SetX(kRightIconX);
                                this->instpage->installIconImage->SetY(kRightIconY);
                                this->instpage->installIconImage->SetWidth(kRightIconSize);
                                this->instpage->installIconImage->SetHeight(kRightIconSize);
                                this->instpage->installIconImage->SetVisible(true);
                                this->instpage->awooImage->SetVisible(false);
                                const std::string msg = last_name + "inst.info_page.desc1"_lang + "\n\n" + "inst.info_page.complete"_lang;
                                this->instpage->installInfoText->SetText(WrapForTextBlock(this->instpage->installInfoText, msg, 900));
                                this->instpage->installInfoText->SetX(40);
                                this->instpage->installInfoText->SetY(240);
                                icon_set = true;
                            }
                        }
                    }
                }
            }

            if (!active && last_active) instPage::finishInstall();
            if (inst::mtp::ConsumeStreamInstallComplete()) {
                instPage::finishInstall();
                this->instpage->installBar->SetVisible(true);
                this->instpage->installBar->SetProgress(100);
                const std::string done_msg = last_name + "inst.info_page.desc1"_lang + "\n\n" + "inst.info_page.complete"_lang + "\n\n" + "inst.mtp.waiting.hint"_lang;
                this->instpage->installInfoText->SetText(WrapForTextBlock(this->instpage->installInfoText, done_msg, 900));
                this->instpage->installInfoText->SetX(40);
                this->instpage->installInfoText->SetY(240);
                this->instpage->hintText->SetVisible(true);
                this->instpage->progressText->SetText("100% • done");
                this->instpage->progressText->SetX((1280 - this->instpage->progressText->GetTextWidth()) / 2);
                this->instpage->progressText->SetVisible(true);
                this->instpage->progressDetailText->SetText("Copied complete");
                this->instpage->progressDetailText->SetX((1280 - this->instpage->progressDetailText->GetTextWidth()) / 2);
                this->instpage->progressDetailText->SetVisible(true);

                if (this->instpage->installIconImage->IsVisible()) {
                    this->instpage->installIconImage->SetX(kRightIconX);
                    this->instpage->installIconImage->SetY(kRightIconY);
                    this->instpage->installIconImage->SetWidth(kRightIconSize);
                    this->instpage->installIconImage->SetHeight(kRightIconSize);
                } else if (!inst::config::gayMode) {
                    this->instpage->awooImage->SetVisible(true);
                    this->instpage->awooImage->SetX(kRightIconX + 10);
                    this->instpage->awooImage->SetY(kRightIconY - 10);
                }

                if (!complete_notified) {
                    std::string audioPath = "romfs:/audio/success.wav";
                    if (!inst::config::soundEnabled) audioPath = "";
                    if (std::filesystem::exists(inst::config::appDir + "/success.wav")) {
                        audioPath = inst::config::appDir + "/success.wav";
                    }
                    std::thread audioThread(inst::util::playAudio, audioPath);
                    audioThread.join();
                    complete_notified = true;
                }
            }


            if (!server_running && last_server_running) {
                this->instpage->hintText->SetVisible(false);
            }

            last_active = active;
            last_server_running = server_running;
        });

        this->AddThread([this]() {
            static u64 lastTick = 0;
            const u64 now = armGetSystemTick();
            const u64 freq = armGetSystemTickFreq();
            if (lastTick != 0 && (now - lastTick) < freq)
                return;
            lastTick = now;

            std::string timeText = "--:--";
            if (R_SUCCEEDED(timeInitialize())) {
                u64 posix = 0;
                if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &posix))) {
                    std::time_t t = static_cast<std::time_t>(posix);
                    std::tm* local = std::localtime(&t);
                    char buf[16] = {0};
                    if (local && std::strftime(buf, sizeof(buf), "%I:%M %p", local) > 0)
                        timeText = buf;
                }
                timeExit();
            }

            bool internetUp = false;
            bool wifiConnected = false;
            u32 wifiStrength = 0;
            if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
                NifmInternetConnectionStatus status = static_cast<NifmInternetConnectionStatus>(0);
                NifmInternetConnectionType type = static_cast<NifmInternetConnectionType>(0);
                if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &wifiStrength, &status))) {
                    internetUp = (status == NifmInternetConnectionStatus_Connected);
                    wifiConnected = internetUp && (type == NifmInternetConnectionType_WiFi);
                }
                nifmExit();
            }

            int batteryPct = -1;
            if (R_SUCCEEDED(psmInitialize())) {
                u32 pct = 0;
                if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) {
                    batteryPct = static_cast<int>(pct);
                }
                psmExit();
            }

            s64 systemTotal = 0;
            s64 systemFree = 0;
            s64 sdTotalBytes = 0;
            s64 sdFreeBytes = 0;
            if (R_SUCCEEDED(ncmInitialize())) {
                NcmContentStorage storage{};
                Result rc = ncmOpenContentStorage(&storage, NcmStorageId_BuiltInUser);
                if (R_SUCCEEDED(rc)) {
                    ncmContentStorageGetTotalSpaceSize(&storage, &systemTotal);
                    ncmContentStorageGetFreeSpaceSize(&storage, &systemFree);
                    ncmContentStorageClose(&storage);
                }
                rc = ncmOpenContentStorage(&storage, NcmStorageId_SdCard);
                if (R_SUCCEEDED(rc)) {
                    ncmContentStorageGetTotalSpaceSize(&storage, &sdTotalBytes);
                    ncmContentStorageGetFreeSpaceSize(&storage, &sdFreeBytes);
                    ncmContentStorageClose(&storage);
                }
                ncmExit();
            }

            const int cardWidth = 180;
            const int cardGap = 12;
            const int timeY = 24;
            const int iconY = 50;
            const int cardsTopY = 24;
            const int barY = 42;
            const int freeY = 52;
            const int netSize = 6;
            const int wifiBarW = 4;
            const int wifiBarGap = 2;
            const int wifiWidth = (wifiBarW * 3) + (wifiBarGap * 2);
            const int wifiMaxH = 10;
            const int batteryW = 24;
            const int batteryH = 12;
            const int batteryCapW = 3;
            const int iconGap = 6;
            const int iconsWidth = netSize + iconGap + wifiWidth + iconGap + batteryW + batteryCapW;
            auto applyStatus = [&](TextBlock::Ref timeBlock,
                                   TextBlock::Ref ipText,
                                   TextBlock::Ref sysLabel,
                                   TextBlock::Ref sysFree,
                                   Rectangle::Ref sysBarBack,
                                   Rectangle::Ref sysBarFill,
                                   TextBlock::Ref sdLabel,
                                   TextBlock::Ref sdFree,
                                   Rectangle::Ref sdBarBack,
                                   Rectangle::Ref sdBarFill,
                                   Rectangle::Ref netIndicator,
                                   Rectangle::Ref wifiBar1,
                                   Rectangle::Ref wifiBar2,
                                   Rectangle::Ref wifiBar3,
                                   Rectangle::Ref batteryOutline,
                                   Rectangle::Ref batteryFill,
                                   Rectangle::Ref batteryCap) {
                int right = 1280 - 10;
                int cardsRight = right;
                int timeX = right;
                int timeW = 0;
                if (timeBlock) {
                    timeBlock->SetText(timeText);
                    timeBlock->SetY(timeY);
                    timeW = timeBlock->GetTextWidth();
                    timeX = right - timeW;
                    timeBlock->SetX(timeX);
                    cardsRight = timeX - cardGap;
                }

                int iconsX = right - iconsWidth;
                if (timeW > 0) {
                    iconsX = timeX + (timeW - iconsWidth) / 2;
                    if (iconsX < 0)
                        iconsX = 0;
                }
                int netX = iconsX;
                int wifiX = netX + netSize + iconGap;
                int batteryX = wifiX + wifiWidth + iconGap;

                if (netIndicator) {
                    netIndicator->SetX(netX);
                    netIndicator->SetY(iconY + 2);
                    netIndicator->SetColor(internetUp ? COLOR("#4CD964FF") : COLOR("#FF3B30FF"));
                }

                if (wifiBar1 && wifiBar2 && wifiBar3) {
                    const int wifiBaseY = iconY + wifiMaxH;
                    wifiBar1->SetX(wifiX);
                    wifiBar1->SetY(wifiBaseY - 4);
                    wifiBar2->SetX(wifiX + wifiBarW + wifiBarGap);
                    wifiBar2->SetY(wifiBaseY - 7);
                    wifiBar3->SetX(wifiX + (wifiBarW + wifiBarGap) * 2);
                    wifiBar3->SetY(wifiBaseY - 10);
                    pu::ui::Color onColor = COLOR("#FFFFFFFF");
                    pu::ui::Color offColor = COLOR("#FFFFFF55");
                    const bool showWifi = wifiConnected && wifiStrength > 0;
                    wifiBar1->SetColor((showWifi && wifiStrength >= 1) ? onColor : offColor);
                    wifiBar2->SetColor((showWifi && wifiStrength >= 2) ? onColor : offColor);
                    wifiBar3->SetColor((showWifi && wifiStrength >= 3) ? onColor : offColor);
                    wifiBar1->SetVisible(true);
                    wifiBar2->SetVisible(true);
                    wifiBar3->SetVisible(true);
                }

                if (batteryOutline && batteryFill && batteryCap) {
                    batteryOutline->SetX(batteryX);
                    batteryOutline->SetY(iconY);
                    batteryOutline->SetWidth(batteryW);
                    batteryOutline->SetHeight(batteryH);
                    batteryCap->SetX(batteryX + batteryW + 1);
                    batteryCap->SetY(iconY + 3);
                    batteryCap->SetWidth(batteryCapW);
                    batteryCap->SetHeight(6);

                    int fillWidth = 0;
                    if (batteryPct >= 0) {
                        double ratio = static_cast<double>(batteryPct) / 100.0;
                        if (ratio < 0.0) ratio = 0.0;
                        if (ratio > 1.0) ratio = 1.0;
                        fillWidth = static_cast<int>((batteryW - 2) * ratio);
                        if (fillWidth < 2 && ratio > 0.0) fillWidth = 2;
                    }
                    batteryFill->SetX(batteryX + 1);
                    batteryFill->SetY(iconY + 1);
                    batteryFill->SetWidth(fillWidth);
                    batteryFill->SetHeight(batteryH - 2);
                    batteryFill->SetColor((batteryPct >= 0 && batteryPct <= 20) ? COLOR("#FF3B30FF") : COLOR("#4CD964FF"));
                }

                int sdX = cardsRight - cardWidth;
                int sysX = sdX - cardGap - cardWidth;

                if (ipText) {
                    std::string ipAddress = inst::util::getIPAddress();
                    if (ipAddress == "1.0.0.127") ipAddress = "--";
                    ipText->SetText("IP: " + ipAddress);
                    int ipWidth = ipText->GetTextWidth();
                    int ipX = sysX - cardGap - ipWidth;
                    if (ipX < 10) ipX = 10;
                    ipText->SetX(ipX);
                    ipText->SetY(cardsTopY);
                }

                if (sysLabel) {
                    sysLabel->SetX(sysX);
                    sysLabel->SetY(cardsTopY);
                }
                if (sysFree) {
                    sysFree->SetX(sysX);
                    sysFree->SetY(freeY);
                }
                if (sysBarBack) {
                    sysBarBack->SetX(sysX);
                    sysBarBack->SetY(barY);
                }
                if (sysBarFill) {
                    sysBarFill->SetX(sysX);
                    sysBarFill->SetY(barY);
                }

                if (sdLabel) {
                    sdLabel->SetX(sdX);
                    sdLabel->SetY(cardsTopY);
                }
                if (sdFree) {
                    sdFree->SetX(sdX);
                    sdFree->SetY(freeY);
                }
                if (sdBarBack) {
                    sdBarBack->SetX(sdX);
                    sdBarBack->SetY(barY);
                }
                if (sdBarFill) {
                    sdBarFill->SetX(sdX);
                    sdBarFill->SetY(barY);
                }
            };

            auto updateCards = [&](TextBlock::Ref sysLabel,
                                   TextBlock::Ref sysFree,
                                   Rectangle::Ref sysBarBack,
                                   Rectangle::Ref sysBarFill,
                                   TextBlock::Ref sdLabel,
                                   TextBlock::Ref sdFree,
                                   Rectangle::Ref sdBarBack,
                                   Rectangle::Ref sdBarFill) {
                if (sysLabel) sysLabel->SetText("System Memory");
                if (sdLabel) sdLabel->SetText("microSD Card");

                auto setCard = [&](TextBlock::Ref freeText, Rectangle::Ref barFill, s64 freeBytes, s64 totalBytes) {
                    if (freeText) {
                        char buf[64] = {0};
                        if (totalBytes > 0) {
                            double freeGb = static_cast<double>(freeBytes) / (1024.0 * 1024.0 * 1024.0);
                            std::snprintf(buf, sizeof(buf), "Free Space %.1f GB", freeGb);
                        } else {
                            std::snprintf(buf, sizeof(buf), "Free Space --");
                        }
                        freeText->SetText(buf);
                    }
                    if (barFill) {
                        int width = 0;
                        if (totalBytes > 0) {
                            double usedBytes = static_cast<double>(totalBytes - freeBytes);
                            if (usedBytes < 0.0) usedBytes = 0.0;
                            double ratio = usedBytes / static_cast<double>(totalBytes);
                            if (ratio < 0.0) ratio = 0.0;
                            if (ratio > 1.0) ratio = 1.0;
                            width = static_cast<int>(cardWidth * ratio);
                            if (width < 2 && ratio > 0.0) width = 2;
                        }
                        barFill->SetWidth(width);
                    }
                };

                setCard(sysFree, sysBarFill, systemFree, systemTotal);
                setCard(sdFree, sdBarFill, sdFreeBytes, sdTotalBytes);
            };

            auto applyAll = [&](auto& page) {
                applyStatus(page->timeText, page->ipText, page->sysLabelText, page->sysFreeText, page->sysBarBack, page->sysBarFill,
                            page->sdLabelText, page->sdFreeText, page->sdBarBack, page->sdBarFill,
                            page->netIndicator, page->wifiBar1, page->wifiBar2, page->wifiBar3,
                            page->batteryOutline, page->batteryFill, page->batteryCap);
                updateCards(page->sysLabelText, page->sysFreeText, page->sysBarBack, page->sysBarFill,
                            page->sdLabelText, page->sdFreeText, page->sdBarBack, page->sdBarFill);
            };

            applyAll(this->mainPage);
            applyAll(this->netinstPage);
            applyAll(this->remoteinstPage);
            applyAll(this->sdinstPage);
            applyAll(this->usbinstPage);
            applyAll(this->hddinstPage);
            applyAll(this->instpage);
            applyAll(this->optionspage);
        });
    }
}
