#pragma once
#include <pu/Plutonium>
#include <switch.h>
#include <atomic>
#include <vector>
#include "util/ui_dispatcher.hpp"
#include "ui/mainPage.hpp"
#include "ui/netInstPage.hpp"
#include "ui/remoteInstPage.hpp"
#include "ui/sdInstPage.hpp"
#include "ui/usbInstPage.hpp"
#include "ui/hddInstPage.hpp"
#include "ui/instPage.hpp"
#include "ui/optionsPage.hpp"

namespace inst::ui {
    class MainApplication;
    extern MainApplication *mainApp;

    class MainApplication : public pu::ui::Application {
        public:
            using Application::Application;
            PU_SMART_CTOR(MainApplication)
            void Close();
            void OnLoad() override;
            void RefreshInputDevice(bool force = false);
            HidAnalogStickState GetAnalogStick(unsigned index) const { return padGetStickPos(&this->input_pad, index); }
            void RecreateLayouts();
            void RequestThemeReload();
            bool IsUiThread() const { return uiDispatcher.isOwner(); }
            bool IsInstallWorkActive() const { return installWorkActive.load(); }
            template<class F> auto RunOnUiThread(F fn) -> std::invoke_result_t<F> { return uiDispatcher.invoke(std::move(fn)); }
            void PostInstallUiUpdate(const std::string& key, std::function<void()> fn) { uiDispatcher.postLatest(key, std::move(fn)); }
            void RunInstallWork(std::function<void()> work);
            bool CallForRender();
            int CreateShowDialog(const std::string& title, const std::string& content, std::vector<std::string> options, bool useLastOptionAsCancel, const std::string& icon = "");
            pu::ui::Layout::Ref GetCurrentLayout() const { return this->lyt; }
            MainPage::Ref mainPage;
            netInstPage::Ref netinstPage;
            remoteInstPage::Ref remoteinstPage;
            sdInstPage::Ref sdinstPage;
            usbInstPage::Ref usbinstPage;
            hddInstPage::Ref hddinstPage;
            instPage::Ref instpage;
            optionsPage::Ref optionspage;
        private:
            inst::UiDispatcher uiDispatcher;
            std::atomic<bool> installWorkActive{false};
            bool themeReloadPending = false;
            int themeReloadStage = -1;
            unsigned themeLoadingFrame = 0;
            pu::ui::Layout::Ref themeLoadingLayout;
            std::vector<pu::ui::elm::Rectangle::Ref> themeLoadingDots;
            pu::ui::elm::Rectangle::Ref themeLoadingProgress;
            void CreateLayoutStep(int step);
            void ShowThemeLoading();
            void ApplyPendingTheme();
            AppletFocusState lastFocusState = AppletFocusState_InFocus;
    };
}
