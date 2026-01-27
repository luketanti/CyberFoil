#include <filesystem>
#include "ui/MainApplication.hpp"
#include "ui/instPage.hpp"
#include "util/config.hpp"
#include "mtp_server.hpp"

#define COLOR(hex) pu::ui::Color::FromHex(hex)

namespace inst::ui {
    extern MainApplication *mainApp;

    constexpr int kInstallIconSize = 256;
    constexpr int kInstallIconX = (1280 - kInstallIconSize) / 2;
    constexpr int kInstallIconY = 220;

    instPage::instPage() : Layout::Layout() {
        if (inst::config::oledMode) {
            this->SetBackgroundColor(COLOR("#000000FF"));
        } else {
            this->SetBackgroundColor(COLOR("#670000FF"));
            if (std::filesystem::exists(inst::config::appDir + "/background.png")) this->SetBackgroundImage(inst::config::appDir + "/background.png");
            else this->SetBackgroundImage("romfs:/images/background.jpg");
        }
        const auto topColor = inst::config::oledMode ? COLOR("#000000FF") : COLOR("#170909FF");
        const auto infoColor = inst::config::oledMode ? COLOR("#000000FF") : COLOR("#17090980");
        const auto botColor = inst::config::oledMode ? COLOR("#000000FF") : COLOR("#17090980");
        this->topRect = Rectangle::New(0, 0, 1280, 94, topColor);
        this->infoRect = Rectangle::New(0, 95, 1280, 60, infoColor);
        this->botRect = Rectangle::New(0, 659, 1280, 61, botColor);
        if (inst::config::gayMode) {
            this->titleImage = Image::New(-113, 0, "romfs:/images/logo.png");
            this->appVersionText = TextBlock::New(367, 49, "v" + inst::config::appVersion, 22);
        }
        else {
            this->titleImage = Image::New(0, 0, "romfs:/images/logo.png");
            this->appVersionText = TextBlock::New(480, 49, "v" + inst::config::appVersion, 22);
        }
        this->appVersionText->SetColor(COLOR("#FFFFFFFF"));
        this->pageInfoText = TextBlock::New(10, 109, "", 30);
        this->pageInfoText->SetColor(COLOR("#FFFFFFFF"));
        this->installInfoText = TextBlock::New(15, 568, "", 22);
        this->installInfoText->SetColor(COLOR("#FFFFFFFF"));
        this->installBar = pu::ui::elm::ProgressBar::New(10, 600, 850, 40, 100.0f);
        this->installBar->SetColor(COLOR("#222222FF"));
        this->hintText = TextBlock::New(0, 678, " Back", 24);
        this->hintText->SetColor(COLOR("#FFFFFFFF"));
        this->hintText->SetX(1280 - 10 - this->hintText->GetTextWidth());
        this->hintText->SetVisible(false);
        if (std::filesystem::exists(inst::config::appDir + "/awoo_inst.png")) this->awooImage = Image::New(410, 190, inst::config::appDir + "/awoo_inst.png");
        else this->awooImage = Image::New(510, 166, "romfs:/images/awoos/7d8a05cddfef6da4901b20d2698d5a71.png");
        this->installIconImage = Image::New(kInstallIconX, kInstallIconY, "romfs:/images/awoos/7d8a05cddfef6da4901b20d2698d5a71.png");
        this->installIconImage->SetWidth(kInstallIconSize);
        this->installIconImage->SetHeight(kInstallIconSize);
        this->installIconImage->SetVisible(false);
        this->Add(this->topRect);
        this->Add(this->infoRect);
        this->Add(this->botRect);
        this->Add(this->titleImage);
        this->Add(this->appVersionText);
        this->Add(this->pageInfoText);
        this->Add(this->installInfoText);
        this->Add(this->installBar);
        this->Add(this->hintText);
        this->Add(this->awooImage);
        this->Add(this->installIconImage);
        if (inst::config::gayMode) this->awooImage->SetVisible(false);
    }

    void instPage::setTopInstInfoText(std::string ourText){
        mainApp->instpage->pageInfoText->SetText(ourText);
        mainApp->CallForRender();
    }

    void instPage::setInstInfoText(std::string ourText){
        mainApp->instpage->installInfoText->SetText(ourText);
        mainApp->CallForRender();
    }

    void instPage::setInstBarPerc(double ourPercent){
        mainApp->instpage->installBar->SetVisible(true);
        mainApp->instpage->installBar->SetProgress(ourPercent);
        mainApp->CallForRender();
    }

    void instPage::setInstallIcon(const std::string& imagePath){
        if (imagePath.empty()) {
            clearInstallIcon();
            return;
        }
        mainApp->instpage->installIconImage->SetImage(imagePath);
        mainApp->instpage->installIconImage->SetX(kInstallIconX);
        mainApp->instpage->installIconImage->SetY(kInstallIconY);
        mainApp->instpage->installIconImage->SetWidth(kInstallIconSize);
        mainApp->instpage->installIconImage->SetHeight(kInstallIconSize);
        mainApp->instpage->installIconImage->SetVisible(true);
        mainApp->instpage->awooImage->SetVisible(false);
        mainApp->CallForRender();
    }

    void instPage::clearInstallIcon(){
        mainApp->instpage->installIconImage->SetVisible(false);
        if (!inst::config::gayMode)
            mainApp->instpage->awooImage->SetVisible(true);
        mainApp->CallForRender();
    }

    void instPage::loadMainMenu(){
        mainApp->LoadLayout(mainApp->mainPage);
    }

    void instPage::loadInstallScreen(){
        mainApp->instpage->pageInfoText->SetText("");
        mainApp->instpage->installInfoText->SetText("");
        mainApp->instpage->installBar->SetProgress(0);
        mainApp->instpage->installBar->SetVisible(false);
        mainApp->instpage->hintText->SetVisible(false);
        mainApp->instpage->installIconImage->SetVisible(false);
        mainApp->instpage->awooImage->SetVisible(!inst::config::gayMode);
        mainApp->LoadLayout(mainApp->instpage);
        mainApp->CallForRender();
    }

    void instPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
        if ((Down & HidNpadButton_B) && inst::mtp::IsInstallServerRunning()) {
            inst::mtp::StopInstallServer();
            loadMainMenu();
        }
    }
}
