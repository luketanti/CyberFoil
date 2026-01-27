#include "ui/MainApplication.hpp"
#include "util/lang.hpp"
#include "util/config.hpp"
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
                icon_set = false;
            }

            if (active && !last_active) {
                last_name = inst::mtp::GetStreamInstallName();
                if (last_name.empty()) {
                    last_name = "MTP Install";
                }
                complete_notified = false;
                this->LoadLayout(this->instpage);
                this->instpage->pageInfoText->SetText("inst.info_page.top_info0"_lang + last_name + " (MTP)");
                this->instpage->installInfoText->SetText("inst.info_page.preparing"_lang);
                this->instpage->installBar->SetVisible(true);
                this->instpage->installBar->SetProgress(0);
                this->instpage->installIconImage->SetVisible(false);
                this->instpage->awooImage->SetVisible(!inst::config::gayMode);
                this->instpage->hintText->SetVisible(true);
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
