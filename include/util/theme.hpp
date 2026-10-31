#pragma once

#include <string>
#include <functional>
#include <vector>
#include <pu/ui/ui_Layout.hpp>
#include <pu/ui/elm/elm_Menu.hpp>

namespace inst::theme {
    // Load on the UI thread before creating or recreating themed layouts.
    struct Choice {
        std::string file; // Relative to appDir/theme; empty selects the built-in appearance.
        std::string name;
    };
    std::vector<Choice> Discover(const std::string& appDir);
    void Load(const std::string& appDir, const std::string& selectedFile = "theme.json");
    std::string Name();
    bool HasColor(const std::string& role);
    pu::ui::Color Color(const std::string& role, pu::ui::Color fallback);
    pu::ui::Color Palette(const std::string& fallbackHex);
    std::string Asset(const std::string& defaultPath);
    // True when the theme replaces or hides this romfs image.
    bool HasImage(const std::string& defaultPath);
    void ApplyBackground(pu::ui::Layout& layout, bool oledMode, const std::string& appDir);

    // Apply focus text colors without modifying the bundled Plutonium library.
    class Menu : public pu::ui::elm::Menu {
    public:
        Menu(s32 x, s32 y, s32 width, pu::ui::Color optionColor, s32 itemSize, s32 itemsToShow, s32 fontSize = 25);
        PU_SMART_CTOR(Menu)
        void SetSelectedIndex(s32 index);
        s32 GetVisibleTopIndex();
        void SetOnRowsRendered(std::function<void()> callback);
        void OnRender(pu::ui::render::Renderer::Ref& drawer, s32 x, s32 y) override;
        void OnInput(u64 down, u64 up, u64 held, pu::ui::Touch pos) override;
    private:
        std::weak_ptr<pu::ui::elm::MenuItem> focusedItem;
        pu::ui::Color originalColor;
        s32 visibleTopIndex = 0;
        std::function<void()> onRowsRendered;
    };
}
