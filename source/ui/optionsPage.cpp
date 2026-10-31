#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <switch.h>
#include "ui/MainApplication.hpp"
#include "ui/mainPage.hpp"
#include "ui/instPage.hpp"
#include "ui/optionsPage.hpp"
#include "util/util.hpp"
#include "util/config.hpp"
#include "util/curl.hpp"
#include "util/offline_db_update.hpp"
#include "util/unzip.hpp"
#include "util/lang.hpp"
#include "ui/instPage.hpp"
#include "remoteInstall.hpp"
#include "ui/bottomHint.hpp"

#include "util/theme.hpp"

#define COLOR(hex) inst::theme::Palette(hex)

namespace inst::ui {
    extern MainApplication *mainApp;

    std::vector<std::string> languageStrings = {"English", "日本語", "Français", "Deutsch", "Italiano", "Español", "Português", "Korean", "Русский", "簡体中文","繁體中文"};

    namespace {
        // Keep the form highlight in place while its dropdown handles input.
        class RemoteFormMenu final : public inst::theme::Menu {
        public:
            explicit RemoteFormMenu(bool& dropdownVisible)
                : inst::theme::Menu(260, 152, 760, COLOR("#00000000"), 40, 11, 22), dropdownVisible(dropdownVisible) {}
            void OnInput(u64 down, u64 up, u64 held, pu::ui::Touch pos) override {
                if (!dropdownVisible) inst::theme::Menu::OnInput(down, up, held, pos);
            }
        private:
            bool& dropdownVisible;
        };
        bool IsActiveRemote(const inst::config::RemoteProfile& remote)
        {
            return (inst::config::remoteProfileFile.empty() ? inst::config::RemoteMatchesUrl(remote, inst::config::remoteUrl)
                : remote.fileName == inst::config::remoteProfileFile) &&
                   remote.username == inst::config::remoteUser &&
                   remote.password == inst::config::remotePass &&
                   remote.legacyMode == inst::config::remoteLegacyMode;
        }

        std::string TrimString(const std::string& value)
        {
            if (value.empty())
                return "";
            std::size_t start = value.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                return "";
            std::size_t end = value.find_last_not_of(" \t\r\n");
            return value.substr(start, (end - start) + 1);
        }

        std::string ActiveRemoteLabel(const std::vector<inst::config::RemoteProfile>& remotes)
        {
            for (const auto& remote : remotes) {
                if (!IsActiveRemote(remote))
                    continue;
                if (remote.favourite)
                    return "* " + remote.title;
                return remote.title;
            }

            if (!inst::config::remoteUrl.empty())
                return inst::util::shortenString(inst::config::remoteUrl, 42, false);
            return "-";
        }

        std::vector<std::string> BuildRemoteChoices(const std::vector<inst::config::RemoteProfile>& remotes)
        {
            constexpr int kMaxRemoteChoiceLength = 48;
            std::vector<std::string> labels;
            labels.reserve(remotes.size());
            for (const auto& remote : remotes) {
                const std::string prefix = remote.favourite ? "* " : "";
                const std::string suffix = IsActiveRemote(remote) ? " (Active)" : "";
                const int titleLength = std::max(1, kMaxRemoteChoiceLength - static_cast<int>(prefix.size() + suffix.size()));
                std::string label = prefix + inst::util::shortenString(remote.title, titleLength, false) + suffix;
                labels.push_back(label);
            }
            return labels;
        }

        std::string GetUserAgentProfileLabel(const std::string& mode)
        {
            const std::string normalized = inst::config::NormalizeHttpUserAgentMode(mode);
            if (normalized == "chrome")
                return "Chrome (Windows)";
            if (normalized == "safari")
                return "Safari (iPhone)";
            if (normalized == "tinfoil")
                return "Tinfoil";
            if (normalized == "firefox")
                return "Firefox (Windows)";
            if (normalized == "custom")
                return "Custom";
            return "Default (CyberFoil)";
        }

        int GetUserAgentProfileChoiceIndex(const std::string& mode)
        {
            const std::string normalized = inst::config::NormalizeHttpUserAgentMode(mode);
            if (normalized == "tinfoil")
                return 1;
            if (normalized == "chrome")
                return 2;
            if (normalized == "safari")
                return 3;
            if (normalized == "firefox")
                return 4;
            if (normalized == "custom")
                return 5;
            return 0;
        }

        std::string GetUserAgentProfileModeFromChoice(int choice)
        {
            if (choice == 1)
                return "tinfoil";
            if (choice == 2)
                return "chrome";
            if (choice == 3)
                return "safari";
            if (choice == 4)
                return "firefox";
            if (choice == 5)
                return "custom";
            return "default";
        }

        std::vector<std::string> WrapDialogText(const std::string& text, std::size_t maxLineChars)
        {
            std::vector<std::string> lines;
            if (text.empty()) {
                lines.push_back("No changelog available for this release.");
                return lines;
            }

            std::stringstream paragraphs(text);
            std::string paragraph;
            while (std::getline(paragraphs, paragraph)) {
                if (paragraph.empty()) {
                    lines.push_back("");
                    continue;
                }

                std::stringstream words(paragraph);
                std::string word;
                std::string line;
                while (words >> word) {
                    const std::string candidate = line.empty() ? word : (line + " " + word);
                    if (candidate.size() <= maxLineChars) {
                        line = candidate;
                        continue;
                    }

                    if (!line.empty()) {
                        lines.push_back(line);
                        line.clear();
                    }

                    if (word.size() <= maxLineChars) {
                        line = word;
                        continue;
                    }

                    std::size_t start = 0;
                    while (start < word.size()) {
                        lines.push_back(word.substr(start, maxLineChars));
                        start += maxLineChars;
                    }
                }

                if (!line.empty())
                    lines.push_back(line);
            }

            if (lines.empty())
                lines.push_back("No changelog available for this release.");
            return lines;
        }

        void ShowPagedTextDialog(const std::string& title, const std::string& text)
        {
            auto lines = WrapDialogText(text, 68);
            static constexpr int kLinesPerPage = 18;
            const int totalPages = std::max(1, static_cast<int>((lines.size() + kLinesPerPage - 1) / kLinesPerPage));
            int page = 0;

            while (true) {
                const int start = page * kLinesPerPage;
                const int end = std::min<int>(static_cast<int>(lines.size()), start + kLinesPerPage);
                std::string body;
                for (int i = start; i < end; i++) {
                    if (!body.empty())
                        body.push_back('\n');
                    body += lines[static_cast<std::size_t>(i)];
                }
                body += "\n\nPage " + std::to_string(page + 1) + "/" + std::to_string(totalPages);

                std::vector<std::string> options;
                std::vector<int> actions;
                if (page > 0) {
                    options.push_back("Previous");
                    actions.push_back(-1);
                }
                if (page + 1 < totalPages) {
                    options.push_back("Next");
                    actions.push_back(1);
                }
                options.push_back("Close");
                actions.push_back(0);

                const int choice = mainApp->CreateShowDialog(title, body, options, false);
                if (choice < 0 || choice >= static_cast<int>(actions.size()) || actions[choice] == 0)
                    break;

                page += actions[choice];
                if (page < 0)
                    page = 0;
                if (page >= totalPages)
                    page = totalPages - 1;
            }
        }

    }

    optionsPage::optionsPage() : Layout::Layout() {
        if (inst::config::remoteLegacyMode && inst::config::NormalizeHttpUserAgentMode(inst::config::httpUserAgentMode) != "tinfoil") {
            inst::config::httpUserAgentMode = "tinfoil";
            inst::config::httpUserAgent.clear();
            inst::config::setConfig();
        }

        inst::theme::ApplyBackground(*this, inst::config::oledMode, inst::config::appDir);
        const auto topColor = inst::theme::Color("header", inst::config::oledMode ? COLOR("#000000FF") : COLOR("#170909FF"));
        const auto infoColor = inst::theme::Color("panel", inst::config::oledMode ? COLOR("#000000FF") : COLOR("#17090980"));
        const auto botColor = inst::theme::Color("footer", inst::config::oledMode ? COLOR("#000000FF") : COLOR("#17090980"));
        this->topRect = Rectangle::New(0, 0, 1280, 74, topColor);
        this->infoRect = Rectangle::New(0, 75, 1280, 60, infoColor);
        this->botRect = Rectangle::New(0, 660, 1280, 60, botColor);
        this->sideNavRect = Rectangle::New(0, 136, 300, 523, inst::config::oledMode ? COLOR("#FFFFFF18") : COLOR("#170909A0"));
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
        this->pageInfoText = TextBlock::New(10, 89, "options.title"_lang, 30);
        this->pageInfoText->SetColor(COLOR("#FFFFFFFF"));
        const std::string optionsHintText = " Select/Change    / Section     Back";
        this->butText = TextBlock::New(10, 678, optionsHintText, 20);
        this->butText->SetColor(COLOR("#FFFFFFFF"));
        this->bottomHintSegments = BuildBottomHintSegments(optionsHintText, 10, 20);
        this->menu = inst::theme::Menu::New(301, 136, 979, COLOR("#FFFFFF00"), 72, (506 / 72), 20);
        if (inst::config::oledMode) {
            this->menu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#FFFFFF33")));
            this->menu->SetScrollbarColor(inst::theme::Color("scrollbar", COLOR("#FFFFFF66")));
        } else {
            this->menu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#00000033")));
            this->menu->SetScrollbarColor(inst::theme::Color("scrollbar", COLOR("#17090980")));
        }
        this->Add(this->topRect);
        this->Add(this->infoRect);
        this->Add(this->botRect);
        this->Add(this->sideNavRect);
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
        this->Add(this->butText);
        this->Add(this->pageInfoText);
        for (int i = 0; i < 3; i++) {
            auto sectionHighlight = Rectangle::New(30, 152 + (i * 56), 240, 48, COLOR("#FFFFFF00"), 10);
            this->sectionHighlights.push_back(sectionHighlight);
            this->Add(sectionHighlight);
            auto sectionText = TextBlock::New(40, 170 + (i * 56), "", 26);
            sectionText->SetColor(COLOR("#FFFFFFFF"));
            this->sectionTexts.push_back(sectionText);
            this->Add(sectionText);
        }
        this->sectionMenuIndices.assign(this->sectionTexts.size(), 0);
        this->refreshOptions(true);
        this->Add(this->menu);

        this->remoteFormShade = Rectangle::New(0, 74, 1280, 586, COLOR("#00000088"));
        this->remoteFormBorder = Rectangle::New(238, 80, 804, 574, inst::config::oledMode ? COLOR("#FFFFFF66") : COLOR("#170909FF"));
        this->remoteFormPanel = Rectangle::New(241, 83, 798, 568, inst::config::oledMode ? COLOR("#000000FA") : COLOR("#170909FA"));
        this->remoteFormTitle = TextBlock::New(270, 104, "Remote", 32);
        this->remoteFormHint = TextBlock::New(270, 614, " Edit/Select     Cancel", 20);
        this->remoteFormMenu = std::make_shared<RemoteFormMenu>(this->remoteFormLanProtocolDropdownVisible);
        this->remoteFormMenu->SetOnFocusColor(inst::theme::Color("highlight",
            inst::config::oledMode ? COLOR("#FFFFFF33") : COLOR("#00000070")));
        this->remoteFormTitle->SetColor(COLOR("#FFFFFFFF"));
        this->remoteFormHint->SetColor(COLOR("#FFFFFFFF"));
        this->remoteFormShade->SetVisible(false);
        this->remoteFormBorder->SetVisible(false);
        this->remoteFormPanel->SetVisible(false);
        this->remoteFormTitle->SetVisible(false);
        this->remoteFormHint->SetVisible(false);
        this->remoteFormMenu->SetVisible(false);
        this->Add(this->remoteFormShade);
        this->Add(this->remoteFormBorder);
        this->Add(this->remoteFormPanel);
        this->Add(this->remoteFormTitle);
        this->Add(this->remoteFormHint);
        this->Add(this->remoteFormMenu);
        for (int i = 0; i < 10; ++i) {
            auto value = TextBlock::New(510, 152 + i * 40, "", 22);
            value->SetColor(COLOR("#FFFFFFFF"));
            value->SetVisible(false);
            this->remoteFormValues.push_back(value);
            this->Add(value);
        }
        this->remoteFormLanProtocolPanel = Rectangle::New(502, 192, 310, 104,
            inst::config::oledMode ? COLOR("#000000FF") : COLOR("#170909FF"));
        this->remoteFormLanProtocolMenu = inst::theme::Menu::New(510, 200, 294, COLOR("#00000000"), 44, 2, 22);
        this->remoteFormLanProtocolMenu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#FFFFFF33")));
        for (const char* label : {"HTTP", "HTTPS"}) {
            auto item = MenuItem::New(label);
            item->SetColor(COLOR("#FFFFFFFF"));
            // Controller A is handled by the page. Key 0 keeps this callback
            // touch-only, so the A press opening the menu cannot also accept it
            // when Plutonium dispatches that same input to visible elements.
            item->AddOnClick([this]() { this->applyRemoteFormLanProtocol(); }, 0);
            this->remoteFormLanProtocolMenu->AddItem(item);
        }
        this->remoteFormLanProtocolPanel->SetVisible(false);
        this->remoteFormLanProtocolMenu->SetVisible(false);
        this->Add(this->remoteFormLanProtocolPanel);
        this->Add(this->remoteFormLanProtocolMenu);

        this->discoveryMenu = inst::theme::Menu::New(310, 140, 950, COLOR("#00000000"), 80, 5, 20);
        this->discoveryMenu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#FFFFFF33")));
        this->discoveryStatus = TextBlock::New(320, 554, "", 20);
        this->discoveryStatus->SetColor(COLOR("#FFFFFFFF"));
        this->discoverySpinner = TextBlock::New(740, 320, "●···", 48);
        this->discoverySpinner->SetColor(COLOR("#FFFFFFFF"));
        this->discoverySpinner->SetVisible(false);
        this->discoveryMenu->SetVisible(false);
        this->discoveryStatus->SetVisible(false);
        this->Add(this->discoveryMenu);
        this->Add(this->discoveryStatus);
        this->Add(this->discoverySpinner);
    }

    optionsPage::~optionsPage() {
        this->stopDiscoveryWorker();
    }

    void optionsPage::stopDiscoveryWorker() {
        this->discoveryCancel.store(true);
        if (this->discoveryThread.joinable()) this->discoveryThread.join();
    }

    void optionsPage::startDiscovery() {
        if (this->discoveryThread.joinable()) this->discoveryThread.join();
        this->discoveryResult = {};
        this->discoveryCancel.store(false);
        this->discoveryDone.store(false);
        this->discoveryVisible = true;
        this->discoveryFinished = false;
        this->discoverySpinnerFrame = 0;
        this->discoverySpinnerLastTick = armGetSystemTick();
        this->discoverySpinner->SetText("●···");
        this->discoverySpinner->SetX(785 - this->discoverySpinner->GetTextWidth() / 2);
        this->discoverySpinner->SetVisible(true);
        this->menu->SetVisible(false);
        this->remoteFormShade->SetVisible(false);
        this->remoteFormBorder->SetVisible(false);
        this->remoteFormPanel->SetVisible(false);
        this->remoteFormTitle->SetVisible(false);
        this->remoteFormHint->SetVisible(false);
        this->remoteFormMenu->SetVisible(false);
        for (auto& value : this->remoteFormValues) value->SetVisible(false);
        this->remoteFormLanProtocolDropdownVisible = false;
        this->remoteFormLanProtocolPanel->SetVisible(false);
        this->remoteFormLanProtocolMenu->SetVisible(false);
        this->pageInfoText->SetVisible(true);
        this->pageInfoText->SetText("Discover LAN Servers");
        this->butText->SetVisible(true);
        const std::string hint = " Cancel";
        this->butText->SetText(hint);
        this->bottomHintSegments = BuildBottomHintSegments(hint, 10, 20);
        this->discoveryMenu->ClearItems();
        this->discoveryMenu->SetVisible(true);
        this->discoveryStatus->SetVisible(true);
        this->discoveryStatus->SetText("Scanning LAN for about three seconds...\nAeroFoil LAN discovery must be enabled; allow UDP 8465.");
        try {
            this->discoveryThread = std::thread([this]() {
                try { this->discoveryResult = inst::discovery::ScanLan(this->discoveryCancel); }
                catch (...) { this->discoveryResult.error = "Discovery failed: networking or memory unavailable."; }
                this->discoveryDone.store(true, std::memory_order_release);
            });
        } catch (...) {
            this->discoveryResult.error = "Cannot start discovery worker.";
            this->discoveryDone.store(true, std::memory_order_release);
        }
    }

    void optionsPage::closeDiscovery() {
        this->discoveryVisible = false;
        this->discoveryMenu->SetVisible(false);
        this->discoveryStatus->SetVisible(false);
        this->discoverySpinner->SetVisible(false);
        this->menu->SetVisible(true);
        if (this->remoteFormVisible)
            this->openRemoteForm(this->remoteFormProfile, this->remoteFormWasActive,
                this->remoteFormReturnToList, this->remoteFormReturnIndex);
        else this->closeRemoteList();
    }

    void optionsPage::updateDiscovery(u64 down) {
        if (down & HidNpadButton_B) {
            this->discoveryCancel.store(true);
            this->discoveryStatus->SetText("Cancelling discovery...");
        }
        if (this->discoveryDone.load(std::memory_order_acquire) && !this->discoveryFinished) {
            if (this->discoveryThread.joinable()) this->discoveryThread.join();
            this->discoveryFinished = true;
            this->discoverySpinner->SetVisible(false);
            for (const auto& server : this->discoveryResult.servers) {
                auto item = MenuItem::New((server.name.empty() ? "AeroFoil" : server.name) + "\n" +
                    server.address + ":" + std::to_string(server.port) + "    " + server.version +
                    (server.isPublic ? "    Public" : "    Private"));
                item->SetColor(COLOR("#FFFFFFFF"));
                item->AddOnClick([this]() {
                    if (this->discoveryVisible && this->discoveryFinished && !this->discoveryCancel.load())
                        this->updateDiscovery(HidNpadButton_A);
                });
                this->discoveryMenu->AddItem(item);
            }
            this->discoveryMenu->SetSelectedIndex(0);
            std::string status = this->discoveryResult.error;
            if (status.empty()) status = this->discoveryResult.servers.empty()
                ? "No servers found. Enable LAN discovery and allow UDP 8465."
                : "Select a server to fill the Remote form. Save Remote to apply it.";
            this->discoveryStatus->SetText(status + "\nWi-Fi isolation / separate subnets can prevent discovery.");
            const std::string hint = " Select server     Scan again     Back";
            this->butText->SetText(hint);
            this->bottomHintSegments = BuildBottomHintSegments(hint, 10, 20);
        }
        if (!this->discoveryFinished) {
            const u64 now = armGetSystemTick();
            const u64 stepTicks = (armGetSystemTickFreq() * 140) / 1000;
            if (now - this->discoverySpinnerLastTick >= stepTicks) {
                static const char* frames[] = {"●···", "·●··", "··●·", "···●"};
                this->discoverySpinnerLastTick = now;
                this->discoverySpinnerFrame = (this->discoverySpinnerFrame + 1) % 4;
                this->discoverySpinner->SetText(frames[this->discoverySpinnerFrame]);
                this->discoverySpinner->SetX(785 - this->discoverySpinner->GetTextWidth() / 2);
            }
            return;
        }
        if (this->discoveryCancel.load()) { this->closeDiscovery(); return; }
        if (down & HidNpadButton_X) { this->startDiscovery(); return; }
        if (down & HidNpadButton_A) {
            const int index = this->discoveryMenu->GetSelectedIndex();
            if (index < 0 || index >= static_cast<int>(this->discoveryResult.servers.size())) return;
            const auto server = this->discoveryResult.servers[index];
            inst::config::RemoteProfile profile = this->remoteFormVisible
                ? this->remoteFormProfile : inst::config::RemoteProfile{};
            bool wasActive = this->remoteFormVisible ? this->remoteFormWasActive : true;
            if (!this->remoteFormVisible) {
                for (const auto& saved : inst::config::LoadRemotes()) {
                    if (saved.uid == server.uid || (saved.uid.empty() && inst::config::RemoteMatchesUrl(saved, server.url()))) {
                        profile = saved;
                        break;
                    }
                }
            }
            if (!this->remoteFormVisible) {
                // Preserve the saved credentials; discovery supplies no login details.
                if (profile.fileName.empty()) {
                    profile.username = inst::config::remoteUser;
                    profile.password = inst::config::remotePass;
                    profile.legacyMode = inst::config::remoteLegacyMode;
                }
            }
            profile.protocol = "http";
            profile.host = server.address;
            profile.port = server.port;
            profile.path.clear();
            profile.lanUrl = server.url();
            profile.uid = server.uid;
            std::string suggestedWan;
            if (profile.wanUrl.empty() && !server.remote.empty() &&
                inst::config::NormalizeRemoteAddress(server.remote, "https", suggestedWan))
                profile.wanUrl = suggestedWan;
            if (profile.title.empty()) profile.title = server.name.empty() ? "AeroFoil" : server.name;
            this->closeDiscovery();
            this->openRemoteForm(profile, wasActive, this->remoteFormVisible && this->remoteFormReturnToList,
                this->remoteFormReturnIndex);
        }
    }

    void optionsPage::askToUpdate(std::vector<std::string> updateInfo) {
            const std::string version = updateInfo.empty() ? std::string() : updateInfo[0];
            const std::string downloadUrl = updateInfo.size() > 1 ? updateInfo[1] : std::string();
            const std::string releaseNotes = updateInfo.size() > 2 ? updateInfo[2] : "No changelog available for this release.";

            while (true) {
                int choice = mainApp->CreateShowDialog(
                    "options.update.title"_lang,
                    "options.update.desc0"_lang + version + "options.update.desc1"_lang,
                    {"options.update.opt0"_lang, "View Changelog", "common.cancel"_lang},
                    false);

                if (choice == 1) {
                    ShowPagedTextDialog("Changelog " + version, releaseNotes);
                    continue;
                }

                if (choice != 0)
                    break;

                inst::ui::instPage::loadInstallScreen();
                inst::ui::instPage::setTopInstInfoText("options.update.top_info"_lang + version);
                inst::ui::instPage::setInstBarPerc(0);
                inst::ui::instPage::setInstInfoText("options.update.bot_info"_lang + version);
                try {
                    std::string downloadName = inst::config::appDir + "/temp_download.zip";
                    inst::curl::downloadFile(downloadUrl, downloadName.c_str(), 0, true);
                    romfsExit();
                    inst::ui::instPage::setInstInfoText("options.update.bot_info2"_lang + version);
                    inst::zip::extractFile(downloadName, "sdmc:/");
                    std::filesystem::remove(downloadName);
                    mainApp->CreateShowDialog("options.update.complete"_lang, "options.update.end_desc"_lang, {"common.ok"_lang}, false);
                } catch (...) {
                    mainApp->CreateShowDialog("options.update.failed"_lang, "options.update.end_desc"_lang, {"common.ok"_lang}, false);
                }
                mainApp->FadeOut();
                mainApp->Close();
                break;
            }
        return;
    }

    std::string optionsPage::getMenuOptionIcon(bool ourBool) {
        if(ourBool) return inst::theme::Asset("romfs:/images/icons/check-box-outline.png");
        else return inst::theme::Asset("romfs:/images/icons/checkbox-blank-outline.png");
    }

    std::string optionsPage::getMenuLanguage(int ourLangCode) {
        switch (ourLangCode) {
            case 1:
            case 12:
                return languageStrings[0];
            case 0:
                return languageStrings[1];
            case 2:
            case 13:
                return languageStrings[2];
            case 3:
                return languageStrings[3];
            case 4:
                return languageStrings[4];
            case 5:
            case 14:
                return languageStrings[5];
            case 9:
                return languageStrings[6];
            case 7:
                return languageStrings[7];
            case 10:
                return languageStrings[8];
            case 6:
                return languageStrings[9];
            case 11:
                return languageStrings[10];
            default:
                return "options.language.system_language"_lang;
        }
    }

    void optionsPage::setSectionNavText() {
        static const std::vector<std::string> sectionLabels = {"General", "Remote", "System"};
        const bool customHighlight = inst::theme::HasColor("highlight") || inst::theme::HasColor("accent");
        for (size_t i = 0; i < this->sectionTexts.size() && i < sectionLabels.size(); i++) {
            const bool selected = static_cast<int>(i) == this->selectedSection;
            auto sectionColor = selected
                ? inst::theme::Color("highlight", this->tabsFocused
                    ? (inst::config::oledMode ? COLOR("#FFFFFF55") : COLOR("#FFFFFF66"))
                    : (inst::config::oledMode ? COLOR("#FFFFFF33") : COLOR("#FFFFFF40")))
                : COLOR("#FFFFFF00");
            if (selected && customHighlight && !this->tabsFocused) sectionColor.A /= 4;
            this->sectionHighlights[i]->SetColor(sectionColor);
            this->sectionTexts[i]->SetText(sectionLabels[i]);
            this->sectionTexts[i]->SetColor(selected && this->tabsFocused
                ? inst::theme::Color("selected_text", COLOR("#FFFFFFFF"))
                : (selected ? COLOR("#FFFFFFFF") : (this->tabsFocused ? COLOR("#FFFFFFCC") : COLOR("#FFFFFF99"))));
        }

        // Make the settings-list row highlight clearly reflect which area is active.
        auto rowColor = inst::theme::Color("highlight",
            inst::config::oledMode ? COLOR("#FFFFFF66") : COLOR("#00000070"));
        if (this->tabsFocused) {
            if (customHighlight) rowColor.A /= 4;
            else rowColor = inst::config::oledMode ? COLOR("#FFFFFF18") : COLOR("#00000022");
        }
        this->menu->SetOnFocusColor(rowColor);
    }

    void optionsPage::setSettingsMenuText() {
        this->menu->ClearItems();

        auto addItem = [this](const std::string &label, bool toggle, bool value) {
            auto item = pu::ui::elm::MenuItem::New(label);
            item->SetColor(COLOR("#FFFFFFFF"));
            if (toggle) item->SetIcon(this->getMenuOptionIcon(value));
            this->menu->AddItem(item);
        };

        if (this->selectedSection == 0) {
            addItem("options.menu_items.ignore_firm"_lang, true, inst::config::ignoreReqVers);
            addItem("options.menu_items.nca_verify"_lang, true, inst::config::validateNCAs);
            addItem("Verbose install logs", true, inst::config::verboseInstallLogging);
            addItem("options.menu_items.boost_mode"_lang, true, inst::config::overClock);
            addItem("options.menu_items.ask_delete"_lang, true, inst::config::deletePrompt);
            addItem("options.menu_items.sound"_lang, true, inst::config::soundEnabled);
            addItem("options.menu_items.oled"_lang, true, inst::config::oledMode);
            addItem("options.menu_items.mtp_album"_lang, true, inst::config::mtpExposeAlbum);
            addItem("options.menu_items.usb_hdd_hidden"_lang, true, inst::config::usbHddShowHiddenFiles);
            return;
        }

        if (this->selectedSection == 1) {
            std::vector<inst::config::RemoteProfile> remotes = inst::config::LoadRemotes();
            std::string dbVersion = inst::offline::dbupdate::GetInstalledVersion();
            if (dbVersion.empty())
                dbVersion = "not installed";
            else
                dbVersion = inst::util::shortenString(dbVersion, 24, false);
            const std::string activeLabel = inst::util::shortenString(ActiveRemoteLabel(remotes), 38, false);
            addItem("Remotes: " + activeLabel + " (" + std::to_string(remotes.size()) + " saved)", false, false);
            addItem("Add new Remote", false, false);
            addItem("Discover LAN Servers", false, false);
            const std::string uaMode = inst::config::remoteLegacyMode ? "tinfoil" : inst::config::httpUserAgentMode;
            addItem("User-Agent profile: " + GetUserAgentProfileLabel(uaMode), false, false);
            auto items = this->menu->GetItems();
            if (inst::config::remoteLegacyMode && items.size() > 3 && items[3] != nullptr)
                items[3]->SetColor(COLOR("#FFFFFF88"));
            addItem("Tinfoil Mode (legacy Remote compatibility)", true, inst::config::remoteLegacyMode);
            addItem("options.menu_items.remote_hide_installed"_lang, true, inst::config::remoteHideInstalled);
            addItem("options.menu_items.remote_hide_installed_section"_lang, true, inst::config::remoteHideInstalledSection);
            addItem("Hide cheats not matching local build / installed title", true, inst::config::remoteHideIncompatibleCheats);
            addItem("options.menu_items.remote_all_base_only"_lang, true, inst::config::remoteAllBaseOnly);
            addItem("options.menu_items.remote_start_grid_mode"_lang, true, inst::config::remoteStartGridMode);
            addItem("Offline DB auto-check on startup", true, inst::config::offlineDbAutoCheckOnStartup);
            addItem("Offline DB update (" + dbVersion + ")", false, false);
            return;
        }

        addItem("options.menu_items.auto_update"_lang, true, inst::config::autoUpdate);
        addItem("options.menu_items.gay_option"_lang, true, inst::config::gayMode);
        addItem("options.menu_items.language"_lang + this->getMenuLanguage(inst::config::languageSetting), false, false);
        addItem("options.menu_items.check_update"_lang, false, false);
        addItem("options.menu_items.credits"_lang, false, false);
        std::string themeName = inst::config::themeFile.empty() ? "options.theme_default"_lang : inst::theme::Name();
        for (const auto& choice : inst::theme::Discover(inst::config::appDir)) {
            if (choice.file == inst::config::themeFile) { themeName = choice.name; break; }
        }
        addItem("options.menu_items.theme"_lang + themeName, false, false);
    }

    void optionsPage::refreshOptions(bool resetSelection) {
        this->remoteListVisible = false;
        this->setSectionNavText();
        this->setSettingsMenuText();
        if (resetSelection) this->menu->SetSelectedIndex(0);
    }

    void optionsPage::ShowThemePicker() {
        this->selectedSection = 2;
        this->openThemeList();
    }

    void optionsPage::openThemeList() {
        this->themeChoices = inst::theme::Discover(inst::config::appDir);
        this->themeChoices.insert(this->themeChoices.begin(), {"", "options.theme_default"_lang});
        this->themeListVisible = true;
        this->tabsFocused = false;
        this->touchActive = false;
        this->touchMoved = false;
        this->bottomHintTouch = {};
        this->pageInfoText->SetText("options.theme_picker"_lang);
        const auto hint = "options.theme_picker_hint"_lang;
        this->butText->SetText(hint);
        this->bottomHintSegments = BuildBottomHintSegments(hint, 10, 20);
        this->menu->ClearItems();
        int selected = 0;
        for (std::size_t i = 0; i < this->themeChoices.size(); ++i) {
            const auto& choice = this->themeChoices[i];
            const bool active = choice.file == inst::config::themeFile;
            std::string label = choice.name;
            if (!choice.file.empty()) label += " (" + choice.file + ")";
            auto item = MenuItem::New(inst::util::shortenString(label, 70, false));
            item->SetColor(COLOR("#FFFFFFFF"));
            item->SetIcon(this->getMenuOptionIcon(active));
            this->menu->AddItem(item);
            if (active) selected = static_cast<int>(i);
        }
        this->menu->SetSelectedIndex(selected);
        this->setSectionNavText();
    }

    void optionsPage::closeThemeList() {
        this->themeListVisible = false;
        this->touchActive = false;
        this->bottomHintTouch = {};
        this->pageInfoText->SetText("options.title"_lang);
        const std::string hint = " Select/Change    / Section     Back";
        this->butText->SetText(hint);
        this->bottomHintSegments = BuildBottomHintSegments(hint, 10, 20);
        this->refreshOptions();
        this->menu->SetSelectedIndex(5);
        this->lockedMenuIndex = 5;
    }

    void optionsPage::handleThemeListInput(u64 down, pu::ui::Touch pos) {
        if (down & HidNpadButton_B) { this->closeThemeList(); return; }
        bool tapped = false;
        if (!pos.IsEmpty()) {
            if (!this->touchActive) {
                this->touchActive = true;
                this->touchMoved = false;
                this->touchStartX = pos.X;
                this->touchStartY = pos.Y;
                const int x = this->menu->GetProcessedX(), y = this->menu->GetProcessedY();
                this->touchRegion = pos.X >= x && pos.X <= x + this->menu->GetWidth() &&
                    pos.Y >= y && pos.Y <= y + this->menu->GetHeight() ? 2 : 0;
            } else if (std::abs(pos.X - this->touchStartX) > 12 || std::abs(pos.Y - this->touchStartY) > 12) {
                this->touchMoved = true;
            }
        } else if (this->touchActive) {
            tapped = !this->touchMoved && this->touchRegion == 2;
            this->touchActive = false;
        }
        if ((down & HidNpadButton_A) || tapped) {
            const int index = this->menu->GetSelectedIndex();
            if (index < 0 || index >= static_cast<int>(this->themeChoices.size())) return;
            const auto choice = this->themeChoices[index];
            inst::config::themeFile = choice.file;
            inst::config::setConfig();
            mainApp->RequestThemeReload();
        }
    }

    void optionsPage::openRemoteList(int selectedIndex) {
        this->remoteListProfiles = inst::config::LoadRemotes();
        this->remoteListVisible = true;
        this->remoteListIndex = std::max(0, selectedIndex);
        this->tabsFocused = false;
        this->pageInfoText->SetText("Memorized Remotes (" + std::to_string(this->remoteListProfiles.size()) + ")");

        const std::string hint = " Manage Remote     Back";
        this->butText->SetText(hint);
        this->bottomHintSegments = BuildBottomHintSegments(hint, 10, 20);

        this->menu->ClearItems();
        for (std::size_t i = 0; i < this->remoteListProfiles.size(); i++) {
            const auto& remote = this->remoteListProfiles[i];
            std::string label = std::to_string(i + 1) + ". ";
            if (remote.favourite)
                label += "* ";
            label += inst::util::shortenString(remote.title, 52, false);
            if (IsActiveRemote(remote))
                label += "  (Active)";
            auto item = pu::ui::elm::MenuItem::New(label);
            item->SetColor(COLOR("#FFFFFFFF"));
            this->menu->AddItem(item);
        }

        if (!this->remoteListProfiles.empty()) {
            this->remoteListIndex = std::min(this->remoteListIndex, static_cast<int>(this->remoteListProfiles.size()) - 1);
            this->menu->SetSelectedIndex(this->remoteListIndex);
        }
    }

    void optionsPage::closeRemoteList() {
        this->remoteListVisible = false;
        this->pageInfoText->SetText("options.title"_lang);
        const std::string hint = " Select/Change    / Section     Back";
        this->butText->SetText(hint);
        this->bottomHintSegments = BuildBottomHintSegments(hint, 10, 20);
        this->refreshOptions();
        this->menu->SetSelectedIndex(1);
    }

    void optionsPage::openRemoteForm(const inst::config::RemoteProfile& remote, bool wasActive, bool returnToList, int returnIndex) {
        this->remoteFormProfile = remote;
        if (this->remoteFormProfile.lanUrl.empty() && this->remoteFormProfile.wanUrl.empty() && !remote.host.empty())
            inst::config::NormalizeRemoteProfile(this->remoteFormProfile);
        this->remoteFormLanProtocol = this->remoteFormProfile.lanUrl.rfind("https://", 0) == 0 ? "https" : "http";
        if (this->remoteFormProfile.protocol != "https")
            this->remoteFormProfile.protocol = "http";
        if (this->remoteFormProfile.host.empty())
            this->remoteFormProfile.port = inst::config::DefaultPortForProtocol(this->remoteFormProfile.protocol);
        else if (this->remoteFormProfile.port <= 0 || this->remoteFormProfile.port > 65535)
            this->remoteFormProfile.port = inst::config::DefaultPortForProtocol(this->remoteFormProfile.protocol);
        this->remoteFormProfile.path = inst::config::NormalizeRemotePath(this->remoteFormProfile.path);
        if (!this->remoteFormProfile.lanUrl.empty() || !this->remoteFormProfile.wanUrl.empty()) {
            std::string protocol, host;
            int port = 0;
            inst::config::ParseRemoteUrl(inst::config::BuildRemoteUrl(this->remoteFormProfile),
                protocol, host, port, this->remoteFormProfile.path);
        }
        this->remoteFormWasActive = wasActive;
        this->remoteFormReturnToList = returnToList;
        this->remoteFormReturnIndex = returnIndex;
        this->remoteFormVisible = true;
        this->remoteFormLanProtocolDropdownVisible = false;
        this->remoteFormLanProtocolPanel->SetVisible(false);
        this->remoteFormLanProtocolMenu->SetVisible(false);
        this->menu->SetVisible(false);
        this->pageInfoText->SetVisible(false);
        this->butText->SetVisible(false);
        this->remoteFormShade->SetVisible(true);
        this->remoteFormBorder->SetVisible(true);
        this->remoteFormPanel->SetVisible(true);
        this->remoteFormTitle->SetVisible(true);
        this->remoteFormHint->SetVisible(true);
        this->remoteFormMenu->SetVisible(true);
        for (auto& value : this->remoteFormValues) value->SetVisible(true);
        this->refreshRemoteForm();
    }

    void optionsPage::closeRemoteForm() {
        this->remoteFormVisible = false;
        this->remoteFormLanProtocolDropdownVisible = false;
        this->remoteFormLanProtocolPanel->SetVisible(false);
        this->remoteFormLanProtocolMenu->SetVisible(false);
        this->remoteFormShade->SetVisible(false);
        this->remoteFormBorder->SetVisible(false);
        this->remoteFormPanel->SetVisible(false);
        this->remoteFormTitle->SetVisible(false);
        this->remoteFormHint->SetVisible(false);
        this->remoteFormMenu->SetVisible(false);
        for (auto& value : this->remoteFormValues) value->SetVisible(false);
        this->pageInfoText->SetVisible(true);
        this->butText->SetVisible(true);
        this->menu->SetVisible(true);

        if (this->remoteFormReturnToList)
            this->openRemoteList(this->remoteFormReturnIndex);
        else {
            this->refreshOptions();
            this->menu->SetSelectedIndex(1);
        }
    }

    void optionsPage::refreshRemoteForm() {
        const std::string mode = this->remoteFormProfile.legacyMode ? "Legacy Mode (Tinfoil)" : "CyberFoil Mode";
        const std::string connection = this->remoteFormProfile.connection == "lan" ? "LAN only" :
            (this->remoteFormProfile.connection == "wan" ? "WAN only" : "Auto (LAN first)");
        const auto addressOnly = [](const std::string& url) {
            const auto scheme = url.find("://");
            return scheme == std::string::npos ? url : url.substr(scheme + 3);
        };
        const std::vector<std::string> labels = {
            "Mode", "LAN protocol", "LAN URL", "Remote URL", "Path (optional)", "Connection", "Username", "Password", "Title", "Favourite", "Save Remote"
        };
        const std::vector<std::string> values = {
            mode, this->remoteFormLanProtocol == "https" ? "HTTPS" : "HTTP",
            addressOnly(this->remoteFormProfile.lanUrl), addressOnly(this->remoteFormProfile.wanUrl), this->remoteFormProfile.path, connection,
            this->remoteFormProfile.username, this->remoteFormProfile.password.empty() ? "Not set" : "Set",
            this->remoteFormProfile.title, this->remoteFormProfile.favourite ? "Yes" : "No"
        };
        const int selected = std::max(0, this->remoteFormMenu->GetSelectedIndex());
        this->remoteFormMenu->ClearItems();
        for (const auto& label : labels) {
            auto item = MenuItem::New(label);
            item->SetColor(COLOR("#FFFFFFFF"));
            this->remoteFormMenu->AddItem(item);
        }
        this->remoteFormMenu->SetSelectedIndex(std::min(selected, static_cast<int>(labels.size()) - 1));
        for (std::size_t row = 0; row < values.size(); ++row) {
            auto& text = this->remoteFormValues[row];
            std::string display = values[row].empty() ? (row == 8 ? "Required" : "Not set") : values[row];
            for (char& c : display) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
            // Bound measurement work, preserving UTF-8 boundaries. Values are
            // clipped for display only; the editor receives the full saved URL.
            std::size_t end = std::min<std::size_t>(display.size(), 512);
            while (end < display.size() && end > 0 && (static_cast<unsigned char>(display[end]) & 0xc0) == 0x80) --end;
            const bool shortened = end < display.size();
            display.resize(end);
            text->SetText(display + (shortened ? "..." : ""));
            if (text->GetTextWidth() > 480) {
                std::vector<std::size_t> boundaries{0};
                for (std::size_t i = 1; i <= display.size(); ++i)
                    if (i == display.size() || (static_cast<unsigned char>(display[i]) & 0xc0) != 0x80) boundaries.push_back(i);
                std::size_t low = 0, high = boundaries.size() - 1;
                while (low < high) {
                    const auto middle = (low + high + 1) / 2;
                    text->SetText(display.substr(0, boundaries[middle]) + "...");
                    if (text->GetTextWidth() <= 480) low = middle;
                    else high = middle - 1;
                }
                text->SetText(display.substr(0, boundaries[low]) + "...");
            }
            text->SetY(152 + static_cast<int>(row) * 40 + (40 - text->GetTextHeight()) / 2);
        }
    }

    void optionsPage::applyRemoteFormLanProtocol() {
        const std::string protocol = this->remoteFormLanProtocolMenu->GetSelectedIndex() == 1 ? "https" : "http";
        if (protocol != this->remoteFormLanProtocol && !this->remoteFormProfile.lanUrl.empty()) {
            std::string previousProtocol, host, path;
            int port = 0;
            if (inst::config::ParseRemoteUrl(this->remoteFormProfile.lanUrl, previousProtocol, host, port, path)) {
                const int previousDefault = previousProtocol == "https" ? 443 : 80;
                if (port == previousDefault) port = protocol == "https" ? 443 : 80;
                this->remoteFormProfile.lanUrl = protocol + "://" + host + ":" + std::to_string(port) + path;
            }
        }
        this->remoteFormLanProtocol = protocol;
        this->remoteFormLanProtocolDropdownVisible = false;
        this->remoteFormLanProtocolPanel->SetVisible(false);
        this->remoteFormLanProtocolMenu->SetVisible(false);
        this->refreshRemoteForm();
    }

    void optionsPage::editRemoteFormField() {
        const int field = this->remoteFormMenu->GetSelectedIndex();
        if (field == 0) {
            const int choice = mainApp->CreateShowDialog("Remote mode", "Choose compatibility mode.",
                {"CyberFoil", "Tinfoil (legacy)", "common.cancel"_lang}, false);
            if (choice == 0 || choice == 1) this->remoteFormProfile.legacyMode = choice == 1;
        } else if (field == 1) {
            this->remoteFormLanProtocolDropdownVisible = true;
            this->remoteFormLanProtocolMenu->SetSelectedIndex(this->remoteFormLanProtocol == "https" ? 1 : 0);
            this->remoteFormLanProtocolPanel->SetVisible(true);
            this->remoteFormLanProtocolMenu->SetVisible(true);
        } else if (field == 2 || field == 3) {
            const bool lan = field == 2;
            auto& endpoint = lan ? this->remoteFormProfile.lanUrl : this->remoteFormProfile.wanUrl;
            const int action = mainApp->CreateShowDialog(lan ? "LAN URL" : "Remote URL",
                lan ? "Enter an IP or hostname and port. The LAN protocol is selected separately." :
                    "Enter an IP or hostname and port. HTTPS is used automatically.",
                {"Edit", "Clear", "common.cancel"_lang}, false);
            if (action == 1) endpoint.clear();
            else if (action == 0) {
                const auto scheme = endpoint.find("://");
                const auto currentAddress = scheme == std::string::npos ? endpoint : endpoint.substr(scheme + 3);
                const auto value = TrimString(inst::util::softwareKeyboard("Enter IP or hostname[:port]", currentAddress, 512));
                if (!value.empty()) {
                    std::string normalized;
                    if (inst::config::NormalizeRemoteAddress(value, lan ? this->remoteFormLanProtocol : "https", normalized)) {
                        std::string protocol, host, path;
                        int port = 0;
                        inst::config::ParseRemoteUrl(normalized, protocol, host, port, path);
                        if (path.empty()) normalized += this->remoteFormProfile.path;
                        else this->remoteFormProfile.path = path;
                        endpoint = normalized;
                    }
                    else mainApp->CreateShowDialog("Invalid address", lan ?
                        "Enter a valid IP or hostname and port, without a scheme. Choose HTTP or HTTPS under LAN protocol." :
                        "Enter a valid IP or hostname and port. Remote connections require HTTPS.", {"common.ok"_lang}, true);
                }
            }
        } else if (field == 4) {
            const int action = mainApp->CreateShowDialog("Remote path",
                "Optional URL path, for example /remote.json. Editing or clearing it applies to both LAN and Remote URLs.",
                {"Edit", "Clear", "common.cancel"_lang}, false);
            if (action == 0 || action == 1) {
                const auto path = action == 1 ? std::string{} : inst::config::NormalizeRemotePath(
                    inst::util::softwareKeyboard("Enter URL path (optional)", this->remoteFormProfile.path, 512));
                std::string lan, wan, validated;
                const auto withPath = [&](const std::string& endpoint, std::string& result) {
                    if (endpoint.empty()) return true;
                    const auto slash = endpoint.find('/', endpoint.find("://") + 3);
                    return inst::config::NormalizeRemoteEndpoint(endpoint.substr(0, slash) + path, result);
                };
                if (withPath(this->remoteFormProfile.lanUrl, lan) && withPath(this->remoteFormProfile.wanUrl, wan) &&
                    (path.empty() || inst::config::NormalizeRemoteEndpoint("https://example.com" + path, validated))) {
                    // Validate before changing either endpoint, including when no URL is entered yet.
                    this->remoteFormProfile.path = path;
                    this->remoteFormProfile.lanUrl = lan;
                    this->remoteFormProfile.wanUrl = wan;
                } else mainApp->CreateShowDialog("Invalid path", "Enter a valid URL path.", {"common.ok"_lang}, true);
            }
        } else if (field == 5) {
            const int choice = mainApp->CreateShowDialog("Connection preference", "Auto tries LAN connectivity first, then WAN if LAN is unreachable.",
                {"Auto", "LAN only", "WAN only", "common.cancel"_lang}, false);
            if (choice >= 0 && choice < 3) this->remoteFormProfile.connection = choice == 0 ? "auto" : (choice == 1 ? "lan" : "wan");
        } else if (field == 6) {
            this->remoteFormProfile.username = inst::util::softwareKeyboard("options.remote.user_hint"_lang, this->remoteFormProfile.username, 100);
        } else if (field == 7) {
            this->remoteFormProfile.password = inst::util::softwareKeyboard("options.remote.pass_hint"_lang, this->remoteFormProfile.password, 100);
        } else if (field == 8) {
            this->remoteFormProfile.title = TrimString(inst::util::softwareKeyboard("Enter Remote title (required)", this->remoteFormProfile.title, 80));
        } else if (field == 9) {
            this->remoteFormProfile.favourite = !this->remoteFormProfile.favourite;
        } else if (field == 10) {
            this->saveRemoteForm();
            return;
        }
        this->refreshRemoteForm();
    }

    void optionsPage::saveRemoteForm() {
        std::string error;
        // Clear compatibility fields so deliberately clearing both URLs cannot
        // resurrect the old endpoint through legacy migration.
        this->remoteFormProfile.host.clear();
        if (!inst::config::NormalizeRemoteProfile(this->remoteFormProfile, &error) || this->remoteFormProfile.title.empty()) {
            mainApp->CreateShowDialog("Invalid Remote", error.empty() ? "Title is required." : error, {"common.ok"_lang}, true);
            return;
        }
        if (!inst::config::SaveRemote(this->remoteFormProfile, &error)) {
            mainApp->CreateShowDialog("Failed to save Remote", error.empty() ? "Unknown error." : error, {"common.ok"_lang}, true);
            return;
        }
        if (this->remoteFormWasActive)
            inst::config::SetActiveRemote(this->remoteFormProfile, true);
        this->closeRemoteForm();
    }

    void optionsPage::manageRemote(const inst::config::RemoteProfile& remote) {
        const int selectedIndex = this->menu->GetSelectedIndex();
        std::string favouriteLabel = remote.favourite ? "Unset favourite" : "Set favourite";
        const int action = inst::ui::mainApp->CreateShowDialog(
            remote.title,
            "Choose an action for this Remote.",
            {"Use this Remote", "Edit Remote", favouriteLabel, "Delete Remote", "Cancel"},
            false
        );

        if (action == 0) {
            inst::config::SetActiveRemote(remote, true);
        } else if (action == 1) {
            this->openRemoteForm(remote, IsActiveRemote(remote), true, selectedIndex);
            return;
        } else if (action == 2) {
            inst::config::RemoteProfile updated = remote;
            updated.favourite = !updated.favourite;
            std::string error;
            if (!inst::config::SaveRemote(updated, &error))
                inst::ui::mainApp->CreateShowDialog("Failed to save Remote", error.empty() ? "Unknown error." : error, {"common.ok"_lang}, true);
        } else if (action == 3) {
            const int confirmDelete = inst::ui::mainApp->CreateShowDialog("Delete Remote?", "This cannot be undone.", {"Delete", "common.cancel"_lang}, false);
            if (confirmDelete == 0) {
                if (!inst::config::DeleteRemote(remote.fileName)) {
                    inst::ui::mainApp->CreateShowDialog("Failed to delete Remote", "Could not remove the Remote file.", {"common.ok"_lang}, true);
                } else if (IsActiveRemote(remote)) {
                    inst::config::remoteUrl.clear();
                    inst::config::remoteProfileFile.clear();
                    inst::config::remoteUser.clear();
                    inst::config::remotePass.clear();
                    inst::config::setConfig();
                }
            }
        }

        this->openRemoteList(selectedIndex);
    }

    void optionsPage::rememberCurrentSectionMenuIndex() {
        if (this->selectedSection < 0)
            return;
        if (this->selectedSection >= static_cast<int>(this->sectionMenuIndices.size()))
            this->sectionMenuIndices.resize(this->sectionTexts.size(), 0);
        int selected = this->menu->GetSelectedIndex();
        if (selected < 0)
            selected = 0;
        this->sectionMenuIndices[this->selectedSection] = selected;
    }

    void optionsPage::restoreSelectedSectionMenuIndex() {
        if (this->selectedSection < 0 || this->selectedSection >= static_cast<int>(this->sectionMenuIndices.size()))
            return;
        const int itemCount = static_cast<int>(this->menu->GetItems().size());
        if (itemCount <= 0)
            return;
        int selected = this->sectionMenuIndices[this->selectedSection];
        if (selected < 0)
            selected = 0;
        if (selected >= itemCount)
            selected = itemCount - 1;
        this->sectionMenuIndices[this->selectedSection] = selected;
        this->menu->SetSelectedIndex(selected);
    }

    void optionsPage::setSelectedSectionAndRefresh(int newSection) {
        if (this->sectionTexts.empty())
            return;
        const int sectionCount = static_cast<int>(this->sectionTexts.size());
        if (newSection < 0)
            newSection = sectionCount - 1;
        if (newSection >= sectionCount)
            newSection = 0;

        this->rememberCurrentSectionMenuIndex();
        this->selectedSection = newSection;
        this->refreshOptions(false);
        this->restoreSelectedSectionMenuIndex();
        this->lockedMenuIndex = this->menu->GetSelectedIndex();
    }

    int optionsPage::getSectionFromTouch(int x, int y) const {
        const int navX = this->sideNavRect->GetProcessedX();
        const int navY = this->sideNavRect->GetProcessedY();
        const int navW = this->sideNavRect->GetWidth();
        const int navH = this->sideNavRect->GetHeight();
        const bool inNav = (x >= navX) && (x <= (navX + navW)) && (y >= navY) && (y <= (navY + navH));
        if (!inNav) return -1;

        for (size_t i = 0; i < this->sectionTexts.size(); i++) {
            const int secY = this->sectionTexts[i]->GetProcessedY();
            const int hitTop = secY - 14;
            const int hitBottom = secY + 42;
            if ((y >= hitTop) && (y <= hitBottom)) {
                return static_cast<int>(i);
            }
        }

        int nearestIdx = -1;
        int nearestDist = 1 << 30;
        for (size_t i = 0; i < this->sectionTexts.size(); i++) {
            const int centerY = this->sectionTexts[i]->GetProcessedY() + 14;
            int dist = y - centerY;
            if (dist < 0) dist = -dist;
            if (dist < nearestDist) {
                nearestDist = dist;
                nearestIdx = static_cast<int>(i);
            }
        }
        if (nearestDist <= 90) return nearestIdx;
        return -1;
    }

    void optionsPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
        (void)Up;
        (void)Held;
        int bottomTapX = 0;
        if (DetectBottomHintTap(Pos, this->bottomHintTouch, 668, 52, bottomTapX)) {
            Down |= FindBottomHintButton(this->bottomHintSegments, bottomTapX);
        }
        inst::util::playNavigationClickIfNeeded(Down);
        if (this->themeListVisible) {
            this->handleThemeListInput(Down, Pos);
            return;
        }
        if (this->discoveryVisible) {
            this->updateDiscovery(Down);
            return;
        }
        if (this->remoteFormVisible) {
            if (this->remoteFormLanProtocolDropdownVisible) {
                if (Down & HidNpadButton_B) {
                    this->remoteFormLanProtocolDropdownVisible = false;
                    this->remoteFormLanProtocolPanel->SetVisible(false);
                    this->remoteFormLanProtocolMenu->SetVisible(false);
                } else if (Down & HidNpadButton_A) this->applyRemoteFormLanProtocol();
                return;
            }
            if (Down & HidNpadButton_B) {
                this->closeRemoteForm();
                return;
            }
            if (Down & HidNpadButton_A)
                this->editRemoteFormField();
            return;
        }
        if (Down & HidNpadButton_B) {
            if (this->remoteListVisible) {
                this->closeRemoteList();
                return;
            }
            mainApp->LoadLayout(mainApp->mainPage);
        }
        const bool leftPressed = (Down & (HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_StickRLeft)) != 0;
        const bool rightPressed = (Down & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight)) != 0;
        const bool upPressed = (Down & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) != 0;
        const bool downPressed = (Down & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)) != 0;

        if (!this->remoteListVisible && leftPressed && !this->tabsFocused) {
            this->tabsFocused = true;
            this->rememberCurrentSectionMenuIndex();
            this->lockedMenuIndex = this->menu->GetSelectedIndex();
            this->setSectionNavText();
        } else if (!this->remoteListVisible && rightPressed && this->tabsFocused) {
            this->tabsFocused = false;
            this->restoreSelectedSectionMenuIndex();
            this->setSectionNavText();
        }

        if (!this->remoteListVisible && (Down & HidNpadButton_L)) {
            this->tabsFocused = true;
            this->setSelectedSectionAndRefresh(this->selectedSection - 1);
        }
        if (!this->remoteListVisible && (Down & HidNpadButton_R)) {
            this->tabsFocused = true;
            this->setSelectedSectionAndRefresh(this->selectedSection + 1);
        }

        if (!this->remoteListVisible && this->tabsFocused) {
            if (upPressed && !downPressed) {
                this->setSelectedSectionAndRefresh(this->selectedSection - 1);
            } else if (downPressed) {
                this->setSelectedSectionAndRefresh(this->selectedSection + 1);
            }
            this->menu->SetSelectedIndex(this->lockedMenuIndex);
        } else {
            this->lockedMenuIndex = this->menu->GetSelectedIndex();
            this->rememberCurrentSectionMenuIndex();
        }

        bool touchSelect = false;
        if (!Pos.IsEmpty()) {
            if (!this->touchActive) {
                this->touchActive = true;
                this->touchMoved = false;
                this->touchStartX = Pos.X;
                this->touchStartY = Pos.Y;
                const bool inMenu = (Pos.X >= this->menu->GetProcessedX()) &&
                    (Pos.X <= (this->menu->GetProcessedX() + this->menu->GetWidth())) &&
                    (Pos.Y >= this->menu->GetProcessedY()) &&
                    (Pos.Y <= (this->menu->GetProcessedY() + this->menu->GetHeight()));
                const bool inSideNav = !this->remoteListVisible && this->getSectionFromTouch(Pos.X, Pos.Y) >= 0;
                this->touchRegion = inSideNav ? 1 : (inMenu ? 2 : 0);
            } else {
                int dx = Pos.X - this->touchStartX;
                int dy = Pos.Y - this->touchStartY;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx > 12 || dy > 12) this->touchMoved = true;
            }
        } else if (this->touchActive) {
            if (!this->touchMoved) {
                if (this->touchRegion == 1 && !this->remoteListVisible) {
                    this->tabsFocused = true;
                    int touchedSection = this->getSectionFromTouch(this->touchStartX, this->touchStartY);
                    if (touchedSection >= 0 && touchedSection != this->selectedSection) {
                        this->setSelectedSectionAndRefresh(touchedSection);
                    } else {
                        this->setSectionNavText();
                    }
                } else if (this->touchRegion == 2) {
                    this->tabsFocused = false;
                    this->restoreSelectedSectionMenuIndex();
                    this->setSectionNavText();
                    touchSelect = true;
                }
            }
            this->touchActive = false;
            this->touchMoved = false;
            this->touchRegion = 0;
        }

        bool tabAcceptOnly = false;
        if ((Down & HidNpadButton_A) && this->tabsFocused) {
            this->tabsFocused = false;
            this->restoreSelectedSectionMenuIndex();
            this->setSectionNavText();
            tabAcceptOnly = true;
        }

        if ((((Down & HidNpadButton_A) && !this->tabsFocused) && !tabAcceptOnly) || touchSelect) {
            if (this->remoteListVisible) {
                const int remoteIndex = this->menu->GetSelectedIndex();
                if (remoteIndex >= 0 && remoteIndex < static_cast<int>(this->remoteListProfiles.size()))
                    this->manageRemote(this->remoteListProfiles[remoteIndex]);
                return;
            }
            std::string keyboardResult;
            int rc;
            std::vector<std::string> downloadUrl;
            std::vector<std::string> languageList;
            int selectedIndex = this->menu->GetSelectedIndex();
            if (this->selectedSection == 0) {
                static const int kGeneralMap[] = {0, 1, 10, 2, 3, 6, 7, 8, 29};
                if ((selectedIndex < 0) || (selectedIndex >= static_cast<int>(sizeof(kGeneralMap) / sizeof(kGeneralMap[0])))) return;
                selectedIndex = kGeneralMap[selectedIndex];
            } else if (this->selectedSection == 1) {
                static const int kRemoteMap[] = {20, 21, 28, 25, 26, 12, 13, 27, 24, 19, 23, 22};
                if ((selectedIndex < 0) || (selectedIndex >= static_cast<int>(sizeof(kRemoteMap) / sizeof(kRemoteMap[0])))) return;
                selectedIndex = kRemoteMap[selectedIndex];
            } else {
                static const int kSystemMap[] = {4, 5, 16, 17, 18, 30};
                if ((selectedIndex < 0) || (selectedIndex >= static_cast<int>(sizeof(kSystemMap) / sizeof(kSystemMap[0])))) return;
                selectedIndex = kSystemMap[selectedIndex];
            }
            switch (selectedIndex) {
                case 30:
                    this->openThemeList();
                    break;
                case 29:
                    inst::config::usbHddShowHiddenFiles = !inst::config::usbHddShowHiddenFiles;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 28:
                    this->startDiscovery();
                    break;
                case 0:
                    inst::config::ignoreReqVers = !inst::config::ignoreReqVers;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 1:
                    if (inst::config::validateNCAs) {
                        if (inst::ui::mainApp->CreateShowDialog("options.nca_warn.title"_lang, "options.nca_warn.desc"_lang, {"common.cancel"_lang, "options.nca_warn.opt1"_lang}, false) == 1) inst::config::validateNCAs = false;
                    } else inst::config::validateNCAs = true;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 2:
                    inst::config::overClock = !inst::config::overClock;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 10:
                    inst::config::verboseInstallLogging = !inst::config::verboseInstallLogging;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 3:
                    inst::config::deletePrompt = !inst::config::deletePrompt;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 4:
                    inst::config::autoUpdate = !inst::config::autoUpdate;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 5:
                    if (inst::config::gayMode) {
                        inst::config::gayMode = false;
                        mainApp->mainPage->awooImage->SetVisible(true);
                        mainApp->instpage->awooImage->SetVisible(true);
                        mainApp->instpage->titleImage->SetX(0);
                        mainApp->instpage->appVersionText->SetX(480);
                        mainApp->mainPage->titleImage->SetX(0);
                        mainApp->mainPage->appVersionText->SetX(480);
                        mainApp->netinstPage->titleImage->SetX(0);
                        mainApp->netinstPage->appVersionText->SetX(480);
                        mainApp->remoteinstPage->titleImage->SetX(0);
                        mainApp->remoteinstPage->appVersionText->SetX(480);
                        mainApp->optionspage->titleImage->SetX(0);
                        mainApp->optionspage->appVersionText->SetX(480);
                        mainApp->sdinstPage->titleImage->SetX(0);
                        mainApp->sdinstPage->appVersionText->SetX(480);
                        mainApp->usbinstPage->titleImage->SetX(0);
                        mainApp->usbinstPage->appVersionText->SetX(480);
                    }
                    else {
                        inst::config::gayMode = true;
                        mainApp->mainPage->awooImage->SetVisible(false);
                        mainApp->instpage->awooImage->SetVisible(false);
                        mainApp->instpage->titleImage->SetX(-113);
                        mainApp->instpage->appVersionText->SetX(367);
                        mainApp->mainPage->titleImage->SetX(-113);
                        mainApp->mainPage->appVersionText->SetX(367);
                        mainApp->netinstPage->titleImage->SetX(-113);
                        mainApp->netinstPage->appVersionText->SetX(367);
                        mainApp->remoteinstPage->titleImage->SetX(-113);
                        mainApp->remoteinstPage->appVersionText->SetX(367);
                        mainApp->optionspage->titleImage->SetX(-113);
                        mainApp->optionspage->appVersionText->SetX(367);
                        mainApp->sdinstPage->titleImage->SetX(-113);
                        mainApp->sdinstPage->appVersionText->SetX(367);
                        mainApp->usbinstPage->titleImage->SetX(-113);
                        mainApp->usbinstPage->appVersionText->SetX(367);
                    }
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 6:
                    inst::config::soundEnabled = !inst::config::soundEnabled;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 7:
                    inst::config::oledMode = !inst::config::oledMode;
                    inst::config::setConfig();
                    {
                        auto keepAlive = mainApp->optionspage;
                        mainApp->RecreateLayouts();
                        mainApp->LoadLayout(mainApp->optionspage);
                    }
                    break;
                case 8:
                    inst::config::mtpExposeAlbum = !inst::config::mtpExposeAlbum;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 9: {
                    std::vector<inst::config::RemoteProfile> remotes = inst::config::LoadRemotes();
                    if (remotes.empty()) {
                        inst::ui::mainApp->CreateShowDialog("Active Remote", "No memorized Remotes found. Add one first.", {"common.ok"_lang}, true);
                        break;
                    }
                    std::vector<std::string> choices = BuildRemoteChoices(remotes);
                    int selectedRemote = inst::ui::mainApp->CreateShowDialog("Active Remote", "Choose which memorized Remote to use.", choices, false);
                    if (selectedRemote < 0 || selectedRemote >= static_cast<int>(remotes.size()))
                        break;
                    if (inst::config::SetActiveRemote(remotes[selectedRemote], true))
                        this->refreshOptions();
                    break;
                }
                case 20: {
                    std::vector<inst::config::RemoteProfile> remotes = inst::config::LoadRemotes();
                    if (remotes.empty()) {
                        inst::ui::mainApp->CreateShowDialog("Memorized Remotes", "No memorized Remotes found.", {"common.ok"_lang}, true);
                        break;
                    }
                    this->openRemoteList();
                    break;
                }
                case 21: {
                    inst::config::RemoteProfile newRemote;
                    this->openRemoteForm(newRemote, true, false);
                    break;
                }
                case 25: {
                    if (inst::config::remoteLegacyMode) {
                        inst::ui::mainApp->CreateShowDialog("User-Agent profile", "Locked to Tinfoil while Tinfoil Mode is enabled.", {"common.ok"_lang}, true);
                        break;
                    }

                    const std::vector<std::string> profiles = {
                        "Default (CyberFoil)",
                        "Tinfoil",
                        "Chrome (Windows)",
                        "Safari (iPhone)",
                        "Firefox (Windows)",
                        "Custom"
                    };
                    int currentIndex = GetUserAgentProfileChoiceIndex(inst::config::httpUserAgentMode);
                    if (currentIndex < 0 || currentIndex >= static_cast<int>(profiles.size()))
                        currentIndex = 0;
                    const std::string currentLabel = profiles[currentIndex];
                    int profileChoice = inst::ui::mainApp->CreateShowDialog(
                        "User-Agent profile",
                        "Used for file/media downloads. Remote API always uses CyberFoil.",
                        profiles,
                        false
                    );
                    if (profileChoice < 0 || profileChoice >= static_cast<int>(profiles.size()))
                        break;

                    std::string mode = GetUserAgentProfileModeFromChoice(profileChoice);
                    if (mode == "tinfoil") {
                        inst::config::httpUserAgent.clear();
                    } else if (mode == "custom") {
                        std::string customUserAgent = TrimString(inst::util::softwareKeyboard("Enter custom User-Agent", inst::config::httpUserAgent, 300));
                        if (customUserAgent.empty()) {
                            inst::ui::mainApp->CreateShowDialog("Invalid User-Agent", "Custom User-Agent cannot be empty.", {"common.ok"_lang}, true);
                            break;
                        }
                        inst::config::httpUserAgent = customUserAgent;
                    }

                    inst::config::httpUserAgentMode = mode;
                    inst::config::setConfig();
                    this->refreshOptions();

                    const std::string newLabel = GetUserAgentProfileLabel(inst::config::httpUserAgentMode);
                    if (newLabel != currentLabel) {
                        inst::ui::mainApp->CreateShowDialog(
                            "User-Agent profile",
                            "Current profile: " + newLabel,
                            {"common.ok"_lang},
                            true
                        );
                    }
                    break;
                }
                case 12:
                    inst::config::remoteHideInstalled = !inst::config::remoteHideInstalled;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 26:
                    inst::config::remoteLegacyMode = !inst::config::remoteLegacyMode;
                    if (inst::config::remoteLegacyMode) {
                        inst::config::httpUserAgentMode = "tinfoil";
                        inst::config::httpUserAgent.clear();
                    } else {
                        inst::config::httpUserAgentMode = "default";
                        inst::config::httpUserAgent.clear();
                    }
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 13:
                    inst::config::remoteHideInstalledSection = !inst::config::remoteHideInstalledSection;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 27:
                    inst::config::remoteHideIncompatibleCheats = !inst::config::remoteHideIncompatibleCheats;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 24:
                    inst::config::remoteAllBaseOnly = !inst::config::remoteAllBaseOnly;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 23:
                    inst::config::offlineDbAutoCheckOnStartup = !inst::config::offlineDbAutoCheckOnStartup;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 22: {
                    if (inst::util::getIPAddress() == "1.0.0.127") {
                        inst::ui::mainApp->CreateShowDialog("main.net.title"_lang, "main.net.desc"_lang, {"common.ok"_lang}, true);
                        break;
                    }

                    const std::string manifestUrl = inst::config::offlineDbManifestUrl;
                    if (manifestUrl.empty()) {
                        inst::ui::mainApp->CreateShowDialog("Offline DB update", "Manifest URL is empty. Set offlineDbManifestUrl in config.json.", {"common.ok"_lang}, true);
                        break;
                    }

                    const auto check = inst::offline::dbupdate::CheckForUpdate(manifestUrl);
                    if (!check.success) {
                        inst::ui::mainApp->CreateShowDialog("Offline DB update", "Check failed:\n" + check.error, {"common.ok"_lang}, true);
                        break;
                    }

                    if (!check.updateAvailable) {
                        std::string body = "Offline DB is up to date.";
                        if (!check.remoteVersion.empty())
                            body += "\nVersion: " + check.remoteVersion;
                        inst::ui::mainApp->CreateShowDialog("Offline DB update", body, {"common.ok"_lang}, true);
                        break;
                    }

                    std::string prompt = "A new offline DB is available.";
                    if (!check.localVersion.empty())
                        prompt += "\nCurrent: " + check.localVersion;
                    if (!check.remoteVersion.empty())
                        prompt += "\nLatest: " + check.remoteVersion;
                    prompt += "\n\nDownload and install now?";
                    int applyNow = inst::ui::mainApp->CreateShowDialog("Offline DB update", prompt, {"Update", "common.cancel"_lang}, false);
                    if (applyNow != 0)
                        break;

                    inst::ui::instPage::loadInstallScreen();
                    inst::ui::instPage::setTopInstInfoText("Updating Offline DB");
                    inst::ui::instPage::setInstInfoText("Preparing...");
                    inst::ui::instPage::setInstBarPerc(0);
                    const auto apply = inst::offline::dbupdate::ApplyUpdate(manifestUrl, false,
                        [](const std::string& stage, double percent) {
                            inst::ui::instPage::setInstInfoText(stage);
                            inst::ui::instPage::setInstBarPerc(percent);
                        });
                    mainApp->LoadLayout(mainApp->optionspage);

                    if (!apply.success) {
                        inst::ui::mainApp->CreateShowDialog("Offline DB update", "Update failed:\n" + apply.error, {"common.ok"_lang}, true);
                        break;
                    }

                    this->refreshOptions();
                    std::string done = apply.updated ? "Offline DB updated successfully." : "Offline DB is already up to date.";
                    if (!apply.version.empty())
                        done += "\nVersion: " + apply.version;
                    inst::ui::mainApp->CreateShowDialog("Offline DB update", done, {"common.ok"_lang}, true);
                    break;
                }
                case 19:
                    inst::config::remoteStartGridMode = !inst::config::remoteStartGridMode;
                    inst::config::setConfig();
                    this->refreshOptions();
                    break;
                case 16:
                    languageList = languageStrings;
                    languageList.push_back("options.language.system_language"_lang);
                    rc = inst::ui::mainApp->CreateShowDialog("options.language.title"_lang, "options.language.desc"_lang, languageList, false);
                    if (rc == -1) break;
                    switch(rc) {
                        case 0:
                            inst::config::languageSetting = 1;
                            break;
                        case 1:
                            inst::config::languageSetting = 0;
                            break;
                        case 2:
                            inst::config::languageSetting = 2;
                            break;
                        case 3:
                            inst::config::languageSetting = 3;
                            break;
                        case 4:
                            inst::config::languageSetting = 4;
                            break;
                        case 5:
                            inst::config::languageSetting = 14;
                            break;
                        case 6:
                            inst::config::languageSetting = 9;
                            break;
                        case 7:
                            inst::config::languageSetting = 7;
                            break;
                        case 8:
                            inst::config::languageSetting = 10;
                            break;
                        case 9:
                            inst::config::languageSetting = 6;
                            break;
                        case 10:
                            inst::config::languageSetting = 11;
                            break;
                        default:
                            inst::config::languageSetting = 99;
                    }
                    inst::config::setConfig();
                    mainApp->FadeOut();
                    mainApp->Close();
                    break;
                case 17:
                    if (inst::util::getIPAddress() == "1.0.0.127") {
                        inst::ui::mainApp->CreateShowDialog("main.net.title"_lang, "main.net.desc"_lang, {"common.ok"_lang}, true);
                        break;
                    }
                    downloadUrl = inst::util::checkForAppUpdate();
                    if (!downloadUrl.size()) {
                        mainApp->CreateShowDialog("options.update.title_check_fail"_lang, "options.update.desc_check_fail"_lang, {"common.ok"_lang}, false);
                        break;
                    }
                    this->askToUpdate(downloadUrl);
                    break;
                case 18:
                    inst::ui::mainApp->CreateShowDialog("options.credits.title"_lang, "options.credits.desc"_lang, {"common.close"_lang}, true);
                    break;
                default:
                    break;
            }
        }
    }
}
