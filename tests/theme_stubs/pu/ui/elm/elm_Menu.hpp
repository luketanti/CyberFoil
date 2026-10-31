// Host stub: just enough of pu::ui::elm::Menu for theme.cpp's subclass to compile.
#pragma once
#include <pu/ui/ui_Layout.hpp>

namespace pu::ui::elm {
    class MenuItem {
    public:
        PU_SMART_CTOR(MenuItem)
        Color GetColor() { return clr; }
        void SetColor(Color c) { clr = c; }
    private:
        Color clr;
    };

    class Menu {
    public:
        Menu(s32, s32, s32, Color, s32, s32 rows, s32 = 25) : rows(rows) {}
        virtual ~Menu() = default;
        Color GetOnFocusColor() { return fcs; }
        void SetOnFocusColor(Color c) { fcs = c; }
        Color GetScrollbarColor() { return scb; }
        void SetScrollbarColor(Color c) { scb = c; }
        std::vector<MenuItem::Ref>& GetItems() { return itms; }
        s32 GetSelectedIndex() { return isel; }
        void SetSelectedIndex(s32 i) { isel = i; }
        s32 GetNumberOfItemsToShow() { return rows; }
        virtual void OnInput(u64, u64, u64, Touch) {}
        virtual void OnRender(render::Renderer::Ref&, s32, s32) {}
    private:
        // Plutonium's constructor defaults (elm_Menu.cpp).
        Color fcs{40, 40, 40, 255};
        Color scb{110, 110, 110, 255};
        std::vector<MenuItem::Ref> itms;
        s32 isel = 0;
        s32 rows;
    };
}
