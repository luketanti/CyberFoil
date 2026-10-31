#include "ui/usbInstPage.hpp"
#include "ui/MainApplication.hpp"
#include "util/util.hpp"
#include "util/config.hpp"
#include "util/lang.hpp"
#include "util/usb_util.hpp"
#include "usbInstall.hpp"
#include "ui/bottomHint.hpp"


#include "util/theme.hpp"
#include "util/file_selection.hpp"

#define COLOR(hex) inst::theme::Palette(hex)

namespace inst::ui {
    extern MainApplication *mainApp;

    usbInstPage::usbInstPage() : Layout::Layout() {
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
        this->butText = TextBlock::New(10, 678, "", 20);
        this->butText->SetColor(COLOR("#FFFFFFFF"));
        this->setButtonsText("inst.usb.buttons"_lang);
        this->menu = inst::theme::Menu::New(0, 136, 1280, COLOR("#FFFFFF00"), 50, 10);
        if (inst::config::oledMode) {
            this->menu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#FFFFFF33")));
            this->menu->SetScrollbarColor(inst::theme::Color("scrollbar", COLOR("#FFFFFF66")));
        } else {
            this->menu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#00000033")));
            this->menu->SetScrollbarColor(inst::theme::Color("scrollbar", COLOR("#17090980")));
        }
        this->infoImage = Image::New(460, 332, inst::theme::Asset("romfs:/images/icons/usb-connection-waiting.png"));
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
        this->Add(this->infoImage);
    }

    void usbInstPage::drawMenuItems(bool clearItems) {
        if (clearItems) this->selectedTitles = {};
        this->menu->ClearItems();
        for (auto& url: this->ourTitles) {
            std::string itm = inst::util::shortenString(inst::util::formatUrlString(url), 56, true);
            auto ourEntry = pu::ui::elm::MenuItem::New(itm);
            ourEntry->SetColor(COLOR("#FFFFFFFF"));
            ourEntry->SetIcon(inst::theme::Asset("romfs:/images/icons/checkbox-blank-outline.png"));
            for (long unsigned int i = 0; i < this->selectedTitles.size(); i++) {
                if (this->selectedTitles[i] == url) {
                    ourEntry->SetIcon(inst::theme::Asset("romfs:/images/icons/check-box-outline.png"));
                }
            }
            this->menu->AddItem(ourEntry);
        }
    }

    void usbInstPage::selectTitle(int selectedIndex) {
        const auto fileIndex = inst::util::FileRowIndex(selectedIndex, 0, ourTitles.size());
        if (!fileIndex) return;
        inst::util::ToggleFileSelection(selectedTitles, ourTitles[*fileIndex]);
        this->drawMenuItems(false);
    }

    void usbInstPage::startUsb() {
        this->pageInfoText->SetText("inst.usb.top_info"_lang);
        this->setButtonsText("inst.usb.buttons"_lang);
        this->menu->SetVisible(false);
        this->menu->ClearItems();
        this->infoImage->SetVisible(true);
        mainApp->LoadLayout(mainApp->usbinstPage);
        mainApp->CallForRender();
        this->ourTitles = usbInstStuff::OnSelected();
        if (!this->ourTitles.size()) {
            mainApp->LoadLayout(mainApp->mainPage);
            return;
        } else {
            mainApp->CallForRender(); // If we re-render a few times during this process the main screen won't flicker
            this->pageInfoText->SetText("inst.usb.top_info2"_lang);
            this->setButtonsText("inst.usb.buttons2"_lang);
            this->drawMenuItems(true);
            this->menu->SetSelectedIndex(0);
            mainApp->CallForRender();
            this->infoImage->SetVisible(false);
            this->menu->SetVisible(true);
        }
        return;
    }

    void usbInstPage::startInstall() {
        int dialogResult = -1;
        if (this->selectedTitles.size() == 1) dialogResult = mainApp->CreateShowDialog("inst.target.desc0"_lang + inst::util::shortenString(inst::util::formatUrlString(this->selectedTitles[0]), 32, true) + "inst.target.desc1"_lang, "common.cancel_desc"_lang, {"inst.target.opt0"_lang, "inst.target.opt1"_lang}, false);
        else dialogResult = mainApp->CreateShowDialog("inst.target.desc00"_lang + std::to_string(this->selectedTitles.size()) + "inst.target.desc01"_lang, "common.cancel_desc"_lang, {"inst.target.opt0"_lang, "inst.target.opt1"_lang}, false);
        if (dialogResult == -1) return;
        usbInstStuff::installTitleUsb(this->selectedTitles, dialogResult);
        return;
    }

    void usbInstPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
        (void)Held;
        int bottomTapX = 0;
        if (DetectBottomHintTap(Pos, this->bottomHintTouch, 668, 52, bottomTapX)) {
            Down |= FindBottomHintButton(this->bottomHintSegments, bottomTapX);
        }
        inst::util::playNavigationClickIfNeeded(Down);

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
                    this->selectTitle(tappedIndex);
                    if (this->menu->GetItems().size() == 1 && this->selectedTitles.size() == 1) {
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
            tin::util::USBCmdManager::SendExitCmd();
            mainApp->LoadLayout(mainApp->mainPage);
        }
        if (Down & HidNpadButton_A) {
            this->selectTitle(this->menu->GetSelectedIndex());
            if (this->menu->GetItems().size() == 1 && this->selectedTitles.size() == 1) {
                this->startInstall();
            }
            this->hasLastTap = false;
            this->lastTapIndex = -1;
        }
        if ((Down & HidNpadButton_Y)) {
            if (this->selectedTitles.size() == this->menu->GetItems().size()) this->drawMenuItems(true);
            else {
                this->selectedTitles = this->ourTitles;
                this->drawMenuItems(false);
            }
        }
        if (Down & HidNpadButton_Plus) {
            if (this->selectedTitles.size() == 0) {
                this->selectTitle(this->menu->GetSelectedIndex());
                this->startInstall();
                return;
            }
            this->startInstall();
        }
    }

    void usbInstPage::setButtonsText(const std::string& text) {
        this->butText->SetText(text);
        this->bottomHintSegments = BuildBottomHintSegments(text, 10, 20);
    }
}

