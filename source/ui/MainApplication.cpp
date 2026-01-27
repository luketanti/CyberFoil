#include "ui/MainApplication.hpp"
#include "util/lang.hpp"
#include "util/config.hpp"
#include <chrono>
#include "mtp_install.hpp"
#include "mtp_server.hpp"
#include "switch.h"

namespace inst::ui {
    MainApplication *mainApp;

    void MainApplication::OnLoad() {
        mainApp = this;

        Language::Load();

        this->mainPage = MainPage::New();
        this->netinstPage = netInstPage::New();
        this->shopinstPage = shopInstPage::New();
        this->sdinstPage = sdInstPage::New();
        this->usbinstPage = usbInstPage::New();
        this->instpage = instPage::New();
        this->optionspage = optionsPage::New();
        this->mainPage->SetOnInput(std::bind(&MainPage::onInput, this->mainPage, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
        this->netinstPage->SetOnInput(std::bind(&netInstPage::onInput, this->netinstPage, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
        this->shopinstPage->SetOnInput(std::bind(&shopInstPage::onInput, this->shopinstPage, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
        this->sdinstPage->SetOnInput(std::bind(&sdInstPage::onInput, this->sdinstPage, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
        this->usbinstPage->SetOnInput(std::bind(&usbInstPage::onInput, this->usbinstPage, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
        this->instpage->SetOnInput(std::bind(&instPage::onInput, this->instpage, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
        this->optionspage->SetOnInput(std::bind(&optionsPage::onInput, this->optionspage, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
        this->LoadLayout(this->mainPage);

        this->AddThread([this]() {
            static bool last_active = false;
            static bool last_server_running = false;
            static std::string last_name;
            static bool icon_set = false;
            static bool complete_notified = false;
            static auto last_time = std::chrono::steady_clock::now();
            static std::uint64_t last_bytes = 0;
            static double ema_rate = 0.0;

            const bool active = inst::mtp::IsStreamInstallActive();
            const bool server_running = inst::mtp::IsInstallServerRunning();

            if (server_running && !active && !last_server_running) {
                this->LoadLayout(this->instpage);
                this->instpage->pageInfoText->SetText("inst.mtp.waiting.title"_lang);
                this->instpage->installInfoText->SetText("inst.mtp.waiting.desc"_lang + std::string("\n\n") + "inst.mtp.waiting.hint"_lang);
                this->instpage->installBar->SetVisible(false);
                this->instpage->installBar->SetProgress(0);
                this->instpage->installIconImage->SetVisible(false);
                this->instpage->awooImage->SetVisible(!inst::config::gayMode);
                this->instpage->hintText->SetVisible(true);
                this->instpage->progressText->SetVisible(false);
                icon_set = false;
            }

            if (active && !last_active) {
                last_name = inst::mtp::GetStreamInstallName();
                if (last_name.empty()) {
                    last_name = "MTP Install";
                }
                complete_notified = false;
                last_time = std::chrono::steady_clock::now();
                last_bytes = 0;
                ema_rate = 0.0;
                this->LoadLayout(this->instpage);
                this->instpage->pageInfoText->SetText("inst.info_page.top_info0"_lang + last_name + " (MTP)");
                this->instpage->installInfoText->SetText("inst.info_page.preparing"_lang);
                this->instpage->installBar->SetVisible(true);
                this->instpage->installBar->SetProgress(0);
                this->instpage->installIconImage->SetVisible(false);
                this->instpage->awooImage->SetVisible(!inst::config::gayMode);
                this->instpage->hintText->SetVisible(true);
                this->instpage->progressText->SetVisible(true);
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
                    this->instpage->installInfoText->SetText("inst.info_page.downloading"_lang + last_name);

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
                        const double rounded = std::round(mbps * 10.0) / 10.0;
                        speed_text = std::to_string(rounded);
                        if (speed_text.find('.') != std::string::npos) {
                            while (!speed_text.empty() && speed_text.back() == '0') speed_text.pop_back();
                            if (!speed_text.empty() && speed_text.back() == '.') speed_text.pop_back();
                        }
                        speed_text += " MB/s";
                    } else {
                        speed_text = "-- MB/s";
                    }

                    std::string format_text;
                    const auto dot = last_name.find_last_of('.');
                    if (dot != std::string::npos) {
                        format_text = last_name.substr(dot + 1);
                        std::transform(format_text.begin(), format_text.end(), format_text.begin(), ::toupper);
                    }

                    const int pct = static_cast<int>(percent + 0.5);
                    std::string progress_text = std::to_string(pct) + "% • " + eta_text + " • " + speed_text;
                    if (!format_text.empty()) {
                        progress_text += " • " + format_text;
                    }
                    this->instpage->progressText->SetText(progress_text);
                    this->instpage->progressText->SetX((1280 - this->instpage->progressText->GetTextWidth()) / 2);
                    this->instpage->progressText->SetVisible(true);
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
                                this->instpage->installIconImage->SetVisible(true);
                                this->instpage->awooImage->SetVisible(false);
                                icon_set = true;
                            }
                        }
                    }
                }
            }

            if (inst::mtp::ConsumeStreamInstallComplete()) {
                this->instpage->installBar->SetVisible(true);
                this->instpage->installBar->SetProgress(100);
                this->instpage->installInfoText->SetText("inst.info_page.complete"_lang + std::string("\n\n") + "inst.mtp.waiting.hint"_lang);
                this->instpage->hintText->SetVisible(true);
                this->instpage->progressText->SetText("100% • done");
                this->instpage->progressText->SetX((1280 - this->instpage->progressText->GetTextWidth()) / 2);
                this->instpage->progressText->SetVisible(true);
                if (!complete_notified) {
                    this->CreateShowDialog(last_name + "inst.info_page.desc1"_lang, Language::GetRandomMsg(), {"common.ok"_lang}, true);
                    complete_notified = true;
                }
            }


            if (!server_running && last_server_running) {
                this->instpage->hintText->SetVisible(false);
            }

            last_active = active;
            last_server_running = server_running;
        });
    }
}
