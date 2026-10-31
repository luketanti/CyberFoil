// Host tests for source/util/theme.cpp, compiled against real SDL2 and
// SDL2_image so the background scaling and dim run on real pixels. Plutonium
// and libnx are replaced by the small stubs in theme_stubs/.
#include "../source/util/theme.cpp"
#include "util/file_selection.hpp"

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;
using namespace inst::theme;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static fs::path g_app;

static void writeTheme(const std::string& json) {
    fs::create_directories(g_app / "theme" / "t");
    std::ofstream(g_app / "theme" / "t" / "theme.json") << json;
}

// A w x h PNG filled by `colour(x, y)` (RGBA).
template <typename F>
static void writePng(const std::string& name, int w, int h, F colour) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const Uint32 c = colour(x, y);
            u8* p = static_cast<u8*>(s->pixels) + y * s->pitch + x * 4;
            p[0] = c >> 24; p[1] = c >> 16; p[2] = c >> 8; p[3] = c;
        }
    IMG_SavePNG(s, (g_app / "theme" / "t" / name).string().c_str());
    SDL_FreeSurface(s);
}

static pu::ui::Color pixel(const pu::ui::Layout& l, int x, int y) {
    const u8* p = l.rgb + (static_cast<size_t>(y) * l.rgbWidth + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

static bool same(pu::ui::Color c, int r, int g, int b, int a = 255) { return c.R == r && c.G == g && c.B == b && c.A == a; }

int main() {
    char tmpl[] = "/tmp/cftheme-XXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    g_app = tmpl;

    // ---- colour formats ----
    writeTheme(R"({"colors":{"text":"#fff","accent":"f008","panel":"#11223344","header":"112233",
        "footer":"FF000080","muted_text":"#12345","highlight":"red","scrollbar":"#GGGGGG"}})");
    Load(g_app.string(), "t/theme.json");
    CHECK(same(Color("text", {}), 255, 255, 255));        // #RGB
    CHECK(same(Color("accent", {}), 255, 0, 0, 136));     // RGBA, no '#'
    CHECK(same(Color("panel", {}), 0x11, 0x22, 0x33, 0x44));
    CHECK(same(Color("header", {}), 0x11, 0x22, 0x33));   // RRGGBB, no '#'
    CHECK(same(Color("footer", {}), 255, 0, 0, 0x80));    // Tinfoil's bare RRGGBBAA
    CHECK(!HasColor("muted_text"));                       // 5 digits is no format
    CHECK(!HasColor("highlight"));                        // names stay rejected
    CHECK(!HasColor("scrollbar"));

    // ---- an empty path hides an image ----
    writeTheme(R"({"images":{"icons/settings.png":"","awoos/5bbdbcf9a5625cd307c9e9bc360d78bd.png":""}})");
    Load(g_app.string(), "t/theme.json");
    CHECK(Asset("romfs:/images/icons/settings.png") == "romfs:/images/blank.png");
    CHECK(HasImage("romfs:/images/awoos/5bbdbcf9a5625cd307c9e9bc360d78bd.png"));
    CHECK(Asset("romfs:/images/icons/folder.png") == "romfs:/images/icons/folder.png");
    CHECK(!HasImage("romfs:/images/icons/folder.png"));
    CHECK(fs::exists(fs::path(__FILE__).parent_path() / ".." / "romfs" / "images" / "blank.png"));

    // Identical hidden icons must not make folders or unchecked files look selected
    // to the browser controls. Selection is based on row identity and file state.
    writeTheme(R"({"images":{"icons/check-box-outline.png":"","icons/checkbox-blank-outline.png":"","icons/folder.png":""}})");
    Load(g_app.string(), "t/theme.json");
    CHECK(Asset("romfs:/images/icons/check-box-outline.png") == Asset("romfs:/images/icons/checkbox-blank-outline.png"));
    CHECK(Asset("romfs:/images/icons/folder.png") == Asset("romfs:/images/icons/check-box-outline.png"));
    {
        const std::vector<std::string> files{"first.nsp", "second.nsp"};
        std::vector<std::string> selected;
        CHECK(!inst::util::FileRowIndex(0, 1, files.size()));
        const auto first = inst::util::FileRowIndex(1, 1, files.size());
        CHECK(first.has_value());
        if (first) {
            inst::util::ToggleFileSelection(selected, files[*first]);
            CHECK(selected == std::vector<std::string>{"first.nsp"});
            inst::util::ToggleFileSelection(selected, files[*first]);
            CHECK(selected.empty());
        }
    }

    // ---- a 1920x1080 wallpaper is scaled to the screen ----
    writePng("wide.png", 1920, 1080, [](int x, int) { return x < 960 ? 0xFF0000FFu : 0x0000FFFFu; });
    writeTheme(R"({"images":{"background":"wide.png"}})");
    Load(g_app.string(), "t/theme.json");
    {
        pu::ui::Layout l;
        ApplyBackground(l, false, g_app.string());
        CHECK(l.rgb && l.rgbWidth == 1280 && l.rgbHeight == 720);
        if (l.rgb) {
            CHECK(same(pixel(l, 100, 360), 255, 0, 0));    // left half stays left
            CHECK(same(pixel(l, 1180, 360), 0, 0, 255));   // right half reaches the right edge
        }
    }

    // ---- a square one is cropped to cover, centred ----
    writePng("square.png", 1000, 1000, [](int, int y) { return y < 100 ? 0x00FF00FFu : 0xFF0000FFu; });
    writeTheme(R"({"images":{"background":"square.png"}})");
    Load(g_app.string(), "t/theme.json");
    {
        pu::ui::Layout l;
        ApplyBackground(l, false, g_app.string());
        CHECK(l.rgb != nullptr);
        if (l.rgb) CHECK(same(pixel(l, 640, 0), 255, 0, 0)); // the green top rows are cropped away
    }

    // ---- backgroundDim darkens, even at the exact size ----
    writePng("white.png", 1280, 720, [](int, int) { return 0xFFFFFFFFu; });
    writeTheme(R"({"images":{"background":"white.png"},"backgroundDim":0.5})");
    Load(g_app.string(), "t/theme.json");
    {
        pu::ui::Layout l;
        ApplyBackground(l, false, g_app.string());
        CHECK(l.rgb != nullptr);
        if (l.rgb) CHECK(same(pixel(l, 10, 10), 128, 128, 128));
    }
    writeTheme(R"({"images":{"background":"white.png"},"backgroundDim":7})");
    Load(g_app.string(), "t/theme.json");
    {
        pu::ui::Layout l;
        ApplyBackground(l, false, g_app.string());
        if (l.rgb) CHECK(same(pixel(l, 10, 10), 0, 0, 0)); // clamped to 1
    }

    // ---- an exact 1280x720 image with no dim is left to Plutonium ----
    writeTheme(R"({"images":{"background":"white.png"}})");
    Load(g_app.string(), "t/theme.json");
    {
        pu::ui::Layout l;
        ApplyBackground(l, false, g_app.string());
        CHECK(!l.rgb && l.image == (g_app / "theme" / "t" / "white.png").string());
    }

    // ---- "background": "" still means no image at all ----
    writeTheme(R"({"images":{"background":""},"backgroundDim":0.5})");
    Load(g_app.string(), "t/theme.json");
    {
        pu::ui::Layout l;
        ApplyBackground(l, false, g_app.string());
        CHECK(!l.rgb && l.image.empty());
    }

    fs::remove_all(g_app);
    if (g_failures) { std::fprintf(stderr, "%d theme check(s) failed\n", g_failures); return 1; }
    std::puts("theme tests passed");
    return 0;
}
