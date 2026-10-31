#include <filesystem>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <switch.h>
#include "ui/MainApplication.hpp"
#include "ui/mainPage.hpp"
#include "ui/instPage.hpp"
#include "util/util.hpp"
#include "util/config.hpp"
#include "util/offline_db_update.hpp"
#include "util/save_sync.hpp"
#include "util/error.hpp"
#include "util/lang.hpp"
#include "data/buffered_placeholder_writer.hpp"
#include "mtp_server.hpp"
#include "nx/usbhdd.h"
#include "ui/bottomHint.hpp"

#include "util/theme.hpp"

#define COLOR(hex) inst::theme::Palette(hex)

namespace inst::ui {
    extern MainApplication *mainApp;
    bool appletFinished = false;
    bool updateFinished = false;
    bool offlineDbUpdateCheckFinished = false;
    constexpr int kMainGridCols = 3;
    constexpr int kMainGridRows = 3;
    constexpr int kMainGridTileWidth = 360;
    constexpr int kMainGridTileHeight = 170;
    constexpr int kMainGridGapX = 20;
    constexpr int kMainGridGapY = 18;
    constexpr int kMainGridStartX = (1280 - ((kMainGridCols * kMainGridTileWidth) + ((kMainGridCols - 1) * kMainGridGapX))) / 2;
    constexpr int kMainGridStartY = 100;
    constexpr int kMainLabelPaddingX = 18;
    constexpr int kMainLabelY = 116;
    constexpr int kMainLabelWidth = kMainGridTileWidth - (kMainLabelPaddingX * 2);
    constexpr int kMainLabelHeight = 28;

    struct BackupUserChoice {
        AccountUid uid = {};
        std::string label;
        bool isPreferred = false;
    };

    bool AccountUidEqualsLocal(const AccountUid& a, const AccountUid& b)
    {
        return a.uid[0] == b.uid[0] && a.uid[1] == b.uid[1];
    }

    std::string GetAccountNickname(AccountUid uid)
    {
        AccountProfile profile = {};
        if (R_FAILED(accountGetProfile(&profile, uid)))
            return std::string();

        AccountProfileBase profileBase = {};
        const bool ok = R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profileBase));
        accountProfileClose(&profile);
        if (!ok)
            return std::string();

        const std::size_t len = strnlen(profileBase.nickname, sizeof(profileBase.nickname));
        if (len == 0)
            return std::string();
        return std::string(profileBase.nickname, len);
    }

    bool ListBackupUsers(std::vector<BackupUserChoice>& outUsers, std::string& error)
    {
        outUsers.clear();
        error.clear();

        Result rc = accountInitialize(AccountServiceType_Application);
        if (R_FAILED(rc)) {
            char code[16] = {0};
            std::snprintf(code, sizeof(code), "0x%08X", rc);
            error = "Failed to initialize account service (" + std::string(code) + ").";
            return false;
        }

        AccountUid preferredUid = {};
        bool preferredSet = false;
        if (R_SUCCEEDED(accountGetPreselectedUser(&preferredUid)) && accountUidIsValid(&preferredUid))
            preferredSet = true;
        else if (R_SUCCEEDED(accountGetLastOpenedUser(&preferredUid)) && accountUidIsValid(&preferredUid))
            preferredSet = true;

        AccountUid users[ACC_USER_LIST_SIZE] = {};
        s32 total = 0;
        if (R_FAILED(accountListAllUsers(users, ACC_USER_LIST_SIZE, &total)) || total <= 0) {
            accountExit();
            error = "No user accounts are available.";
            return false;
        }

        int displayIndex = 1;
        for (s32 i = 0; i < total; i++) {
            if (!accountUidIsValid(&users[i]))
                continue;
            BackupUserChoice choice;
            choice.uid = users[i];
            choice.isPreferred = preferredSet && AccountUidEqualsLocal(preferredUid, users[i]);
            std::string nickname = GetAccountNickname(users[i]);
            if (nickname.empty())
                nickname = "User " + std::to_string(displayIndex);
            choice.label = nickname;
            if (choice.isPreferred)
                choice.label += " [active]";
            outUsers.push_back(std::move(choice));
            displayIndex++;
        }

        accountExit();
        if (outUsers.empty()) {
            error = "No valid user accounts were found.";
            return false;
        }
        return true;
    }

    std::string WrapGridLabelText(const std::string& text, int maxWidth, int fontSize, int maxLines)
    {
        if (text.empty() || maxWidth <= 0 || maxLines <= 0)
            return text;

        auto measure = pu::ui::elm::TextBlock::New(0, 0, "", fontSize);
        std::stringstream words(text);
        std::string word;
        std::vector<std::string> lines;
        std::string line;

        while (words >> word) {
            std::string candidate = line.empty() ? word : (line + " " + word);
            measure->SetText(candidate);
            if (measure->GetTextWidth() <= maxWidth) {
                line = candidate;
                continue;
            }

            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
                if (static_cast<int>(lines.size()) >= maxLines)
                    break;
            }

            measure->SetText(word);
            if (measure->GetTextWidth() <= maxWidth) {
                line = word;
                continue;
            }

            std::string trimmed = word;
            while (!trimmed.empty()) {
                std::string candidateToken = trimmed + "...";
                measure->SetText(candidateToken);
                if (measure->GetTextWidth() <= maxWidth)
                    break;
                trimmed.pop_back();
            }
            line = trimmed.empty() ? "..." : (trimmed + "...");
            lines.push_back(line);
            line.clear();
            break;
        }

        if (!line.empty() && static_cast<int>(lines.size()) < maxLines)
            lines.push_back(line);

        if (lines.empty())
            lines.push_back(text);

        if (static_cast<int>(lines.size()) > maxLines)
            lines.resize(maxLines);

        if (static_cast<int>(lines.size()) == maxLines) {
            std::string& last = lines.back();
            measure->SetText(last);
            if (measure->GetTextWidth() > maxWidth) {
                std::string trimmed = last;
                while (!trimmed.empty()) {
                    std::string candidateToken = trimmed + "...";
                    measure->SetText(candidateToken);
                    if (measure->GetTextWidth() <= maxWidth)
                        break;
                    trimmed.pop_back();
                }
                last = trimmed.empty() ? "..." : (trimmed + "...");
            }
        }

        std::string wrapped;
        for (std::size_t i = 0; i < lines.size(); i++) {
            if (i > 0)
                wrapped += "\n";
            wrapped += lines[i];
        }
        return wrapped;
    }

    void mainMenuThread() {
        bool menuLoaded = mainApp->IsShown();
        if (!appletFinished && appletGetAppletType() == AppletType_LibraryApplet) {
            tin::data::NUM_BUFFER_SEGMENTS = 2;
            if (menuLoaded) {
                inst::ui::appletFinished = true;
                mainApp->CreateShowDialog("main.applet.title"_lang, "main.applet.desc"_lang, {"common.ok"_lang}, true);
            } 
        } else if (!appletFinished) {
            inst::ui::appletFinished = true;
            tin::data::NUM_BUFFER_SEGMENTS = 128;
        }
        if (!updateFinished && (!inst::config::autoUpdate || inst::util::getIPAddress() == "1.0.0.127")) updateFinished = true;
        if (!updateFinished && menuLoaded && inst::config::updateInfo.size()) {
            updateFinished = true;
            optionsPage::askToUpdate(inst::config::updateInfo);
        }
        if (!offlineDbUpdateCheckFinished && (!inst::config::offlineDbAutoCheckOnStartup || inst::util::getIPAddress() == "1.0.0.127"))
            offlineDbUpdateCheckFinished = true;
        if (!offlineDbUpdateCheckFinished && menuLoaded) {
            inst::offline::dbupdate::CheckResult checkResult;
            if (inst::offline::dbupdate::TryGetStartupCheckResult(checkResult)) {
                offlineDbUpdateCheckFinished = true;
                if (!checkResult.success) {
                    LOG_DEBUG("Offline DB startup check failed: %s\n", checkResult.error.c_str());
                } else if (checkResult.updateAvailable) {
                    std::string prompt = "A new Offline DB update is available.";
                    if (!checkResult.localVersion.empty())
                        prompt += "\nCurrent: " + checkResult.localVersion;
                    if (!checkResult.remoteVersion.empty())
                        prompt += "\nLatest: " + checkResult.remoteVersion;
                    prompt += "\n\nInstall now?";
                    int applyNow = mainApp->CreateShowDialog("Offline DB update", prompt, {"Update", "Later"}, false);
                    if (applyNow == 0) {
                        inst::ui::instPage::loadInstallScreen();
                        inst::ui::instPage::setTopInstInfoText("Updating Offline DB");
                        inst::ui::instPage::setInstInfoText("Preparing...");
                        inst::ui::instPage::setInstBarPerc(0);
                        const auto apply = inst::offline::dbupdate::ApplyUpdate(inst::config::offlineDbManifestUrl, false,
                            [](const std::string& stage, double percent) {
                                inst::ui::instPage::setInstInfoText(stage);
                                inst::ui::instPage::setInstBarPerc(percent);
                            });
                        mainApp->LoadLayout(mainApp->mainPage);
                        if (!apply.success) {
                            mainApp->CreateShowDialog("Offline DB update", "Update failed:\n" + apply.error, {"common.ok"_lang}, true);
                        } else {
                            std::string done = apply.updated ? "Offline DB updated successfully." : "Offline DB is already up to date.";
                            if (!apply.version.empty())
                                done += "\nVersion: " + apply.version;
                            mainApp->CreateShowDialog("Offline DB update", done, {"common.ok"_lang}, true);
                        }
                    }
                }
            }
        }
    }

    MainPage::MainPage() : Layout::Layout() {
        inst::theme::ApplyBackground(*this, inst::config::oledMode, inst::config::appDir);
        const auto topColor = inst::theme::Color("header", inst::config::oledMode ? COLOR("#000000FF") : COLOR("#170909FF"));
        const auto botColor = inst::theme::Color("footer", inst::config::oledMode ? COLOR("#000000FF") : COLOR("#17090980"));
        this->topRect = Rectangle::New(0, 0, 1280, 74, topColor);
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
        const std::string mainButtonsText = "main.buttons"_lang + "     " + "main.info.button"_lang;
        this->butText = TextBlock::New(10, 678, mainButtonsText, 20);
        this->butText->SetColor(COLOR("#FFFFFFFF"));
        this->bottomHintSegments = BuildBottomHintSegments(mainButtonsText, 10, 20);
        this->backupUserPickerRect = Rectangle::New(196, 102, 888, 516, inst::config::oledMode ? COLOR("#000000EE") : COLOR("#170909EE"));
        this->backupUserPickerRect->SetVisible(false);
        this->backupUserPickerTitle = TextBlock::New(222, 124, "Select user account to back up", 24);
        this->backupUserPickerTitle->SetColor(COLOR("#FFFFFFFF"));
        this->backupUserPickerTitle->SetVisible(false);
        this->optionMenu = inst::theme::Menu::New(222, 168, 836, COLOR("#FFFFFF00"), 50, 7, 22);
        if (inst::config::oledMode) {
            this->optionMenu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#FFFFFF33")));
            this->optionMenu->SetScrollbarColor(inst::theme::Color("scrollbar", COLOR("#FFFFFF66")));
        } else {
            this->optionMenu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#00000033")));
            this->optionMenu->SetScrollbarColor(inst::theme::Color("scrollbar", COLOR("#17090980")));
        }
        this->optionMenu->SetVisible(false);
        this->backupUserPickerHint = TextBlock::New(222, 584, "A Select    B Cancel", 18);
        this->backupUserPickerHint->SetColor(COLOR("#FFFFFFFF"));
        this->backupUserPickerHint->SetVisible(false);
        this->installMenuItem = pu::ui::elm::MenuItem::New("main.menu.sd"_lang);
        this->installMenuItem->SetColor(COLOR("#FFFFFFFF"));
        this->installMenuItem->SetIcon(inst::theme::Asset("romfs:/images/icons/micro-sd.png"));
        this->netInstallMenuItem = pu::ui::elm::MenuItem::New("main.menu.net"_lang);
        this->netInstallMenuItem->SetColor(COLOR("#FFFFFFFF"));
        this->netInstallMenuItem->SetIcon(inst::theme::Asset("romfs:/images/icons/cloud-download.png"));
        this->remoteInstallMenuItem = pu::ui::elm::MenuItem::New("main.menu.remote"_lang);
        this->remoteInstallMenuItem->SetColor(COLOR("#FFFFFFFF"));
        this->remoteInstallMenuItem->SetIcon(inst::theme::Asset("romfs:/images/icons/remote.png"));
        this->usbInstallMenuItem = pu::ui::elm::MenuItem::New("main.menu.usb"_lang);
        this->usbInstallMenuItem->SetColor(COLOR("#FFFFFFFF"));
        this->usbInstallMenuItem->SetIcon(inst::theme::Asset("romfs:/images/icons/usb-port.png"));
        this->hddInstallMenuItem = pu::ui::elm::MenuItem::New("main.menu.hdd"_lang);
        this->hddInstallMenuItem->SetColor(COLOR("#FFFFFFFF"));
        this->hddInstallMenuItem->SetIcon(inst::theme::Asset("romfs:/images/icons/usb-install.png"));
        this->mtpInstallMenuItem = pu::ui::elm::MenuItem::New("main.menu.mtp"_lang);
        this->mtpInstallMenuItem->SetColor(COLOR("#FFFFFFFF"));
        this->mtpInstallMenuItem->SetIcon(inst::theme::Asset("romfs:/images/icons/usb-port.png"));
        this->backupSaveDataMenuItem = pu::ui::elm::MenuItem::New("main.menu.backup"_lang);
        this->backupSaveDataMenuItem->SetColor(COLOR("#FFFFFFFF"));
        this->backupSaveDataMenuItem->SetIcon(inst::theme::Asset("romfs:/images/icons/save-data-cloud.png"));
        this->settingsMenuItem = pu::ui::elm::MenuItem::New("main.menu.set"_lang);
        this->settingsMenuItem->SetColor(COLOR("#FFFFFFFF"));
        this->settingsMenuItem->SetIcon(inst::theme::Asset("romfs:/images/icons/settings.png"));
        this->exitMenuItem = pu::ui::elm::MenuItem::New("main.menu.exit"_lang);
        this->exitMenuItem->SetColor(COLOR("#FFFFFFFF"));
        this->exitMenuItem->SetIcon(inst::theme::Asset("romfs:/images/icons/exit-run.png"));
        const auto tileColor = inst::theme::Color("panel", inst::config::oledMode ? COLOR("#1A1A1ACC") : COLOR("#170909CC"));
        const auto highlightColor = inst::theme::Color("highlight", inst::config::oledMode ? COLOR("#FF4D4D66") : COLOR("#FF4D4D88"));
        const std::vector<std::string> gridLabels = {
            "main.menu.remote"_lang,
            "main.menu.sd"_lang,
            "main.menu.hdd"_lang,
            "main.menu.mtp"_lang,
            "main.menu.usb"_lang,
            "main.menu.net"_lang,
            "main.menu.backup"_lang,
            "main.menu.set"_lang,
            "main.menu.exit"_lang
        };
        const std::vector<std::string> gridIcons = {
            inst::theme::Asset("romfs:/images/icons/remote.png"),
            inst::theme::Asset("romfs:/images/icons/micro-sd.png"),
            inst::theme::Asset("romfs:/images/icons/usb-install.png"),
            inst::theme::Asset("romfs:/images/icons/usb-port.png"),
            inst::theme::Asset("romfs:/images/icons/usb-port.png"),
            inst::theme::Asset("romfs:/images/icons/cloud-download.png"),
            inst::theme::Asset("romfs:/images/icons/save-data-cloud.png"),
            inst::theme::Asset("romfs:/images/icons/settings.png"),
            inst::theme::Asset("romfs:/images/icons/exit-run.png")
        };
        this->mainGridTiles.reserve(kMainGridCols * kMainGridRows);
        this->mainGridIcons.reserve(kMainGridCols * kMainGridRows);
        this->mainGridLabels.reserve(kMainGridCols * kMainGridRows);
        for (int i = 0; i < (kMainGridCols * kMainGridRows); i++) {
            const int col = i % kMainGridCols;
            const int row = i / kMainGridCols;
            const int x = kMainGridStartX + (col * (kMainGridTileWidth + kMainGridGapX));
            const int y = kMainGridStartY + (row * (kMainGridTileHeight + kMainGridGapY));
            auto tile = Rectangle::New(x, y, kMainGridTileWidth, kMainGridTileHeight, tileColor, 18);
            constexpr int kMainIconSize = 96;
            auto icon = Image::New(x + ((kMainGridTileWidth - kMainIconSize) / 2), y + 22, gridIcons[i]);
            icon->SetWidth(kMainIconSize);
            icon->SetHeight(kMainIconSize);
            auto label = OverflowText::New(22, COLOR("#FFFFFFFF"));
            label->SetBounds(x + kMainLabelPaddingX, y + kMainLabelY, kMainLabelWidth, kMainLabelHeight);
            label->SetBackgroundColor(tileColor);
            label->SetText(gridLabels[i]);
            this->mainGridTiles.push_back(tile);
            this->mainGridIcons.push_back(icon);
            this->mainGridLabels.push_back(label);
        }
        this->mainGridHighlight = Rectangle::New(0, 0, kMainGridTileWidth + 8, kMainGridTileHeight + 8, highlightColor, 20);
        // A theme's own mascot (or an empty path hiding it) wins over the legacy override file.
        if (!inst::theme::HasImage("romfs:/images/awoos/5bbdbcf9a5625cd307c9e9bc360d78bd.png") &&
            std::filesystem::exists(inst::config::appDir + "/awoo_main.png")) this->awooImage = Image::New(410, 190, inst::config::appDir + "/awoo_main.png");
        else this->awooImage = Image::New(410, 190, inst::theme::Asset("romfs:/images/awoos/5bbdbcf9a5625cd307c9e9bc360d78bd.png"));
        this->Add(this->awooImage);
        this->Add(this->topRect);
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
        this->Add(this->butText);
        for (auto& tile : this->mainGridTiles)
            this->Add(tile);
        // Selection fills the tile behind its contents, even with opaque theme colors.
        this->Add(this->mainGridHighlight);
        for (auto& icon : this->mainGridIcons)
            this->Add(icon);
        for (auto& label : this->mainGridLabels)
            label->Attach(this);
        this->Add(this->backupUserPickerRect);
        this->Add(this->backupUserPickerTitle);
        this->Add(this->optionMenu);
        this->Add(this->backupUserPickerHint);
        this->awooImage->SetVisible(!inst::config::gayMode);
        this->updateMainGridSelection();
        this->AddThread(mainMenuThread);
        this->AddThread([this]() {
            this->updateMainGridLabelEffects(false);
        });
    }

    void MainPage::installMenuItem_Click() {
        mainApp->sdinstPage->drawMenuItems(true, "sdmc:/");
        mainApp->sdinstPage->menu->SetSelectedIndex(0);
        mainApp->LoadLayout(mainApp->sdinstPage);
    }

    void MainPage::netInstallMenuItem_Click() {
        if (inst::util::getIPAddress() == "1.0.0.127") {
            inst::ui::mainApp->CreateShowDialog("main.net.title"_lang, "main.net.desc"_lang, {"common.ok"_lang}, true);
            return;
        }
        mainApp->netinstPage->startNetwork();
    }

    void MainPage::remoteInstallMenuItem_Click() {
        if (inst::util::getIPAddress() == "1.0.0.127") {
            inst::ui::mainApp->CreateShowDialog("main.net.title"_lang, "main.net.desc"_lang, {"common.ok"_lang}, true);
            return;
        }
        mainApp->remoteinstPage->startRemote();
    }

    void MainPage::usbInstallMenuItem_Click() {
        if (!inst::config::usbAck) {
            if (mainApp->CreateShowDialog("main.usb.warn.title"_lang, "main.usb.warn.desc"_lang, {"common.ok"_lang, "main.usb.warn.opt1"_lang}, false) == 1) {
                inst::config::usbAck = true;
                inst::config::setConfig();
            }
        }
        if (inst::util::usbIsConnected()) mainApp->usbinstPage->startUsb();
        else mainApp->CreateShowDialog("main.usb.error.title"_lang, "main.usb.error.desc"_lang, {"common.ok"_lang}, false);
    }

    void MainPage::hddInstallMenuItem_Click() {
        if (nx::hdd::count() && nx::hdd::rootPath()) {
            mainApp->hddinstPage->drawMenuItems(true, nx::hdd::rootPath());
            mainApp->hddinstPage->menu->SetSelectedIndex(0);
            mainApp->LoadLayout(mainApp->hddinstPage);
        } else {
            mainApp->CreateShowDialog("main.hdd.title"_lang, "main.hdd.notfound"_lang, {"common.ok"_lang}, true);
        }
    }

    void MainPage::mtpInstallMenuItem_Click() {
        int dialogResult = mainApp->CreateShowDialog("inst.mtp.target.title"_lang, "inst.mtp.target.desc"_lang, {"inst.target.opt0"_lang, "inst.target.opt1"_lang}, false);
        if (dialogResult == -1) return;

        if (!inst::mtp::StartInstallServer(dialogResult)) {
            mainApp->CreateShowDialog("inst.mtp.error.title"_lang, "inst.mtp.error.desc"_lang, {"common.ok"_lang}, true);
            return;
        }

        inst::ui::instPage::loadInstallScreen();
        inst::ui::instPage::setTopInstInfoText("inst.mtp.waiting.title"_lang);
        inst::ui::instPage::setInstInfoText("inst.mtp.waiting.desc"_lang);
    }

    void MainPage::backupSaveDataMenuItem_Click() {
        if (inst::config::remoteLegacyMode) {
            mainApp->CreateShowDialog(
                "main.menu.backup"_lang,
                "Save data backups are disabled while Tinfoil Mode is enabled.",
                {"common.ok"_lang},
                true
            );
            return;
        }

        if (inst::util::getIPAddress() == "1.0.0.127") {
            inst::ui::mainApp->CreateShowDialog("main.net.title"_lang, "main.net.desc"_lang, {"common.ok"_lang}, true);
            return;
        }

        std::vector<BackupUserChoice> users;
        std::string userError;
        if (!ListBackupUsers(users, userError)) {
            // Device saves do not require an account to be present.
            users.push_back({{}, "Device (shared saves)", true});
        }

        std::vector<std::string> userLabels;
        userLabels.reserve(users.size());
        int preferredUserIndex = 0;
        for (std::size_t i = 0; i < users.size(); i++) {
            userLabels.push_back(users[i].label);
            if (users[i].isPreferred)
                preferredUserIndex = static_cast<int>(i);
        }

        const int selectedUserIndex = this->promptBackupUserSelection(userLabels, preferredUserIndex);
        if (selectedUserIndex < 0 || selectedUserIndex >= static_cast<int>(users.size()))
            return;
        const AccountUid selectedUid = users[static_cast<std::size_t>(selectedUserIndex)].uid;

        std::string remoteUrl = inst::config::remoteUrl;
        if (remoteUrl.empty()) {
            std::vector<inst::config::RemoteProfile> remotes = inst::config::LoadRemotes();
            if (!remotes.empty() && inst::config::SetActiveRemote(remotes.front(), true))
                remoteUrl = inst::config::remoteUrl;
        }
        if (remoteUrl.empty()) {
            remoteUrl = inst::util::softwareKeyboard("options.remote.url_hint"_lang, "http://", 200);
            if (remoteUrl.empty())
                return;
            inst::config::remoteUrl = remoteUrl;
            inst::config::setConfig();
        }

        std::string backupNote = inst::util::softwareKeyboard("Backup note (required)", "", 120);
        auto trimAscii = [](const std::string& value) {
            const std::size_t start = value.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                return std::string();
            const std::size_t end = value.find_last_not_of(" \t\r\n");
            return value.substr(start, (end - start) + 1);
        };
        backupNote = trimAscii(backupNote);
        if (backupNote.empty()) {
            mainApp->CreateShowDialog("main.menu.backup"_lang, "Backup canceled. A note is required.", {"common.ok"_lang}, true);
            return;
        }

        inst::ui::instPage::loadInstallScreen();
        inst::ui::instPage::setTopInstInfoText("main.menu.backup"_lang);
        inst::ui::instPage::setInstInfoText("Loading backup data...");
        inst::ui::instPage::setProgressDetailText("Fetching server save metadata...");
        inst::ui::instPage::setInstBarPerc(10);

        std::vector<remoteInstStuff::RemoteItem> remoteSaveItems;
        std::string remoteFetchWarning;
        inst::save_sync::FetchRemoteSaveItems(remoteUrl, inst::config::remoteUser, inst::config::remotePass, remoteSaveItems, remoteFetchWarning);

        inst::ui::instPage::setInstInfoText("Scanning local saves...");
        inst::ui::instPage::setProgressDetailText("Reading user and shared device saves...");
        inst::ui::instPage::setInstBarPerc(45);

        std::vector<inst::save_sync::SaveSyncEntry> entries;
        std::string buildWarning;
        inst::save_sync::BuildEntriesForUser(remoteSaveItems, &selectedUid, entries, buildWarning);

        inst::ui::instPage::setInstInfoText("Preparing upload list...");
        inst::ui::instPage::setProgressDetailText("Filtering local save entries...");
        inst::ui::instPage::setInstBarPerc(75);

        std::vector<const inst::save_sync::SaveSyncEntry*> localEntries;
        localEntries.reserve(entries.size());
        for (const auto& entry : entries) {
            if (entry.localAvailable)
                localEntries.push_back(&entry);
        }
        inst::ui::instPage::setInstBarPerc(100);
        inst::ui::instPage::setProgressDetailText("Ready.");

        if (localEntries.empty()) {
            inst::ui::instPage::clearProgressDetailText();
            mainApp->LoadLayout(mainApp->mainPage);
            mainApp->CreateShowDialog(
                "main.menu.backup"_lang,
                "No local user or device save data found.\nLaunch games once to create save data, then retry.",
                {"common.ok"_lang},
                true);
            return;
        }

        std::string remoteDisplayName;
        std::vector<inst::config::RemoteProfile> remotes = inst::config::LoadRemotes();
        for (const auto& remote : remotes) {
            if (inst::config::BuildRemoteUrl(remote) == remoteUrl &&
                remote.username == inst::config::remoteUser &&
                remote.password == inst::config::remotePass) {
                remoteDisplayName = remote.title;
                break;
            }
        }
        if (remoteDisplayName.empty())
            remoteDisplayName = remoteUrl;

        inst::ui::instPage::clearProgressDetailText();
        mainApp->LoadLayout(mainApp->mainPage);
        const int confirm = mainApp->CreateShowDialog(
            "main.menu.backup"_lang,
            "Upload all local saves to the server now?\n"
            "Remote: " + inst::util::shortenString(remoteDisplayName, 86, false) + "\n"
            "Saves to upload: " + std::to_string(localEntries.size()) + "\nIncludes shared device saves.",
            {"Upload All", "common.cancel"_lang},
            false);
        if (confirm != 0)
            return;

        inst::ui::instPage::loadInstallScreen();
        inst::ui::instPage::setTopInstInfoText("main.menu.backup"_lang);
        inst::ui::instPage::setInstInfoText("Preparing uploads...");
        inst::ui::instPage::setInstBarPerc(0);
        inst::ui::instPage::setProgressDetailText("0/" + std::to_string(localEntries.size()));

        std::size_t uploadedCount = 0;
        std::vector<std::string> failedTitles;
        std::string firstError;
        for (std::size_t i = 0; i < localEntries.size(); i++) {
            const auto* entry = localEntries[i];
            const std::size_t current = i + 1;
            const int progress = static_cast<int>((current * 100) / localEntries.size());
            const std::string titleName = (entry->titleName.empty() ? "Unknown title" : entry->titleName) +
                (entry->deviceSave ? " [Device]" : "");
            inst::ui::instPage::setInstallIconFromTitleId(entry->titleId);
            inst::ui::instPage::setInstInfoText("Uploading " + std::to_string(current) + "/" + std::to_string(localEntries.size()) + ": " + inst::util::shortenString(titleName, 64, false));
            inst::ui::instPage::setProgressDetailText(inst::util::shortenString(titleName, 68, false));
            inst::ui::instPage::setInstBarPerc(progress);

            std::string error;
            if (inst::save_sync::UploadSaveToServerForUser(remoteUrl, inst::config::remoteUser, inst::config::remotePass, &selectedUid, *entry, backupNote, error)) {
                uploadedCount++;
                continue;
            }

            failedTitles.push_back(entry->titleName.empty() ? "Unknown title" : entry->titleName);
            if (firstError.empty())
                firstError = error;
        }

        std::string summary = "Uploaded " + std::to_string(uploadedCount) + " of " + std::to_string(localEntries.size()) + " saves.";
        if (!failedTitles.empty()) {
            summary += "\nFailed: " + std::to_string(failedTitles.size());
            summary += "\nFirst failure: " + (firstError.empty() ? "Save upload failed." : firstError);
            summary += "\nExamples:";
            const std::size_t maxList = failedTitles.size() < 3 ? failedTitles.size() : 3;
            for (std::size_t i = 0; i < maxList; i++)
                summary += "\n- " + inst::util::shortenString(failedTitles[i], 56, false);
        }
        if (uploadedCount > 0) {
            const std::string warning = !buildWarning.empty() ? buildWarning : remoteFetchWarning;
            if (!warning.empty())
                summary += "\nWarning: " + warning;
        }
        inst::ui::instPage::setInstBarPerc(100);
        inst::ui::instPage::setInstInfoText("Backup complete.");
        inst::ui::instPage::clearProgressDetailText();
        inst::ui::instPage::clearInstallIcon();
        mainApp->LoadLayout(mainApp->mainPage);
        mainApp->CreateShowDialog("main.menu.backup"_lang, summary, {"common.ok"_lang}, true);
    }

    void MainPage::exitMenuItem_Click() {
        mainApp->FadeOut();
        mainApp->Close();
    }

    void MainPage::settingsMenuItem_Click() {
        mainApp->LoadLayout(mainApp->optionspage);
    }

    void MainPage::updateMainGridSelection() {
        if (this->selectedMainIndex < 0)
            this->selectedMainIndex = 0;
        const int maxIndex = (kMainGridCols * kMainGridRows) - 1;
        if (this->selectedMainIndex > maxIndex)
            this->selectedMainIndex = maxIndex;
        const int col = this->selectedMainIndex % kMainGridCols;
        const int row = this->selectedMainIndex / kMainGridCols;
        const int x = kMainGridStartX + (col * (kMainGridTileWidth + kMainGridGapX));
        const int y = kMainGridStartY + (row * (kMainGridTileHeight + kMainGridGapY));
        this->mainGridHighlight->SetX(x - 4);
        this->mainGridHighlight->SetY(y - 4);
        this->updateMainGridLabelEffects(true);
    }

    void MainPage::updateMainGridLabelEffects(bool force) {
        for (std::size_t i = 0; i < this->mainGridLabels.size(); i++) {
            const bool selected = (static_cast<int>(i) == this->selectedMainIndex);
            this->mainGridLabels[i]->SetSelected(selected, force);
            this->mainGridLabels[i]->Update(force);
        }
    }

    int MainPage::getMainGridIndexFromTouch(int x, int y) const {
        for (int i = 0; i < (kMainGridCols * kMainGridRows); i++) {
            const int col = i % kMainGridCols;
            const int row = i / kMainGridCols;
            const int tx = kMainGridStartX + (col * (kMainGridTileWidth + kMainGridGapX));
            const int ty = kMainGridStartY + (row * (kMainGridTileHeight + kMainGridGapY));
            if (x >= tx && x <= (tx + kMainGridTileWidth) && y >= ty && y <= (ty + kMainGridTileHeight))
                return i;
        }
        return -1;
    }

    void MainPage::setBackupUserPickerVisible(bool visible) {
        this->backupUserPickerRect->SetVisible(visible);
        this->backupUserPickerTitle->SetVisible(visible);
        this->optionMenu->SetVisible(visible);
        this->backupUserPickerHint->SetVisible(visible);
        mainApp->CallForRender();
    }

    int MainPage::promptBackupUserSelection(const std::vector<std::string>& userLabels, int preferredIndex) {
        if (userLabels.empty())
            return -1;
        if (userLabels.size() == 1)
            return 0;

        this->optionMenu->ClearItems();
        for (const auto& label : userLabels) {
            auto item = pu::ui::elm::MenuItem::New(inst::util::shortenString(label, 72, false));
            item->SetColor(COLOR("#FFFFFFFF"));
            this->optionMenu->AddItem(item);
        }

        if (preferredIndex < 0 || preferredIndex >= static_cast<int>(userLabels.size()))
            preferredIndex = 0;
        this->optionMenu->SetSelectedIndex(preferredIndex);
        this->setBackupUserPickerVisible(true);

        // Ignore the initial A press used to open this picker.
        bool waitARelease = true;
        while (mainApp->IsShown()) {
            mainApp->CallForRender();
            const u64 down = mainApp->GetButtonsDown();
            const u64 held = mainApp->GetButtonsHeld();
            inst::util::playNavigationClickIfNeeded(down);

            if (waitARelease) {
                if ((held & HidNpadButton_A) == 0)
                    waitARelease = false;
            } else if (down & HidNpadButton_A) {
                const int selected = this->optionMenu->GetSelectedIndex();
                this->setBackupUserPickerVisible(false);
                this->optionMenu->ClearItems();
                return selected;
            }

            if (down & (HidNpadButton_B | HidNpadButton_Plus | HidNpadButton_Minus)) {
                this->setBackupUserPickerVisible(false);
                this->optionMenu->ClearItems();
                return -1;
            }
            svcSleepThread(10'000'000);
        }

        this->setBackupUserPickerVisible(false);
        this->optionMenu->ClearItems();
        return -1;
    }

    void MainPage::activateSelectedMainItem() {
        switch (this->selectedMainIndex) {
            case 0:
                this->remoteInstallMenuItem_Click();
                break;
            case 1:
                this->installMenuItem_Click();
                break;
            case 2:
                this->hddInstallMenuItem_Click();
                break;
            case 3:
                this->mtpInstallMenuItem_Click();
                break;
            case 4:
                this->usbInstallMenuItem_Click();
                break;
            case 5:
                this->netInstallMenuItem_Click();
                break;
            case 6:
                this->backupSaveDataMenuItem_Click();
                break;
            case 7:
                this->settingsMenuItem_Click();
                break;
            case 8:
                this->exitMenuItem_Click();
                break;
            default:
                break;
        }
    }

    void MainPage::showSelectedMainInfo() {
        std::string title;
        std::string desc;
        switch (this->selectedMainIndex) {
            case 0:
                title = "main.menu.remote"_lang;
                desc = "main.info.remote"_lang;
                break;
            case 1:
                title = "main.menu.sd"_lang;
                desc = "main.info.sd"_lang;
                break;
            case 2:
                title = "main.menu.hdd"_lang;
                desc = "main.info.hdd"_lang;
                break;
            case 3:
                title = "main.menu.mtp"_lang;
                desc = "main.info.mtp"_lang;
                break;
            case 4:
                title = "main.menu.usb"_lang;
                desc = "main.info.usb"_lang;
                break;
            case 5:
                title = "main.menu.net"_lang;
                desc = "main.info.net"_lang;
                break;
            case 6:
                title = "main.menu.backup"_lang;
                desc = "main.info.backup"_lang;
                break;
            case 7:
                title = "main.menu.set"_lang;
                desc = "main.info.set"_lang;
                break;
            case 8:
                title = "main.menu.exit"_lang;
                desc = "main.info.exit"_lang;
                break;
            default:
                return;
        }
        mainApp->CreateShowDialog(title, desc, {"common.ok"_lang}, true);
    }

    void MainPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
        if (this->optionMenu != nullptr && this->optionMenu->IsVisible()) {
            return;
        }

        int bottomTapX = 0;
        if (DetectBottomHintTap(Pos, this->bottomHintTouch, 668, 52, bottomTapX)) {
            Down |= FindBottomHintButton(this->bottomHintSegments, bottomTapX);
        }
        inst::util::playNavigationClickIfNeeded(Down);
        if (((Down & HidNpadButton_Plus) || (Down & HidNpadButton_Minus) || (Down & HidNpadButton_B)) && mainApp->IsShown()) {
            mainApp->FadeOut();
            mainApp->Close();
        }
        if (Down & HidNpadButton_Y) {
            this->showSelectedMainInfo();
        }
        if (Down & (HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_StickRLeft)) {
            if ((this->selectedMainIndex % kMainGridCols) > 0) {
                this->selectedMainIndex--;
                this->updateMainGridSelection();
            }
        }
        if (Down & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight)) {
            if ((this->selectedMainIndex % kMainGridCols) < (kMainGridCols - 1) && this->selectedMainIndex < ((kMainGridCols * kMainGridRows) - 1)) {
                this->selectedMainIndex++;
                this->updateMainGridSelection();
            }
        }
        if (Down & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) {
            if (this->selectedMainIndex >= kMainGridCols) {
                this->selectedMainIndex -= kMainGridCols;
                this->updateMainGridSelection();
            }
        }
        if (Down & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)) {
            if (this->selectedMainIndex + kMainGridCols < (kMainGridCols * kMainGridRows)) {
                this->selectedMainIndex += kMainGridCols;
                this->updateMainGridSelection();
            }
        }
        bool touchSelect = false;
        if (!Pos.IsEmpty()) {
            const int touchedIndex = this->getMainGridIndexFromTouch(Pos.X, Pos.Y);
            if (!this->touchActive && touchedIndex >= 0) {
                this->touchActive = true;
                this->touchMoved = false;
                this->touchStartX = Pos.X;
                this->touchStartY = Pos.Y;
                this->selectedMainIndex = touchedIndex;
                this->updateMainGridSelection();
            } else if (this->touchActive) {
                int dx = Pos.X - this->touchStartX;
                int dy = Pos.Y - this->touchStartY;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx > 12 || dy > 12) {
                    this->touchMoved = true;
                }
                if (touchedIndex >= 0 && touchedIndex != this->selectedMainIndex) {
                    this->selectedMainIndex = touchedIndex;
                    this->updateMainGridSelection();
                }
            }
        } else if (this->touchActive) {
            if (!this->touchMoved) {
                touchSelect = true;
            }
            this->touchActive = false;
            this->touchMoved = false;
        }

        if ((Down & HidNpadButton_A) || touchSelect)
            this->activateSelectedMainItem();
    }
}


