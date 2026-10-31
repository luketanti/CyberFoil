#include "util/theme.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <SDL2/SDL_image.h>
#include "util/error.hpp"
#include "util/json.hpp"

namespace inst::theme {
    namespace {
        constexpr const char* kImagePrefix = "romfs:/images/";
        constexpr const char* kBlankImage = "romfs:/images/blank.png";
        constexpr int kScreenWidth = 1280;
        constexpr int kScreenHeight = 720;
        std::unordered_map<std::string, pu::ui::Color> g_colors;
        std::unordered_map<std::string, std::string> g_images;
        std::unordered_set<std::string> g_hiddenImages;
        std::vector<u8> g_backgroundPixels; // RGBA, kScreenWidth x kScreenHeight; empty when the file is used as-is
        float g_backgroundDim = 0.0f;
        std::string g_name = "Default";
        bool g_noBackgroundImage = false;

        bool ValidManifestPath(const std::string& file) {
            if (file.empty() || file.find(':') != std::string::npos || file.find('\\') != std::string::npos) return false;
            const std::filesystem::path path(file);
            if (path.is_absolute() || path.extension() != ".json") return false;
            for (const auto& part : path) if (part == "..") return false;
            return true;
        }

        bool ReadManifest(const std::filesystem::path& manifest, nlohmann::json& root) {
            std::error_code error;
            const auto size = std::filesystem::file_size(manifest, error);
            if (error || size == 0 || size > 256 * 1024) return false;
            try {
                std::ifstream input(manifest);
                root = nlohmann::json::parse(input);
                return root.is_object() && (!root.contains("version") || root["version"] == 1) &&
                    (!root.contains("enabled") || (root["enabled"].is_boolean() && root["enabled"].get<bool>()));
            } catch (...) { return false; }
        }

        std::string ManifestName(const nlohmann::json& root, const std::string& fallback) {
            if (root.contains("name") && root["name"].is_string()) {
                const auto name = root["name"].get<std::string>();
                if (!name.empty() && name.size() <= 80) return name;
            }
            return fallback;
        }

        bool SameColor(pu::ui::Color left, pu::ui::Color right) {
            return left.R == right.R && left.G == right.G && left.B == right.B && left.A == right.A;
        }

        // Accepts #RGB, #RGBA, #RRGGBB and #RRGGBBAA, with or without the '#',
        // so colours copied from CSS or from a Tinfoil theme (bare RRGGBBAA) work.
        bool ParseColor(const nlohmann::json& value, pu::ui::Color& result) {
            if (!value.is_string()) return false;
            auto text = value.get<std::string>();
            if (!text.empty() && text.front() == '#') text.erase(0, 1);
            if (text.size() != 3 && text.size() != 4 && text.size() != 6 && text.size() != 8) return false;
            if (!std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isxdigit(ch); })) return false;
            if (text.size() <= 4) {
                std::string expanded;
                for (const char ch : text) expanded += std::string(2, ch);
                text = expanded;
            }
            if (text.size() == 6) text += "FF";
            result = pu::ui::Color::FromHex("#" + text);
            return true;
        }

        std::string ValidImage(const std::filesystem::path& directory, const std::string& relative) {
            const std::filesystem::path path(relative);
            if (relative.empty() || relative.find(':') != std::string::npos || relative.find('\\') != std::string::npos || path.is_absolute()) return {};
            for (const auto& part : path) if (part == "..") return {};
            const auto full = directory / path;
            std::error_code error;
            const auto size = std::filesystem::file_size(full, error);
            if (error || size == 0 || size > 16ULL * 1024 * 1024) return {};
            // Decode before accepting the override so corrupt assets fall back safely.
            SDL_Surface* image = IMG_Load(full.string().c_str());
            if (!image) return {};
            const bool valid = image->w > 0 && image->h > 0 && image->w <= 4096 && image->h <= 4096;
            SDL_FreeSurface(image);
            return valid ? full.string() : std::string();
        }

        // Plutonium draws a layout's background texture at its own size from the
        // top-left, so a 1920x1080 wallpaper shows only its top-left corner. Scale
        // to cover the screen (centred, aspect kept) and apply backgroundDim in
        // the same pass; a 1280x720 image with no dim is left to Plutonium as-is.
        bool PrepareBackground(const std::string& path, float dim) {
            g_backgroundPixels.clear();
            SDL_Surface* loaded = IMG_Load(path.c_str());
            if (!loaded) return false;
            const bool exact = loaded->w == kScreenWidth && loaded->h == kScreenHeight;
            if (exact && dim <= 0.0f) {
                SDL_FreeSurface(loaded);
                return true;
            }
            SDL_Surface* source = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
            SDL_FreeSurface(loaded);
            if (!source) return false;
            SDL_Surface* screen = SDL_CreateRGBSurfaceWithFormat(0, kScreenWidth, kScreenHeight, 32, SDL_PIXELFORMAT_RGBA32);
            if (!screen) {
                SDL_FreeSurface(source);
                return false;
            }
            const double scale = std::max(static_cast<double>(kScreenWidth) / source->w, static_cast<double>(kScreenHeight) / source->h);
            SDL_Rect crop;
            crop.w = std::min(source->w, static_cast<int>(kScreenWidth / scale + 0.5));
            crop.h = std::min(source->h, static_cast<int>(kScreenHeight / scale + 0.5));
            crop.x = (source->w - crop.w) / 2;
            crop.y = (source->h - crop.h) / 2;
            SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
            const bool ok = SDL_BlitScaled(source, &crop, screen, nullptr) == 0;
            SDL_FreeSurface(source);
            if (ok) {
                g_backgroundPixels.resize(static_cast<size_t>(kScreenWidth) * kScreenHeight * 4);
                const float keep = 1.0f - std::clamp(dim, 0.0f, 1.0f);
                for (int y = 0; y < kScreenHeight; y++) {
                    const u8* row = static_cast<const u8*>(screen->pixels) + static_cast<size_t>(y) * screen->pitch;
                    u8* out = g_backgroundPixels.data() + static_cast<size_t>(y) * kScreenWidth * 4;
                    for (int x = 0; x < kScreenWidth * 4; x += 4) {
                        out[x] = static_cast<u8>(row[x] * keep + 0.5f);
                        out[x + 1] = static_cast<u8>(row[x + 1] * keep + 0.5f);
                        out[x + 2] = static_cast<u8>(row[x + 2] * keep + 0.5f);
                        out[x + 3] = row[x + 3];
                    }
                }
            }
            SDL_FreeSurface(screen);
            return ok;
        }

        pu::ui::Color Tint(const std::string& role, pu::ui::Color fallback) {
            const auto found = g_colors.find(role);
            if (found == g_colors.end()) return fallback;
            auto result = found->second;
            result.A = static_cast<u8>(static_cast<unsigned>(result.A) * fallback.A / 255);
            return result;
        }
    }

    std::vector<Choice> Discover(const std::string& appDir) {
        std::vector<Choice> choices;
        const auto directory = std::filesystem::path(appDir) / "theme";
        const auto add = [&](const std::filesystem::path& manifest, const std::string& file) {
            nlohmann::json root;
            if (ValidManifestPath(file) && ReadManifest(manifest, root)) {
                choices.push_back({file, ManifestName(root, std::filesystem::path(file).stem().string())});
            }
        };
        std::error_code error;
        std::filesystem::directory_iterator it(directory, error), end;
        while (!error && it != end) {
            const auto entry = *it;
            const auto filename = entry.path().filename().string();
            std::error_code typeError;
            if (entry.is_regular_file(typeError) && entry.path().extension() == ".json") {
                add(entry.path(), filename);
            } else if (!typeError && entry.is_directory(typeError)) {
                add(entry.path() / "theme.json", filename + "/theme.json");
            }
            it.increment(error);
        }
        std::sort(choices.begin(), choices.end(), [](const Choice& left, const Choice& right) {
            return left.name == right.name ? left.file < right.file : left.name < right.name;
        });
        return choices;
    }

    void Load(const std::string& appDir, const std::string& selectedFile) {
        g_colors.clear();
        g_images.clear();
        g_hiddenImages.clear();
        g_backgroundPixels.clear();
        g_backgroundDim = 0.0f;
        g_name = "Default";
        g_noBackgroundImage = false;
        if (!ValidManifestPath(selectedFile)) return;
        const auto manifest = std::filesystem::path(appDir) / "theme" / selectedFile;
        const auto directory = manifest.parent_path();
        try {
            nlohmann::json root;
            if (!ReadManifest(manifest, root)) return;
            if (root.contains("colors") && root["colors"].is_object()) {
                for (const auto& entry : root["colors"].items()) {
                    pu::ui::Color color;
                    if (ParseColor(entry.value(), color)) g_colors[entry.key()] = color;
                    else LOG_DEBUG("Theme: ignored invalid color %s\n", entry.key().c_str());
                }
            }
            if (root.contains("images") && root["images"].is_object()) {
                for (const auto& entry : root["images"].items()) {
                    if (!entry.value().is_string()) continue;
                    const auto relative = entry.value().get<std::string>();
                    auto key = entry.key();
                    if (key == "background") key = "background.jpg";
                    if (key == "logo") key = "logo.png";
                    if (key == "background.jpg" && relative.empty()) {
                        g_noBackgroundImage = true;
                        continue;
                    }
                    // An empty path hides any other built-in image, as it already
                    // does for the background - a theme cannot otherwise remove one.
                    if (relative.empty()) {
                        g_hiddenImages.insert(key);
                        continue;
                    }
                    const auto image = ValidImage(directory, relative);
                    if (!image.empty()) g_images[key] = image;
                    else LOG_DEBUG("Theme: ignored invalid image %s\n", relative.c_str());
                }
            }
            if (root.contains("backgroundDim") && root["backgroundDim"].is_number()) {
                g_backgroundDim = std::clamp(root["backgroundDim"].get<float>(), 0.0f, 1.0f);
            }
            const auto background = g_images.find("background.jpg");
            if (background != g_images.end() && !g_noBackgroundImage && !PrepareBackground(background->second, g_backgroundDim)) {
                LOG_DEBUG("Theme: could not prepare background, using it unscaled\n");
            }
            g_name = ManifestName(root, manifest.stem().string());
        } catch (...) {
            g_colors.clear();
            g_images.clear();
            g_hiddenImages.clear();
            g_backgroundPixels.clear();
            g_backgroundDim = 0.0f;
            g_noBackgroundImage = false;
            g_name = "Default";
            LOG_DEBUG("Theme: could not load theme.json, using defaults\n");
        }
    }

    std::string Name() { return g_name; }
    bool HasColor(const std::string& role) { return g_colors.find(role) != g_colors.end(); }

    pu::ui::Color Color(const std::string& role, pu::ui::Color fallback) {
        const auto found = g_colors.find(role);
        if (found != g_colors.end()) return found->second;
        if (role == "highlight" || role == "scrollbar") return Tint("accent", fallback);
        return fallback;
    }

    pu::ui::Color Palette(const std::string& fallbackHex) {
        const auto fallback = pu::ui::Color::FromHex(fallbackHex);
        const auto rgb = fallbackHex.substr(0, 7);
        if (rgb == "#FFFFFF" && fallback.A == 255) return Color("text", fallback);
        if (rgb == "#A8A8B8") return Tint("muted_text", fallback);
        if (rgb == "#FF4D4D" || rgb == "#FF7777" || rgb == "#FF9999") return Tint("accent", fallback);
        if (rgb == "#170909" || rgb == "#17171C" || rgb == "#222228" || rgb == "#1A1A1A" ||
            (rgb == "#000000" && (fallback.A == 204 || fallback.A == 238))) return Tint("panel", fallback);
        if (rgb == "#222222") return Color("progress_track", fallback);
        return fallback;
    }

    std::string Asset(const std::string& defaultPath) {
        if (defaultPath.rfind(kImagePrefix, 0) != 0) return defaultPath;
        const auto key = defaultPath.substr(std::char_traits<char>::length(kImagePrefix));
        if (g_hiddenImages.count(key)) return kBlankImage;
        const auto found = g_images.find(key);
        return found == g_images.end() ? defaultPath : found->second;
    }

    bool HasImage(const std::string& defaultPath) {
        if (defaultPath.rfind(kImagePrefix, 0) != 0) return false;
        const auto key = defaultPath.substr(std::char_traits<char>::length(kImagePrefix));
        return g_hiddenImages.count(key) || g_images.count(key);
    }

    void ApplyBackground(pu::ui::Layout& layout, bool oledMode, const std::string& appDir) {
        layout.SetBackgroundColor(Color("background", pu::ui::Color::FromHex(oledMode ? "#000000FF" : "#670000FF")));
        const auto background = g_images.find("background.jpg");
        if (background != g_images.end() && !g_noBackgroundImage) {
            if (!g_backgroundPixels.empty()) layout.SetBackgroundRgbImage(g_backgroundPixels.data(), kScreenWidth, kScreenHeight, 4);
            else layout.SetBackgroundImage(background->second);
        } else if (!HasColor("background") && !g_noBackgroundImage && !oledMode) {
            std::error_code error;
            const auto legacy = appDir + "/background.png";
            layout.SetBackgroundImage(std::filesystem::is_regular_file(legacy, error) ? legacy : "romfs:/images/background.jpg");
        }
    }

    Menu::Menu(s32 x, s32 y, s32 width, pu::ui::Color optionColor, s32 itemSize, s32 itemsToShow, s32 fontSize)
        : pu::ui::elm::Menu(x, y, width, optionColor, itemSize, itemsToShow, fontSize) {
        SetOnFocusColor(inst::theme::Color("highlight", GetOnFocusColor()));
        SetScrollbarColor(inst::theme::Color("scrollbar", GetScrollbarColor()));
    }

    void Menu::SetSelectedIndex(s32 index) {
        const auto count = static_cast<s32>(GetItems().size());
        if (index < 0 || index >= count) return;
        pu::ui::elm::Menu::SetSelectedIndex(index);
        const auto rows = GetNumberOfItemsToShow();
        // Match Plutonium's explicit-selection viewport, including short lists.
        visibleTopIndex = index >= count - rows ? std::max<s32>(0, count - rows)
            : (index < rows ? 0 : index);
    }

    s32 Menu::GetVisibleTopIndex() {
        return std::clamp<s32>(visibleTopIndex, 0, std::max<s32>(0, static_cast<s32>(GetItems().size()) - 1));
    }

    void Menu::SetOnRowsRendered(std::function<void()> callback) {
        onRowsRendered = std::move(callback);
    }

    void Menu::OnInput(u64 down, u64 up, u64 held, pu::ui::Touch pos) {
        // Plutonium delays right-stick movement even on its first press.
        // Route directions through the left-stick path for identical behavior.
        const auto normalizeStick = [](u64 buttons) {
            if (buttons & HidNpadButton_StickRUp) buttons |= HidNpadButton_StickLUp;
            if (buttons & HidNpadButton_StickRDown) buttons |= HidNpadButton_StickLDown;
            if (buttons & HidNpadButton_StickRLeft) buttons |= HidNpadButton_StickLLeft;
            if (buttons & HidNpadButton_StickRRight) buttons |= HidNpadButton_StickLRight;
            return buttons & ~(HidNpadButton_StickRUp | HidNpadButton_StickRDown |
                HidNpadButton_StickRLeft | HidNpadButton_StickRRight);
        };
        pu::ui::elm::Menu::OnInput(normalizeStick(down), normalizeStick(up), normalizeStick(held), pos);
        if (!GetItems().empty()) {
            // Ordinary movement/touch preserves the viewport unless selection
            // crosses an edge. Wrapping naturally moves it to the first/last page.
            const auto index = GetSelectedIndex();
            const auto rows = GetNumberOfItemsToShow();
            if (index < visibleTopIndex) visibleTopIndex = index;
            else if (index >= visibleTopIndex + rows) visibleTopIndex = index - rows + 1;
        }
    }

    void Menu::OnRender(pu::ui::render::Renderer::Ref& drawer, s32 x, s32 y) {
        if (HasColor("selected_text")) {
            const auto index = GetSelectedIndex();
            if (index >= 0 && index < static_cast<s32>(GetItems().size())) {
                const auto item = GetItems()[index];
                const auto previous = focusedItem.lock();
                const auto selectedColor = Color("selected_text", item->GetColor());
                if (previous != item || !SameColor(item->GetColor(), selectedColor)) {
                    if (previous && previous != item) previous->SetColor(originalColor);
                    originalColor = item->GetColor();
                    focusedItem = item;
                    item->SetColor(selectedColor);
                    // Rebuild text textures once per focus change, not on every frame.
                    this->SetSelectedIndex(index);
                }
            }
        }
        pu::ui::elm::Menu::OnRender(drawer, x, y);
        // Row overlays follow the viewport used for this frame, before the
        // layout forwards this frame's input to the menu.
        if (onRowsRendered) onRowsRendered();
    }
}
