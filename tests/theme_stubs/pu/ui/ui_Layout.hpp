// Host stub of the slice of Plutonium that source/util/theme.cpp touches, for
// tests/theme_tests.cpp. Only Color::FromHex carries behaviour, copied from
// Plutonium's Source/pu/ui/ui_Types.cpp because the loader's results go
// through it; Layout records what it was given so the test can inspect it.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using u8 = std::uint8_t;
using s32 = std::int32_t;
using u64 = std::uint64_t;

enum : u64 {
    HidNpadButton_StickLUp = 1ULL << 16,
    HidNpadButton_StickLDown = 1ULL << 17,
    HidNpadButton_StickLLeft = 1ULL << 18,
    HidNpadButton_StickLRight = 1ULL << 19,
    HidNpadButton_StickRUp = 1ULL << 20,
    HidNpadButton_StickRDown = 1ULL << 21,
    HidNpadButton_StickRLeft = 1ULL << 22,
    HidNpadButton_StickRRight = 1ULL << 23
};

#define PU_SMART_CTOR(type) \
    using Ref = std::shared_ptr<type>; \
    template<typename ...Args> \
    static Ref New(Args &&...CtorArgs) { return std::make_shared<type>(std::forward<Args>(CtorArgs)...); }

namespace pu::ui {
    struct Touch { s32 X = 0, Y = 0; };
    struct Color {
        u8 R = 0, G = 0, B = 0, A = 255;
        Color() = default;
        Color(u8 r, u8 g, u8 b, u8 a) : R(r), G(g), B(b), A(a) {}
        static Color FromHex(const std::string& HexFmt) {
            Color clr;
            std::string r = "00", g = "00", b = "00", a = "FF";
            if (HexFmt.length() >= 9) a = HexFmt.substr(7, 2);
            if (HexFmt.length() >= 7) { r = HexFmt.substr(1, 2); g = HexFmt.substr(3, 2); b = HexFmt.substr(5, 2); }
            clr.R = (u8)std::stoul(r, NULL, 16);
            clr.G = (u8)std::stoul(g, NULL, 16);
            clr.B = (u8)std::stoul(b, NULL, 16);
            clr.A = (u8)std::stoul(a, NULL, 16);
            return clr;
        }
    };

    namespace render { struct Renderer { using Ref = std::shared_ptr<Renderer>; }; }

    class Layout {
    public:
        Color bg;
        std::string image;
        const u8* rgb = nullptr;
        u64 rgbWidth = 0, rgbHeight = 0;
        void SetBackgroundColor(Color c) { bg = c; }
        void SetBackgroundImage(const std::string& path) { image = path; rgb = nullptr; }
        void SetBackgroundRgbImage(void* buffer, u64 width, u64 height, u8) {
            rgb = static_cast<const u8*>(buffer); rgbWidth = width; rgbHeight = height; image.clear();
        }
    };
}
