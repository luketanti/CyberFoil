#include <algorithm>
#include <cctype>
#include <filesystem>
#include "ui/MainApplication.hpp"
#include "ui/mainPage.hpp"
#include "ui/hddInstPage.hpp"
#include "hddInstall.hpp"
#include "util/util.hpp"
#include "util/config.hpp"
#include "util/lang.hpp"
#include "ui/bottomHint.hpp"

#include "util/theme.hpp"
#include "util/file_selection.hpp"

#define COLOR(hex) inst::theme::Palette(hex)

namespace inst::ui {
    extern MainApplication *mainApp;

    namespace {
        std::filesystem::path normalizeRootPath(const std::filesystem::path& path) {
            std::filesystem::path normalized = path;
            const auto pathStr = normalized.string();
            if (!pathStr.empty() && pathStr.back() == ':') {
                normalized /= "";
            }
            return normalized;
        }
    }

    hddInstPage::hddInstPage() : Layout::Layout() {
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
        this->pageInfoText = TextBlock::New(10, 89, "inst.hdd.top_info"_lang, 30);
        this->pageInfoText->SetColor(COLOR("#FFFFFFFF"));
        this->butText = TextBlock::New(10, 678, "inst.hdd.buttons"_lang, 20);
        this->butText->SetColor(COLOR("#FFFFFFFF"));
        this->refreshBottomHint();
        this->menu = inst::theme::Menu::New(0, 136, 1280, COLOR("#FFFFFF00"), 50, 10);
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
        this->Add(this->menu);
    }

    void hddInstPage::refreshBottomHint() {
        const std::string base = queueAddMode
            ? " Select     Add to queue     Queue"
            : "inst.hdd.buttons"_lang;
        const std::string action = inst::config::usbHddShowHiddenFiles
            ? "inst.hdd.hide_hidden"_lang : "inst.hdd.show_hidden"_lang;
        this->butText->SetText(base + "     " + action);
        int fontSize = 20;
        this->butText->SetFontSize(fontSize);
        while (fontSize > 14 && this->butText->GetTextWidth() > 1260) {
            this->butText->SetFontSize(--fontSize);
        }
        this->bottomHintSegments = BuildBottomHintSegments(this->butText->GetText(), 10, fontSize);
    }

    void hddInstPage::drawMenuItems(bool clearItems, std::filesystem::path ourPath) {
        this->refreshBottomHint();
        if (clearItems) this->selectedTitles = {};
        const auto normalizedPath = normalizeRootPath(ourPath);
        const auto normalizedParent = normalizeRootPath(normalizedPath.parent_path());
        const bool isRootPath = (normalizedParent == normalizedPath);
        if (this->rootDir.empty() || (clearItems && isRootPath)) {
            this->rootDir = normalizedPath;
        }
        this->currentDir = normalizedPath;
        this->menu->ClearItems();
        try {
            this->ourDirectories = util::getDirsAtPath(this->currentDir);
            this->ourFiles = util::getDirectoryFiles(this->currentDir, {".nsp", ".nsz", ".xci", ".xcz"});
        } catch (std::exception& e) {
            this->drawMenuItems(false, this->currentDir.parent_path());
            return;
        }
        if (!inst::config::usbHddShowHiddenFiles) {
            const auto hidden = [](const std::filesystem::path& path) {
                auto name = path.filename().string();
                if (!name.empty() && name.front() == '.') return true;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                return name == "$recycle.bin" || name == "system volume information" ||
                    name == "desktop.ini" || name == "thumbs.db";
            };
            const auto removeHidden = [&](auto& paths) {
                paths.erase(std::remove_if(paths.begin(), paths.end(), hidden), paths.end());
            };
            removeHidden(this->ourDirectories);
            removeHidden(this->ourFiles);
            // Hidden selections must not remain queued for a later install.
            removeHidden(this->selectedTitles);
            removeHidden(this->queueSavedSelection);
        }
        if (this->currentDir != this->rootDir) {
            std::string itm = "..";
            auto ourEntry = pu::ui::elm::MenuItem::New(itm);
            ourEntry->SetColor(COLOR("#FFFFFFFF"));
            ourEntry->SetIcon(inst::theme::Asset("romfs:/images/icons/folder-upload.png"));
            this->menu->AddItem(ourEntry);
        }
        for (auto& file: this->ourDirectories) {
            if (file == "..") break;
            std::string itm = file.filename().string();
            auto ourEntry = pu::ui::elm::MenuItem::New(itm);
            ourEntry->SetColor(COLOR("#FFFFFFFF"));
            ourEntry->SetIcon(inst::theme::Asset("romfs:/images/icons/folder.png"));
            this->menu->AddItem(ourEntry);
        }
        for (auto& file: this->ourFiles) {
            std::string itm = file.filename().string();
            auto ourEntry = pu::ui::elm::MenuItem::New(itm);
            ourEntry->SetColor(COLOR("#FFFFFFFF"));
            ourEntry->SetIcon(inst::theme::Asset("romfs:/images/icons/checkbox-blank-outline.png"));
            for (long unsigned int i = 0; i < this->selectedTitles.size(); i++) {
                if (this->selectedTitles[i] == file) {
                    ourEntry->SetIcon(inst::theme::Asset("romfs:/images/icons/check-box-outline.png"));
                }
            }
            this->menu->AddItem(ourEntry);
        }
    }

    void hddInstPage::followDirectory() {
        if (queueAddMode) return;
        int selectedIndex = this->menu->GetSelectedIndex();
        int dirListSize = this->ourDirectories.size();
        if (this->currentDir != this->rootDir) {
            dirListSize++;
            selectedIndex--;
        }
        if (selectedIndex < dirListSize) {
            if (this->menu->GetItems()[this->menu->GetSelectedIndex()]->GetName() == ".." && this->menu->GetSelectedIndex() == 0) {
                this->drawMenuItems(true, this->currentDir.parent_path());
            } else {
                this->drawMenuItems(true, this->ourDirectories[selectedIndex]);
            }
            this->menu->SetSelectedIndex(0);
        }
    }

    void hddInstPage::selectNsp(int selectedIndex) {
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(this->menu->GetItems().size())) return;
        const auto directoryRows = ourDirectories.size() + (currentDir != rootDir ? 1 : 0);
        const auto fileIndex = inst::util::FileRowIndex(selectedIndex, directoryRows, ourFiles.size());
        if (!fileIndex) {
            if (static_cast<std::size_t>(selectedIndex) < directoryRows) this->followDirectory();
            return;
        }
        if (queueAddMode && !instPage::canEnqueueTitle(ourFiles[*fileIndex].string())) return;
        inst::util::ToggleFileSelection(selectedTitles, ourFiles[*fileIndex]);
        this->drawMenuItems(false, currentDir);
    }

    void hddInstPage::beginQueueAddMode() {
        queueSavedSelection = selectedTitles; selectedTitles.clear(); queueAddMode = true;
        touchTapActive = false; hasLastTap = false; bottomHintTouch = {};
        drawMenuItems(false, currentDir);
        refreshBottomHint();
        mainApp->LoadLayout(mainApp->hddinstPage);
    }

    void hddInstPage::endQueueAddMode() {
        if (!queueAddMode) return;
        queueAddMode = false; selectedTitles = std::move(queueSavedSelection);
        touchTapActive = false; hasLastTap = false; bottomHintTouch = {};
        refreshBottomHint();
    }

    void hddInstPage::startInstall() {
        if (queueAddMode) {
            std::vector<std::string> keys;
            for (const auto& path : selectedTitles) keys.push_back(path.string());
            instPage::enqueueTitles(keys);
            selectedTitles.clear(); instPage::showQueue(); return;
        }
        int dialogResult = -1;
        if (this->selectedTitles.size() == 1) {
            dialogResult = mainApp->CreateShowDialog("inst.target.desc0"_lang + inst::util::shortenString(std::filesystem::path(this->selectedTitles[0]).filename().string(), 32, true) + "inst.target.desc1"_lang, "common.cancel_desc"_lang, {"inst.target.opt0"_lang, "inst.target.opt1"_lang}, false);
        } else dialogResult = mainApp->CreateShowDialog("inst.target.desc00"_lang + std::to_string(this->selectedTitles.size()) + "inst.target.desc01"_lang, "common.cancel_desc"_lang, {"inst.target.opt0"_lang, "inst.target.opt1"_lang}, false);
        if (dialogResult == -1) return;
        hddInstStuff::installNspFromFile(this->selectedTitles, dialogResult, this->ourFiles);
    }

    void hddInstPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
        (void)Held;
        int bottomTapX = 0;
        if (DetectBottomHintTap(Pos, this->bottomHintTouch, 668, 52, bottomTapX)) {
            Down |= FindBottomHintButton(this->bottomHintSegments, bottomTapX);
        }
        inst::util::playNavigationClickIfNeeded(Down);
        if (Down & HidNpadButton_Minus) {
            inst::config::usbHddShowHiddenFiles = !inst::config::usbHddShowHiddenFiles;
            inst::config::setConfig();
            this->hasLastTap = false;
            this->lastTapIndex = -1;
            this->drawMenuItems(false, this->currentDir);
            if (!this->menu->GetItems().empty()) this->menu->SetSelectedIndex(0);
            return;
        }
        if (queueAddMode && (Down & HidNpadButton_B)) { instPage::showQueue(); return; }
        if (queueAddMode && (Down & HidNpadButton_Y)) {
            if (!selectedTitles.empty()) selectedTitles.clear();
            else for (const auto& path : ourFiles) if (instPage::canEnqueueTitle(path.string())) selectedTitles.push_back(path);
            drawMenuItems(false, currentDir); Down &= ~HidNpadButton_Y;
        }

        if (!Pos.IsEmpty()) {
            if (!this->touchTapActive) {
                this->touchTapActive = true;
                this->touchTapMoved = false;
                this->touchTapStartX = Pos.X;
                this->touchTapStartY = Pos.Y;
            } else {
                int dx = Pos.X - this->touchTapStartX;
                int dy = Pos.Y - this->touchTapStartY;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx > 12 || dy > 12) this->touchTapMoved = true;
            }
        } else if (this->touchTapActive) {
            if (!this->touchTapMoved) {
                const int tappedIndex = this->menu->GetSelectedIndex();
                const auto now = std::chrono::steady_clock::now();
                bool doubleTap = false;
                if (this->hasLastTap && (this->lastTapIndex == tappedIndex)) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - this->lastTapTime).count();
                    if (ms <= 350) doubleTap = true;
                }

                if (doubleTap) {
                    this->selectNsp(tappedIndex);
                    if (this->ourFiles.size() == 1 && this->selectedTitles.size() == 1) {
                        this->startInstall();
                    }
                    this->hasLastTap = false;
                    this->lastTapIndex = -1;
                } else {
                    this->hasLastTap = true;
                    this->lastTapIndex = tappedIndex;
                    this->lastTapTime = now;
                }
            }
            this->touchTapActive = false;
            this->touchTapMoved = false;
        }

        if (Down & HidNpadButton_B) {
            mainApp->LoadLayout(mainApp->mainPage);
        }
        if (Down & HidNpadButton_A) {
            this->selectNsp(this->menu->GetSelectedIndex());
            if (this->ourFiles.size() == 1 && this->selectedTitles.size() == 1) {
                this->startInstall();
            }
            this->hasLastTap = false;
            this->lastTapIndex = -1;
        }
        if ((Down & HidNpadButton_Y)) {
            if (this->selectedTitles.size() == this->ourFiles.size()) this->drawMenuItems(true, currentDir);
            else {
                this->selectedTitles = this->ourFiles;
                this->drawMenuItems(false, currentDir);
            }
        }
        if ((Down & HidNpadButton_X)) {
            inst::ui::mainApp->CreateShowDialog("inst.hdd.help.title"_lang, "inst.hdd.help.desc"_lang, {"common.ok"_lang}, true);
        }
        if (Down & HidNpadButton_Plus) {
            if (this->menu->GetItems().empty()) return;
            if (this->selectedTitles.empty() && inst::util::FileRowIndex(this->menu->GetSelectedIndex(),
                    ourDirectories.size() + (currentDir != rootDir ? 1 : 0), ourFiles.size())) {
                this->selectNsp(this->menu->GetSelectedIndex());
            }
            if (this->selectedTitles.size() > 0) this->startInstall();
        }
    }
}

