#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <exception>
#include <stdexcept>
#include <functional>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <switch.h>
#include "ui/MainApplication.hpp"
#include "ui/instPage.hpp"
#include "ui/remoteInstPage.hpp"
#include "ui/overflowText.hpp"
#include "util/config.hpp"
#include "util/remote_connection.hpp"
#include "util/curl.hpp"
#include "cheat_manager.hpp"
#include "util/lang.hpp"
#include "util/offline_title_db.hpp"
#include "util/save_sync.hpp"
#include "util/title_util.hpp"
#include "util/util.hpp"
#include "ui/bottomHint.hpp"

#include "util/theme.hpp"

#define COLOR(hex) inst::theme::Palette(hex)
#define RemoteDlcTrace(...) ((void)0)
#define ResetRemoteDlcTrace() ((void)0)

namespace {
    constexpr int kGridCols = 7;
    constexpr int kGridRows = 3;
    constexpr int kGridTileWidth = 136;
    constexpr int kGridTileHeight = 136;
    constexpr int kGridGap = 32;
    constexpr int kGridWidth = (kGridCols * kGridTileWidth) + ((kGridCols - 1) * kGridGap);
    constexpr int kGridStartX = (1280 - kGridWidth) / 2;
    constexpr int kGridStartY = 148;
    constexpr int kGridItemsPerPage = kGridCols * kGridRows;
    constexpr int kBrowseListX = 24;
    constexpr int kBrowseListY = 150;
    constexpr int kBrowseListWidth = 840;
    constexpr int kBrowseRowHeight = 68;
    constexpr int kBrowseVisibleRows = 7;
    constexpr int kListMarqueeStartDelayMs = 2000;
    constexpr int kListMarqueeEndPauseMs = 260;
    constexpr int kListMarqueeFadeDurationMs = 260;
    constexpr int kListMarqueeSpeedPxPerSec = 72;
    constexpr int kListMarqueePhasePause = 0;
    constexpr int kListMarqueePhaseScroll = 1;
    constexpr int kListMarqueePhaseEndPause = 2;
    constexpr int kListMarqueePhaseFadeOut = 3;
    constexpr int kListMarqueePhaseFadeIn = 4;

    class MarqueeClipElement : public pu::ui::elm::Element
    {
        public:
            MarqueeClipElement(bool beginClip, bool* enabled, int* clipX, int* clipY, int* clipW, int* clipH)
                : beginClip(beginClip), enabled(enabled), clipX(clipX), clipY(clipY), clipW(clipW), clipH(clipH)
            {}

            static pu::ui::elm::Element::Ref New(bool beginClip, bool* enabled, int* clipX, int* clipY, int* clipW, int* clipH)
            {
                return std::make_shared<MarqueeClipElement>(beginClip, enabled, clipX, clipY, clipW, clipH);
            }

            s32 GetX() override { return 0; }
            s32 GetY() override { return 0; }
            s32 GetWidth() override { return 0; }
            s32 GetHeight() override { return 0; }

            void OnRender(pu::ui::render::Renderer::Ref &Drawer, s32 X, s32 Y) override
            {
                (void)Drawer;
                (void)X;
                (void)Y;
                if (this->beginClip) {
                    if (!this->enabled || !(*this->enabled) || !this->clipW || !this->clipH || (*this->clipW <= 0) || (*this->clipH <= 0))
                        return;
                    SDL_Rect rect = { *this->clipX, *this->clipY, *this->clipW, *this->clipH };
                    SDL_RenderSetClipRect(pu::ui::render::GetMainRenderer(), &rect);
                    return;
                }
                SDL_RenderSetClipRect(pu::ui::render::GetMainRenderer(), NULL);
            }

            void OnInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) override
            {
                (void)Down;
                (void)Up;
                (void)Held;
                (void)Pos;
            }

        private:
            bool beginClip = true;
            bool* enabled = nullptr;
            int* clipX = nullptr;
            int* clipY = nullptr;
            int* clipW = nullptr;
            int* clipH = nullptr;
    };

    bool TryParseHexU64(const std::string& hex, std::uint64_t& out);

    pu::ui::Color BlendOverOpaque(const pu::ui::Color& base, const pu::ui::Color& overlay)
    {
        const int a = static_cast<int>(overlay.A);
        const int invA = 255 - a;
        pu::ui::Color out;
        out.R = static_cast<u8>((static_cast<int>(overlay.R) * a + static_cast<int>(base.R) * invA) / 255);
        out.G = static_cast<u8>((static_cast<int>(overlay.G) * a + static_cast<int>(base.G) * invA) / 255);
        out.B = static_cast<u8>((static_cast<int>(overlay.B) * a + static_cast<int>(base.B) * invA) / 255);
        out.A = 255;
        return out;
    }

    bool ShouldUseDarkText(const pu::ui::Color& bg)
    {
        const int luma = (static_cast<int>(bg.R) * 299) + (static_cast<int>(bg.G) * 587) + (static_cast<int>(bg.B) * 114);
        return luma >= (1000 * 150);
    }

    std::string NormalizeHex(std::string hex)
    {
        std::string out;
        out.reserve(hex.size());
        for (char c : hex) {
            if (std::isxdigit(static_cast<unsigned char>(c)))
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    }

    std::string TrimAscii(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return std::string();
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(start, (end - start) + 1);
    }

    bool TryParseTitleIdText(const std::string& value, std::uint64_t& out)
    {
        std::string text = TrimAscii(value);
        if (text.empty())
            return false;

        if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0)
            text = text.substr(2);

        bool hasHexLetters = false;
        bool allDigits = !text.empty();
        for (unsigned char c : text) {
            if (!std::isxdigit(c))
                return false;
            if (std::isalpha(c))
                hasHexLetters = true;
            if (!std::isdigit(c))
                allDigits = false;
        }

        if (hasHexLetters || text.size() == 16) {
            return TryParseHexU64(text, out);
        }

        if (allDigits) {
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
            if (end == text.c_str() || (end && *end != '\0'))
                return false;
            out = static_cast<std::uint64_t>(parsed);
            return true;
        }

        return false;
    }

    std::uint32_t DecodeUtf8CodePoint(const std::string& text, std::size_t& i)
    {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        if (c0 < 0x80) {
            i += 1;
            return c0;
        }
        if ((c0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            if ((c1 & 0xC0) == 0x80) {
                const std::uint32_t cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
                if (cp >= 0x80) {
                    i += 2;
                    return cp;
                }
            }
        } else if ((c0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
                const std::uint32_t cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
                if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
                    i += 3;
                    return cp;
                }
            }
        } else if ((c0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            const unsigned char c3 = static_cast<unsigned char>(text[i + 3]);
            if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
                const std::uint32_t cp = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
                if (cp >= 0x10000 && cp <= 0x10FFFF) {
                    i += 4;
                    return cp;
                }
            }
        }
        i += 1;
        return c0;
    }

    void AppendUtf8(std::string& out, std::uint32_t cp)
    {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool IsCombiningMark(std::uint32_t cp)
    {
        return (cp >= 0x0300 && cp <= 0x036F)
            || (cp >= 0x1AB0 && cp <= 0x1AFF)
            || (cp >= 0x1DC0 && cp <= 0x1DFF)
            || (cp >= 0x20D0 && cp <= 0x20FF)
            || (cp >= 0xFE20 && cp <= 0xFE2F);
    }

    char FoldLatinDiacritic(std::uint32_t cp)
    {
        switch (cp) {
            case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
            case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
            case 0x0100: case 0x0101: case 0x0102: case 0x0103: case 0x0104: case 0x0105:
                return 'a';
            case 0x00C7: case 0x00E7: case 0x0106: case 0x0107: case 0x0108: case 0x0109:
            case 0x010A: case 0x010B: case 0x010C: case 0x010D:
                return 'c';
            case 0x00D0: case 0x00F0: case 0x010E: case 0x010F: case 0x0110: case 0x0111:
                return 'd';
            case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: case 0x00E8: case 0x00E9:
            case 0x00EA: case 0x00EB: case 0x0112: case 0x0113: case 0x0114: case 0x0115:
            case 0x0116: case 0x0117: case 0x0118: case 0x0119: case 0x011A: case 0x011B:
                return 'e';
            case 0x011C: case 0x011D: case 0x011E: case 0x011F: case 0x0120: case 0x0121:
            case 0x0122: case 0x0123:
                return 'g';
            case 0x0124: case 0x0125: case 0x0126: case 0x0127:
                return 'h';
            case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: case 0x00EC: case 0x00ED:
            case 0x00EE: case 0x00EF: case 0x0128: case 0x0129: case 0x012A: case 0x012B:
            case 0x012C: case 0x012D: case 0x012E: case 0x012F: case 0x0130: case 0x0131:
                return 'i';
            case 0x0134: case 0x0135:
                return 'j';
            case 0x0136: case 0x0137: case 0x0138:
                return 'k';
            case 0x0139: case 0x013A: case 0x013B: case 0x013C: case 0x013D: case 0x013E:
            case 0x013F: case 0x0140: case 0x0141: case 0x0142:
                return 'l';
            case 0x00D1: case 0x00F1: case 0x0143: case 0x0144: case 0x0145: case 0x0146:
            case 0x0147: case 0x0148:
                return 'n';
            case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
            case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
            case 0x014C: case 0x014D: case 0x014E: case 0x014F: case 0x0150: case 0x0151:
                return 'o';
            case 0x0154: case 0x0155: case 0x0156: case 0x0157: case 0x0158: case 0x0159:
                return 'r';
            case 0x015A: case 0x015B: case 0x015C: case 0x015D: case 0x015E: case 0x015F:
            case 0x0160: case 0x0161:
                return 's';
            case 0x0162: case 0x0163: case 0x0164: case 0x0165: case 0x0166: case 0x0167:
                return 't';
            case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: case 0x00F9: case 0x00FA:
            case 0x00FB: case 0x00FC: case 0x0168: case 0x0169: case 0x016A: case 0x016B:
            case 0x016C: case 0x016D: case 0x016E: case 0x016F: case 0x0170: case 0x0171:
            case 0x0172: case 0x0173:
                return 'u';
            case 0x00DD: case 0x00FD: case 0x00FF: case 0x0176: case 0x0177: case 0x0178:
                return 'y';
            case 0x0179: case 0x017A: case 0x017B: case 0x017C: case 0x017D: case 0x017E:
                return 'z';
            default:
                return 0;
        }
    }

    std::string NormalizeSearchKey(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size();) {
            const std::uint32_t cp = DecodeUtf8CodePoint(text, i);
            if (cp < 0x80) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(cp))));
                continue;
            }
            if (IsCombiningMark(cp))
                continue;

            const char folded = FoldLatinDiacritic(cp);
            if (folded != 0) {
                out.push_back(folded);
                continue;
            }
            if (cp == 0x00DF) {
                out += "ss";
                continue;
            }
            if (cp == 0x00C6 || cp == 0x00E6) {
                out += "ae";
                continue;
            }
            if (cp == 0x0152 || cp == 0x0153) {
                out += "oe";
                continue;
            }
            if (cp == 0x00DE || cp == 0x00FE) {
                out += "th";
                continue;
            }

            AppendUtf8(out, cp);
        }
        return out;
    }

    bool TryParseDateSortKey(const std::string& text, std::uint64_t& out)
    {
        std::string digits;
        digits.reserve(14);
        for (unsigned char c : text) {
            if (std::isdigit(c))
                digits.push_back(static_cast<char>(c));
        }

        if (digits.size() < 8)
            return false;

        const unsigned year = static_cast<unsigned>(std::strtoul(digits.substr(0, 4).c_str(), nullptr, 10));
        if (year < 1900 || year > 9999)
            return false;

        std::string normalized = digits.substr(0, std::min<std::size_t>(digits.size(), 14));
        while (normalized.size() < 14)
            normalized.push_back('0');

        out = static_cast<std::uint64_t>(std::strtoull(normalized.c_str(), nullptr, 10));
        return true;
    }

    bool TryGetItemSortDateKey(const remoteInstStuff::RemoteItem& item, std::uint64_t& out)
    {
        if (item.saveCreatedTs > 0) {
            out = item.saveCreatedTs;
            return true;
        }
        if (!item.saveCreatedAt.empty())
            return TryParseDateSortKey(item.saveCreatedAt, out);
        if (item.hasReleaseDate && item.releaseDate > 0) {
            out = item.releaseDate;
            return true;
        }
        return false;
    }

    std::vector<std::size_t> BuildUtf8Boundaries(const std::string& text)
    {
        std::vector<std::size_t> boundaries;
        boundaries.reserve(text.size() + 1);
        boundaries.push_back(0);
        for (std::size_t i = 0; i < text.size();) {
            const std::size_t start = i;
            (void)DecodeUtf8CodePoint(text, i);
            if (i <= start)
                i = start + 1;
            boundaries.push_back(i);
        }
        return boundaries;
    }

    std::string ClipBrowseText(const std::string& text, int fontSize, int width) {
        const auto font = pu::ui::render::LoadDefaultFont(fontSize);
        const auto fallback = pu::ui::render::LoadSharedFont(pu::ui::render::SharedFont::NintendoExtended, fontSize);
        const auto fits = [&](const std::string& value) {
            int measuredWidth = 0, measuredHeight = 0;
            return font && TTF_SizeUTF8(font, fallback, value.c_str(), &measuredWidth, &measuredHeight) == 0
                && measuredWidth <= width;
        };
        if (fits(text)) return text;
        const auto boundaries = BuildUtf8Boundaries(text);
        int low = 0, high = static_cast<int>(boundaries.size()) - 1, best = 0;
        while (low <= high) {
            const int mid = (low + high) / 2;
            if (fits(text.substr(0, boundaries[mid]) + "...")) { best = mid; low = mid + 1; }
            else high = mid - 1;
        }
        return text.substr(0, boundaries[best]) + "...";
    }

    bool FitsSingleLineMenuRender(const std::string& text, int fontSize, int maxWidth, int maxHeight)
    {
        if (maxWidth <= 0 || maxHeight <= 0)
            return false;
        const auto font = pu::ui::render::LoadDefaultFont(fontSize);
        const auto meme = pu::ui::render::LoadSharedFont(pu::ui::render::SharedFont::NintendoExtended, fontSize);
        auto texture = pu::ui::render::RenderText(font, meme, text, COLOR("#FFFFFFFF"));
        if (texture == nullptr)
            return false;
        const int width = pu::ui::render::GetTextureWidth(texture);
        const int height = pu::ui::render::GetTextureHeight(texture);
        pu::ui::render::DeleteTexture(texture);
        return (width <= maxWidth) && (height <= maxHeight);
    }

    std::string ClipSingleLineByMenuRender(const std::string& text, int fontSize, int maxWidth, int maxHeight, bool* overflow = nullptr)
    {
        if (overflow != nullptr)
            *overflow = false;
        if (FitsSingleLineMenuRender(text, fontSize, maxWidth, maxHeight))
            return text;

        if (overflow != nullptr)
            *overflow = true;
        const auto boundaries = BuildUtf8Boundaries(text);
        int low = 0;
        int high = static_cast<int>(boundaries.size()) - 1;
        int best = -1;
        while (low <= high) {
            const int mid = low + ((high - low) / 2);
            const std::string candidate = text.substr(0, boundaries[static_cast<std::size_t>(mid)]);
            if (FitsSingleLineMenuRender(candidate, fontSize, maxWidth, maxHeight)) {
                best = mid;
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }

        if (best <= 0)
            return std::string();
        return text.substr(0, boundaries[static_cast<std::size_t>(best)]);
    }

    std::string ClipSingleLinePrefixSuffixByMenuRender(const std::string& prefix, const std::string& suffix, int fontSize, int maxWidth, int maxHeight, bool* overflow = nullptr)
    {
        if (overflow != nullptr)
            *overflow = false;

        const std::string full = prefix + suffix;
        if (FitsSingleLineMenuRender(full, fontSize, maxWidth, maxHeight))
            return full;

        if (overflow != nullptr)
            *overflow = true;
        const std::string marker = prefix.empty() ? std::string() : std::string("...");
        const auto boundaries = BuildUtf8Boundaries(prefix);
        int low = 0;
        int high = static_cast<int>(boundaries.size()) - 1;
        int best = -1;
        while (low <= high) {
            const int mid = low + ((high - low) / 2);
            std::string candidate = prefix.substr(0, boundaries[static_cast<std::size_t>(mid)]);
            if (!candidate.empty() && !marker.empty())
                candidate += marker;
            candidate += suffix;
            if (FitsSingleLineMenuRender(candidate, fontSize, maxWidth, maxHeight)) {
                best = mid;
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }

        if (best < 0) {
            bool suffixOverflow = false;
            return ClipSingleLineByMenuRender(suffix, fontSize, maxWidth, maxHeight, &suffixOverflow);
        }

        std::string clipped = prefix.substr(0, boundaries[static_cast<std::size_t>(best)]);
        if (!clipped.empty() && !marker.empty())
            clipped += marker;
        clipped += suffix;
        return clipped;
    }

    bool TryParseHexU64(const std::string& hex, std::uint64_t& out)
    {
        if (hex.empty())
            return false;
        char* end = nullptr;
        unsigned long long parsed = std::strtoull(hex.c_str(), &end, 16);
        if (end == hex.c_str() || (end && *end != '\0'))
            return false;
        out = static_cast<std::uint64_t>(parsed);
        return true;
    }

    bool TryResolveBaseTitleId(const remoteInstStuff::RemoteItem& item, std::uint64_t& outBaseId);

    bool DeriveBaseTitleId(const remoteInstStuff::RemoteItem& item, std::uint64_t& out)
    {
        return TryResolveBaseTitleId(item, out);
    }

    bool NormalizeAppTypeValue(std::int32_t rawValue, std::int32_t& out)
    {
        switch (rawValue) {
            case NcmContentMetaType_Application:
            case 0:
                out = NcmContentMetaType_Application;
                return true;
            case NcmContentMetaType_Patch:
            case 1:
                out = NcmContentMetaType_Patch;
                return true;
            case NcmContentMetaType_AddOnContent:
            case 2:
                out = NcmContentMetaType_AddOnContent;
                return true;
            default:
                return false;
        }
    }

    bool TryInferNormalizedAppType(const remoteInstStuff::RemoteItem& item, std::int32_t& outType)
    {
        if (NormalizeAppTypeValue(item.appType, outType))
            return true;

        if (item.hasAppId) {
            std::uint64_t parsedAppId = 0;
            if (TryParseTitleIdText(item.appId, parsedAppId)) {
                const std::uint64_t suffix = parsedAppId & 0xFFFULL;
                if (suffix == 0x000ULL)
                    outType = NcmContentMetaType_Application;
                else if (suffix == 0x800ULL)
                    outType = NcmContentMetaType_Patch;
                else
                    outType = NcmContentMetaType_AddOnContent;
                return true;
            }
        }

        if (!item.hasTitleId)
            return false;

        const std::uint64_t suffix = item.titleId & 0xFFFULL;
        if (suffix == 0x000ULL)
            outType = NcmContentMetaType_Application;
        else if (suffix == 0x800ULL)
            outType = NcmContentMetaType_Patch;
        else
            outType = NcmContentMetaType_AddOnContent;
        return true;
    }

    bool TryResolveBaseTitleId(const remoteInstStuff::RemoteItem& item, std::uint64_t& outBaseId)
    {
        // Some remotes publish title_id as the base title even for UPDATE/DLC entries.
        if (item.hasTitleId && ((item.titleId & 0xFFFULL) == 0x000ULL)) {
            outBaseId = item.titleId;
            return true;
        }

        std::uint64_t parsedAppId = 0;
        bool hasParsedAppId = false;
        if (item.hasAppId)
            hasParsedAppId = TryParseTitleIdText(item.appId, parsedAppId);

        if (hasParsedAppId) {
            const std::uint64_t suffix = parsedAppId & 0xFFFULL;
            NcmContentMetaType metaType = NcmContentMetaType_Application;
            if (suffix == 0x800ULL)
                metaType = NcmContentMetaType_Patch;
            else if (suffix != 0x000ULL)
                metaType = NcmContentMetaType_AddOnContent;
            outBaseId = tin::util::GetBaseTitleId(parsedAppId, metaType);
            return outBaseId != 0;
        }

        if (!item.hasTitleId)
            return false;

        std::int32_t inferredType = item.appType;
        if (inferredType >= 0) {
            std::int32_t normalizedType = -1;
            if (NormalizeAppTypeValue(inferredType, normalizedType))
                inferredType = normalizedType;
            else
                inferredType = -1;
        }
        if (inferredType < 0) {
            const std::uint64_t suffix = item.titleId & 0xFFFULL;
            if (suffix == 0x800ULL)
                inferredType = NcmContentMetaType_Patch;
            else if (suffix == 0x000ULL)
                inferredType = NcmContentMetaType_Application;
            else
                inferredType = NcmContentMetaType_AddOnContent;
        }

        NcmContentMetaType metaType = NcmContentMetaType_Application;
        if (inferredType >= 0)
            metaType = static_cast<NcmContentMetaType>(inferredType);
        outBaseId = tin::util::GetBaseTitleId(item.titleId, metaType);
        return outBaseId != 0;
    }

    // An item can answer yes to more than one of these (a base-suffixed title id whose name says
    // "DLC" is both), so this is a flag set, not an enum. Bit 0 only marks "already computed".
    constexpr std::uint8_t kTypeComputed = 1 << 0;
    constexpr std::uint8_t kTypeBase = 1 << 1;
    constexpr std::uint8_t kTypeUpdate = 1 << 2;
    constexpr std::uint8_t kTypeDlc = 1 << 3;

    std::uint8_t ClassifyItem(const remoteInstStuff::RemoteItem& item)
    {
        std::uint8_t flags = kTypeComputed;

        std::int32_t normalizedType = -1;
        if (TryInferNormalizedAppType(item, normalizedType)) {
            if (normalizedType == NcmContentMetaType_Application)
                flags |= kTypeBase;
            else if (normalizedType == NcmContentMetaType_Patch)
                flags |= kTypeUpdate;
            else if (normalizedType == NcmContentMetaType_AddOnContent)
                return flags | kTypeDlc;
        }

        auto containsDlcMarker = [](const std::string& text) {
            const std::string normalized = NormalizeSearchKey(text);
            return normalized.find("dlc") != std::string::npos
                || normalized.find("season pass") != std::string::npos
                || normalized.find("addon") != std::string::npos
                || normalized.find("add on") != std::string::npos
                || normalized.find("add-on") != std::string::npos
                || normalized.find("expansion") != std::string::npos;
        };

        if (containsDlcMarker(item.name) || containsDlcMarker(item.url) || (item.hasAppId && containsDlcMarker(item.appId)))
            flags |= kTypeDlc;
        return flags;
    }

    // ponytail: items built after the classification pass (installed/save entries) fall back to
    // computing on the spot. Cheap, because those lists are small.
    std::uint8_t ItemTypeFlags(const remoteInstStuff::RemoteItem& item)
    {
        return item.typeFlags != 0 ? item.typeFlags : ClassifyItem(item);
    }

    bool IsBaseItem(const remoteInstStuff::RemoteItem& item)
    {
        return (ItemTypeFlags(item) & kTypeBase) != 0;
    }

    bool IsUpdateItem(const remoteInstStuff::RemoteItem& item)
    {
        return (ItemTypeFlags(item) & kTypeUpdate) != 0;
    }

    bool IsDlcItem(const remoteInstStuff::RemoteItem& item)
    {
        return (ItemTypeFlags(item) & kTypeDlc) != 0;
    }

    std::string BuildItemIdentityKey(const remoteInstStuff::RemoteItem& item)
    {
        if (!item.url.empty())
            return "url:" + item.url;
        if (item.hasTitleId)
            return "tid:" + std::to_string(static_cast<unsigned long long>(item.titleId));
        if (item.hasAppId) {
            std::uint64_t parsedAppId = 0;
            if (TryParseTitleIdText(item.appId, parsedAppId))
                return "aid:" + std::to_string(static_cast<unsigned long long>(parsedAppId));
            return "aid:" + NormalizeHex(item.appId);
        }
        return std::string();
    }

    bool TryGetOfflineIconBaseId(const remoteInstStuff::RemoteItem& item, std::uint64_t& outBaseId)
    {
        return TryResolveBaseTitleId(item, outBaseId);
    }

    bool HasOfflineIconForItem(const remoteInstStuff::RemoteItem& item, std::uint64_t* outBaseId = nullptr)
    {
        std::uint64_t baseId = 0;
        if (!TryGetOfflineIconBaseId(item, baseId))
            return false;
        if (outBaseId)
            *outBaseId = baseId;
        return inst::offline::HasIcon(baseId);
    }

    bool TryLoadOfflineIconForItem(const remoteInstStuff::RemoteItem& item, std::vector<std::uint8_t>& outData)
    {
        std::uint64_t baseId = 0;
        if (!TryGetOfflineIconBaseId(item, baseId))
            return false;
        return inst::offline::TryGetIconData(baseId, outData);
    }

    std::string GetRemoteGridIconCachePath(const remoteInstStuff::RemoteItem& item)
    {
        if (!item.hasIconUrl)
            return "";

        const std::filesystem::path primaryDir(inst::config::remoteIconsDir);
        const std::filesystem::path legacyDir(inst::config::legacyShopIconsDir);
        std::error_code ec;
        bool primaryUsable = std::filesystem::exists(primaryDir, ec);
        if (!primaryUsable) {
            ec.clear();
            primaryUsable = std::filesystem::create_directory(primaryDir, ec);
        }
        const bool legacyUsable = std::filesystem::exists(legacyDir, ec);

        std::string urlPath = item.iconUrl;
        std::string ext = ".jpg";
        auto queryPos = urlPath.find('?');
        std::string cleanPath = queryPos == std::string::npos ? urlPath : urlPath.substr(0, queryPos);
        auto dotPos = cleanPath.find_last_of('.');
        if (dotPos != std::string::npos) {
            std::string suffix = cleanPath.substr(dotPos);
            if (suffix.size() <= 5 && suffix.find('/') == std::string::npos && suffix.find('?') == std::string::npos)
                ext = suffix;
        }

        std::string fileName;
        if (item.hasTitleId)
            fileName = std::to_string(item.titleId);
        else
            fileName = std::to_string(std::hash<std::string>{}(item.iconUrl));

        const std::filesystem::path primaryPath = primaryDir / (fileName + ext);
        const std::filesystem::path legacyPath = legacyDir / (fileName + ext);
        if (std::filesystem::exists(primaryPath))
            return primaryPath.string();
        if (std::filesystem::exists(legacyPath))
            return legacyPath.string();
        if (!primaryUsable && legacyUsable)
            return legacyPath.string();
        return primaryPath.string();
    }

    bool TryGetInstalledUpdateVersionNcm(u64 baseTitleId, u32& outVersion)
    {
        outVersion = 0;
        const u64 patchTitleId = baseTitleId ^ 0x800;
        const NcmStorageId storages[] = {NcmStorageId_BuiltInUser, NcmStorageId_SdCard};
        for (auto storage : storages) {
            NcmContentMetaDatabase db;
            if (R_FAILED(ncmOpenContentMetaDatabase(&db, storage)))
                continue;
            NcmContentMetaKey key = {};
            if (R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, patchTitleId))) {
                if (key.type == NcmContentMetaType_Patch && key.id == patchTitleId) {
                    if (key.version > outVersion)
                        outVersion = key.version;
                }
            }
            ncmContentMetaDatabaseClose(&db);
        }
        return outVersion > 0;
    }

    void CenterTextX(const TextBlock::Ref& text, int containerWidth = 1280)
    {
        int textX = (containerWidth - text->GetTextWidth()) / 2;
        if (textX < 0)
            textX = 0;
        text->SetX(textX);
    }

    std::string FormatSizeText(std::uint64_t bytes)
    {
        if (bytes == 0)
            return std::string();
        const double kb = 1024.0;
        const double mb = kb * 1024.0;
        const double gb = mb * 1024.0;
        char buf[32] = {0};
        if (bytes >= static_cast<std::uint64_t>(gb))
            std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / gb);
        else
            std::snprintf(buf, sizeof(buf), "%.0f MB", bytes / mb);
        return std::string(buf);
    }

    std::string FormatGridSizeSuffix(std::uint64_t bytes)
    {
        const std::string formatted = FormatSizeText(bytes);
        if (formatted.empty())
            return std::string();
        return " [" + formatted + "]";
    }

    std::string FormatReleaseDate(std::uint32_t yyyymmdd)
    {
        if (yyyymmdd == 0)
            return std::string();
        const std::uint32_t year = yyyymmdd / 10000;
        const std::uint32_t month = (yyyymmdd / 100) % 100;
        const std::uint32_t day = yyyymmdd % 100;
        if (year == 0 || month == 0 || day == 0)
            return std::to_string(yyyymmdd);
        char buf[16] = {0};
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", year, month, day);
        return std::string(buf);
    }

    std::string BuildGridTitleWithSize(const remoteInstStuff::RemoteItem& item)
    {
        const std::string suffix = FormatGridSizeSuffix(item.size);
        int nameLimit = 70;
        if (!suffix.empty()) {
            const int suffixChars = static_cast<int>(suffix.size()) + 1;
            if (nameLimit > suffixChars)
                nameLimit -= suffixChars;
        }
        if (nameLimit < 8)
            nameLimit = 8;
        return inst::util::shortenString(item.name, nameLimit, true) + suffix;
    }

    std::string NormalizeDescriptionWhitespace(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        bool inSpace = false;
        for (char c : text) {
            if (c == '\r')
                continue;
            if (c == '\n' || c == '\t' || c == ' ') {
                if (!inSpace) {
                    out.push_back(' ');
                    inSpace = true;
                }
                continue;
            }
            out.push_back(c);
            inSpace = false;
        }
        // Trim
        const auto first = out.find_first_not_of(' ');
        if (first == std::string::npos)
            return std::string();
        const auto last = out.find_last_not_of(' ');
        return out.substr(first, (last - first) + 1);
    }

    // Validate the bounded server JPEG without invoking image codecs on the worker.
    bool ReadScreenshotSize(const std::string& path, int& width, int& height) {
        width = height = 0;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (!file || size < 12 || size > 8 * 1024 * 1024) return false;
        const auto byte = [&]() -> int {
            char value;
            return file.get(value) ? static_cast<unsigned char>(value) : -1;
        };
        const auto word = [&]() -> int {
            const int high = byte(), low = byte();
            return high < 0 || low < 0 ? -1 : (high << 8) | low;
        };
        file.seekg(-2, std::ios::end);
        if (word() != 0xFFD9) return false;
        file.seekg(0);
        if (word() != 0xFFD8) return false;
        for (int segment = 0; segment < 256 && file; ++segment) {
            if (byte() != 0xFF) return false;
            int marker;
            do { marker = byte(); } while (marker == 0xFF);
            if (marker < 0 || marker == 0 || marker == 0xDA || marker == 0xD9) return false;
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
            const int length = word();
            if (length < 2) return false;
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                if (length < 8 || byte() != 8) return false;
                height = word();
                width = word();
                const int components = byte();
                return (components == 1 || components == 3) && length == 8 + 3 * components
                    && width > 0 && height > 0 && width <= 1280 && height <= 720;
            }
            file.seekg(length - 2, std::ios::cur);
        }
        return false;
    }

    std::vector<std::string> WrapDescriptionLines(const std::string& text, std::size_t maxLineChars)
    {
        std::vector<std::string> lines;
        if (text.empty() || maxLineChars == 0)
            return lines;

        std::istringstream iss(text);
        std::string word;
        std::string line;
        while (iss >> word) {
            if (word.size() > maxLineChars)
                word = word.substr(0, maxLineChars);

            if (line.empty()) {
                line = word;
                continue;
            }

            if ((line.size() + 1 + word.size()) <= maxLineChars) {
                line += " " + word;
                continue;
            }

            lines.push_back(line);
            line = word;
        }

        if (!line.empty())
            lines.push_back(line);
        return lines;
    }

    std::string WrapDescriptionText(const std::string& text, std::size_t maxLineChars, std::size_t maxLines)
    {
        if (text.empty() || maxLineChars == 0 || maxLines == 0)
            return std::string();

        std::vector<std::string> lines = WrapDescriptionLines(text, maxLineChars);
        bool truncated = lines.size() > maxLines;
        if (lines.size() > maxLines)
            lines.resize(maxLines);

        if (lines.empty())
            return std::string();

        if (truncated) {
            std::string& last = lines.back();
            if (last.size() + 3 <= maxLineChars)
                last += "...";
            else if (maxLineChars >= 3)
                last = last.substr(0, maxLineChars - 3) + "...";
        }

        std::string out;
        for (std::size_t i = 0; i < lines.size(); i++) {
            if (i > 0)
                out.push_back('\n');
            out += lines[i];
        }
        return out;
    }
}

namespace inst::ui {
    // Image/texture creation is confined to the UI thread, with explicit failure checks.
    class RemoteScreenshotImage : public pu::ui::elm::Element {
        public:
            ~RemoteScreenshotImage() override { this->Clear(); }
            void Clear() {
                pu::ui::render::DeleteTexture(this->texture);
                this->SetVisible(false);
            }
            bool SetImage(const std::string& path) {
                this->Clear();
                int sourceWidth = 0, sourceHeight = 0;
                if (!ReadScreenshotSize(path, sourceWidth, sourceHeight)) return false;
                SDL_Surface* surface = IMG_Load(path.c_str());
                if (!surface) return false;
                if (surface->w != sourceWidth || surface->h != sourceHeight) {
                    SDL_FreeSurface(surface);
                    return false;
                }
                this->texture = SDL_CreateTextureFromSurface(pu::ui::render::GetMainRenderer(), surface);
                SDL_FreeSurface(surface);
                if (!this->texture) return false;
                const double scale = std::min(1.0, std::min(624.0 / sourceWidth, 351.0 / sourceHeight));
                this->options.Width = std::max(1, static_cast<int>(sourceWidth * scale));
                this->options.Height = std::max(1, static_cast<int>(sourceHeight * scale));
                this->x = 600 + (624 - this->options.Width) / 2;
                this->y = 170 + (351 - this->options.Height) / 2;
                return true;
            }
            bool IsImageValid() const { return this->texture != nullptr; }
            s32 GetX() override { return this->x; }
            s32 GetY() override { return this->y; }
            s32 GetWidth() override { return this->options.Width; }
            s32 GetHeight() override { return this->options.Height; }
            void OnRender(pu::ui::render::Renderer::Ref& drawer, s32 x, s32 y) override {
                if (this->texture) drawer->RenderTexture(this->texture, x, y, this->options);
            }
            void OnInput(u64, u64, u64, pu::ui::Touch) override {}
        private:
            pu::ui::render::NativeTexture texture = nullptr;
            pu::ui::render::NativeTextureRenderOptions options = pu::ui::render::NativeTextureRenderOptions::Default;
            s32 x = 600, y = 170;
    };

    extern MainApplication *mainApp;

    remoteInstPage::remoteInstPage() : Layout::Layout() {
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
        this->pageInfoText = TextBlock::New(10, 81, "", 26);
        this->pageInfoText->SetColor(COLOR("#FFFFFFFF"));
        this->loadingProgressText = TextBlock::New(0, 320, "", 24);
        this->loadingProgressText->SetColor(COLOR("#FFFFFFFF"));
        this->loadingProgressText->SetVisible(false);
        this->loadingBarBack = Rectangle::New(260, 386, 760, 8, inst::theme::Color("progress_track", COLOR("#FFFFFF33")), 4);
        this->loadingBarBack->SetVisible(false);
        this->loadingBarFill = Rectangle::New(260, 386, 0, 8, inst::theme::Color("accent", COLOR("#34C759FF")), 4);
        this->loadingBarFill->SetVisible(false);
        this->loadingStagesBack = Rectangle::New(220, 220, 840, 260, inst::theme::Color("panel", inst::config::oledMode ? COLOR("#101010EE") : COLOR("#170909EE")), 18);
        this->loadingStagesBack->SetVisible(false);
        this->loadingStagesText = TextBlock::New(260, 252, "", 32);
        this->loadingStagesText->SetColor(COLOR("#E9FFF2FF"));
        this->loadingStagesText->SetVisible(false);
        this->loadingDetailText = TextBlock::New(260, 420, "", 20);
        this->loadingDetailText->SetColor(COLOR("#FFFFFFBB"));
        this->loadingDetailText->SetVisible(false);
        this->searchInfoText = TextBlock::New(24, 116, "", 16);
        this->searchInfoText->SetColor(COLOR("#FFFFFFFF"));
        this->searchInfoText->SetVisible(false);
        this->butText = TextBlock::New(24, 672, "", 18);
        this->butText->SetColor(COLOR("#FFFFFFFF"));
        this->secondaryHintText = TextBlock::New(24, 698, "", 16);
        this->secondaryHintText->SetColor(inst::theme::Color("muted_text", COLOR("#A8A8B8FF")));
        this->secondaryHintText->SetVisible(false);
        this->setButtonsText("inst.remote.buttons_loading"_lang);
        this->menu = inst::theme::Menu::New(kBrowseListX, kBrowseListY, kBrowseListWidth, COLOR("#17171CCC"), kBrowseRowHeight, kBrowseVisibleRows, 22);
        this->menu->SetOnRowsRendered([this] {
            if (this->remoteGridMode || this->loadingProgressText->IsVisible()) return;
            const int top = this->menu->GetVisibleTopIndex();
            if (top != this->listVisibleTopIndex) {
                this->listVisibleTopIndex = top;
                this->updateListMarquee(false);
                this->updateBrowseDecorations();
            }
        });
        if (inst::config::oledMode) {
            this->menu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#FFFFFF33")));
            this->menu->SetScrollbarColor(inst::theme::Color("scrollbar", COLOR("#FFFFFF66")));
        } else {
            this->menu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#00000033")));
            this->menu->SetScrollbarColor(inst::theme::Color("scrollbar", COLOR("#17090980")));
        }
        this->infoImage = Image::New(34, 90, inst::theme::Asset("romfs:/images/icons/remote-connection-waiting.png"));
        this->previewImage = Image::New(920, 170, inst::theme::Asset("romfs:/images/icons/title-placeholder.png"));
        this->previewImage->SetWidth(304);
        this->previewImage->SetHeight(304);
        this->gridHighlight = Rectangle::New(0, 0, kGridTileWidth + 8, kGridTileHeight + 8, inst::theme::Color("accent", COLOR("#FF4D4DFF")), 8);
        this->gridHighlight->SetVisible(false);
        this->gridImages.reserve(kGridItemsPerPage);
        for (int i = 0; i < kGridItemsPerPage; i++) {
            auto img = Image::New(0, 0, inst::theme::Asset("romfs:/images/icons/title-placeholder.png"));
            img->SetWidth(kGridTileWidth);
            img->SetHeight(kGridTileHeight);
            img->SetVisible(false);
            this->gridImages.push_back(img);
        }
        auto selectedColor = pu::ui::Color::FromHex("#34C759FF");
        this->remoteGridSelectHighlights.reserve(kGridItemsPerPage);
        for (int i = 0; i < kGridItemsPerPage; i++) {
            auto highlight = Rectangle::New(0, 0, kGridTileWidth + 8, kGridTileHeight + 8, selectedColor);
            highlight->SetVisible(false);
            this->remoteGridSelectHighlights.push_back(highlight);
        }
        this->remoteGridSelectIcons.reserve(kGridItemsPerPage);
        for (int i = 0; i < kGridItemsPerPage; i++) {
            auto badge = Rectangle::New(0, 0, 56, 56, pu::ui::Color::FromHex("#147D43FF"), 28);
            badge->SetVisible(false);
            this->remoteGridSelectBadgeBacks.push_back(badge);
            auto icon = TextBlock::New(0, 0, "✓", 40);
            icon->SetColor(pu::ui::Color::FromHex("#FFFFFFFF"));
            icon->SetVisible(false);
            this->remoteGridSelectIcons.push_back(icon);
        }
        this->gridTitleText = TextBlock::New(10, 649, "", 18);
        this->gridTitleText->SetColor(inst::theme::Color("selected_text", COLOR("#FFFFFFFF")));
        this->gridTitleText->SetVisible(false);
        this->imageLoadingText = TextBlock::New(0, 116, "Fetching images...", 16);
        this->imageLoadingText->SetColor(COLOR("#FFFFFFFF"));
        this->imageLoadingText->SetVisible(false);
        this->listMarqueeMaskRect = Rectangle::New(0, 0, 0, 0, this->menu->GetOnFocusColor());
        this->listMarqueeMaskRect->SetVisible(false);
        this->listMarqueeTintRect = Rectangle::New(0, 0, 0, 0, this->menu->GetOnFocusColor());
        this->listMarqueeTintRect->SetVisible(false);
        this->listMarqueeOverlayText = TextBlock::New(0, 0, "", 22);
        this->listMarqueeOverlayText->SetColor(COLOR("#FFFFFFFF"));
        this->listMarqueeOverlayText->SetVisible(false);
        this->listMarqueeClipBegin = MarqueeClipElement::New(true, &this->listMarqueeClipEnabled, &this->listMarqueeClipX, &this->listMarqueeClipY, &this->listMarqueeClipW, &this->listMarqueeClipH);
        this->listMarqueeClipEnd = MarqueeClipElement::New(false, &this->listMarqueeClipEnabled, &this->listMarqueeClipX, &this->listMarqueeClipY, &this->listMarqueeClipW, &this->listMarqueeClipH);
        this->listMarqueeFadeRect = Rectangle::New(0, 0, 0, 0, COLOR("#00000000"));
        this->listMarqueeFadeRect->SetVisible(false);
        this->debugText = TextBlock::New(10, 620, "", 18);
        this->debugText->SetColor(COLOR("#FFFFFFFF"));
        this->debugText->SetVisible(false);
        this->emptySectionText = TextBlock::New(0, 350, "", 28);
        this->emptySectionText->SetColor(COLOR("#FFFFFFFF"));
        this->emptySectionText->SetVisible(false);
        this->descriptionRect = Rectangle::New(10, 508, 1260, 142, inst::config::oledMode ? COLOR("#000000CC") : COLOR("#170909CC"));
        this->descriptionRect->SetVisible(false);
        this->descriptionText = TextBlock::New(22, 518, "", 18);
        this->descriptionText->SetColor(COLOR("#FFFFFFFF"));
        this->descriptionText->SetVisible(false);
        this->descriptionOverlayRect = Rectangle::New(24, 86, 1232, 564, inst::config::oledMode ? COLOR("#000000EE") : COLOR("#170909EE"));
        this->descriptionOverlayRect->SetVisible(false);
        this->descriptionOverlayTitleText = TextBlock::New(46, 102, "", 24);
        this->descriptionOverlayTitleText->SetColor(COLOR("#FFFFFFFF"));
        this->descriptionOverlayTitleText->SetVisible(false);
        this->descriptionOverlayBodyText = TextBlock::New(46, 142, "", 19);
        this->descriptionOverlayBodyText->SetColor(COLOR("#FFFFFFFF"));
        this->descriptionOverlayBodyText->SetVisible(false);
        this->descriptionOverlayHintText = TextBlock::New(46, 548, "", 18);
        this->descriptionOverlayHintText->SetColor(inst::theme::Color("muted_text", COLOR("#A8A8B8FF")));
        this->descriptionOverlayHintText->SetVisible(false);
        this->descriptionScreenshotBack = Rectangle::New(592, 162, 640, 367, inst::theme::Color("panel", COLOR("#17171CFF")), 10);
        this->descriptionScreenshotBack->SetVisible(false);
        this->descriptionScreenshotImage = std::make_shared<RemoteScreenshotImage>();
        this->descriptionScreenshotImage->SetVisible(false);
        this->descriptionScreenshotStatus = TextBlock::New(600, 548, "", 18);
        this->descriptionScreenshotStatus->SetColor(inst::theme::Color("muted_text", COLOR("#A8A8B8FF")));
        this->descriptionScreenshotStatus->SetVisible(false);
        this->saveVersionSelectorRect = Rectangle::New(90, 96, 1100, 548, inst::config::oledMode ? COLOR("#000000EE") : COLOR("#170909EE"));
        this->saveVersionSelectorRect->SetVisible(false);
        this->saveVersionSelectorTitleText = TextBlock::New(114, 112, "", 24);
        this->saveVersionSelectorTitleText->SetColor(COLOR("#FFFFFFFF"));
        this->saveVersionSelectorTitleText->SetVisible(false);
        this->saveVersionSelectorMenu = inst::theme::Menu::New(114, 152, 1052, COLOR("#FFFFFF00"), 44, 8, 20);
        if (inst::config::oledMode) {
            this->saveVersionSelectorMenu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#FFFFFF33")));
            this->saveVersionSelectorMenu->SetScrollbarColor(inst::theme::Color("scrollbar", COLOR("#FFFFFF66")));
        } else {
            this->saveVersionSelectorMenu->SetOnFocusColor(inst::theme::Color("highlight", COLOR("#00000033")));
            this->saveVersionSelectorMenu->SetScrollbarColor(inst::theme::Color("scrollbar", COLOR("#17090980")));
        }
        this->saveVersionSelectorMenu->SetVisible(false);
        this->saveVersionSelectorDetailText = TextBlock::New(114, 524, "", 18);
        this->saveVersionSelectorDetailText->SetColor(COLOR("#FFFFFFFF"));
        this->saveVersionSelectorDetailText->SetVisible(false);
        this->saveVersionSelectorHintText = TextBlock::New(114, 614, "A Download    B Back", 18);
        this->saveVersionSelectorHintText->SetColor(COLOR("#FFFFFFFF"));
        this->saveVersionSelectorHintText->SetVisible(false);
        this->sectionHighlight = Rectangle::New(430, 78, 420, 32, inst::theme::Color("highlight", COLOR("#FFFFFF22")), 12);
        this->previousSectionText = TextBlock::New(24, 85, "", 18);
        this->nextSectionText = TextBlock::New(900, 85, "", 18);
        this->browseSummaryText = TextBlock::New(900, 649, "", 16);
        this->browseDetailPanel = Rectangle::New(888, 150, 368, 486, inst::theme::Color("panel", COLOR("#17171CEE")), 12);
        this->focusedTitleText = TextBlock::New(912, 491, "", 22);
        this->focusedMetadataText = TextBlock::New(912, 532, "", 17);
        for (auto text : {this->previousSectionText, this->nextSectionText, this->browseSummaryText, this->focusedMetadataText})
            text->SetColor(inst::theme::Color("muted_text", COLOR("#A8A8B8FF")));
        this->focusedTitleText->SetColor(COLOR("#FFFFFFFF"));
        for (int i = 0; i < kGridItemsPerPage; ++i) {
            const int x = kGridStartX + (i % kGridCols) * (kGridTileWidth + kGridGap);
            const int y = kGridStartY + (i / kGridCols) * (kGridTileHeight + kGridGap);
            auto card = Rectangle::New(x - 6, y - 6, kGridTileWidth + 12, kGridTileHeight + 30,
                inst::theme::Color("panel", COLOR("#17171CEE")), 10);
            card->SetVisible(false);
            this->gridCardBacks.push_back(card);
            auto caption = TextBlock::New(x, y + kGridTileHeight + 4, "", 15);
            caption->SetColor(COLOR("#FFFFFFFF"));
            caption->SetVisible(false);
            this->gridCaptions.push_back(caption);
        }
        for (int i = 0; i < kBrowseVisibleRows; ++i) {
            const int y = kBrowseListY + i * kBrowseRowHeight;
            auto cover = Image::New(49, y + 5, inst::theme::Asset("romfs:/images/icons/title-placeholder.png"));
            cover->SetWidth(58); cover->SetHeight(58); cover->SetVisible(false);
            this->listRowCovers.push_back(cover);
            this->listRowCoverKeys.push_back("");
            auto detail = TextBlock::New(132, y + 50, "", 14);
            detail->SetColor(inst::theme::Color("muted_text", COLOR("#A8A8B8FF")));
            this->listRowMetadata.push_back(detail);
            auto size = TextBlock::New(760, y + 20, "", 17);
            size->SetColor(COLOR("#FFFFFFFF"));
            this->listRowSizes.push_back(size);
            auto selected = TextBlock::New(754, y + 46, "", 14);
            selected->SetColor(inst::theme::Color("accent", COLOR("#FF7777FF")));
            this->listRowSelections.push_back(selected);
            for (auto text : {detail, size, selected}) text->SetVisible(false);
        }
        for (auto text : {this->previousSectionText, this->nextSectionText, this->browseSummaryText,
                this->focusedTitleText, this->focusedMetadataText}) text->SetVisible(false);
        this->sectionHighlight->SetVisible(false);
        this->browseDetailPanel->SetVisible(false);
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
        this->Add(this->secondaryHintText);
        this->Add(this->sectionHighlight);
        this->Add(this->pageInfoText);
        this->Add(this->loadingStagesBack);
        this->Add(this->loadingStagesText);
        this->Add(this->loadingProgressText);
        this->Add(this->loadingBarBack);
        this->Add(this->loadingBarFill);
        this->Add(this->loadingDetailText);
        this->Add(this->searchInfoText);
        this->Add(this->previousSectionText);
        this->Add(this->nextSectionText);
        this->Add(this->browseSummaryText);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        this->Add(this->menu);
#pragma GCC diagnostic pop
        this->Add(this->infoImage);
        this->Add(this->browseDetailPanel);
        this->Add(this->previewImage);
        this->Add(this->focusedTitleText);
        this->Add(this->focusedMetadataText);
        for (auto& cover : this->listRowCovers) this->Add(cover);
        for (auto& text : this->listRowMetadata) this->Add(text);
        for (auto& text : this->listRowSizes) this->Add(text);
        for (auto& text : this->listRowSelections) this->Add(text);
        for (auto& card : this->gridCardBacks) this->Add(card);
        for (auto& highlight : this->remoteGridSelectHighlights)
            this->Add(highlight);
        // Keep focus behind cover art and selection marks for opaque theme colors.
        this->Add(this->gridHighlight);
        for (auto& img : this->gridImages)
            this->Add(img);
        for (auto& badge : this->remoteGridSelectBadgeBacks) this->Add(badge);
        for (auto& icon : this->remoteGridSelectIcons)
            this->Add(icon);
        for (auto& text : this->gridCaptions) this->Add(text);
        this->gridCaptionMarquee = OverflowText::New(15, inst::theme::Color("accent", COLOR("#FF7777FF")));
        auto captionBackground = inst::theme::Color("panel", COLOR("#17171CEE"));
        captionBackground.A = 255;
        this->gridCaptionMarquee->SetBackgroundColor(captionBackground);
        this->gridCaptionMarquee->Attach(this);
        this->gridCaptionMarquee->SetVisible(false);
        this->Add(this->gridTitleText);
        this->Add(this->imageLoadingText);
        this->Add(this->listMarqueeMaskRect);
        this->Add(this->listMarqueeTintRect);
        this->Add(this->listMarqueeClipBegin);
        this->Add(this->listMarqueeOverlayText);
        this->Add(this->listMarqueeClipEnd);
        this->Add(this->listMarqueeFadeRect);
        this->Add(this->debugText);
        this->Add(this->emptySectionText);
        this->Add(this->descriptionRect);
        this->Add(this->descriptionText);
        this->Add(this->descriptionOverlayRect);
        this->Add(this->descriptionOverlayTitleText);
        this->Add(this->descriptionOverlayBodyText);
        this->Add(this->descriptionOverlayHintText);
        this->Add(this->descriptionScreenshotBack);
        this->Add(this->descriptionScreenshotImage);
        this->Add(this->descriptionScreenshotStatus);
        this->Add(this->saveVersionSelectorRect);
        this->Add(this->saveVersionSelectorTitleText);
        this->Add(this->saveVersionSelectorMenu);
        this->Add(this->saveVersionSelectorDetailText);
        this->Add(this->saveVersionSelectorHintText);
        this->AddThread([this] { this->updateDescriptionMedia(); this->updateBrowseDecorations(); });
        const std::string screenshotCacheDir = inst::config::appDir + "/cache/screenshots";
        this->descriptionMediaThread = std::thread([this, screenshotCacheDir] { this->descriptionMediaThreadMain(screenshotCacheDir); });
        this->iconDownloadThread = std::thread([this]() {
            this->iconDownloadThreadMain();
        });
    }

    remoteInstPage::~remoteInstPage() {
        this->stopIconDownloadWorker();
    }

    void remoteInstPage::stopIconDownloadWorker() {
        this->descriptionMediaStop.store(true);
        this->descriptionMediaGeneration.fetch_add(1);
        this->descriptionMediaCv.notify_all();
        if (this->descriptionMediaThread.joinable()) this->descriptionMediaThread.join();
        this->iconDownloadStopRequested.store(true);
        this->iconDownloadCv.notify_all();
        if (this->iconDownloadThread.joinable())
            this->iconDownloadThread.join();
    }

    void remoteInstPage::resetIconDownloadState() {
        {
            std::lock_guard<std::mutex> lock(this->iconDownloadMutex);
            this->iconDownloadGeneration++;
            this->iconDownloadQueue.clear();
            this->iconDownloadQueuedKeys.clear();
            this->iconDownloadTotal = 0;
            this->iconDownloadCompleted = 0;
        }
        this->iconDownloadUiDirty.store(false);
        this->imageLoadingUntilTick = 0;
        this->imageLoadingText->SetVisible(false);
    }

    void remoteInstPage::queueInstallIcon(const std::string& key, const std::string& iconUrl, const std::string& filePath) {
        if (iconUrl.empty() || filePath.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(filePath).parent_path(), error);
        if (error) return;
        remoteInstStuff::RemoteItem item{};
        item.url = key; item.iconUrl = iconUrl; item.hasIconUrl = true;
        queueIconDownload(item, filePath);
    }

    void remoteInstPage::queueIconDownload(const remoteInstStuff::RemoteItem& item, const std::string& filePath) {
        if (!item.hasIconUrl || item.iconUrl.empty() || filePath.empty())
            return;

        std::string key = BuildItemIdentityKey(item);
        if (key.empty())
            key = item.iconUrl;

        std::lock_guard<std::mutex> lock(this->iconDownloadMutex);
        if (this->iconDownloadQueuedKeys.count(key))
            return;

        this->iconDownloadQueuedKeys.insert(key);
        this->iconDownloadQueue.push_back({this->iconDownloadGeneration, key, item.iconUrl, filePath});
        this->iconDownloadTotal++;
        this->iconDownloadCv.notify_one();
    }

    void remoteInstPage::refreshImageLoadingText(bool showCompleted) {
        std::size_t total = 0;
        std::size_t completed = 0;
        {
            std::lock_guard<std::mutex> lock(this->iconDownloadMutex);
            total = this->iconDownloadTotal;
            completed = this->iconDownloadCompleted;
        }

        if (total == 0) {
            this->imageLoadingText->SetVisible(false);
            return;
        }

        this->imageLoadingText->SetText(
            "Fetching images " + std::to_string(completed) + "/" + std::to_string(total));
        this->imageLoadingText->SetX(990);

        if (completed < total) {
            this->imageLoadingText->SetVisible(true);
            return;
        }

        if (showCompleted && this->imageLoadingUntilTick == 0) {
            const u64 now = armGetSystemTick();
            const u64 freq = armGetSystemTickFreq();
            this->imageLoadingUntilTick = now + (freq * 2);
        }

        if (this->imageLoadingUntilTick > 0) {
            const u64 now = armGetSystemTick();
            const bool show = now < this->imageLoadingUntilTick;
            this->imageLoadingText->SetVisible(show);
            if (!show) {
                this->imageLoadingUntilTick = 0;
                std::lock_guard<std::mutex> lock(this->iconDownloadMutex);
                this->iconDownloadTotal = 0;
                this->iconDownloadCompleted = 0;
                this->iconDownloadQueuedKeys.clear();
            }
            return;
        }

        this->imageLoadingText->SetVisible(false);
    }

    void remoteInstPage::iconDownloadThreadMain() {
        while (true) {
            IconDownloadRequest request;
            {
                std::unique_lock<std::mutex> lock(this->iconDownloadMutex);
                this->iconDownloadCv.wait(lock, [this]() {
                    return this->iconDownloadStopRequested.load() || !this->iconDownloadQueue.empty();
                });
                if (this->iconDownloadStopRequested.load())
                    return;

                request = this->iconDownloadQueue.front();
                this->iconDownloadQueue.pop_front();
            }

            bool ok = true;
            const std::string tempPath = request.filePath + ".part";
            if (!std::filesystem::exists(request.filePath)) {
                if (std::filesystem::exists(tempPath))
                    std::filesystem::remove(tempPath);

                ok = inst::curl::downloadImageWithAuth(request.iconUrl, tempPath.c_str(), inst::config::remoteUser, inst::config::remotePass, 8000);
                if (ok) {
                    std::error_code ec;
                    std::filesystem::rename(tempPath, request.filePath, ec);
                    ok = !ec;
                }
            }

            if (std::filesystem::exists(tempPath))
                std::filesystem::remove(tempPath);
            if (!ok && std::filesystem::exists(request.filePath))
                std::filesystem::remove(request.filePath);

            bool refreshCurrentUi = false;
            {
                std::lock_guard<std::mutex> lock(this->iconDownloadMutex);
                if (request.generation == this->iconDownloadGeneration) {
                    this->iconDownloadCompleted++;
                    refreshCurrentUi = true;
                }
            }

            if (refreshCurrentUi)
                this->iconDownloadUiDirty.store(true);
        }
    }

    bool remoteInstPage::isAllSection() const {
        if (this->remoteSections.empty())
            return false;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= (int)this->remoteSections.size())
            return false;
        return this->remoteSections[this->selectedSectionIndex].id == "all";
    }

    bool remoteInstPage::isInstalledSection() const {
        if (this->remoteSections.empty())
            return false;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= (int)this->remoteSections.size())
            return false;
        return this->remoteSections[this->selectedSectionIndex].id == "installed";
    }

    bool remoteInstPage::isSaveSyncSection() const {
        if (!this->saveSyncEnabled)
            return false;
        if (this->remoteSections.empty())
            return false;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= (int)this->remoteSections.size())
            return false;
        const std::string id = this->remoteSections[this->selectedSectionIndex].id;
        return id == "saves" || id == "save";
    }

    bool remoteInstPage::isCheatsSection() const {
        if (this->remoteSections.empty() || this->selectedSectionIndex < 0 || this->selectedSectionIndex >= (int)this->remoteSections.size())
            return false;
        return this->remoteSections[this->selectedSectionIndex].id == "cheats";
    }

    bool remoteInstPage::isCheatCompatible(const remoteInstStuff::RemoteItem& item) const {
        if (!item.isCheat)
            return false;
        std::uint64_t titleId = 0;
        if (!TryParseHexU64(item.cheatTitleId, titleId))
            return false;
        // Horizon's title metadata does not expose program build IDs. A local
        // Atmosphere cheat is therefore the only reliable build-ID signal.
        const auto titleIt = this->installedSnapshot.baseInstalled.find(titleId);
        if (titleIt == this->installedSnapshot.baseInstalled.end() || !titleIt->second)
            return false;
        std::string localError;
        const auto localCheats = inst::cheats::List(item.cheatTitleId, localError);
        if (localCheats.empty())
            return true; // No local build signal: keep the server candidate visible.
        return inst::cheats::IsInstalled(item.cheatTitleId, item.cheatBuildId);
    }

    const std::vector<remoteInstStuff::RemoteItem>& remoteInstPage::getCurrentItems() const {
        static const std::vector<remoteInstStuff::RemoteItem> empty;
        if (this->remoteSections.empty())
            return empty;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= (int)this->remoteSections.size())
            return empty;
        if (queueAddMode && !isQueueSection(selectedSectionIndex)) return empty;
        return this->remoteSections[this->selectedSectionIndex].items;
    }

    void remoteInstPage::applyAllSectionSort() {
        if (this->remoteSections.empty())
            return;

        auto it = std::find_if(this->remoteSections.begin(), this->remoteSections.end(), [](const auto& section) {
            return section.id == "all";
        });
        if (it == this->remoteSections.end())
            return;

        auto byNameAsc = [](const remoteInstStuff::RemoteItem& a, const remoteInstStuff::RemoteItem& b) {
            return inst::util::ignoreCaseCompare(a.name, b.name);
        };

        auto byDateAsc = [&](const remoteInstStuff::RemoteItem& a, const remoteInstStuff::RemoteItem& b) {
            if (a.hasReleaseDate != b.hasReleaseDate)
                return a.hasReleaseDate;
            if (a.hasReleaseDate && b.hasReleaseDate && a.releaseDate != b.releaseDate)
                return a.releaseDate < b.releaseDate;
            return byNameAsc(a, b);
        };

        auto byDateDesc = [&](const remoteInstStuff::RemoteItem& a, const remoteInstStuff::RemoteItem& b) {
            if (a.hasReleaseDate != b.hasReleaseDate)
                return a.hasReleaseDate;
            if (a.hasReleaseDate && b.hasReleaseDate && a.releaseDate != b.releaseDate)
                return a.releaseDate > b.releaseDate;
            return byNameAsc(a, b);
        };

        switch (this->allSortMode) {
            default:
            case 0:
                std::sort(it->items.begin(), it->items.end(), byNameAsc);
                break;
            case 1:
                std::sort(it->items.begin(), it->items.end(), [&](const auto& a, const auto& b) {
                    return byNameAsc(b, a);
                });
                break;
            case 2:
                std::sort(it->items.begin(), it->items.end(), byDateAsc);
                break;
            case 3:
                std::sort(it->items.begin(), it->items.end(), byDateDesc);
                break;
        }
    }

    std::string remoteInstPage::getAllSortModeLabel() const {
        switch (this->allSortMode) {
            case 1:
                return "Name Z-A";
            case 2:
                return "Date Old-New";
            case 3:
                return "Date New-Old";
            default:
                return "Name A-Z";
        }
    }

    void remoteInstPage::updateSectionText() {
        this->pageInfoText->SetVisible(true);
        if (this->remoteSections.empty()) {
            this->pageInfoText->SetText("inst.remote.loading"_lang);
            this->pageInfoText->SetY(96);
            CenterTextX(this->pageInfoText);
            this->searchInfoText->SetVisible(false);
            return;
        }
        const auto& section = this->remoteSections[this->selectedSectionIndex];
        this->pageInfoText->SetText(ClipBrowseText(queueAddMode ? "Add to queue  •  " + section.title : section.title, 26, 396));
        this->pageInfoText->SetY(81);
        CenterTextX(this->pageInfoText);
        std::string rightInfo = std::to_string(section.items.size()) + " titles";
        if (!this->searchQuery.empty()) {
            std::string query = inst::util::shortenString(this->searchQuery, 28, true);
            rightInfo += "   •   Search: " + query;
        }
        if (this->isAllSection()) {
            if (!rightInfo.empty())
                rightInfo += " | ";
            rightInfo += "Sort: " + this->getAllSortModeLabel();
        } else {
            if (!rightInfo.empty())
                rightInfo += " | ";
            rightInfo += "Sort: ";
            rightInfo += this->getBrowseSortLabel();
        }

        if (!rightInfo.empty()) {
            this->searchInfoText->SetText(rightInfo);
            this->searchInfoText->SetX(24);
            this->searchInfoText->SetVisible(true);
        } else {
            this->searchInfoText->SetVisible(false);
        }
    }

    void remoteInstPage::updateBrowseDecorations() {
        const bool blocked = this->loadingProgressText->IsVisible() || this->saveVersionSelectorVisible
            || this->descriptionOverlayVisible;
        // Do not read browser data while the refresh worker owns it.
        if (blocked || this->remoteSections.empty()) {
            this->sectionHighlight->SetVisible(false);
            this->previousSectionText->SetVisible(false);
            this->nextSectionText->SetVisible(false);
            this->browseSummaryText->SetVisible(false);
            this->browseDetailPanel->SetVisible(false);
            this->focusedTitleText->SetVisible(false);
            this->focusedMetadataText->SetVisible(false);
            for (auto& badge : this->remoteGridSelectBadgeBacks) badge->SetVisible(false);
            this->gridCaptionMarquee->SetVisible(false);
            this->gridCaptionMarqueeKey.clear();
            for (auto& card : this->gridCardBacks) card->SetVisible(false);
            for (auto& text : this->gridCaptions) text->SetVisible(false);
            for (auto& text : this->listRowMetadata) text->SetVisible(false);
            for (auto& text : this->listRowSizes) text->SetVisible(false);
            for (auto& text : this->listRowSelections) text->SetVisible(false);
            for (auto& image : this->listRowCovers) image->SetVisible(false);
            if (blocked) this->secondaryHintText->SetVisible(false);
            this->browseDecorationKey.clear();
            return;
        }
        this->secondaryHintText->SetVisible(!this->suppressBottomHints && !this->secondaryHintText->GetText().empty());
        if (!this->remoteGridMode) this->listVisibleTopIndex = this->menu->GetVisibleTopIndex();
        const bool fastScroll = this->isFastListScrolling();
        const int selected = this->remoteGridMode ? this->remoteGridIndex : this->menu->GetSelectedIndex();
        std::size_t completed = 0;
        { std::lock_guard<std::mutex> lock(this->iconDownloadMutex); completed = this->iconDownloadCompleted; }
        const std::string key = std::to_string(this->selectedSectionIndex) + ":" + std::to_string(selected)
            + ":" + std::to_string(this->remoteGridMode) + ":" + std::to_string(this->visibleItems.size())
            + ":" + std::to_string(this->selectedItems.size()) + ":" + std::to_string(this->listVisibleTopIndex)
            + ":" + std::to_string(completed) + ":" + std::to_string(fastScroll) + ":" + this->searchQuery;
        if (key == this->browseDecorationKey) {
            this->gridCaptionMarquee->Update();
            return;
        }
        this->browseDecorationKey = key;
        const auto text = [](TextBlock::Ref target, const std::string& value) {
            if (target->GetText() != value) target->SetText(value);
        };
        const auto neighbor = [&](int step) {
            int index = this->selectedSectionIndex;
            for (std::size_t i = 0; i < this->remoteSections.size(); ++i) {
                index = (index + step + static_cast<int>(this->remoteSections.size())) % this->remoteSections.size();
                if (!this->queueAddMode || this->isQueueSection(index)) return index;
            }
            return this->selectedSectionIndex;
        };
        const int previous = neighbor(-1), next = neighbor(1);
        text(this->previousSectionText, "  " + ClipBrowseText(this->remoteSections[previous].title, 18, 320));
        text(this->nextSectionText, ClipBrowseText(this->remoteSections[next].title, 18, 320) + "  ");
        this->nextSectionText->SetX(1256 - this->nextSectionText->GetTextWidth());
        this->sectionHighlight->SetVisible(true);
        this->previousSectionText->SetVisible(this->remoteSections.size() > 1);
        this->nextSectionText->SetVisible(this->remoteSections.size() > 1);
        std::string context = std::to_string(this->visibleItems.size()) + " titles";
        if (!this->searchQuery.empty()) context += "   •   Search: " + this->searchQuery;
        context += "   •   Sort: " + (this->isAllSection() ? this->getAllSortModeLabel() : std::string(this->getBrowseSortLabel()));
        text(this->searchInfoText, ClipBrowseText(context, 16, 920));
        this->searchInfoText->SetVisible(true);
        std::string summary = this->visibleItems.empty() ? "No titles"
            : std::to_string(std::max(0, selected) + 1) + " of " + std::to_string(this->visibleItems.size());
        if (this->remoteGridMode && !this->visibleItems.empty()) {
            summary = "Page " + std::to_string(selected / kGridItemsPerPage + 1) + "/"
                + std::to_string((this->visibleItems.size() + kGridItemsPerPage - 1) / kGridItemsPerPage);
        }
        if (!this->selectedItems.empty()) {
            std::uint64_t bytes = 0;
            for (const auto& item : this->selectedItems) bytes += item.size;
            summary += "   •   " + std::to_string(this->selectedItems.size()) + " selected";
            const auto size = FormatSizeText(bytes);
            if (!size.empty()) summary += " · " + size;
        }
        text(this->browseSummaryText, ClipBrowseText(summary, 16, 450));
        this->browseSummaryText->SetX(1256 - this->browseSummaryText->GetTextWidth());
        this->browseSummaryText->SetVisible(true);
        const auto status = [&](const remoteInstStuff::RemoteItem& item) -> std::string {
            if (item.isCheat) return "Cheat";
            if (!item.saveId.empty() || this->isSaveSyncSection()) return "Save backup";
            std::string type = IsDlcItem(item) ? "DLC" : (IsUpdateItem(item) ? "Update" : "Game");
            std::uint64_t baseId = 0;
            const bool installedBase = IsBaseItem(item) && DeriveBaseTitleId(item, baseId)
                && this->installedSnapshot.baseInstalled.count(baseId) > 0
                && this->installedSnapshot.baseInstalled.at(baseId);
            if (this->isInstalledSection() || installedBase) type += "  •  Installed";
            else if (this->queueAddMode && !instPage::canEnqueueTitle(item.url)) type += "  •  Queued";
            return type;
        };
        const bool marqueeVisible = this->remoteGridMode && selected >= 0
            && selected < static_cast<int>(this->visibleItems.size());
        if (marqueeVisible) {
            const auto& item = this->visibleItems[selected];
            const auto marqueeKey = std::to_string(selected) + ":" + BuildItemIdentityKey(item) + ":" + item.name;
            if (marqueeKey != this->gridCaptionMarqueeKey) {
                this->gridCaptionMarqueeKey = marqueeKey;
                const int slot = selected % kGridItemsPerPage;
                this->gridCaptionMarquee->SetBounds(
                    kGridStartX + (slot % kGridCols) * (kGridTileWidth + kGridGap),
                    kGridStartY + (slot / kGridCols) * (kGridTileHeight + kGridGap) + kGridTileHeight + 4,
                    kGridTileWidth, 20);
                this->gridCaptionMarquee->SetText(item.name);
                this->gridCaptionMarquee->SetVisible(true);
                this->gridCaptionMarquee->SetSelected(true, true);
            }
            this->gridCaptionMarquee->Update();
        } else {
            this->gridCaptionMarquee->SetVisible(false);
            this->gridCaptionMarqueeKey.clear();
            for (auto& badge : this->remoteGridSelectBadgeBacks) badge->SetVisible(false);
        }
        for (int slot = 0; slot < kGridItemsPerPage; ++slot) {
            const int index = (selected / kGridItemsPerPage) * kGridItemsPerPage + slot;
            const bool show = this->remoteGridMode && index >= 0 && index < static_cast<int>(this->visibleItems.size());
            this->gridCardBacks[slot]->SetVisible(show);
            this->gridCaptions[slot]->SetVisible(show && index != selected);
            if (show) {
                text(this->gridCaptions[slot], ClipBrowseText(OverflowText::NormalizeSingleLineText(this->visibleItems[index].name), 15, kGridTileWidth));
                this->gridCaptions[slot]->SetColor(index == selected
                    ? inst::theme::Color("accent", COLOR("#FF7777FF")) : COLOR("#FFFFFFFF"));
            }
        }
        const bool list = !this->remoteGridMode && !this->visibleItems.empty();
        this->browseDetailPanel->SetVisible(list);
        this->focusedTitleText->SetVisible(list);
        this->focusedMetadataText->SetVisible(list);
        if (list && selected >= 0 && selected < static_cast<int>(this->visibleItems.size())) {
            const auto& item = this->visibleItems[selected];
            text(this->focusedTitleText, ClipBrowseText(OverflowText::NormalizeSingleLineText(item.name), 22, 320));
            std::string detail = status(item);
            const auto size = FormatSizeText(item.size);
            if (!size.empty()) detail += "\nSize: " + size;
            if (item.hasAppVersion) detail += "\nVersion: " + std::to_string(item.appVersion);
            if (item.hasReleaseDate) detail += "\nReleased: " + FormatReleaseDate(item.releaseDate);
            text(this->focusedMetadataText, detail);
        }
        for (int row = 0; row < kBrowseVisibleRows; ++row) {
            const int index = this->listVisibleTopIndex + row;
            const bool show = list && index >= 0 && index < static_cast<int>(this->visibleItems.size());
            this->listRowMetadata[row]->SetVisible(show);
            this->listRowSizes[row]->SetVisible(show);
            this->listRowSelections[row]->SetVisible(show);
            this->listRowCovers[row]->SetVisible(show && !fastScroll);
            if (!show) continue;
            const auto& item = this->visibleItems[index];
            text(this->listRowMetadata[row], status(item));
            text(this->listRowSizes[row], FormatSizeText(item.size));
            this->listRowSizes[row]->SetX(842 - this->listRowSizes[row]->GetTextWidth());
            const bool selectedForInstall = std::any_of(this->selectedItems.begin(), this->selectedItems.end(), [&](const auto& entry) {
                return !item.url.empty() && entry.url == item.url;
            });
            text(this->listRowSelections[row], selectedForInstall ? "✓ Selected" : "");
            if (fastScroll) continue;
            const auto iconPath = GetRemoteGridIconCachePath(item);
            const bool cached = !iconPath.empty() && std::filesystem::exists(iconPath);
            const auto coverKey = BuildItemIdentityKey(item) + (cached ? iconPath : ":placeholder");
            if (this->listRowCoverKeys[row] != coverKey) {
                this->listRowCoverKeys[row] = coverKey;
                std::vector<std::uint8_t> offlineIcon;
                if (cached) this->listRowCovers[row]->SetImage(iconPath);
                else if (TryLoadOfflineIconForItem(item, offlineIcon) && !offlineIcon.empty())
                    this->listRowCovers[row]->SetJpegImage(offlineIcon.data(), static_cast<s32>(offlineIcon.size()));
                else this->listRowCovers[row]->SetImage(inst::theme::Asset("romfs:/images/icons/title-placeholder.png"));
                this->listRowCovers[row]->SetWidth(58); this->listRowCovers[row]->SetHeight(58);
                if (!cached && !iconPath.empty()) this->queueIconDownload(item, iconPath);
            }
        }
    }

    void remoteInstPage::refreshLoadingStagesText()
    {
        const std::string title = this->refreshingLibrary ? "Refreshing library..." : "inst.remote.loading"_lang;
        if (this->loadingStagesText->GetText() != title) {
            this->loadingStagesText->SetText(title);
            CenterTextX(this->loadingStagesText);
        }
    }

    void remoteInstPage::updateLoadingActivity(std::uint64_t downloaded, std::uint64_t total, bool transfer)
    {
        const u64 now = armGetSystemTick();
        const u64 frequency = armGetSystemTickFreq();
        // Animate geometry each frame; throttle transfer text independently.
        const u64 elapsed = now - this->loadingStartTick;
        if (this->loadingIndeterminate) {
            const u64 period = frequency * 2;
            const u64 phase = elapsed % period;
            const u64 distance = phase < period / 2 ? phase : period - phase;
            this->loadingBarFill->SetX(260 + static_cast<int>((distance * 600) / (period / 2)));
            this->loadingBarFill->SetWidth(160);
        }
        if (this->loadingLastUpdateTick != 0 && now - this->loadingLastUpdateTick < frequency / 4)
            return;
        this->loadingLastUpdateTick = now;
        if (transfer) {
            char detail[96];
            if (downloaded == 0) {
                this->loadingTransferDetail = "Waiting for Remote";
            } else if (total > 0) {
                std::snprintf(detail, sizeof(detail), "%.1f / %.1f MiB received",
                    downloaded / 1048576.0, total / 1048576.0);
                this->loadingTransferDetail = detail;
            } else {
                std::snprintf(detail, sizeof(detail), "%.1f MiB received", downloaded / 1048576.0);
                this->loadingTransferDetail = detail;
            }
        }
        std::string detail = this->loadingTransferDetail;
        if (!detail.empty()) detail += "   |   ";
        detail += std::to_string(elapsed / frequency) + "s elapsed";
        if (elapsed / frequency >= 15 && transfer)
            detail += "   |   Still working...";
        if (this->loadingDetailText->GetText() != detail) {
            this->loadingDetailText->SetText(detail);
            CenterTextX(this->loadingDetailText);
        }
    }

    void remoteInstPage::setLoadingProgressStage(const std::string& stage)
    {
        if (this->loadingStageLabel == stage)
            return;
        this->loadingStageLabel = stage;
        this->loadingTransferDetail.clear();
        if (this->loadingProgressText->IsVisible())
            this->setLoadingProgress(this->loadingIndeterminate ? -1 : this->loadingPercent, true);
    }

    void remoteInstPage::setLoadingProgress(int percent, bool visible)
    {
        this->loadingIndeterminate = percent < 0;
        this->loadingPercent = std::clamp(percent, 0, 100);
        if (percent < 0)
            percent = 0;
        if (percent > 100)
            percent = 100;

        // Cheats share the progress controls but keep their existing placement.
        this->loadingProgressText->SetY(this->cheatInstallProgressVisible ? 504 : 320);
        this->loadingBarBack->SetY(this->cheatInstallProgressVisible ? 540 : 386);
        this->loadingBarFill->SetY(this->cheatInstallProgressVisible ? 540 : 386);
        this->loadingProgressText->SetVisible(visible);
        this->loadingBarBack->SetVisible(visible);
        this->loadingBarFill->SetVisible(visible);
        const bool showRemoteStages = visible && !this->cheatInstallProgressVisible;
        this->loadingStagesBack->SetVisible(showRemoteStages);
        this->loadingStagesText->SetVisible(showRemoteStages);
        this->loadingDetailText->SetVisible(showRemoteStages);
        if (!visible) {
            this->loadingStartTick = 0;
            this->loadingLastUpdateTick = 0;
            this->loadingTransferDetail.clear();
            return;
        }

        std::string label = this->loadingStageLabel;
        if (label.empty())
            label = "inst.remote.loading"_lang;
        // Percent is shown for explicit install progress only. Catalog requests
        // can span multiple endpoints, so transfer percentages are not overall progress.
        if (this->cheatInstallProgressVisible)
            label += " " + std::to_string(percent) + "%";
        if (this->loadingProgressText->GetText() != label) {
            this->loadingProgressText->SetText(label);
            CenterTextX(this->loadingProgressText);
        }
        if (this->loadingStartTick == 0)
            this->loadingStartTick = armGetSystemTick();
        this->loadingBarFill->SetX(260);
        this->loadingBarFill->SetWidth(this->loadingIndeterminate ? 160 : (760 * percent) / 100);
        this->refreshLoadingStagesText();
        this->updateLoadingActivity();
    }

    const char* remoteInstPage::getBrowseSortLabel() const
    {
        switch (this->browseSortMode) {
            case BrowseSortMode::DateDesc:
                return inst::config::remoteLegacyMode ? "Release Date" : "Date";
            case BrowseSortMode::NameAsc:
                return "Name";
            default:
                return "Remote Order";
        }
    }

    void remoteInstPage::applyBrowseSort()
    {
        switch (this->browseSortMode) {
            case BrowseSortMode::NameAsc:
                std::stable_sort(this->visibleItems.begin(), this->visibleItems.end(), [](const auto& a, const auto& b) {
                    return inst::util::ignoreCaseCompare(a.name, b.name);
                });
                break;
            case BrowseSortMode::DateDesc:
                std::stable_sort(this->visibleItems.begin(), this->visibleItems.end(), [](const auto& a, const auto& b) {
                    std::uint64_t aKey = 0;
                    std::uint64_t bKey = 0;
                    const bool aHasDate = TryGetItemSortDateKey(a, aKey);
                    const bool bHasDate = TryGetItemSortDateKey(b, bKey);
                    if (aHasDate != bHasDate)
                        return aHasDate && !bHasDate;
                    if (aHasDate && bHasDate) {
                        if (aKey != bKey)
                            return aKey > bKey;
                        return inst::util::ignoreCaseCompare(a.name, b.name);
                    }
                    return false;
                });
                break;
            default:
                break;
        }
    }

    void remoteInstPage::openSearchDialog()
    {
        std::string query = inst::util::softwareKeyboard("inst.remote.search_hint"_lang, this->searchQuery, 60);
        if (query == this->searchQuery)
            return;

        this->searchQuery = query;
        this->remoteGridPage = -1;
        this->gridPage = -1;
        this->updateSectionText();
        this->drawMenuItems(false);
    }

    void remoteInstPage::openSortDialog()
    {
        const bool allSection = this->isAllSection();
        std::string details = "Current sort: ";
        details += allSection ? this->getAllSortModeLabel() : this->getBrowseSortLabel();
        std::vector<std::string> options;
        if (allSection) {
            options = {
                "Name A-Z",
                "Name Z-A",
                "Date Old-New",
                "Date New-Old"
            };
        } else {
            options = {
                inst::config::remoteLegacyMode ? "Sort by Release Date" : "Sort by Date",
                "Sort by Name",
                "Use Remote Order"
            };
        }
        options.push_back("common.cancel"_lang);

        const int choice = mainApp->CreateShowDialog("Sort Remote", details, options, false);
        if (choice < 0)
            return;

        bool needsRedraw = false;
        if (allSection) {
            if (choice == 0 && this->allSortMode != 0) {
                this->allSortMode = 0;
                this->applyAllSectionSort();
                needsRedraw = true;
            } else if (choice == 1 && this->allSortMode != 1) {
                this->allSortMode = 1;
                this->applyAllSectionSort();
                needsRedraw = true;
            } else if (choice == 2 && this->allSortMode != 2) {
                this->allSortMode = 2;
                this->applyAllSectionSort();
                needsRedraw = true;
            } else if (choice == 3 && this->allSortMode != 3) {
                this->allSortMode = 3;
                this->applyAllSectionSort();
                needsRedraw = true;
            }
        } else if (choice == 0) {
            if (this->browseSortMode != BrowseSortMode::DateDesc) {
                this->browseSortMode = BrowseSortMode::DateDesc;
                needsRedraw = true;
            }
        } else if (choice == 1) {
            if (this->browseSortMode != BrowseSortMode::NameAsc) {
                this->browseSortMode = BrowseSortMode::NameAsc;
                needsRedraw = true;
            }
        } else if (choice == 2) {
            if (this->browseSortMode != BrowseSortMode::Default) {
                this->browseSortMode = BrowseSortMode::Default;
                needsRedraw = true;
            }
        }

        if (!needsRedraw)
            return;

        this->remoteGridPage = -1;
        this->gridPage = -1;
        this->updateSectionText();
        this->drawMenuItems(false);
    }

    void remoteInstPage::getListTextBounds(int& textX, int& textWidth) const
    {
        textX = 0;
        textWidth = 0;
        if (this->menu == nullptr)
            return;

        const int menuX = this->menu->GetProcessedX();
        textX = menuX + this->menu->GetItemSize() + 40;
        int maxRowRight = textX + 550;
        if (maxRowRight < textX)
            maxRowRight = textX;
        textWidth = maxRowRight - textX;
    }

    static std::string BuildSingleLineGridTitle(const std::string& title)
    {
        const std::string normalized = OverflowText::NormalizeSingleLineText(title);
        return ClipBrowseText(normalized, 18, 680);
    }

    std::string remoteInstPage::buildListMenuLabel(const remoteInstStuff::RemoteItem& item) {
        return ClipBrowseText(OverflowText::NormalizeSingleLineText(item.name), 22, 550);
    }

    void remoteInstPage::updateListMarquee(bool force)
    {
        auto hideMarquee = [&]() {
            this->listMarqueeMaskRect->SetVisible(false);
            this->listMarqueeTintRect->SetVisible(false);
            this->listMarqueeOverlayText->SetVisible(false);
            this->listMarqueeClipEnabled = false;
            this->listMarqueeFadeRect->SetVisible(false);
            this->listMarqueeFadeAlpha = 0;
            this->listMarqueePhase = kListMarqueePhasePause;
            this->listMarqueeEndPauseUntilTick = 0;
            this->listMarqueeFullLabel.clear();
        };

        if (this->remoteGridMode || !this->menu->IsVisible() || this->isFastListScrolling()) {
            hideMarquee();
            this->listMarqueeIndex = -1;
            return;
        }
        auto& items = this->menu->GetItems();
        if (items.empty() || this->visibleItems.empty()) {
            hideMarquee();
            this->listMarqueeIndex = -1;
            return;
        }

        int selectedIndex = this->menu->GetSelectedIndex();
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(this->visibleItems.size())) {
            hideMarquee();
            this->listMarqueeIndex = -1;
            return;
        }

        const u64 now = armGetSystemTick();
        const u64 freq = armGetSystemTickFreq();
        if (freq == 0) {
            hideMarquee();
            return;
        }
        const u64 startDelayTicks = (freq * kListMarqueeStartDelayMs) / 1000;
        const u64 endPauseTicks = (freq * kListMarqueeEndPauseMs) / 1000;
        const u64 fadeDurationTicks = (freq * kListMarqueeFadeDurationMs) / 1000;

        const int visibleCount = std::max(1, this->menu->GetNumberOfItemsToShow());

        // Stopping a marquee/hold does not reposition the menu. Read its
        // tracked viewport rather than guessing a top row from selection.
        this->listVisibleTopIndex = this->menu->GetVisibleTopIndex();

        const auto& item = this->visibleItems[static_cast<std::size_t>(selectedIndex)];
        const std::string normalizedName = OverflowText::NormalizeSingleLineText(item.name);
        std::string suffix;

        int row = selectedIndex - this->listVisibleTopIndex;
        if (row < 0 || row >= visibleCount) {
            hideMarquee();
            return;
        }

        const int itemHeight = this->menu->GetItemSize();
        const int rowY = this->menu->GetProcessedY() + (row * itemHeight);
        int textX = 0;
        int maskWidth = 0;
        this->getListTextBounds(textX, maskWidth);

        if (maskWidth <= 0) {
            hideMarquee();
            return;
        }

        constexpr int kRemoteListMenuFontSize = 22;
        constexpr int kRenderNoWrapWidth = 1279;
        std::string label = ClipSingleLinePrefixSuffixByMenuRender(
            normalizedName, suffix, kRemoteListMenuFontSize, kRenderNoWrapWidth, 30);
        this->listMarqueeOverlayText->SetText(label);
        this->listMarqueeMaxOffset = this->listMarqueeOverlayText->GetTextWidth() - maskWidth;
        if (this->listMarqueeMaxOffset < 0)
            this->listMarqueeMaxOffset = 0;

        if (this->listMarqueeMaxOffset <= 0) {
            hideMarquee();
            this->listMarqueeOffset = 0;
            this->listMarqueeSpeedRemainder = 0;
            return;
        }

        if (this->listMarqueeIndex != selectedIndex || force || this->listMarqueeFullLabel != label) {
            this->listMarqueeIndex = selectedIndex;
            this->listMarqueeOffset = 0;
            this->listMarqueeLastTick = now;
            this->listMarqueePauseUntilTick = now + startDelayTicks;
            this->listMarqueeEndPauseUntilTick = 0;
            this->listMarqueeSpeedRemainder = 0;
            this->listMarqueeFadeStartTick = 0;
            this->listMarqueePhase = kListMarqueePhasePause;
            this->listMarqueeFadeAlpha = 0;
            this->listMarqueeFullLabel = label;
        }

        if (this->listMarqueeMaxOffset > 0) {
            switch (this->listMarqueePhase) {
                case kListMarqueePhasePause:
                    this->listMarqueeFadeAlpha = 0;
                    if (now >= this->listMarqueePauseUntilTick) {
                        this->listMarqueePhase = kListMarqueePhaseScroll;
                        this->listMarqueeLastTick = now;
                        this->listMarqueeSpeedRemainder = 0;
                    }
                    break;
                case kListMarqueePhaseScroll: {
                    if (now > this->listMarqueeLastTick) {
                        const u64 elapsedTicks = now - this->listMarqueeLastTick;
                        unsigned long long scaled = (static_cast<unsigned long long>(elapsedTicks) * static_cast<unsigned long long>(kListMarqueeSpeedPxPerSec)) + static_cast<unsigned long long>(this->listMarqueeSpeedRemainder);
                        const int advance = static_cast<int>(scaled / static_cast<unsigned long long>(freq));
                        this->listMarqueeSpeedRemainder = static_cast<u64>(scaled % static_cast<unsigned long long>(freq));
                        this->listMarqueeLastTick = now;
                        if (advance > 0) {
                            this->listMarqueeOffset += advance;
                            if (this->listMarqueeOffset >= this->listMarqueeMaxOffset) {
                                this->listMarqueeOffset = this->listMarqueeMaxOffset;
                                this->listMarqueePhase = kListMarqueePhaseEndPause;
                                this->listMarqueeEndPauseUntilTick = now + endPauseTicks;
                            }
                        }
                    }
                    break;
                }
                case kListMarqueePhaseEndPause:
                    this->listMarqueeFadeAlpha = 0;
                    if (now >= this->listMarqueeEndPauseUntilTick) {
                        this->listMarqueePhase = kListMarqueePhaseFadeOut;
                        this->listMarqueeFadeStartTick = now;
                        this->listMarqueeFadeAlpha = 0;
                    }
                    break;
                case kListMarqueePhaseFadeOut: {
                    if (fadeDurationTicks == 0) {
                        this->listMarqueeFadeAlpha = 255;
                        this->listMarqueeOffset = 0;
                        this->listMarqueePhase = kListMarqueePhaseFadeIn;
                        this->listMarqueeFadeStartTick = now;
                    } else {
                        const u64 fadeElapsed = (now > this->listMarqueeFadeStartTick) ? (now - this->listMarqueeFadeStartTick) : 0;
                        if (fadeElapsed >= fadeDurationTicks) {
                            this->listMarqueeFadeAlpha = 255;
                            this->listMarqueeOffset = 0;
                            this->listMarqueePhase = kListMarqueePhaseFadeIn;
                            this->listMarqueeFadeStartTick = now;
                        } else {
                            this->listMarqueeFadeAlpha = static_cast<int>((fadeElapsed * 255ULL) / fadeDurationTicks);
                        }
                    }
                    break;
                }
                case kListMarqueePhaseFadeIn: {
                    if (fadeDurationTicks == 0) {
                        this->listMarqueeFadeAlpha = 0;
                        this->listMarqueePhase = kListMarqueePhasePause;
                        this->listMarqueePauseUntilTick = now + startDelayTicks;
                        this->listMarqueeLastTick = now;
                    } else {
                        const u64 fadeElapsed = (now > this->listMarqueeFadeStartTick) ? (now - this->listMarqueeFadeStartTick) : 0;
                        if (fadeElapsed >= fadeDurationTicks) {
                            this->listMarqueeFadeAlpha = 0;
                            this->listMarqueePhase = kListMarqueePhasePause;
                            this->listMarqueePauseUntilTick = now + startDelayTicks;
                            this->listMarqueeLastTick = now;
                        } else {
                            this->listMarqueeFadeAlpha = 255 - static_cast<int>((fadeElapsed * 255ULL) / fadeDurationTicks);
                        }
                    }
                    break;
                }
                default:
                    this->listMarqueePhase = kListMarqueePhasePause;
                    this->listMarqueeFadeAlpha = 0;
                    this->listMarqueePauseUntilTick = now + startDelayTicks;
                    this->listMarqueeLastTick = now;
                    this->listMarqueeSpeedRemainder = 0;
                    break;
            }
        } else {
            this->listMarqueeOffset = 0;
            this->listMarqueeFadeAlpha = 0;
            this->listMarqueePhase = kListMarqueePhasePause;
            this->listMarqueeSpeedRemainder = 0;
        }

        if (this->listMarqueeOffset < 0)
            this->listMarqueeOffset = 0;
        if (this->listMarqueeOffset > this->listMarqueeMaxOffset)
            this->listMarqueeOffset = this->listMarqueeMaxOffset;

        this->listMarqueeOverlayText->SetText(label);
        int textY = rowY + ((itemHeight - this->listMarqueeOverlayText->GetTextHeight()) / 2);
        pu::ui::Color marqueeBaseColor = this->menu->GetColor();
        marqueeBaseColor.A = 255;
        const pu::ui::Color marqueeHighlightColor = inst::theme::Color("highlight", COLOR("#303030FF"));
        const pu::ui::Color marqueeResolvedColor = BlendOverOpaque(marqueeBaseColor, marqueeHighlightColor);
        pu::ui::Color marqueeTextColor = ShouldUseDarkText(marqueeResolvedColor) ? COLOR("#000000FF") : COLOR("#FFFFFFFF");
        marqueeTextColor = inst::theme::Color("selected_text", inst::theme::Color("text", marqueeTextColor));
        const pu::ui::Color currentMarqueeTextColor = this->listMarqueeOverlayText->GetColor();
        if (currentMarqueeTextColor.R != marqueeTextColor.R || currentMarqueeTextColor.G != marqueeTextColor.G
            || currentMarqueeTextColor.B != marqueeTextColor.B || currentMarqueeTextColor.A != marqueeTextColor.A) {
            this->listMarqueeOverlayText->SetColor(marqueeTextColor);
        }
        this->listMarqueeMaskRect->SetColor(marqueeBaseColor);
        this->listMarqueeMaskRect->SetX(textX);
        this->listMarqueeMaskRect->SetY(rowY + 17);
        this->listMarqueeMaskRect->SetWidth(maskWidth);
        this->listMarqueeMaskRect->SetHeight(32);
        this->listMarqueeMaskRect->SetVisible(true);
        this->listMarqueeTintRect->SetColor(marqueeHighlightColor);
        this->listMarqueeTintRect->SetX(textX);
        this->listMarqueeTintRect->SetY(rowY + 17);
        this->listMarqueeTintRect->SetWidth(maskWidth);
        this->listMarqueeTintRect->SetHeight(32);
        this->listMarqueeTintRect->SetVisible(true);
        this->listMarqueeClipEnabled = true;
        this->listMarqueeClipX = textX;
        this->listMarqueeClipY = rowY + 17;
        this->listMarqueeClipW = maskWidth;
        this->listMarqueeClipH = 32;
        this->listMarqueeOverlayText->SetX(textX - this->listMarqueeOffset);
        this->listMarqueeOverlayText->SetY(textY);
        this->listMarqueeOverlayText->SetVisible(true);

        if (this->listMarqueeFadeAlpha > 0) {
            pu::ui::Color fadeColor = marqueeResolvedColor;
            fadeColor.A = static_cast<u8>(this->listMarqueeFadeAlpha);
            this->listMarqueeFadeRect->SetColor(fadeColor);
            this->listMarqueeFadeRect->SetX(textX);
            this->listMarqueeFadeRect->SetY(rowY + 17);
            this->listMarqueeFadeRect->SetWidth(maskWidth);
            this->listMarqueeFadeRect->SetHeight(32);
            this->listMarqueeFadeRect->SetVisible(true);
        } else {
            this->listMarqueeFadeRect->SetVisible(false);
        }
    }

    void remoteInstPage::updateButtonsText() {
        if (queueAddMode) {
            this->setButtonsText(" Select     Add to queue     Queue    / Sections     Search     View");
            return;
        }
        if (this->saveVersionSelectorVisible) {
            if (this->saveVersionSelectorDeleteMode)
                this->setButtonsText(" Delete Backup    / Select Version     Back");
            else
                this->setButtonsText(" Download    / Select Version     Back");
        }
        else if (this->isSaveSyncSection())
            this->setButtonsText(" Manage Save     Refresh    / Section     Search    \xEE\x83\x85 Sort     Cancel");
        else if (this->isCheatsSection())
            this->setButtonsText(" Manage Cheat     Install All     Refresh    / Section     Search    \xEE\x83\x85 Sort     Cancel");
        else if (this->isInstalledSection())
            this->setButtonsText(" Details     Refresh    / Section     Search    \xEE\x83\x85 Sort     View     Cancel");
        else {
            std::string buttonsText = "inst.remote.buttons_all"_lang;
            buttonsText += "    \xEE\x83\x85 Sort";
            this->setButtonsText(buttonsText);
        }
    }

    void remoteInstPage::buildInstalledSection() {
        (void)this->ensureInstalledSectionBuilt();
    }

    bool remoteInstPage::buildInstalledSnapshot() {
        if (this->installedSnapshot.ready)
            return true;

        this->installedSnapshot = {};
        Result rc = nsInitialize();
        if (R_FAILED(rc))
            return false;

        const s32 chunk = 64;
        s32 offset = 0;
        std::vector<NsApplicationContentMetaStatus> list;
        while (true) {
            NsApplicationRecord records[chunk];
            s32 outCount = 0;
            rc = nsListApplicationRecord(records, chunk, offset, &outCount);
            if (R_FAILED(rc) || outCount <= 0)
                break;

            for (s32 i = 0; i < outCount; i++) {
                const u64 baseId = records[i].application_id;

                // One content-meta listing answers all three questions we have about a record:
                // is the base installed, which patch version is on, which DLC ids are owned.
                // Probing installation with nsGetApplicationControlData instead (the old
                // IsBaseTitleCurrentlyInstalled path) pulled a 144KB NACP+icon blob off NAND for
                // every record just to test one boolean. Side effect: an archived title keeps its
                // control data but loses its content meta, so it now reads as not installed.
                s32 metaCount = 0;
                s32 metaOut = 0;
                if (R_SUCCEEDED(nsCountApplicationContentMeta(baseId, &metaCount)) && metaCount > 0) {
                    list.resize(static_cast<std::size_t>(metaCount));
                    if (R_FAILED(nsListApplicationContentMetaStatus(baseId, 0, list.data(), metaCount, &metaOut)))
                        metaOut = 0;
                }

                bool installed = false;
                for (s32 j = 0; j < metaOut; j++) {
                    if (list[j].meta_type == NcmContentMetaType_Application && list[j].storageID != NcmStorageId_None) {
                        installed = true;
                        break;
                    }
                }
                this->installedSnapshot.baseInstalled[baseId] = installed;
                if (!installed)
                    continue;

                this->installedSnapshot.installedBaseIds.push_back(baseId);
                for (s32 j = 0; j < metaOut; j++) {
                    if (list[j].meta_type == NcmContentMetaType_Patch) {
                        auto& version = this->installedSnapshot.installedUpdateVersion[baseId];
                        if (list[j].version > version)
                            version = list[j].version;
                    } else if (list[j].meta_type == NcmContentMetaType_AddOnContent) {
                        this->installedSnapshot.installedDlcIds.insert(list[j].application_id);
                    }
                }
            }

            offset += outCount;
        }

        nsExit();
        this->installedSnapshot.ready = true;
        return true;
    }

    void remoteInstPage::ensureInstalledSectionPlaceholder() {
        if (inst::config::remoteHideInstalledSection)
            return;

        auto it = std::find_if(this->remoteSections.begin(), this->remoteSections.end(), [](const auto& section) {
            return section.id == "installed";
        });
        if (it != this->remoteSections.end())
            return;

        remoteInstStuff::RemoteSection installedSection;
        installedSection.id = "installed";
        installedSection.title = "Installed";
        this->remoteSections.insert(this->remoteSections.begin(), std::move(installedSection));
    }

    bool remoteInstPage::ensureInstalledSectionBuilt() {
        this->ensureInstalledSectionPlaceholder();
        if (this->installedSnapshot.installedSectionBuilt)
            return true;
        if (!this->buildInstalledSnapshot())
            return false;

        auto sectionIt = std::find_if(this->remoteSections.begin(), this->remoteSections.end(), [](const auto& section) {
            return section.id == "installed";
        });
        if (sectionIt == this->remoteSections.end())
            return false;

        Result rc = nsInitialize();
        if (R_FAILED(rc))
            return false;

        std::vector<remoteInstStuff::RemoteItem> installedItems;
        installedItems.reserve(this->installedSnapshot.installedBaseIds.size() * 2);

        for (const auto baseId : this->installedSnapshot.installedBaseIds) {
            remoteInstStuff::RemoteItem baseItem;
            baseItem.name = tin::util::GetTitleName(baseId, NcmContentMetaType_Application);
            baseItem.url = "";
            baseItem.size = 0;
            baseItem.titleId = baseId;
            baseItem.hasTitleId = true;
            baseItem.appType = NcmContentMetaType_Application;
            installedItems.push_back(baseItem);

            s32 metaCount = 0;
            if (R_SUCCEEDED(nsCountApplicationContentMeta(baseId, &metaCount)) && metaCount > 0) {
                std::vector<NsApplicationContentMetaStatus> list(static_cast<std::size_t>(metaCount));
                s32 metaOut = 0;
                if (R_SUCCEEDED(nsListApplicationContentMetaStatus(baseId, 0, list.data(), metaCount, &metaOut)) && metaOut > 0) {
                    for (s32 j = 0; j < metaOut; j++) {
                        if (list[j].meta_type != NcmContentMetaType_Patch && list[j].meta_type != NcmContentMetaType_AddOnContent)
                            continue;
                        remoteInstStuff::RemoteItem item;
                        item.titleId = list[j].application_id;
                        item.hasTitleId = true;
                        item.appVersion = list[j].version;
                        item.hasAppVersion = true;
                        item.appType = list[j].meta_type;
                        item.name = tin::util::GetTitleName(item.titleId, static_cast<NcmContentMetaType>(item.appType));
                        item.url = "";
                        item.size = 0;
                        installedItems.push_back(item);
                    }
                }
            }
        }

        nsExit();

        std::sort(installedItems.begin(), installedItems.end(), [](const auto& a, const auto& b) {
            return inst::util::ignoreCaseCompare(a.name, b.name);
        });

        sectionIt->items = std::move(installedItems);
        this->installedSnapshot.installedSectionBuilt = true;
        return true;
    }

    void remoteInstPage::ensureSaveSyncSectionLoaded() {
        if (!this->saveSyncEnabled || this->saveSyncLoaded)
            return;
        if (this->remoteSections.empty())
            return;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= static_cast<int>(this->remoteSections.size()))
            return;

        const std::string id = this->remoteSections[static_cast<std::size_t>(this->selectedSectionIndex)].id;
        if (id != "saves" && id != "save")
            return;

        // Hide active list/grid visuals so the loading bar remains unobstructed.
        this->menu->SetVisible(false);
        this->previewImage->SetVisible(false);
        this->gridHighlight->SetVisible(false);
        this->gridTitleText->SetVisible(false);
        this->imageLoadingText->SetVisible(false);
        for (auto& img : this->gridImages)
            img->SetVisible(false);
        for (auto& highlight : this->remoteGridSelectHighlights)
            highlight->SetVisible(false);
        for (auto& icon : this->remoteGridSelectIcons)
            icon->SetVisible(false);

        const int previousSectionIndex = this->selectedSectionIndex;
        this->setLoadingProgressStage("Loading save sync list...");
        this->setLoadingProgress(-1, true);
        mainApp->CallForRender();
        this->buildSaveSyncSection(this->activeRemoteUrl);
        this->saveSyncLoaded = true;

        int saveSectionIndex = -1;
        for (std::size_t i = 0; i < this->remoteSections.size(); i++) {
            const std::string& sectionId = this->remoteSections[i].id;
            if (sectionId == "saves" || sectionId == "save") {
                saveSectionIndex = static_cast<int>(i);
                break;
            }
        }

        if (saveSectionIndex >= 0) {
            this->selectedSectionIndex = saveSectionIndex;
        } else if (this->remoteSections.empty()) {
            this->selectedSectionIndex = 0;
        } else {
            int clampedIndex = previousSectionIndex;
            if (clampedIndex >= static_cast<int>(this->remoteSections.size()))
                clampedIndex = static_cast<int>(this->remoteSections.size()) - 1;
            if (clampedIndex < 0)
                clampedIndex = 0;
            this->selectedSectionIndex = clampedIndex;
        }

        this->setLoadingProgress(0, false);
        this->setLoadingProgressStage("");
    }

    void remoteInstPage::buildSaveSyncSection(const std::string& remoteUrl) {
        this->saveSyncLoaded = true;
        this->saveSyncEntries.clear();
        std::unordered_map<std::uint64_t, std::string> saveIconUrlByTitleId;

        auto normalizeSectionId = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        };

        auto buildDefaultRemoteIconUrl = [&](std::uint64_t titleId) -> std::string {
            if (titleId == 0 || remoteUrl.empty())
                return std::string();
            std::string baseUrl = remoteUrl;
            while (!baseUrl.empty() && baseUrl.back() == '/')
                baseUrl.pop_back();
            char titleIdHex[17] = {};
            std::snprintf(titleIdHex, sizeof(titleIdHex), "%016lX", titleId);
            return baseUrl + remoteInstStuff::GetRemoteApiPrefix() + "/icon/" + std::string(titleIdHex);
        };

        std::vector<remoteInstStuff::RemoteItem> remoteSaveItems;
        std::vector<remoteInstStuff::RemoteSection> retainedSections;
        retainedSections.reserve(this->remoteSections.size());
        for (auto& section : this->remoteSections) {
            const std::string id = normalizeSectionId(section.id);
            if (id == "saves" || id == "save" || id == "savegames" || id == "save_games" || id == "save-game") {
                for (const auto& item : section.items) {
                    if (item.hasTitleId && item.hasIconUrl && !item.iconUrl.empty())
                        saveIconUrlByTitleId[item.titleId] = item.iconUrl;
                }
                remoteSaveItems.insert(remoteSaveItems.end(), section.items.begin(), section.items.end());
                continue;
            }
            retainedSections.push_back(std::move(section));
        }
        this->remoteSections = std::move(retainedSections);

        std::vector<remoteInstStuff::RemoteItem> apiRemoteSaveItems;
        std::string remoteFetchWarning;
        if (!inst::save_sync::FetchRemoteSaveItems(remoteUrl, inst::config::remoteUser, inst::config::remotePass, apiRemoteSaveItems, remoteFetchWarning)) {
            if (!remoteFetchWarning.empty())
                RemoteDlcTrace("save sync remote list warning: %s", remoteFetchWarning.c_str());
            // Keep save sync enabled so local saves can still be shown and uploaded.
        }
        if (!apiRemoteSaveItems.empty()) {
            remoteSaveItems.insert(remoteSaveItems.end(), apiRemoteSaveItems.begin(), apiRemoteSaveItems.end());
        }

        std::string warning;
        inst::save_sync::BuildEntries(remoteSaveItems, this->saveSyncEntries, warning);
        if (!warning.empty())
            RemoteDlcTrace("save sync warning: %s", warning.c_str());
        if (this->saveSyncEntries.empty())
            return;

        remoteInstStuff::RemoteSection saveSection;
        saveSection.id = "saves";
        saveSection.title = "Saves";
        saveSection.items.reserve(this->saveSyncEntries.size());
        for (const auto& entry : this->saveSyncEntries) {
            remoteInstStuff::RemoteItem item;
            item.name = entry.titleName;
            item.name += entry.saveType == "device" ? " [Device]" : (entry.saveType == "account" ? " [Account]" : " [Unknown type]");
            item.saveType = entry.saveType;
            if (entry.localAvailable && entry.remoteAvailable) {
                if (entry.remoteVersions.size() > 1) {
                    item.name += " [Console + Server x" + std::to_string(entry.remoteVersions.size()) + "]";
                } else {
                    item.name += " [Console + Server]";
                }
            }
            else if (entry.localAvailable)
                item.name += " [Console]";
            else if (entry.remoteAvailable) {
                if (entry.remoteVersions.size() > 1) {
                    item.name += " [Server x" + std::to_string(entry.remoteVersions.size()) + "]";
                } else {
                    item.name += " [Server]";
                }
            }
            item.url = entry.remoteDownloadUrl;
            if (!entry.remoteVersions.empty() && entry.remoteVersions.front().size > 0)
                item.size = entry.remoteVersions.front().size;
            else
                item.size = entry.remoteSize;
            item.titleId = entry.titleId;
            item.hasTitleId = true;
            const auto iconIt = saveIconUrlByTitleId.find(entry.titleId);
            if (iconIt != saveIconUrlByTitleId.end()) {
                item.iconUrl = iconIt->second;
                item.hasIconUrl = true;
            } else {
                item.iconUrl = buildDefaultRemoteIconUrl(entry.titleId);
                item.hasIconUrl = !item.iconUrl.empty();
            }
            saveSection.items.push_back(std::move(item));
        }

        this->remoteSections.push_back(std::move(saveSection));
    }

    void remoteInstPage::refreshSaveSyncSection(std::uint64_t selectedTitleId, int previousSectionIndex, const std::string& selectedSaveType) {
        this->buildSaveSyncSection(this->activeRemoteUrl);

        int saveSectionIndex = -1;
        for (std::size_t i = 0; i < this->remoteSections.size(); i++) {
            const std::string& id = this->remoteSections[i].id;
            if (id == "saves" || id == "save") {
                saveSectionIndex = static_cast<int>(i);
                break;
            }
        }

        if (saveSectionIndex >= 0) {
            this->selectedSectionIndex = saveSectionIndex;
        } else if (this->remoteSections.empty()) {
            this->selectedSectionIndex = 0;
        } else {
            if (previousSectionIndex >= static_cast<int>(this->remoteSections.size()))
                previousSectionIndex = static_cast<int>(this->remoteSections.size()) - 1;
            if (previousSectionIndex < 0)
                previousSectionIndex = 0;
            this->selectedSectionIndex = previousSectionIndex;
        }

        this->remoteGridPage = -1;
        this->gridPage = -1;
        this->updateSectionText();
        this->updateButtonsText();
        this->drawMenuItems(false);

        if (this->isSaveSyncSection() && !this->visibleItems.empty()) {
            int restoredIndex = 0;
            for (std::size_t i = 0; i < this->visibleItems.size(); i++) {
                if (this->visibleItems[i].hasTitleId && this->visibleItems[i].titleId == selectedTitleId &&
                        this->visibleItems[i].saveType == selectedSaveType) {
                    restoredIndex = static_cast<int>(i);
                    break;
                }
            }

            if (this->remoteGridMode) {
                this->remoteGridIndex = restoredIndex;
                this->updateRemoteGrid();
            } else {
                this->menu->SetSelectedIndex(restoredIndex);
                this->updatePreview();
                this->updateDescriptionPanel();
            }
        } else if (this->remoteGridMode) {
            this->updateRemoteGrid();
        } else {
            this->updatePreview();
            this->updateDescriptionPanel();
        }
    }

    bool remoteInstPage::openSaveVersionSelector(const inst::save_sync::SaveSyncEntry& entry, int previousSectionIndex, bool deleteMode) {
        this->saveVersionSelectorVersions.clear();
        for (const auto& version : entry.remoteVersions) {
            if (!version.downloadUrl.empty() || !version.saveId.empty())
                this->saveVersionSelectorVersions.push_back(version);
        }
        if (this->saveVersionSelectorVersions.size() <= 1)
            return false;

        this->saveVersionSelectorTitleId = entry.titleId;
        this->saveVersionSelectorSaveType = entry.saveType;
        this->saveVersionSelectorTitleName = entry.titleName;
        this->saveVersionSelectorLocalAvailable = entry.localAvailable;
        this->saveVersionSelectorDeleteMode = deleteMode;
        this->saveVersionSelectorPreviousSectionIndex = previousSectionIndex;

        this->saveVersionSelectorMenu->ClearItems();
        for (std::size_t i = 0; i < this->saveVersionSelectorVersions.size(); i++) {
            const auto& version = this->saveVersionSelectorVersions[i];
            std::string created = version.createdAt.empty() ? "Unknown date" : inst::util::shortenString(version.createdAt, 19, false);
            std::string sizeText = FormatSizeText(version.size);
            if (sizeText.empty())
                sizeText = "-";
            std::string note = version.note.empty() ? "-" : inst::util::shortenString(version.note, 24, false);
            std::string row = std::to_string(i + 1) + ". " + created + " | " + sizeText + " | " + note;
            auto menuItem = pu::ui::elm::MenuItem::New(inst::util::shortenString(row, 88, false));
            menuItem->SetColor(COLOR("#FFFFFFFF"));
            this->saveVersionSelectorMenu->AddItem(menuItem);
        }
        this->saveVersionSelectorMenu->SetOnSelectionChanged([this]() {
            this->refreshSaveVersionSelectorDetailText();
        });
        this->saveVersionSelectorMenu->SetSelectedIndex(0);

        this->saveVersionSelectorTitleText->SetText((deleteMode ? "Delete Save Backup: " : "Save Versions: ") + inst::util::shortenString(entry.titleName, 72, true));
        this->saveVersionSelectorRect->SetVisible(true);
        this->saveVersionSelectorTitleText->SetVisible(true);
        this->saveVersionSelectorMenu->SetVisible(true);
        this->saveVersionSelectorDetailText->SetVisible(true);
        this->saveVersionSelectorHintText->SetVisible(true);
        this->saveVersionSelectorVisible = true;
        this->saveVersionSelectorHintText->SetText(deleteMode ? "A Delete Backup    B Back" : "A Download    B Back");
        this->menu->SetVisible(false);
        this->updateButtonsText();
        this->refreshSaveVersionSelectorDetailText();
        return true;
    }

    void remoteInstPage::closeSaveVersionSelector(bool refreshList) {
        if (!this->saveVersionSelectorVisible && !this->saveVersionSelectorRect->IsVisible())
            return;

        this->saveVersionSelectorVisible = false;
        this->saveVersionSelectorTitleId = 0;
        this->saveVersionSelectorLocalAvailable = false;
        this->saveVersionSelectorDeleteMode = false;
        this->saveVersionSelectorPreviousSectionIndex = 0;
        this->saveVersionSelectorTitleName.clear();
        this->saveVersionSelectorVersions.clear();
        this->saveVersionSelectorRect->SetVisible(false);
        this->saveVersionSelectorTitleText->SetVisible(false);
        this->saveVersionSelectorMenu->SetVisible(false);
        this->saveVersionSelectorDetailText->SetVisible(false);
        this->saveVersionSelectorHintText->SetVisible(false);
        this->saveVersionSelectorMenu->ClearItems();

        if (!refreshList)
            return;

        this->updateButtonsText();
        this->drawMenuItems(false);
        if (this->remoteGridMode) {
            this->updateRemoteGrid();
        } else {
            this->updatePreview();
        }
        this->updateDescriptionPanel();
    }

    void remoteInstPage::refreshSaveVersionSelectorDetailText() {
        if (!this->saveVersionSelectorVisible || this->saveVersionSelectorVersions.empty()) {
            this->saveVersionSelectorDetailText->SetText("");
            return;
        }

        int selectedIndex = this->saveVersionSelectorMenu->GetSelectedIndex();
        if (selectedIndex < 0)
            selectedIndex = 0;
        if (selectedIndex >= static_cast<int>(this->saveVersionSelectorVersions.size()))
            selectedIndex = static_cast<int>(this->saveVersionSelectorVersions.size()) - 1;

        const auto& version = this->saveVersionSelectorVersions[static_cast<std::size_t>(selectedIndex)];
        std::string created = version.createdAt.empty() ? "Unknown date" : version.createdAt;
        std::string sizeText = FormatSizeText(version.size);
        if (sizeText.empty())
            sizeText = "-";
        std::string note = version.note.empty() ? "-" : inst::util::shortenString(version.note, 84, false);
        std::string saveId = version.saveId.empty() ? "-" : inst::util::shortenString(version.saveId, 48, false);
        std::string details =
            "Selected " + std::to_string(selectedIndex + 1) + "/" + std::to_string(this->saveVersionSelectorVersions.size()) + "\n"
            "Date: " + created + "    Size: " + sizeText + "\n"
            "Note: " + note + "\n"
            "ID: " + saveId;
        this->saveVersionSelectorDetailText->SetText(details);
    }

    bool remoteInstPage::handleSaveVersionSelectorInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
        (void)Up;
        (void)Held;
        (void)Pos;
        if (!this->saveVersionSelectorVisible)
            return false;

        if (Down & HidNpadButton_B) {
            this->closeSaveVersionSelector(true);
            return true;
        }

        if (Down & HidNpadButton_A) {
            if (this->saveVersionSelectorVersions.empty())
                return true;

            int selectedIndex = this->saveVersionSelectorMenu->GetSelectedIndex();
            if (selectedIndex < 0 || selectedIndex >= static_cast<int>(this->saveVersionSelectorVersions.size()))
                return true;

            auto it = std::find_if(this->saveSyncEntries.begin(), this->saveSyncEntries.end(), [&](const auto& entry) {
                return entry.titleId == this->saveVersionSelectorTitleId && entry.saveType == this->saveVersionSelectorSaveType;
            });
            if (it == this->saveSyncEntries.end()) {
                this->closeSaveVersionSelector(true);
                mainApp->CreateShowDialog("inst.remote.save_sync.title"_lang, "inst.remote.save_sync.dialog.unable_resolve_entry"_lang, {"common.ok"_lang}, true);
                return true;
            }

            if (this->saveVersionSelectorLocalAvailable) {
                if (!this->saveVersionSelectorDeleteMode) {
                    const int overwriteChoice = mainApp->CreateShowDialog(
                        "inst.remote.save_sync.title"_lang,
                        "inst.remote.save_sync.dialog.replace_local_prompt"_lang +
                            (it->deviceSave ? "\nThis device save is shared by all users on this console." : ""),
                        {"common.yes"_lang, "common.no"_lang},
                        false);
                    if (overwriteChoice != 0)
                        return true;
                }
            }

            const auto& selectedVersion = this->saveVersionSelectorVersions[static_cast<std::size_t>(selectedIndex)];
            std::string error;
            bool ok = false;
            const bool showTransferProgress = !this->saveVersionSelectorDeleteMode;
            if (showTransferProgress) {
                inst::ui::instPage::loadInstallScreen();
                inst::ui::instPage::setTopInstInfoText("inst.remote.save_sync.title"_lang);
                inst::ui::instPage::setInstInfoText("inst.remote.save_sync.status.downloading_backup"_lang);
                inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.preparing_download"_lang);
                inst::ui::instPage::setInstBarPerc(0);
            }
            if (this->saveVersionSelectorDeleteMode) {
                std::string saveIdText = selectedVersion.saveId.empty() ? "unknown" : selectedVersion.saveId;
                const int confirmDelete = mainApp->CreateShowDialog(
                    "inst.remote.save_sync.title"_lang,
                    "inst.remote.save_sync.dialog.delete_selected_fmt"_lang + "\nID: " + inst::util::shortenString(saveIdText, 52, false),
                    {"Delete", "common.cancel"_lang},
                    false);
                if (confirmDelete != 0)
                    return true;

                ok = inst::save_sync::DeleteSaveFromServer(
                    this->activeRemoteUrl,
                    inst::config::remoteUser,
                    inst::config::remotePass,
                    *it,
                    &selectedVersion,
                    error);
            } else {
                ok = inst::save_sync::DownloadSaveToConsole(
                    this->activeRemoteUrl,
                    inst::config::remoteUser,
                    inst::config::remotePass,
                    *it,
                    &selectedVersion,
                    error);
            }
            if (showTransferProgress)
                mainApp->LoadLayout(mainApp->remoteinstPage);
            if (!ok) {
                if (error.empty())
                    error = "Save sync failed.";
                mainApp->CreateShowDialog("inst.remote.save_sync.title"_lang, error, {"common.ok"_lang}, true);
                return true;
            }

            mainApp->CreateShowDialog("inst.remote.save_sync.title"_lang, this->saveVersionSelectorDeleteMode ? "inst.remote.save_sync.dialog.deleted_success"_lang : "inst.remote.save_sync.dialog.downloaded_success"_lang, {"common.ok"_lang}, true);
            const std::uint64_t selectedTitleId = this->saveVersionSelectorTitleId;
            const std::string selectedSaveType = this->saveVersionSelectorSaveType;
            const int previousSectionIndex = this->saveVersionSelectorPreviousSectionIndex;
            this->closeSaveVersionSelector(false);
            this->refreshSaveSyncSection(selectedTitleId, previousSectionIndex, selectedSaveType);
            return true;
        }

        this->refreshSaveVersionSelectorDetailText();
        return true;
    }

    void remoteInstPage::handleSaveSyncAction(int selectedIndex) {
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(this->visibleItems.size()))
            return;
        const auto& selectedItem = this->visibleItems[selectedIndex];
        if (!selectedItem.hasTitleId)
            return;

        auto it = std::find_if(this->saveSyncEntries.begin(), this->saveSyncEntries.end(), [&](const auto& entry) {
            return entry.titleId == selectedItem.titleId && entry.saveType == selectedItem.saveType;
        });
        if (it == this->saveSyncEntries.end()) {
            mainApp->CreateShowDialog("inst.remote.save_sync.title"_lang, "inst.remote.save_sync.dialog.unable_resolve_entry"_lang, {"common.ok"_lang}, true);
            return;
        }

        const std::uint64_t selectedTitleId = it->titleId;
        const std::string selectedSaveType = it->saveType;
        int previousSectionIndex = this->selectedSectionIndex;
        if (previousSectionIndex < 0)
            previousSectionIndex = 0;

        std::vector<std::string> options;
        std::vector<int> actions;
        if (it->localAvailable) {
            options.push_back("Upload to server");
            actions.push_back(1);
        }
        if (it->remoteAvailable) {
            options.push_back("Download to console");
            actions.push_back(2);
            options.push_back("Delete from server");
            actions.push_back(3);
        }
        if (actions.empty()) {
            mainApp->CreateShowDialog("inst.remote.save_sync.title"_lang, "inst.remote.save_sync.dialog.none_available"_lang, {"common.ok"_lang}, true);
            return;
        }
        options.push_back("common.cancel"_lang);

        int choice = mainApp->CreateShowDialog("inst.remote.save_sync.title"_lang, it->titleName, options, false);
        if (choice < 0 || choice >= static_cast<int>(actions.size()))
            return;

        std::string uploadNote;
        const inst::save_sync::SaveSyncRemoteVersion* selectedRemoteVersion = nullptr;
        if (actions[choice] == 1) {
            uploadNote = inst::util::softwareKeyboard("Save note (optional)", "", 120);
        } else if (actions[choice] == 2 || actions[choice] == 3) {
            std::vector<const inst::save_sync::SaveSyncRemoteVersion*> availableVersions;
            availableVersions.reserve(it->remoteVersions.size());
            for (const auto& version : it->remoteVersions) {
                if (!version.downloadUrl.empty() || !version.saveId.empty())
                    availableVersions.push_back(&version);
            }

            if (!availableVersions.empty()) {
                selectedRemoteVersion = availableVersions.front();
                if (availableVersions.size() > 1 && this->openSaveVersionSelector(*it, previousSectionIndex, actions[choice] == 3))
                    return;
            }
        }

        if (actions[choice] == 2 && it->localAvailable) {
            const int overwriteChoice = mainApp->CreateShowDialog(
                "inst.remote.save_sync.title"_lang,
                "inst.remote.save_sync.dialog.replace_local_prompt"_lang +
                    (it->deviceSave ? "\nThis device save is shared by all users on this console." : ""),
                {"common.yes"_lang, "common.no"_lang},
                false);
            if (overwriteChoice != 0)
                return;
        }
        if (actions[choice] == 3) {
            std::string saveIdText = selectedRemoteVersion && !selectedRemoteVersion->saveId.empty()
                ? selectedRemoteVersion->saveId
                : "latest";
            const int confirmDelete = mainApp->CreateShowDialog(
                "inst.remote.save_sync.title"_lang,
                "inst.remote.save_sync.dialog.delete_fmt"_lang + "\nID: " + inst::util::shortenString(saveIdText, 52, false),
                {"Delete", "common.cancel"_lang},
                false);
            if (confirmDelete != 0)
                return;
        }

        std::string error;
        bool ok = false;
        const bool showTransferProgress = (actions[choice] == 1 || actions[choice] == 2);
        if (showTransferProgress) {
            inst::ui::instPage::loadInstallScreen();
            inst::ui::instPage::setTopInstInfoText("inst.remote.save_sync.title"_lang);
            if (actions[choice] == 1) {
                inst::ui::instPage::setInstInfoText("inst.remote.save_sync.status.uploading_backup"_lang);
                inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.preparing_upload"_lang);
            } else {
                inst::ui::instPage::setInstInfoText("inst.remote.save_sync.status.downloading_backup"_lang);
                inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.preparing_download"_lang);
            }
            inst::ui::instPage::setInstBarPerc(0);
        }

        if (actions[choice] == 1) {
            ok = inst::save_sync::UploadSaveToServer(this->activeRemoteUrl, inst::config::remoteUser, inst::config::remotePass, *it, uploadNote, error);
        } else if (actions[choice] == 2) {
            ok = inst::save_sync::DownloadSaveToConsole(this->activeRemoteUrl, inst::config::remoteUser, inst::config::remotePass, *it, selectedRemoteVersion, error);
        } else if (actions[choice] == 3) {
            ok = inst::save_sync::DeleteSaveFromServer(this->activeRemoteUrl, inst::config::remoteUser, inst::config::remotePass, *it, selectedRemoteVersion, error);
        }

        if (showTransferProgress)
            mainApp->LoadLayout(mainApp->remoteinstPage);

        if (!ok) {
            if (error.empty())
                error = "Save sync failed.";
            mainApp->CreateShowDialog("inst.remote.save_sync.title"_lang, error, {"common.ok"_lang}, true);
            return;
        }

        std::string successMessage = "inst.remote.save_sync.dialog.success_generic"_lang;
        if (actions[choice] == 1)
            successMessage = "inst.remote.save_sync.dialog.uploaded_success"_lang;
        else if (actions[choice] == 2)
            successMessage = "inst.remote.save_sync.dialog.downloaded_success"_lang;
        else if (actions[choice] == 3)
            successMessage = "inst.remote.save_sync.dialog.deleted_success"_lang;
        mainApp->CreateShowDialog("inst.remote.save_sync.title"_lang, successMessage, {"common.ok"_lang}, true);
        this->refreshSaveSyncSection(selectedTitleId, previousSectionIndex, selectedSaveType);
    }

    void remoteInstPage::buildLegacyOwnedSections() {
        if (this->remoteSections.empty())
            return;
        if (!this->buildInstalledSnapshot())
            return;

        auto normalizeSectionId = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        };

        bool hasAllSection = false;
        bool hasUpdatesSection = false;
        bool hasDlcSection = false;
        int allSectionIndex = -1;
        for (std::size_t i = 0; i < this->remoteSections.size(); i++) {
            const std::string id = normalizeSectionId(this->remoteSections[i].id);
            if (id == "all") {
                hasAllSection = true;
                if (allSectionIndex < 0)
                    allSectionIndex = static_cast<int>(i);
            } else if (id == "updates" || id == "update") {
                hasUpdatesSection = true;
            } else if (id == "dlc" || id == "addon" || id == "add-on" || id == "add_ons") {
                hasDlcSection = true;
            }
        }

        if (!hasAllSection || (hasUpdatesSection && hasDlcSection))
            return;

        auto isBaseInstalled = [&](std::uint64_t baseTitleId) {
            if (baseTitleId == 0)
                return false;
            const auto it = this->installedSnapshot.baseInstalled.find(baseTitleId);
            return (it != this->installedSnapshot.baseInstalled.end()) && it->second;
        };

        std::vector<remoteInstStuff::RemoteItem> updates;
        std::vector<remoteInstStuff::RemoteItem> dlcs;
        std::unordered_set<std::string> seenUpdateKeys;
        std::unordered_set<std::string> seenDlcKeys;

        for (const auto& section : this->remoteSections) {
            const std::string id = normalizeSectionId(section.id);
            if (id == "installed" || id == "updates" || id == "update" || id == "dlc" || id == "addon" || id == "add-on" || id == "add_ons")
                continue;

            for (const auto& item : section.items) {
                if (item.url.empty() && !item.hasTitleId && !item.hasAppId)
                    continue;

                std::uint64_t baseTitleId = 0;
                const bool hasBaseTitleId = DeriveBaseTitleId(item, baseTitleId);
                const bool baseInstalled = hasBaseTitleId && isBaseInstalled(baseTitleId);

                if (!hasUpdatesSection && IsUpdateItem(item)) {
                    if (!baseInstalled)
                        continue;
                    const std::string key = BuildItemIdentityKey(item);
                    if (!key.empty() && !seenUpdateKeys.insert(key).second)
                        continue;
                    updates.push_back(item);
                    continue;
                }

                if (!hasDlcSection && IsDlcItem(item)) {
                    if (item.hasTitleId && this->installedSnapshot.installedDlcIds.count(item.titleId))
                        continue;
                    if (hasBaseTitleId && !baseInstalled)
                        RemoteDlcTrace("legacy dlc keep without base installed name='%s'", TraceNamePreview(item.name).c_str());
                    const std::string key = BuildItemIdentityKey(item);
                    if (!key.empty() && !seenDlcKeys.insert(key).second)
                        continue;
                    dlcs.push_back(item);
                }
            }
        }

        auto sortByName = [](std::vector<remoteInstStuff::RemoteItem>& items) {
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                return inst::util::ignoreCaseCompare(a.name, b.name);
            });
        };
        sortByName(updates);
        sortByName(dlcs);

        int insertIndex = (allSectionIndex >= 0) ? (allSectionIndex + 1) : static_cast<int>(this->remoteSections.size());
        if (!hasUpdatesSection && !updates.empty()) {
            remoteInstStuff::RemoteSection updatesSection;
            updatesSection.id = "updates";
            updatesSection.title = "Updates";
            updatesSection.items = std::move(updates);
            this->remoteSections.insert(this->remoteSections.begin() + insertIndex, std::move(updatesSection));
            insertIndex++;
        }
        if (!hasDlcSection && !dlcs.empty()) {
            remoteInstStuff::RemoteSection dlcSection;
            dlcSection.id = "dlc";
            dlcSection.title = "DLC";
            dlcSection.items = std::move(dlcs);
            this->remoteSections.insert(this->remoteSections.begin() + insertIndex, std::move(dlcSection));
        }
    }

    void remoteInstPage::cacheAvailableUpdates() {
        this->availableUpdates.clear();
        std::unordered_set<std::string> seenKeys;
        for (const auto& section : this->remoteSections) {
            const bool sectionLooksLikeUpdates = (section.id == "updates" || section.id == "update");
            for (const auto& item : section.items) {
                if (!sectionLooksLikeUpdates && !IsUpdateItem(item))
                    continue;

                const std::string identity = BuildItemIdentityKey(item);
                if (!identity.empty() && !seenKeys.insert(identity).second)
                    continue;
                this->availableUpdates.push_back(item);
            }
        }
    }

    void remoteInstPage::filterOwnedSections() {
        if (this->remoteSections.empty())
            return;
        if (!this->buildInstalledSnapshot()) {
            RemoteDlcTrace("filterOwnedSections buildInstalledSnapshot failed");
            return;
        }
        RemoteDlcTrace("filterOwnedSections begin sections=%llu snapshotBases=%llu snapshotDlcs=%llu",
            static_cast<unsigned long long>(this->remoteSections.size()),
            static_cast<unsigned long long>(this->installedSnapshot.baseInstalled.size()),
            static_cast<unsigned long long>(this->installedSnapshot.installedDlcIds.size()));
        RemoteDlcTrace("filter mode nativeDlcSectionPresent=%d", this->nativeDlcSectionPresent ? 1 : 0);

        auto looksLikeDlcTitleId = [](std::uint64_t titleId) {
            const std::uint64_t suffix = titleId & 0xFFFULL;
            return suffix != 0x000ULL && suffix != 0x800ULL;
        };

        auto resolveItemDlcTitleId = [&](const remoteInstStuff::RemoteItem& item, std::uint64_t& outTitleId) {
            std::uint64_t parsedAppId = 0;
            if (item.hasAppId && TryParseTitleIdText(item.appId, parsedAppId) && looksLikeDlcTitleId(parsedAppId)) {
                outTitleId = parsedAppId;
                return true;
            }

            if (item.hasTitleId && looksLikeDlcTitleId(item.titleId)) {
                outTitleId = item.titleId;
                return true;
            }

            return false;
        };

        auto isDlcInstalledByTitleId = [&](std::uint64_t dlcTitleId) {
            if (dlcTitleId == 0)
                return false;
            return this->installedSnapshot.installedDlcIds.count(dlcTitleId) > 0;
        };

        auto isBaseInstalled = [&](const remoteInstStuff::RemoteItem& item, std::uint32_t& outVersion) {
            std::uint64_t baseTitleId = 0;
            if (!DeriveBaseTitleId(item, baseTitleId))
                return false;
            const auto baseIt = this->installedSnapshot.baseInstalled.find(baseTitleId);
            if (baseIt == this->installedSnapshot.baseInstalled.end() || !baseIt->second)
                return false;

            const auto verIt = this->installedSnapshot.installedUpdateVersion.find(baseTitleId);
            outVersion = (verIt != this->installedSnapshot.installedUpdateVersion.end()) ? verIt->second : 0;
            return true;
        };

        auto isDlcInstalled = [&](const remoteInstStuff::RemoteItem& item) {
            if (!IsDlcItem(item))
                return false;

            std::uint64_t dlcTitleId = 0;
            if (!resolveItemDlcTitleId(item, dlcTitleId)) {
                RemoteDlcTrace("dlc resolve failed name='%s' appType=%d hasTitleId=%d titleId='%s' hasAppId=%d appId='%s'",
                    TraceNamePreview(item.name).c_str(), item.appType, item.hasTitleId ? 1 : 0,
                    item.hasTitleId ? FormatTitleIdHex(item.titleId).c_str() : "none",
                    item.hasAppId ? 1 : 0, item.hasAppId ? item.appId.c_str() : "none");
                return false;
            }
            if (isDlcInstalledByTitleId(dlcTitleId)) {
                RemoteDlcTrace("dlc installed yes dlcId=%s name='%s'", FormatTitleIdHex(dlcTitleId).c_str(), TraceNamePreview(item.name).c_str());
                return true;
            }
            RemoteDlcTrace("dlc installed no dlcId=%s name='%s'", FormatTitleIdHex(dlcTitleId).c_str(), TraceNamePreview(item.name).c_str());
            return false;
        };

        for (auto& section : this->remoteSections) {
            if (section.items.empty())
                continue;
            if (section.id != "updates" && section.id != "dlc")
                continue;

            // Both branches require the base to be installed, so the check is unconditional.
            std::erase_if(section.items, [&](const remoteInstStuff::RemoteItem& item) {
                std::uint32_t installedVersion = 0;
                if (!isBaseInstalled(item, installedVersion)) {
                    RemoteDlcTrace("drop reason=base_not_installed name='%s'", TraceNamePreview(item.name).c_str());
                    return true;
                }
                if (section.id == "updates" || IsUpdateItem(item))
                    return item.hasAppVersion && item.appVersion <= installedVersion;
                return isDlcInstalled(item);
            });
        }

        if (inst::config::remoteLegacyMode || this->catalogCacheUsedLegacyFallback) {
            for (auto& section : this->remoteSections) {
                if (section.items.empty())
                    continue;
                if (section.id == "all" || section.id == "installed" || section.id == "cheats")
                    continue;
                if (section.id == "updates" || section.id == "dlc")
                    continue;

                std::erase_if(section.items, [&](const remoteInstStuff::RemoteItem& item) {
                    if (!IsDlcItem(item))
                        return false;
                    std::uint32_t installedVersion = 0;
                    return isDlcInstalled(item) || !isBaseInstalled(item, installedVersion);
                });
            }
        }

        if (inst::config::remoteHideInstalled) {
            for (auto& section : this->remoteSections) {
                if (section.items.empty())
                    continue;
                if (section.id == "installed" || section.id == "updates" || section.id == "update" || section.id == "cheats" ||
                    (this->saveSyncEnabled && (section.id == "saves" || section.id == "save")))
                    continue;

                std::erase_if(section.items, [&](const remoteInstStuff::RemoteItem& item) {
                    std::uint32_t installedVersion = 0;
                    if (IsBaseItem(item))
                        return isBaseInstalled(item, installedVersion);
                    if (IsUpdateItem(item))
                        return isBaseInstalled(item, installedVersion) && item.hasAppVersion && item.appVersion <= installedVersion;
                    if (IsDlcItem(item))
                        return isDlcInstalled(item);
                    return false;
                });
            }
        }

        auto hasSuffix = [](const std::string& text, const std::string& suffix) {
            if (text.size() < suffix.size())
                return false;
            return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        };

        auto appendTypeLabels = [&](remoteInstStuff::RemoteSection& section) {
            static const std::string kUpdateSuffix = " (Update)";
            static const std::string kDlcSuffix = " (DLC)";
            for (auto& item : section.items) {
                if (item.appType == NcmContentMetaType_Patch) {
                    if (!hasSuffix(item.name, kUpdateSuffix))
                        item.name += kUpdateSuffix;
                } else if (item.appType == NcmContentMetaType_AddOnContent) {
                    if (!hasSuffix(item.name, kDlcSuffix))
                        item.name += kDlcSuffix;
                }
            }
        };

        for (auto& section : this->remoteSections) {
            if (section.items.empty())
                continue;
            appendTypeLabels(section);
        }

    }

    bool remoteInstPage::isFastListScrolling() const {
        return !this->remoteGridMode && this->holdDirection != 0 && this->holdStartTick != 0
            && armGetSystemTick() - this->holdStartTick >= armGetSystemTickFreq() * 400 / 1000;
    }

    void remoteInstPage::updatePreview() {
        if (this->isFastListScrolling()) {
            this->previewImage->SetVisible(false);
            this->imageLoadingText->SetVisible(false);
            this->previewKey.clear();
            return;
        }
        if (this->remoteGridMode) {
            this->previewImage->SetVisible(false);
            this->previewKey.clear();
            this->refreshImageLoadingText();
            return;
        }
        if (this->visibleItems.empty()) {
            this->previewImage->SetVisible(false);
            this->previewKey.clear();
            this->refreshImageLoadingText();
            return;
        }

        int selectedIndex = this->menu->GetSelectedIndex();
        if (selectedIndex < 0 || selectedIndex >= (int)this->visibleItems.size())
            return;
        const auto& item = this->visibleItems[selectedIndex];
        std::uint64_t offlineIconBaseId = 0;
        const bool hasOfflineIcon = HasOfflineIconForItem(item, &offlineIconBaseId);
        std::string key;
        if (item.url.empty()) {
            key = "installed:" + std::to_string(item.titleId);
        } else if (hasOfflineIcon) {
            key = "offline:" + std::to_string(static_cast<unsigned long long>(offlineIconBaseId));
        } else if (item.hasIconUrl) {
            key = item.iconUrl;
        } else {
            key = item.url;
        }

        const bool selectionChanged = key != this->previewKey;
        this->previewKey = key;
        const bool previewNeedsRefresh = this->iconDownloadUiDirty.exchange(false);
        if (!selectionChanged && !previewNeedsRefresh) {
            this->refreshImageLoadingText(true);
            return;
        }
        auto applyPreviewLayout = [&]() {
            this->previewImage->SetX(920);
            this->previewImage->SetY(170);
            this->previewImage->SetWidth(304);
            this->previewImage->SetHeight(304);
        };

        if (item.url.empty()) {
            Result rc = nsInitialize();
            if (R_SUCCEEDED(rc)) {
                u64 baseId = tin::util::GetBaseTitleId(item.titleId, static_cast<NcmContentMetaType>(item.appType));
                NsApplicationControlData appControlData;
                u64 sizeRead = 0;
                if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, baseId, &appControlData, sizeof(NsApplicationControlData), &sizeRead))) {
                    u64 iconSize = 0;
                    if (sizeRead > sizeof(appControlData.nacp))
                        iconSize = sizeRead - sizeof(appControlData.nacp);
                    if (iconSize > 0) {
                        this->previewImage->SetJpegImage(appControlData.icon, iconSize);
                        applyPreviewLayout();
                        this->previewImage->SetVisible(true);
                        nsExit();
                        return;
                    }
                }
                nsExit();
            }
            this->previewImage->SetImage(inst::theme::Asset("romfs:/images/icons/title-placeholder.png"));
            applyPreviewLayout();
            this->previewImage->SetVisible(true);
            this->refreshImageLoadingText();
            return;
        }

        if (hasOfflineIcon) {
            std::vector<std::uint8_t> offlineIconData;
            if (TryLoadOfflineIconForItem(item, offlineIconData) && !offlineIconData.empty()) {
                this->previewImage->SetJpegImage(offlineIconData.data(), static_cast<s32>(offlineIconData.size()));
                applyPreviewLayout();
                this->previewImage->SetVisible(true);
                this->refreshImageLoadingText();
                return;
            }
        }

        if (item.hasIconUrl) {
            const std::string filePath = GetRemoteGridIconCachePath(item);
            if (std::filesystem::exists(filePath)) {
                this->previewImage->SetImage(filePath);
                applyPreviewLayout();
                this->previewImage->SetVisible(true);
                this->refreshImageLoadingText(true);
                return;
            }

            this->queueIconDownload(item, filePath);
            this->refreshImageLoadingText();
        }

        this->previewImage->SetImage(inst::theme::Asset("romfs:/images/icons/title-placeholder.png"));
        applyPreviewLayout();
        this->previewImage->SetVisible(true);
        this->refreshImageLoadingText();
    }


    void remoteInstPage::updateDebug() {
        if (!this->debugVisible) {
            this->debugText->SetVisible(false);
            return;
        }
        if (this->visibleItems.empty()) {
            std::string text = "debug: no items";
            if (!this->remoteSections.empty() && this->selectedSectionIndex >= 0 && this->selectedSectionIndex < (int)this->remoteSections.size()) {
                const auto& section = this->remoteSections[this->selectedSectionIndex];
                text += " section=" + section.id;
                if (section.id == "updates") {
                    text += " pre=" + std::to_string(this->availableUpdates.size());
                    text += " post=" + std::to_string(section.items.size());
                }
            }
            this->debugText->SetText(text);
            this->debugText->SetVisible(true);
            return;
        }

        int selectedIndex = this->remoteGridMode ? this->remoteGridIndex : this->menu->GetSelectedIndex();
        if (this->isInstalledSection() && this->remoteGridMode)
            selectedIndex = this->gridSelectedIndex;
        if (selectedIndex < 0 || selectedIndex >= (int)this->visibleItems.size())
            return;
        const auto& item = this->visibleItems[selectedIndex];

        std::uint64_t baseTitleId = 0;
        bool hasBase = DeriveBaseTitleId(item, baseTitleId);
        bool installed = false;
        std::uint32_t installedVersion = 0;

        if (hasBase) {
            if (R_SUCCEEDED(nsInitialize()) && R_SUCCEEDED(ncmInitialize())) {
                installed = tin::util::IsTitleInstalled(baseTitleId);
                if (installed) {
                    tin::util::GetInstalledUpdateVersion(baseTitleId, installedVersion);
                    if (installedVersion == 0)
                        TryGetInstalledUpdateVersionNcm(baseTitleId, installedVersion);
                }
                ncmExit();
                nsExit();
            }
        }

        char baseBuf[32] = {0};
        if (hasBase)
            std::snprintf(baseBuf, sizeof(baseBuf), "%016lx", baseTitleId);
        else
            std::snprintf(baseBuf, sizeof(baseBuf), "unknown");

        std::string text = "debug: base=" + std::string(baseBuf);
        text += " installed=" + std::string(installed ? "1" : "0");
        text += " inst_ver=" + std::to_string(installedVersion);
        text += " avail_ver=" + (item.hasAppVersion ? std::to_string(item.appVersion) : std::string("n/a"));
        text += " type=" + std::to_string(item.appType);
        text += " has_appv=" + std::string(item.hasAppVersion ? "1" : "0");
        text += " has_tid=" + std::string(item.hasTitleId ? "1" : "0");
        text += " has_appid=" + std::string(item.hasAppId ? "1" : "0");
        if (item.hasAppId)
            text += " app_id=" + item.appId;
        this->debugText->SetText(text);
        this->debugText->SetVisible(true);
    }

    void remoteInstPage::drawMenuItems(bool clearItems) {
        this->browseDecorationKey.clear();
        std::string previousSelectionKey;
        int previousSelectionIndex = 0;
        if (!this->visibleItems.empty()) {
            int currentIndex = this->remoteGridMode ? this->remoteGridIndex : this->menu->GetSelectedIndex();
            if (this->isInstalledSection() && this->remoteGridMode)
                currentIndex = this->gridSelectedIndex;
            if (currentIndex >= 0 && currentIndex < static_cast<int>(this->visibleItems.size())) {
                previousSelectionKey = BuildItemIdentityKey(this->visibleItems[static_cast<std::size_t>(currentIndex)]);
                previousSelectionIndex = currentIndex;
            }
        }

        if (clearItems)
            this->selectedItems.clear();
        this->resetIconDownloadState();
        if (this->isInstalledSection())
            this->ensureInstalledSectionBuilt();
        this->emptySectionText->SetVisible(false);
        this->listMarqueeMaskRect->SetVisible(false);
        this->listMarqueeTintRect->SetVisible(false);
        this->listMarqueeOverlayText->SetVisible(false);
        this->listMarqueeClipEnabled = false;
        this->listMarqueeFadeRect->SetVisible(false);
        this->menu->ClearItems();
        this->visibleItems.clear();
        const auto& items = this->getCurrentItems();
        if (!this->searchQuery.empty()) {
            const std::string normalizedQuery = NormalizeSearchKey(this->searchQuery);
            for (const auto& item : items) {
                std::string name = NormalizeSearchKey(item.name);
                if (name.find(normalizedQuery) != std::string::npos)
                    this->visibleItems.push_back(item);
            }
        } else {
            this->visibleItems = items;
        }

        if (this->isCheatsSection() && inst::config::remoteHideIncompatibleCheats) {
            this->visibleItems.erase(std::remove_if(this->visibleItems.begin(), this->visibleItems.end(), [&](const auto& item) {
                return !this->isCheatCompatible(item);
            }), this->visibleItems.end());
        }

        if (this->isAllSection() && inst::config::remoteAllBaseOnly) {
            std::vector<remoteInstStuff::RemoteItem> baseOnlyItems;
            baseOnlyItems.reserve(this->visibleItems.size());
            for (const auto& item : this->visibleItems) {
                if (IsBaseItem(item))
                    baseOnlyItems.push_back(item);
            }
            this->visibleItems = std::move(baseOnlyItems);
        }

        if (!this->isAllSection())
            this->applyBrowseSort();


        if (!this->remoteSections.empty() && this->selectedSectionIndex >= 0 && this->selectedSectionIndex < static_cast<int>(this->remoteSections.size()) && this->visibleItems.empty()) {
            const auto &section = this->remoteSections[this->selectedSectionIndex];
            if (section.id == "updates" || section.id == "dlc") {
                this->emptySectionText->SetText(section.id == "updates" ? "No updates available." : "No DLC available.");
                CenterTextX(this->emptySectionText);
                this->emptySectionText->SetY(350);
                this->emptySectionText->SetVisible(true);
            } else if (section.id == "saves" || section.id == "save") {
                this->emptySectionText->SetText("No saves available.");
                CenterTextX(this->emptySectionText);
                this->emptySectionText->SetY(350);
                this->emptySectionText->SetVisible(true);
            } else if (section.id == "cheats") {
                this->emptySectionText->SetText("No cheats available from this AeroFoil server.");
                CenterTextX(this->emptySectionText);
                this->emptySectionText->SetY(350);
                this->emptySectionText->SetVisible(true);
            }
        }

        if (this->isInstalledSection() && this->remoteGridMode) {
            this->menu->SetVisible(false);
            this->previewImage->SetVisible(false);
            this->emptySectionText->SetVisible(false);
            this->listMarqueeMaskRect->SetVisible(false);
            this->listMarqueeTintRect->SetVisible(false);
            this->listMarqueeOverlayText->SetVisible(false);
            this->listMarqueeClipEnabled = false;
            this->listMarqueeFadeRect->SetVisible(false);
            if (this->gridSelectedIndex >= (int)this->visibleItems.size())
                this->gridSelectedIndex = 0;
            this->updateInstalledGrid();
            this->updateDescriptionPanel();
            return;
        }

        if (this->remoteGridMode) {
            this->listMarqueeMaskRect->SetVisible(false);
            this->listMarqueeTintRect->SetVisible(false);
            this->listMarqueeOverlayText->SetVisible(false);
            this->listMarqueeClipEnabled = false;
            this->listMarqueeFadeRect->SetVisible(false);
            this->updateRemoteGrid();
            this->updateDescriptionPanel();
            return;
        }

        for (auto& img : this->gridImages)
            img->SetVisible(false);
        this->gridHighlight->SetVisible(false);
        this->gridTitleText->SetVisible(false);
        for (auto& icon : this->remoteGridSelectIcons)
            icon->SetVisible(false);
        this->menu->SetVisible(!this->refreshingLibrary);

        for (std::size_t i = 0; i < this->visibleItems.size(); i++) {
            this->renderLibraryRefresh();
            const auto& item = this->visibleItems[i];
            const std::string itm = this->buildListMenuLabel(item);
            auto entry = pu::ui::elm::MenuItem::New(itm);
            entry->SetColor(COLOR("#FFFFFFFF"));
            entry->SetIcon(inst::theme::Asset("romfs:/images/icons/title-placeholder.png"));
            this->menu->AddItem(entry);
        }

        int restoredIndex = previousSelectionIndex;
        if (!previousSelectionKey.empty()) {
            for (std::size_t i = 0; i < this->visibleItems.size(); i++) {
                if (BuildItemIdentityKey(this->visibleItems[i]) == previousSelectionKey) {
                    restoredIndex = static_cast<int>(i);
                    break;
                }
            }
        }

        if (!this->visibleItems.empty()) {
            if (restoredIndex < 0 || restoredIndex >= static_cast<int>(this->visibleItems.size()))
                restoredIndex = 0;
        } else {
            restoredIndex = 0;
        }

        if (!this->menu->GetItems().empty()) {
            const int maxIndex = static_cast<int>(this->menu->GetItems().size()) - 1;
            if (restoredIndex > maxIndex)
                restoredIndex = maxIndex;
            if (restoredIndex < 0)
                restoredIndex = 0;

            // Avoid resetting the menu's internal top row when the same item remains selected.
            if (restoredIndex != previousSelectionIndex || previousSelectionKey.empty())
                this->menu->SetSelectedIndex(restoredIndex);
        }
        this->remoteGridIndex = restoredIndex;
        this->gridSelectedIndex = restoredIndex;
        this->listMarqueeIndex = -1;
        this->listVisibleTopIndex = this->menu->GetVisibleTopIndex();
        this->listMarqueeOffset = 0;
        this->listMarqueeMaxOffset = 0;
        this->listMarqueeLastTick = 0;
        this->listMarqueePauseUntilTick = 0;
        this->listMarqueeEndPauseUntilTick = 0;
        this->listMarqueeSpeedRemainder = 0;
        this->listMarqueeFadeStartTick = 0;
        this->listMarqueePhase = kListMarqueePhasePause;
        this->listMarqueeFadeAlpha = 0;
        this->updateListMarquee(true);
        this->updateDescriptionPanel();
    }

    void remoteInstPage::refreshListSelectionIcons() {
        this->browseDecorationKey.clear();
        this->updateBrowseDecorations();
    }

    void remoteInstPage::updateInstalledGrid() {
        for (auto& badge : this->remoteGridSelectBadgeBacks) badge->SetVisible(false);
        this->listMarqueeMaskRect->SetVisible(false);
        this->listMarqueeTintRect->SetVisible(false);
        this->listMarqueeOverlayText->SetVisible(false);
        this->listMarqueeClipEnabled = false;
        this->listMarqueeFadeRect->SetVisible(false);
        if (!this->isInstalledSection() || !this->remoteGridMode) {
            for (auto& img : this->gridImages)
                img->SetVisible(false);
            this->gridHighlight->SetVisible(false);
            this->gridTitleText->SetVisible(false);
            this->gridPage = -1;
            this->updateDescriptionPanel();
            return;
        }

        this->menu->SetVisible(false);
        this->previewImage->SetVisible(false);
        this->emptySectionText->SetVisible(false);

        if (this->visibleItems.empty()) {
            for (auto& img : this->gridImages)
                img->SetVisible(false);
            this->gridHighlight->SetVisible(false);
            this->gridTitleText->SetVisible(false);
            this->gridPage = -1;
            this->updateDescriptionPanel();
            return;
        }

        if (this->gridSelectedIndex < 0)
            this->gridSelectedIndex = 0;
        if (this->gridSelectedIndex >= (int)this->visibleItems.size())
            this->gridSelectedIndex = (int)this->visibleItems.size() - 1;

        int page = this->gridSelectedIndex / kGridItemsPerPage;
        int pageStart = page * kGridItemsPerPage;
        int maxIndex = (int)this->visibleItems.size();

        if (page != this->gridPage) {
            bool nsReady = R_SUCCEEDED(nsInitialize());
            for (int i = 0; i < kGridItemsPerPage; i++) {
                int itemIndex = pageStart + i;
                int row = i / kGridCols;
                int col = i % kGridCols;
                int x = kGridStartX + (col * (kGridTileWidth + kGridGap));
                int y = kGridStartY + (row * (kGridTileHeight + kGridGap));
                this->gridImages[i]->SetX(x);
                this->gridImages[i]->SetY(y);

                if (itemIndex >= maxIndex) {
                    this->gridImages[i]->SetVisible(false);
                    continue;
                }

                const auto& item = this->visibleItems[itemIndex];
                bool applied = false;
                if (nsReady && item.hasTitleId) {
                    u64 baseId = tin::util::GetBaseTitleId(item.titleId, static_cast<NcmContentMetaType>(item.appType));
                    NsApplicationControlData appControlData;
                    u64 sizeRead = 0;
                    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, baseId, &appControlData, sizeof(NsApplicationControlData), &sizeRead))) {
                        u64 iconSize = 0;
                        if (sizeRead > sizeof(appControlData.nacp))
                            iconSize = sizeRead - sizeof(appControlData.nacp);
                        if (iconSize > 0) {
                            this->gridImages[i]->SetJpegImage(appControlData.icon, iconSize);
                            this->gridImages[i]->SetWidth(kGridTileWidth);
                            this->gridImages[i]->SetHeight(kGridTileHeight);
                            applied = true;
                        }
                    }
                }

                if (!applied && item.hasTitleId) {
                    std::vector<std::uint8_t> offlineIconData;
                    if (TryLoadOfflineIconForItem(item, offlineIconData) && !offlineIconData.empty()) {
                        this->gridImages[i]->SetJpegImage(offlineIconData.data(), static_cast<s32>(offlineIconData.size()));
                        this->gridImages[i]->SetWidth(kGridTileWidth);
                        this->gridImages[i]->SetHeight(kGridTileHeight);
                        applied = true;
                    }
                }

                if (!applied) {
                    this->gridImages[i]->SetImage(inst::theme::Asset("romfs:/images/icons/title-placeholder.png"));
                    this->gridImages[i]->SetWidth(kGridTileWidth);
                    this->gridImages[i]->SetHeight(kGridTileHeight);
                }

                this->gridImages[i]->SetVisible(true);
            }
            if (nsReady)
                nsExit();
            this->gridPage = page;
        }

        int slot = this->gridSelectedIndex - pageStart;
        if (slot >= 0 && slot < kGridItemsPerPage) {
            int row = slot / kGridCols;
            int col = slot % kGridCols;
            int x = kGridStartX + (col * (kGridTileWidth + kGridGap)) - 4;
            int y = kGridStartY + (row * (kGridTileHeight + kGridGap)) - 4;
            this->gridHighlight->SetX(x);
            this->gridHighlight->SetY(y);
            this->gridHighlight->SetWidth(kGridTileWidth + 8);
            this->gridHighlight->SetHeight(kGridTileHeight + 8);
            this->gridHighlight->SetVisible(true);
        } else {
            this->gridHighlight->SetVisible(false);
        }

        if (this->gridSelectedIndex >= 0 && this->gridSelectedIndex < (int)this->visibleItems.size()) {
            std::string title = BuildGridTitleWithSize(this->visibleItems[this->gridSelectedIndex]);
            this->gridTitleText->SetText(BuildSingleLineGridTitle(title));
            this->gridTitleText->SetVisible(true);
        } else {
            this->gridTitleText->SetVisible(false);
        }
        this->updateDescriptionPanel();
    }

    void remoteInstPage::updateRemoteGrid() {
        this->listMarqueeMaskRect->SetVisible(false);
        this->listMarqueeTintRect->SetVisible(false);
        this->listMarqueeOverlayText->SetVisible(false);
        this->listMarqueeClipEnabled = false;
        this->listMarqueeFadeRect->SetVisible(false);
        if (!this->remoteGridMode || this->visibleItems.empty()) {
            for (auto& img : this->gridImages)
                img->SetVisible(false);
            this->gridHighlight->SetVisible(false);
            this->gridTitleText->SetVisible(false);
            for (auto& highlight : this->remoteGridSelectHighlights)
                highlight->SetVisible(false);
            for (auto& icon : this->remoteGridSelectIcons)
                icon->SetVisible(false);
            this->imageLoadingText->SetVisible(false);
            this->remoteGridPage = -1;
            this->updateDescriptionPanel();
            return;
        }

        this->menu->SetVisible(false);
        this->previewImage->SetVisible(false);

        if (this->remoteGridIndex < 0)
            this->remoteGridIndex = 0;
        if (this->remoteGridIndex >= (int)this->visibleItems.size())
            this->remoteGridIndex = (int)this->visibleItems.size() - 1;

        int page = this->remoteGridIndex / kGridItemsPerPage;
        int pageStart = page * kGridItemsPerPage;
        int maxIndex = (int)this->visibleItems.size();
        if (page != this->remoteGridPage) {
            this->resetIconDownloadState();

            for (int i = 0; i < kGridItemsPerPage; i++) {
                int itemIndex = pageStart + i;
                int row = i / kGridCols;
                int col = i % kGridCols;
                int x = kGridStartX + (col * (kGridTileWidth + kGridGap));
                int y = kGridStartY + (row * (kGridTileHeight + kGridGap));
                this->gridImages[i]->SetX(x);
                this->gridImages[i]->SetY(y);
                this->gridImages[i]->SetWidth(kGridTileWidth);
                this->gridImages[i]->SetHeight(kGridTileHeight);
                if (itemIndex >= maxIndex) {
                    this->gridImages[i]->SetVisible(false);
                    continue;
                }

                const auto& item = this->visibleItems[itemIndex];
                bool applied = false;
                std::vector<std::uint8_t> offlineIconData;
                if (TryLoadOfflineIconForItem(item, offlineIconData) && !offlineIconData.empty()) {
                    this->gridImages[i]->SetJpegImage(offlineIconData.data(), static_cast<s32>(offlineIconData.size()));
                    this->gridImages[i]->SetWidth(kGridTileWidth);
                    this->gridImages[i]->SetHeight(kGridTileHeight);
                    applied = true;
                } else if (item.hasIconUrl) {
                    std::string filePath = GetRemoteGridIconCachePath(item);
                    if (!filePath.empty() && std::filesystem::exists(filePath)) {
                        this->gridImages[i]->SetImage(filePath);
                        this->gridImages[i]->SetWidth(kGridTileWidth);
                        this->gridImages[i]->SetHeight(kGridTileHeight);
                        applied = true;
                    } else if (!filePath.empty()) {
                        this->queueIconDownload(item, filePath);
                    }
                }

                if (!applied) {
                    this->gridImages[i]->SetImage(inst::theme::Asset("romfs:/images/icons/title-placeholder.png"));
                    this->gridImages[i]->SetWidth(kGridTileWidth);
                    this->gridImages[i]->SetHeight(kGridTileHeight);
                }
                this->gridImages[i]->SetVisible(true);
            }

            this->remoteGridPage = page;
        }

        if (this->iconDownloadUiDirty.exchange(false)) {
            for (int i = 0; i < kGridItemsPerPage; i++) {
                const int itemIndex = pageStart + i;
                if (itemIndex < 0 || itemIndex >= maxIndex)
                    continue;

                const auto& item = this->visibleItems[itemIndex];
                const std::string filePath = GetRemoteGridIconCachePath(item);
                if (!filePath.empty() && std::filesystem::exists(filePath)) {
                    this->gridImages[i]->SetImage(filePath);
                    this->gridImages[i]->SetWidth(kGridTileWidth);
                    this->gridImages[i]->SetHeight(kGridTileHeight);
                    this->gridImages[i]->SetVisible(true);
                }
            }
        }
        this->refreshImageLoadingText(true);

        for (int i = 0; i < kGridItemsPerPage; i++) {
            int itemIndex = pageStart + i;
            int row = i / kGridCols;
            int col = i % kGridCols;
            int x = kGridStartX + (col * (kGridTileWidth + kGridGap));
            int y = kGridStartY + (row * (kGridTileHeight + kGridGap));

            if (itemIndex >= maxIndex) {
                this->remoteGridSelectBadgeBacks[i]->SetVisible(false);
                this->remoteGridSelectHighlights[i]->SetVisible(false);
                this->remoteGridSelectIcons[i]->SetVisible(false);
                continue;
            }

            const auto& item = this->visibleItems[itemIndex];
            bool isSelected = false;
            if (!this->selectedItems.empty() && !item.url.empty()) {
                isSelected = std::any_of(this->selectedItems.begin(), this->selectedItems.end(), [&](const auto& entry) {
                    return entry.url == item.url;
                });
            }
            const int iconSize = 56;
            const int badgeX = x + kGridTileWidth - iconSize - 6;
            const int badgeY = y + 6;
            this->remoteGridSelectBadgeBacks[i]->SetX(badgeX);
            this->remoteGridSelectBadgeBacks[i]->SetY(badgeY);
            this->remoteGridSelectBadgeBacks[i]->SetVisible(isSelected);
            this->remoteGridSelectIcons[i]->SetX(badgeX + (iconSize - this->remoteGridSelectIcons[i]->GetTextWidth()) / 2);
            this->remoteGridSelectIcons[i]->SetY(badgeY + (iconSize - this->remoteGridSelectIcons[i]->GetTextHeight()) / 2);
            this->remoteGridSelectIcons[i]->SetVisible(isSelected);
            this->remoteGridSelectHighlights[i]->SetX(x - 4);
            this->remoteGridSelectHighlights[i]->SetY(y - 4);
            this->remoteGridSelectHighlights[i]->SetWidth(kGridTileWidth + 8);
            this->remoteGridSelectHighlights[i]->SetHeight(kGridTileHeight + 8);
            this->remoteGridSelectHighlights[i]->SetVisible(isSelected);
        }

        int slot = this->remoteGridIndex - pageStart;
        if (slot >= 0 && slot < kGridItemsPerPage) {
            int row = slot / kGridCols;
            int col = slot % kGridCols;
            int x = kGridStartX + (col * (kGridTileWidth + kGridGap)) - 4;
            int y = kGridStartY + (row * (kGridTileHeight + kGridGap)) - 4;
            this->gridHighlight->SetX(x);
            this->gridHighlight->SetY(y);
            this->gridHighlight->SetWidth(kGridTileWidth + 8);
            this->gridHighlight->SetHeight(kGridTileHeight + 8);
            this->gridHighlight->SetVisible(true);
        } else {
            this->gridHighlight->SetVisible(false);
        }

        if (this->remoteGridIndex >= 0 && this->remoteGridIndex < (int)this->visibleItems.size()) {
            std::string title = BuildGridTitleWithSize(this->visibleItems[this->remoteGridIndex]);
            this->gridTitleText->SetText(BuildSingleLineGridTitle(title));
            this->gridTitleText->SetVisible(true);
        } else {
            this->gridTitleText->SetVisible(false);
        }
        this->updateDescriptionPanel();
    }

    void remoteInstPage::selectTitle(int selectedIndex) {
        if (this->isSaveSyncSection())
            return;
        if (selectedIndex < 0 || selectedIndex >= (int)this->visibleItems.size())
            return;
        const auto& item = this->visibleItems[selectedIndex];
        if (item.url.empty())
            return;
        if (queueAddMode && !instPage::canEnqueueTitle(item.url)) return;
        auto selected = std::find_if(this->selectedItems.begin(), this->selectedItems.end(), [&](const auto& entry) {
            return entry.url == item.url;
        });
        bool wasSelected = (selected != this->selectedItems.end());
        if (wasSelected)
            this->selectedItems.erase(selected);
        else
            this->selectedItems.push_back(item);
        if (wasSelected && IsBaseItem(item)) {
            std::uint64_t baseTitleId = 0;
            if (DeriveBaseTitleId(item, baseTitleId)) {
                this->selectedItems.erase(std::remove_if(this->selectedItems.begin(), this->selectedItems.end(), [&](const auto& entry) {
                    if (!IsUpdateItem(entry))
                        return false;
                    std::uint64_t updateBaseId = 0;
                    if (!DeriveBaseTitleId(entry, updateBaseId))
                        return false;
                    return updateBaseId == baseTitleId;
                }), this->selectedItems.end());
            }
        }
        this->updateRememberedSelection();
        if (this->remoteGridMode) {
            if (this->isInstalledSection()) {
                this->gridSelectedIndex = this->remoteGridIndex;
                this->updateInstalledGrid();
            } else {
                this->updateRemoteGrid();
            }
            return;
        }

        this->refreshListSelectionIcons();
    }

    void remoteInstPage::updateRememberedSelection() {
    }

    void remoteInstPage::startRemote(bool forceRefresh) {
        this->closeDescriptionOverlay();
        this->refreshingLibrary = false;
        ResetRemoteDlcTrace();
        RemoteDlcTrace("startRemote begin forceRefresh=%d remoteHideInstalled=%d hideInstalledSection=%d", forceRefresh ? 1 : 0, inst::config::remoteHideInstalled ? 1 : 0, inst::config::remoteHideInstalledSection ? 1 : 0);
        this->suppressBottomHints = true;
        this->nativeUpdatesSectionPresent = false;
        this->nativeDlcSectionPresent = false;
        this->saveSyncEnabled = false;
        this->saveSyncLoaded = false;
        this->pendingMotdFetch = false;
        this->descriptionVisible = false;
        this->descriptionOverlayVisible = false;
        this->descriptionOverlayLines.clear();
        this->descriptionOverlayOffset = 0;
        this->saveVersionSelectorVisible = false;
        this->saveVersionSelectorTitleId = 0;
        this->saveVersionSelectorLocalAvailable = false;
        this->saveVersionSelectorDeleteMode = false;
        this->saveVersionSelectorPreviousSectionIndex = 0;
        this->saveVersionSelectorTitleName.clear();
        this->saveVersionSelectorVersions.clear();
        this->setButtonsText("inst.remote.buttons_loading"_lang);
        this->menu->SetVisible(false);
        this->menu->ClearItems();
        this->infoImage->SetVisible(true);
        this->previewImage->SetVisible(false);
        this->emptySectionText->SetVisible(false);
        this->imageLoadingText->SetVisible(false);
        this->gridHighlight->SetVisible(false);
        this->gridTitleText->SetVisible(false);
        this->descriptionRect->SetVisible(false);
        this->descriptionText->SetVisible(false);
        this->descriptionOverlayRect->SetVisible(false);
        this->descriptionOverlayTitleText->SetVisible(false);
        this->descriptionOverlayBodyText->SetVisible(false);
        this->descriptionOverlayHintText->SetVisible(false);
        this->saveVersionSelectorRect->SetVisible(false);
        this->saveVersionSelectorTitleText->SetVisible(false);
        this->saveVersionSelectorMenu->SetVisible(false);
        this->saveVersionSelectorMenu->ClearItems();
        this->saveVersionSelectorDetailText->SetVisible(false);
        this->saveVersionSelectorHintText->SetVisible(false);
        for (auto& img : this->gridImages)
            img->SetVisible(false);
        for (auto& highlight : this->remoteGridSelectHighlights)
            highlight->SetVisible(false);
        for (auto& icon : this->remoteGridSelectIcons)
            icon->SetVisible(false);
        this->selectedItems.clear();
        this->visibleItems.clear();
        this->remoteSections.clear();
        this->availableUpdates.clear();
        this->saveSyncEntries.clear();
        if (forceRefresh) {
            this->installedSnapshot = {};
        } else {
            // Reuse installed-state caches across normal reloads to avoid rescanning NS metadata.
            // The per-section installed list is tied to remoteSections and must be rebuilt each time.
            this->installedSnapshot.installedSectionBuilt = false;
        }
        this->activeRemoteUrl.clear();
        this->searchQuery.clear();
        this->previewKey.clear();
        this->pageInfoText->SetText("");
        this->pageInfoText->SetVisible(false);
        this->infoImage->SetX((1280 - this->infoImage->GetWidth()) / 2);
        this->infoImage->SetY(86);
        this->setLoadingProgress(0, false);
        this->setLoadingProgressStage("Preparing Remote...");
        this->setLoadingProgress(-1, true);
        mainApp->LoadLayout(mainApp->remoteinstPage);
        mainApp->CallForRender();

        std::string remoteUrl = inst::config::remoteUrl;
        if (remoteUrl.empty()) {
            std::vector<inst::config::RemoteProfile> remotes = inst::config::LoadRemotes();
            if (!remotes.empty() && inst::config::SetActiveRemote(remotes.front(), true))
                remoteUrl = inst::config::remoteUrl;
        }
        if (remoteUrl.empty()) {
            remoteUrl = inst::util::softwareKeyboard("options.remote.url_hint"_lang, "http://", 200);
            if (remoteUrl.empty()) {
                mainApp->LoadLayout(mainApp->mainPage);
                return;
            }
            inst::config::remoteUrl = remoteUrl;
            inst::config::setConfig();
        }
        // Resolve once per profile/session, on a worker so the loading UI keeps
        // rendering. Explicit refresh rechecks connectivity for network changes.
        for (const auto& profile : inst::config::LoadRemotes()) {
            if ((inst::config::remoteProfileFile.empty() ? !inst::config::RemoteMatchesUrl(profile, remoteUrl)
                    : profile.fileName != inst::config::remoteProfileFile) ||
                profile.username != inst::config::remoteUser || profile.password != inst::config::remotePass ||
                profile.legacyMode != inst::config::remoteLegacyMode) continue;
            const std::string key = profile.fileName + "\n" + profile.lanUrl + "\n" + profile.wanUrl +
                "\n" + profile.connection + "\n" + profile.username + "\n" + profile.password +
                (profile.legacyMode ? "\nlegacy" : "\ncyberfoil") + "\n" + inst::util::getIPAddress();
            if (!forceRefresh && this->sessionRemoteKey == key && !this->sessionRemoteUrl.empty()) {
                remoteUrl = this->sessionRemoteUrl;
            } else {
                std::atomic<bool> resolved{false};
                std::string selected, resolveError;
                this->setLoadingProgressStage("Choosing Remote connection...");
                std::thread resolver([&]() {
                    try { selected = inst::remote::ResolveEndpoint(profile); }
                    catch (...) { resolveError = "Unable to choose a Remote connection."; }
                    resolved.store(true, std::memory_order_release);
                });
                while (!resolved.load(std::memory_order_acquire)) {
                    this->updateLoadingActivity();
                    mainApp->CallForRender();
                    svcSleepThread(33'000'000ULL);
                }
                resolver.join();
                if (!resolveError.empty() || selected.empty()) {
                    mainApp->CreateShowDialog("Remote connection", resolveError.empty() ? "No URL configured for this connection preference." : resolveError,
                        {"common.ok"_lang}, true);
                    mainApp->LoadLayout(mainApp->mainPage);
                    return;
                }
                remoteUrl = selected;
                this->sessionRemoteUrl = selected;
                this->sessionRemoteKey = key;
            }
            break;
        }
        this->activeRemoteUrl = remoteUrl;
        // Downloads scope Basic Auth to this origin. The profile's two saved
        // endpoints and preference remain unchanged; no setConfig() here.
        inst::config::remoteUrl = remoteUrl;

        std::string error;
        bool usedLegacyFallback = false;
        int loadingPercent = 5;
        auto updateLoadingProgress = [&](int percent, const char* stage = nullptr, bool forceRender = false) {
            const bool stageChanged = stage != nullptr && this->loadingStageLabel != stage;
            if (stage != nullptr)
                this->setLoadingProgressStage(stage);
            if (percent > 100)
                percent = 100;
            if (percent < loadingPercent)
                return;
            if (!forceRender && !stageChanged && percent == loadingPercent)
                return;
            loadingPercent = percent;
            this->setLoadingProgress(-1, true);
            mainApp->CallForRender();
        };
        this->setLoadingProgressStage("Fetching Remote list...");
        this->setLoadingProgress(-1, true);
        mainApp->CallForRender();

        const std::string cacheKey = remoteUrl + "\n" + inst::config::remoteUser + "\n" + inst::config::remotePass + "\n" + (inst::config::remoteLegacyMode ? "1" : "0");
        const bool canUseCatalogCache = !forceRefresh
            && this->catalogCacheValid
            && this->catalogCacheKey == cacheKey
            && !this->catalogCacheSections.empty()
            && (!this->catalogCacheUsedLegacyFallback || inst::config::remoteLegacyMode);

        if (canUseCatalogCache) {
            updateLoadingProgress(89, "Using cached catalog...", true);
            this->remoteSections = this->catalogCacheSections;
            usedLegacyFallback = this->catalogCacheUsedLegacyFallback;
        } else {
            std::atomic<bool> fetchDone{false};
            std::atomic<std::uint64_t> fetchDownloaded{0};
            std::atomic<std::uint64_t> fetchTotal{0};
            std::vector<remoteInstStuff::RemoteSection> fetchedSections;
            std::string fetchError;
            bool fetchUsedLegacyFallback = false;

            std::thread fetchThread([&]() {
                auto fetchProgressCb = [&](std::uint64_t downloaded, std::uint64_t total) {
                    fetchDownloaded.store(downloaded, std::memory_order_relaxed);
                    fetchTotal.store(total, std::memory_order_relaxed);
                };
                fetchedSections = remoteInstStuff::FetchRemoteSections(
                    remoteUrl,
                    inst::config::remoteUser,
                    inst::config::remotePass,
                    fetchError,
                    &fetchUsedLegacyFallback,
                    fetchProgressCb
                );
                fetchDone.store(true, std::memory_order_release);
            });

            while (!fetchDone.load(std::memory_order_acquire)) {
                const std::uint64_t downloaded = fetchDownloaded.load(std::memory_order_relaxed);
                const std::uint64_t total = fetchTotal.load(std::memory_order_relaxed);
                // Show the actual request's progress. Unknown lengths use a
                // moving bar instead of guessing completion from received bytes.
                const int requestPercent = total > 0 && downloaded < total
                    ? static_cast<int>(std::min(1.0, static_cast<double>(downloaded) / total) * 100.0)
                    : -1;
                const char* stage = total > 0 && downloaded >= total
                    ? "Preparing catalog..." : "Fetching Remote list...";
                this->setLoadingProgressStage(stage);
                if (this->loadingIndeterminate != (requestPercent < 0)
                    || (requestPercent >= 0 && requestPercent != this->loadingPercent))
                    this->setLoadingProgress(requestPercent, true);
                this->updateLoadingActivity(downloaded, total, true);
                mainApp->CallForRender();
                svcSleepThread(33'000'000ULL);
            }

            if (fetchThread.joinable())
                fetchThread.join();

            this->remoteSections = std::move(fetchedSections);
            error = fetchError;
            usedLegacyFallback = fetchUsedLegacyFallback;
            if (error.empty() && !this->remoteSections.empty() &&
                (!usedLegacyFallback || inst::config::remoteLegacyMode)) {
                this->catalogCacheValid = true;
                this->catalogCacheKey = cacheKey;
                this->catalogCacheUsedLegacyFallback = usedLegacyFallback;
                this->catalogCacheSections = this->remoteSections;
            }
        }
        updateLoadingProgress(90, "Preparing Remote library...");
        this->saveSyncEnabled = !usedLegacyFallback && !inst::config::remoteLegacyMode;
        RemoteDlcTrace("FetchRemoteSections done sections=%llu errorLen=%llu", static_cast<unsigned long long>(this->remoteSections.size()), static_cast<unsigned long long>(error.size()));
        RemoteDlcTrace(
            "save sync eligibility legacyFallback=%d tinfoilMode=%d enabled=%d",
            usedLegacyFallback ? 1 : 0,
            inst::config::remoteLegacyMode ? 1 : 0,
            this->saveSyncEnabled ? 1 : 0
        );
        if (!error.empty()) {
            RemoteDlcTrace("FetchRemoteSections error: %s", error.c_str());
            std::string audioPath = "romfs:/audio/bark.wav";
            if (!inst::config::soundEnabled)
                audioPath.clear();
            else if (std::filesystem::exists(inst::config::appDir + "/bark.wav"))
                audioPath = inst::config::appDir + "/bark.wav";
            std::thread errorSound(inst::util::playAudio, audioPath);
            mainApp->CreateShowDialog("inst.remote.failed"_lang, error, {"common.ok"_lang}, true);
            errorSound.join();
            mainApp->LoadLayout(mainApp->mainPage);
            return;
        }
        if (this->remoteSections.empty()) {
            RemoteDlcTrace("FetchRemoteSections returned empty sections");
            mainApp->CreateShowDialog("inst.remote.empty"_lang, "", {"common.ok"_lang}, true);
            mainApp->LoadLayout(mainApp->mainPage);
            return;
        }
        updateLoadingProgress(91, "Scanning Remote sections...");

        for (std::size_t i = 0; i < this->remoteSections.size(); i++) {
            auto& section = this->remoteSections[i];
            // Classify once here: the passes below ask IsBase/IsUpdate/IsDlcItem up to eight times
            // per item, and an IsDlcItem miss normalizes three strings every time.
            for (auto& item : section.items)
                item.typeFlags = ClassifyItem(item);
            if (section.id == "updates" || section.id == "update")
                this->nativeUpdatesSectionPresent = true;
            if (section.id == "dlc" || section.id == "addon" || section.id == "add-on" || section.id == "add_ons")
                this->nativeDlcSectionPresent = true;
            RemoteDlcTrace("section-before id='%s' title='%s' items=%llu", section.id.c_str(), section.title.c_str(), static_cast<unsigned long long>(section.items.size()));

            if (!this->remoteSections.empty()) {
                const int scanProgress = 91 + static_cast<int>(((i + 1) * 3ULL) / this->remoteSections.size()); // 91..94
                updateLoadingProgress(scanProgress);
            }
        }
        RemoteDlcTrace("native sections updates=%d dlc=%d", this->nativeUpdatesSectionPresent ? 1 : 0, this->nativeDlcSectionPresent ? 1 : 0);
        updateLoadingProgress(95, "Preparing sections...");

        if ((this->nativeUpdatesSectionPresent || this->nativeDlcSectionPresent) && !this->remoteSections.empty()) {
            auto normalizeSectionId = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return value;
            };

            int allIndex = -1;
            int updatesIndex = -1;
            int dlcIndex = -1;
            for (std::size_t i = 0; i < this->remoteSections.size(); i++) {
                const std::string id = normalizeSectionId(this->remoteSections[i].id);
                if (id == "all")
                    allIndex = static_cast<int>(i);
                else if (id == "updates" || id == "update")
                    updatesIndex = static_cast<int>(i);
                else if (id == "dlc" || id == "addon" || id == "add-on" || id == "add_ons")
                    dlcIndex = static_cast<int>(i);
            }

            if (allIndex >= 0) {
                auto augmentSectionFromAll = [&](int targetIndex, bool (*predicate)(const remoteInstStuff::RemoteItem&)) {
                    if (targetIndex < 0 || targetIndex >= static_cast<int>(this->remoteSections.size()))
                        return;
                    if (targetIndex == allIndex)
                        return;

                    auto& targetItems = this->remoteSections[targetIndex].items;
                    const auto& allItems = this->remoteSections[allIndex].items;
                    std::unordered_set<std::string> seenKeys;
                    seenKeys.reserve(targetItems.size() + allItems.size());

                    for (const auto& item : targetItems) {
                        std::string key = BuildItemIdentityKey(item);
                        if (key.empty())
                            key = "name:" + NormalizeSearchKey(item.name);
                        if (!key.empty())
                            seenKeys.insert(key);
                    }

                    for (const auto& item : allItems) {
                        if (!predicate(item))
                            continue;
                        std::string key = BuildItemIdentityKey(item);
                        if (key.empty())
                            key = "name:" + NormalizeSearchKey(item.name);
                        if (!key.empty() && !seenKeys.insert(key).second)
                            continue;
                        targetItems.push_back(item);
                    }

                    std::sort(targetItems.begin(), targetItems.end(), [](const auto& a, const auto& b) {
                        return inst::util::ignoreCaseCompare(a.name, b.name);
                    });
                };

                if (this->nativeUpdatesSectionPresent)
                    augmentSectionFromAll(updatesIndex, IsUpdateItem);
                if (this->nativeUpdatesSectionPresent)
                    updateLoadingProgress(96, "Preparing updates section...");
                if (this->nativeDlcSectionPresent)
                    augmentSectionFromAll(dlcIndex, IsDlcItem);
                if (this->nativeDlcSectionPresent)
                    updateLoadingProgress(97, "Preparing DLC section...");
            } else {
                RemoteDlcTrace("augment skipped: no all section present");
                updateLoadingProgress(97, "Preparing sections...");
            }
        }

        updateLoadingProgress(98, "Preparing installed section...");
        this->ensureInstalledSectionPlaceholder();
        RemoteDlcTrace("after ensureInstalledSectionPlaceholder sections=%llu", static_cast<unsigned long long>(this->remoteSections.size()));
        updateLoadingProgress(99, "Preparing owned sections...");
        this->buildLegacyOwnedSections();
        RemoteDlcTrace("after buildLegacyOwnedSections sections=%llu", static_cast<unsigned long long>(this->remoteSections.size()));
        updateLoadingProgress(99, "Checking available updates...");
        this->cacheAvailableUpdates();
        RemoteDlcTrace("after cacheAvailableUpdates availableUpdates=%llu", static_cast<unsigned long long>(this->availableUpdates.size()));
        updateLoadingProgress(99, "Filtering sections...");
        this->filterOwnedSections();
        updateLoadingProgress(99, "Sorting titles...");
        this->applyAllSectionSort();
        updateLoadingProgress(99, "Preparing save sync...");
        if (this->saveSyncEnabled) {
            const bool hasSaveSection = std::any_of(this->remoteSections.begin(), this->remoteSections.end(), [](const auto& section) {
                return section.id == "saves" || section.id == "save";
            });
            if (!hasSaveSection) {
                remoteInstStuff::RemoteSection saveSection;
                saveSection.id = "saves";
                saveSection.title = "Saves";
                this->remoteSections.push_back(std::move(saveSection));
            }
        }
        updateLoadingProgress(99, "Finalizing Remote...");
        this->setLoadingProgressStage("Remote ready");
        this->setLoadingProgress(100, true);
        mainApp->CallForRender();

        this->selectedSectionIndex = 0;
        for (size_t i = 0; i < this->remoteSections.size(); i++) {
            if (this->remoteSections[i].id == "new") {
                this->selectedSectionIndex = static_cast<int>(i);
                break;
            }
        }
        this->remoteGridMode = inst::config::remoteStartGridMode;
        this->remoteGridIndex = 0;
        this->remoteGridPage = -1;
        this->gridSelectedIndex = 0;
        this->gridPage = -1;
        this->setLoadingProgress(0, false);
        this->setLoadingProgressStage("");
        this->suppressBottomHints = false;
        this->pageInfoText->SetVisible(true);
        this->updateSectionText();
        this->updateButtonsText();
        this->ensureSaveSyncSectionLoaded();
        this->selectedItems.clear();
        this->drawMenuItems(false);
        this->infoImage->SetVisible(false);
        if (!this->remoteGridMode) {
            this->menu->SetSelectedIndex(0);
            this->menu->SetVisible(true);
            this->updatePreview();
        }
        this->pendingMotdFetch = true;
    }

    void remoteInstPage::renderLibraryRefresh(bool force) {
        if (!this->refreshingLibrary || !mainApp->IsUiThread())
            return;
        const u64 now = armGetSystemTick();
        if (!force && this->libraryRefreshLastRenderTick != 0
            && now - this->libraryRefreshLastRenderTick < armGetSystemTickFreq() / 60)
            return;
        this->libraryRefreshLastRenderTick = now;
        this->updateLoadingActivity();
        mainApp->CallForRender();
    }

    void remoteInstPage::showLibraryRefresh() {
        if (!mainApp->IsUiThread()) {
            mainApp->RunOnUiThread([this] { this->showLibraryRefresh(); });
            return;
        }
        if (this->refreshingLibrary)
            return;
        this->refreshingLibrary = true;
        this->libraryRefreshLastRenderTick = 0;
        this->suppressBottomHints = true;
        this->setButtonsText("");
        // The card is below the browser controls in draw order, so hide them
        // before rendering the first refresh frame.
        this->menu->SetVisible(false);
        this->pageInfoText->SetVisible(false);
        this->searchInfoText->SetVisible(false);
        this->previewImage->SetVisible(false);
        this->infoImage->SetVisible(false);
        this->emptySectionText->SetVisible(false);
        this->imageLoadingText->SetVisible(false);
        this->gridHighlight->SetVisible(false);
        this->gridTitleText->SetVisible(false);
        this->debugText->SetVisible(false);
        this->descriptionRect->SetVisible(false);
        this->descriptionText->SetVisible(false);
        this->closeDescriptionOverlay();
        this->listMarqueeMaskRect->SetVisible(false);
        this->listMarqueeTintRect->SetVisible(false);
        this->listMarqueeOverlayText->SetVisible(false);
        this->listMarqueeFadeRect->SetVisible(false);
        this->listMarqueeClipEnabled = false;
        for (auto& image : this->gridImages) image->SetVisible(false);
        for (auto& highlight : this->remoteGridSelectHighlights) highlight->SetVisible(false);
        for (auto& icon : this->remoteGridSelectIcons) icon->SetVisible(false);
        this->setLoadingProgressStage("Installation complete. Updating your library...");
        this->setLoadingProgress(-1, true);
        mainApp->LoadLayout(mainApp->remoteinstPage);
        this->renderLibraryRefresh(true);
    }

    void remoteInstPage::refreshAfterInstall() {
        if (this->remoteSections.empty() || this->activeRemoteUrl.empty()) {
            this->startRemote(true);
            return;
        }

        this->showLibraryRefresh();
        this->setLoadingProgressStage("Checking installed games, updates and DLC...");
        this->renderLibraryRefresh(true);

        const bool wasGridMode = this->remoteGridMode;
        const int previousSectionIndex = this->selectedSectionIndex;
        std::string previousSectionId;
        if (previousSectionIndex >= 0 && previousSectionIndex < static_cast<int>(this->remoteSections.size()))
            previousSectionId = this->remoteSections[static_cast<std::size_t>(previousSectionIndex)].id;

        // The hidden browser model belongs exclusively to this worker until
        // join. The UI pumps only the loading controls: onInput blocks browser
        // access, and MainApplication defers close/theme replacement while busy.
        std::atomic<bool> refreshDone{false};
        std::atomic<int> refreshStage{0};
        std::exception_ptr refreshFailure;
        std::jthread refreshWorker;
        try {
            refreshWorker = std::jthread([&] {
                try {
                    this->installedSnapshot = {};
                    if (!this->buildInstalledSnapshot())
                        throw std::runtime_error("Unable to scan installed content");
                    this->ensureInstalledSectionPlaceholder();
                    (void)this->ensureInstalledSectionBuilt();
                    refreshStage.store(1, std::memory_order_relaxed);
                    this->cacheAvailableUpdates();
                    this->filterOwnedSections();
                    refreshStage.store(2, std::memory_order_relaxed);
                    this->applyAllSectionSort();
                } catch (...) {
                    refreshFailure = std::current_exception();
                }
                refreshDone.store(true, std::memory_order_release);
            });
        } catch (...) {
            refreshFailure = std::current_exception();
            refreshDone.store(true, std::memory_order_release);
        }

        static const char* stages[] = {
            "Checking installed games, updates and DLC...",
            "Updating available updates and DLC...",
            "Sorting your library..."
        };
        int displayedStage = 0;
        const u64 frameTicks = armGetSystemTickFreq() / 60;
        while (!refreshDone.load(std::memory_order_acquire)) {
            const u64 frameStart = armGetSystemTick();
            const int stage = refreshStage.load(std::memory_order_relaxed);
            if (stage != displayedStage) {
                displayedStage = stage;
                this->setLoadingProgressStage(stages[stage]);
            }
            this->renderLibraryRefresh(true);
            const u64 elapsed = armGetSystemTick() - frameStart;
            if (elapsed < frameTicks)
                svcSleepThread(static_cast<s64>((frameTicks - elapsed) * 1000000000ULL / armGetSystemTickFreq()));
        }
        if (refreshWorker.joinable())
            refreshWorker.join();
        if (refreshFailure) {
            // Do not retry a failed scan synchronously while rebuilding widgets.
            this->installedSnapshot = {};
            this->setLoadingProgress(0, false);
            this->setLoadingProgressStage("");
            this->refreshingLibrary = false;
            this->suppressBottomHints = false;
            mainApp->LoadLayout(mainApp->mainPage);
            mainApp->CreateShowDialog("Library refresh", "Could not refresh the library. Reopen Remote to try again.", {"common.ok"_lang}, true);
            return;
        }

        int nextSectionIndex = 0;
        if (!previousSectionId.empty()) {
            for (std::size_t i = 0; i < this->remoteSections.size(); i++) {
                if (this->remoteSections[i].id == previousSectionId) {
                    nextSectionIndex = static_cast<int>(i);
                    break;
                }
            }
        } else if (!this->remoteSections.empty()) {
            nextSectionIndex = std::clamp(previousSectionIndex, 0, static_cast<int>(this->remoteSections.size()) - 1);
        }

        this->selectedSectionIndex = nextSectionIndex;
        this->remoteGridMode = wasGridMode;
        this->remoteGridIndex = 0;
        this->remoteGridPage = -1;
        this->gridSelectedIndex = 0;
        this->gridPage = -1;
        this->selectedItems.clear();
        this->previewKey.clear();
        this->setLoadingProgressStage("Preparing your title list...");
        this->renderLibraryRefresh(true);
        this->updateSectionText();
        this->drawMenuItems(false);
        this->infoImage->SetVisible(false);
        if (!this->remoteGridMode) {
            this->menu->SetSelectedIndex(0);
            this->menu->SetVisible(true);
            this->updatePreview();
        }
        this->updateDescriptionPanel();
        this->setLoadingProgress(0, false);
        this->setLoadingProgressStage("");
        this->refreshingLibrary = false;
        this->suppressBottomHints = false;
        this->pageInfoText->SetVisible(true);
        this->updateButtonsText();
        mainApp->CallForRender();
    }

    bool remoteInstPage::isQueueSection(int index) const {
        if (index < 0 || index >= static_cast<int>(remoteSections.size())) return false;
        const auto& section = remoteSections[index];
        if (section.id == "installed" || section.id == "saves" || section.id == "save" || section.id == "cheats" || section.id == "cheat") return false;
        return std::any_of(section.items.begin(), section.items.end(), [](const auto& item) {
            return !item.url.empty() && !item.isCheat && item.saveId.empty();
        });
    }

    std::vector<remoteInstStuff::RemoteItem> remoteInstPage::getQueueCatalog() const {
        std::vector<remoteInstStuff::RemoteItem> items;
        std::unordered_set<std::string> seen;
        for (size_t i = 0; i < remoteSections.size(); ++i) {
            if (!isQueueSection(static_cast<int>(i))) continue;
            for (const auto& item : remoteSections[i].items)
                if (!item.url.empty() && !item.isCheat && item.saveId.empty() && seen.insert(item.url).second) items.push_back(item);
        }
        return items;
    }

    void remoteInstPage::beginQueueAddMode() {
        if (queueAddMode) return;
        queueSavedSelection = selectedItems; selectedItems.clear(); queueAddMode = true;
        pendingMotdFetch = false; touchActive = false; bottomHintTouch = {};
        closeDescriptionOverlay();
        if (!isQueueSection(selectedSectionIndex)) {
            for (size_t i = 0; i < remoteSections.size(); ++i)
                if (isQueueSection(static_cast<int>(i))) { selectedSectionIndex = static_cast<int>(i); break; }
            searchQuery.clear(); remoteGridIndex = 0; remoteGridPage = -1;
        }
        updateSectionText(); updateButtonsText(); drawMenuItems(false);
        mainApp->LoadLayout(mainApp->remoteinstPage);
    }

    void remoteInstPage::endQueueAddMode() {
        if (!queueAddMode) return;
        queueAddMode = false; selectedItems = std::move(queueSavedSelection);
        closeDescriptionOverlay();
        touchActive = false; bottomHintTouch = {};
        updateSectionText(); updateButtonsText();
    }

    void remoteInstPage::startInstall() {
        if (queueAddMode) {
            std::vector<std::string> keys;
            for (const auto& item : selectedItems) keys.push_back(item.url);
            instPage::enqueueTitles(keys);
            selectedItems.clear();
            instPage::showQueue();
            return;
        }
        std::vector<remoteInstStuff::RemoteItem> autoAddedItems;
        if (!this->selectedItems.empty()) {
            auto isAlreadySelected = [&](const remoteInstStuff::RemoteItem& candidate) {
                return std::any_of(this->selectedItems.begin(), this->selectedItems.end(), [&](const auto& entry) {
                    if (!candidate.url.empty() && !entry.url.empty())
                        return entry.url == candidate.url;
                    if (candidate.hasTitleId && entry.hasTitleId)
                        return entry.titleId == candidate.titleId;
                    if (candidate.hasAppId && entry.hasAppId)
                        return entry.appId == candidate.appId;
                    return false;
                });
            };

            std::vector<remoteInstStuff::RemoteItem> updatesToAdd;
            std::unordered_map<std::uint64_t, remoteInstStuff::RemoteItem> latestUpdates;
            auto shouldReplaceUpdateCandidate = [](const remoteInstStuff::RemoteItem& current, const remoteInstStuff::RemoteItem& candidate) {
                if (candidate.hasAppVersion) {
                    if (!current.hasAppVersion)
                        return true;
                    if (candidate.appVersion > current.appVersion)
                        return true;
                    if (candidate.appVersion == current.appVersion && current.url.empty() && !candidate.url.empty())
                        return true;
                    return false;
                }
                if (current.hasAppVersion)
                    return false;
                if (current.url.empty() && !candidate.url.empty())
                    return true;
                return false;
            };
            for (const auto& update : this->availableUpdates) {
                if (!IsUpdateItem(update))
                    continue;
                std::uint64_t baseTitleId = 0;
                if (!DeriveBaseTitleId(update, baseTitleId))
                    continue;
                auto it = latestUpdates.find(baseTitleId);
                if (it == latestUpdates.end() || shouldReplaceUpdateCandidate(it->second, update))
                    latestUpdates[baseTitleId] = update;
            }

            for (const auto& item : this->selectedItems) {
                if (!IsBaseItem(item))
                    continue;
                std::uint64_t baseTitleId = 0;
                if (!DeriveBaseTitleId(item, baseTitleId))
                    continue;
                auto updateIt = latestUpdates.find(baseTitleId);
                if (updateIt == latestUpdates.end())
                    continue;
                if (!isAlreadySelected(updateIt->second) && !updateIt->second.url.empty())
                    updatesToAdd.push_back(updateIt->second);
            }

            if (!updatesToAdd.empty()) {
                int res = mainApp->CreateShowDialog("inst.remote.update_prompt_title"_lang,
                    "inst.remote.update_prompt_desc"_lang + std::to_string(updatesToAdd.size()),
                    {"common.yes"_lang, "common.no"_lang}, false);
                if (res == 0) {
                    for (const auto& update : updatesToAdd) {
                        this->selectedItems.push_back(update);
                        autoAddedItems.push_back(update);
                    }
                }
            }

            std::unordered_map<std::uint64_t, bool> selectedBaseTitleIds;
            for (const auto& item : this->selectedItems) {
                if (!IsBaseItem(item))
                    continue;
                std::uint64_t baseTitleId = 0;
                if (!DeriveBaseTitleId(item, baseTitleId))
                    continue;
                selectedBaseTitleIds[baseTitleId] = true;
            }

            if (!selectedBaseTitleIds.empty()) {
                std::vector<remoteInstStuff::RemoteItem> dlcToAdd;
                std::unordered_map<std::string, bool> seenDlcKeys;

                for (const auto& section : this->remoteSections) {
                    for (const auto& item : section.items) {
                        if (!IsDlcItem(item))
                            continue;
                        if (item.url.empty())
                            continue;
                        std::uint64_t baseTitleId = 0;
                        if (!DeriveBaseTitleId(item, baseTitleId))
                            continue;
                        if (selectedBaseTitleIds.find(baseTitleId) == selectedBaseTitleIds.end())
                            continue;
                        if (isAlreadySelected(item))
                            continue;

                        std::string dedupeKey = item.url;
                        if (dedupeKey.empty() && item.hasTitleId)
                            dedupeKey = std::to_string(item.titleId);
                        if (dedupeKey.empty() && item.hasAppId)
                            dedupeKey = item.appId;
                        if (dedupeKey.empty())
                            continue;
                        if (seenDlcKeys.find(dedupeKey) != seenDlcKeys.end())
                            continue;
                        seenDlcKeys[dedupeKey] = true;
                        dlcToAdd.push_back(item);
                    }
                }

                if (!dlcToAdd.empty()) {
                    int res = mainApp->CreateShowDialog("inst.remote.dlc_prompt_title"_lang,
                        "inst.remote.dlc_prompt_desc"_lang + std::to_string(dlcToAdd.size()),
                        {"common.yes"_lang, "common.no"_lang}, false);
                    if (res == 0) {
                        for (const auto& dlc : dlcToAdd) {
                            this->selectedItems.push_back(dlc);
                            autoAddedItems.push_back(dlc);
                        }
                    }
                }
            }
        }

        int dialogResult = -1;
        if (this->selectedItems.size() == 1) {
            std::string name = inst::util::shortenString(this->selectedItems[0].name, 32, true);
            dialogResult = mainApp->CreateShowDialog("inst.target.desc0"_lang + name + "inst.target.desc1"_lang, "common.cancel_desc"_lang, {"inst.target.opt0"_lang, "inst.target.opt1"_lang}, false);
        } else {
            dialogResult = mainApp->CreateShowDialog("inst.target.desc00"_lang + std::to_string(this->selectedItems.size()) + "inst.target.desc01"_lang, "common.cancel_desc"_lang, {"inst.target.opt0"_lang, "inst.target.opt1"_lang}, false);
        }
        if (dialogResult == -1) {
            if (!autoAddedItems.empty()) {
                auto matchesItem = [](const remoteInstStuff::RemoteItem& lhs, const remoteInstStuff::RemoteItem& rhs) {
                    if (!lhs.url.empty() && !rhs.url.empty())
                        return lhs.url == rhs.url;
                    if (lhs.hasTitleId && rhs.hasTitleId)
                        return lhs.titleId == rhs.titleId;
                    if (lhs.hasAppId && rhs.hasAppId)
                        return lhs.appId == rhs.appId;
                    return false;
                };

                for (const auto& autoItem : autoAddedItems) {
                    auto it = std::find_if(this->selectedItems.begin(), this->selectedItems.end(), [&](const auto& selected) {
                        return matchesItem(selected, autoItem);
                    });
                    if (it != this->selectedItems.end())
                        this->selectedItems.erase(it);
                }

                if (this->remoteGridMode)
                    this->updateRemoteGrid();
                else
                    this->drawMenuItems(false);
            }
            return;
        }

        this->updateRememberedSelection();
        remoteInstStuff::installTitleRemote(this->selectedItems, dialogResult, "inst.remote.source_string"_lang, getQueueCatalog());
        this->refreshAfterInstall();
    }

    void remoteInstPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
        // Loading renders pump input too. Do not let browser shortcuts start
        // another operation while the catalog or save list is being replaced.
        if (this->loadingProgressText->IsVisible()) {
            if (this->refreshingLibrary)
                this->updateLoadingActivity();
            return;
        }
        int bottomTapX = 0;
        if (DetectBottomHintTap(Pos, this->bottomHintTouch, 668, 52, bottomTapX)) {
            Down |= FindBottomHintButton(this->bottomHintTouch.startY >= 697
                ? this->secondaryHintSegments : this->bottomHintSegments, bottomTapX);
        }
        int sectionTapX = 0;
        if (!this->descriptionOverlayVisible && !this->saveVersionSelectorVisible
            && DetectBottomHintTap(Pos, this->sectionHintTouch, 78, 34, sectionTapX)) {
            if (sectionTapX < 410) Down |= HidNpadButton_L;
            else if (sectionTapX > 870) Down |= HidNpadButton_R;
        }
        if (Down & (HidNpadButton_L | HidNpadButton_R | HidNpadButton_Minus | HidNpadButton_X | HidNpadButton_ZR)) {
            this->holdDirection = 0;
            this->holdStartTick = 0;
            this->lastHoldTick = 0;
        }
        inst::util::playNavigationClickIfNeeded(Down);
        if (this->descriptionOverlayVisible) {
            if (Down & (HidNpadButton_B | HidNpadButton_ZL)) {
                this->closeDescriptionOverlay();
                return;
            }

            if (!this->descriptionScreenshotPaths.empty()) {
                int tapX = 0;
                const bool tapped = DetectBottomHintTap(Pos, this->descriptionScreenshotTouch, 162, 367, tapX);
                if ((Down & (HidNpadButton_L | HidNpadButton_AnyLeft)) || (tapped && tapX >= 592 && tapX < 912))
                    this->changeDescriptionScreenshot(-1);
                else if ((Down & (HidNpadButton_R | HidNpadButton_AnyRight)) || (tapped && tapX >= 912 && tapX < 1232))
                    this->changeDescriptionScreenshot(1);
            }
            int delta = 0;
            if (Down & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp))
                delta = -1;
            else if (Down & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown))
                delta = 1;
            else if (this->descriptionScreenshotPaths.empty() && (Down & HidNpadButton_AnyLeft))
                delta = -(this->descriptionOverlayVisibleLines / 2);
            else if (this->descriptionScreenshotPaths.empty() && (Down & HidNpadButton_AnyRight))
                delta = (this->descriptionOverlayVisibleLines / 2);

            if (delta != 0) {
                this->scrollDescriptionOverlay(delta);
                return;
            }
            return;
        }
        if (this->handleSaveVersionSelectorInput(Down, Up, Held, Pos))
            return;
        if (queueAddMode) {
            if (Down & HidNpadButton_B) { instPage::showQueue(); return; }
            if (Down & HidNpadButton_Plus) {
                if (selectedItems.empty()) selectTitle(remoteGridMode ? remoteGridIndex : menu->GetSelectedIndex());
                if (!selectedItems.empty()) startInstall();
                return;
            }
            if (Down & (HidNpadButton_L | HidNpadButton_R)) {
                const int step = Down & HidNpadButton_R ? 1 : -1;
                for (size_t tries = 0; tries < remoteSections.size(); ++tries) {
                    selectedSectionIndex = (selectedSectionIndex + step + static_cast<int>(remoteSections.size())) % static_cast<int>(remoteSections.size());
                    if (isQueueSection(selectedSectionIndex)) break;
                }
                searchQuery.clear(); gridSelectedIndex = 0; gridPage = -1; remoteGridIndex = 0; remoteGridPage = -1;
                updateSectionText(); updateButtonsText(); drawMenuItems(false);
                Down &= ~(HidNpadButton_L | HidNpadButton_R);
            }
            if (Down & HidNpadButton_Y) {
                std::unordered_set<std::string> selectedUrls;
                for (const auto& item : selectedItems) selectedUrls.insert(item.url);
                const bool allSelected = std::all_of(visibleItems.begin(), visibleItems.end(), [&](const auto& item) {
                    return !instPage::canEnqueueTitle(item.url) || selectedUrls.count(item.url) != 0;
                });
                if (allSelected) selectedItems.clear();
                else for (const auto& item : visibleItems)
                    if (instPage::canEnqueueTitle(item.url) && selectedUrls.insert(item.url).second) selectedItems.push_back(item);
                refreshListSelectionIcons();
                Down &= ~HidNpadButton_Y;
            }
            // Keep the session's source, credentials and catalogue stable.
            Down &= ~HidNpadButton_X;
        }
        if (this->pendingMotdFetch && Down == 0 && Up == 0 && Held == 0 && Pos.IsEmpty()) {
            this->pendingMotdFetch = false;
            std::string motd = remoteInstStuff::FetchRemoteMotd(this->activeRemoteUrl, inst::config::remoteUser, inst::config::remotePass);
            if (!motd.empty())
                mainApp->CreateShowDialog("inst.remote.motd_title"_lang, motd, {"common.ok"_lang}, true);
            return;
        }
        if (Down & HidNpadButton_B) {
            this->updateRememberedSelection();
            mainApp->LoadLayout(mainApp->mainPage);
        }
        if (Down & HidNpadButton_Minus) {
            const int focusedIndex = this->remoteGridMode ? this->remoteGridIndex : this->menu->GetSelectedIndex();
            this->remoteGridMode = !this->remoteGridMode;
            this->touchActive = false;
            this->touchMoved = false;
            if (this->remoteGridMode) {
                this->listMarqueeMaskRect->SetVisible(false);
                this->listMarqueeTintRect->SetVisible(false);
                this->listMarqueeOverlayText->SetVisible(false);
                this->listMarqueeClipEnabled = false;
                this->listMarqueeFadeRect->SetVisible(false);
                this->remoteGridIndex = focusedIndex;
                if (this->remoteGridIndex < 0)
                    this->remoteGridIndex = 0;
                this->remoteGridPage = -1;
                this->gridPage = -1;
                if (this->isInstalledSection()) {
                    this->gridSelectedIndex = this->remoteGridIndex;
                    this->updateInstalledGrid();
                } else {
                    this->updateRemoteGrid();
                }
            } else {
                // Ensure list mode visibility is fully applied even when we skip full list rebuild.
                for (auto& img : this->gridImages)
                    img->SetVisible(false);
                this->gridHighlight->SetVisible(false);
                this->gridTitleText->SetVisible(false);
                for (auto& highlight : this->remoteGridSelectHighlights)
                    highlight->SetVisible(false);
                for (auto& icon : this->remoteGridSelectIcons)
                    icon->SetVisible(false);
                this->menu->SetVisible(true);
                if (!this->menu->GetItems().empty()) {
                    int sel = this->remoteGridIndex;
                    if (sel < 0 || sel >= (int)this->menu->GetItems().size())
                        sel = 0;
                    this->menu->SetSelectedIndex(sel);
                }
                this->updateSectionText();
                this->updateButtonsText();
                if (this->menu->GetItems().size() == this->visibleItems.size())
                    this->refreshListSelectionIcons();
                else
                    this->drawMenuItems(false);
                if (!this->menu->GetItems().empty()) {
                    const int restored = std::clamp(focusedIndex, 0, static_cast<int>(this->menu->GetItems().size()) - 1);
                    this->menu->SetSelectedIndex(restored);
                    this->remoteGridIndex = restored;
                    this->gridSelectedIndex = restored;
                }
                this->updatePreview();
                this->updateListMarquee(true);
            }
            this->updateDescriptionPanel();
            this->browseDecorationKey.clear();
            this->updateBrowseDecorations();
            return;
        }
        if (Down & HidNpadButton_StickR) {
            this->openSortDialog();
            this->updateButtonsText();
            this->updateDescriptionPanel();
            return;
        }
        if (Down & HidNpadButton_ZL) {
            this->showCurrentDescriptionDialog();
            return;
        }
        if (this->remoteGridMode) {
            if (Down & HidNpadButton_Plus) {
                if (!this->isInstalledSection() && !this->isSaveSyncSection() && !this->isCheatsSection() && !this->visibleItems.empty() && this->selectedItems.empty()) {
                    this->selectTitle(this->remoteGridIndex);
                }
                if (!this->isInstalledSection() && !this->isSaveSyncSection() && !this->isCheatsSection() && !this->selectedItems.empty())
                    this->startInstall();
            }
            if (Down & HidNpadButton_Y) {
                if (this->isCheatsSection()) {
                    this->installAllCheats();
                    return;
                } else if (!this->isInstalledSection() && !this->isSaveSyncSection()) {
                    if (this->selectedItems.size() == this->visibleItems.size()) {
                        this->selectedItems.clear();
                        this->updateRememberedSelection();
                        if (this->menu->GetItems().size() == this->visibleItems.size())
                            this->refreshListSelectionIcons();
                        else
                            this->drawMenuItems(false);
                        this->updateRemoteGrid();
                    } else {
                        std::unordered_set<std::string> selectedUrls;
                        selectedUrls.reserve(this->selectedItems.size());
                        for (const auto& selected : this->selectedItems) {
                            if (!selected.url.empty())
                                selectedUrls.insert(selected.url);
                        }
                        for (std::size_t i = 0; i < this->visibleItems.size(); i++) {
                            const auto& item = this->visibleItems[i];
                            if (item.url.empty() || selectedUrls.find(item.url) != selectedUrls.end())
                                continue;
                            this->selectedItems.push_back(item);
                            selectedUrls.insert(item.url);
                        }
                        this->updateRememberedSelection();
                        if (this->menu->GetItems().size() == this->visibleItems.size())
                            this->refreshListSelectionIcons();
                        else
                            this->drawMenuItems(false);
                        this->updateRemoteGrid();
                    }
                }
            }
            if (Down & HidNpadButton_X) {
                this->startRemote(true);
            }
            if (Down & HidNpadButton_L) {
                if (this->remoteSections.size() > 1) {
                    this->selectedSectionIndex = (this->selectedSectionIndex - 1 + (int)this->remoteSections.size()) % (int)this->remoteSections.size();
                    this->searchQuery.clear();
                    this->remoteGridIndex = 0;
                    this->remoteGridPage = -1;
                    this->gridSelectedIndex = 0;
                    this->gridPage = -1;
                    this->ensureSaveSyncSectionLoaded();
                    this->updateSectionText();
                    this->updateButtonsText();
                    this->drawMenuItems(false);
                }
            }
            if (Down & HidNpadButton_R) {
                if (this->remoteSections.size() > 1) {
                    this->selectedSectionIndex = (this->selectedSectionIndex + 1) % (int)this->remoteSections.size();
                    this->searchQuery.clear();
                    this->remoteGridIndex = 0;
                    this->remoteGridPage = -1;
                    this->gridSelectedIndex = 0;
                    this->gridPage = -1;
                    this->ensureSaveSyncSectionLoaded();
                    this->updateSectionText();
                    this->updateButtonsText();
                    this->drawMenuItems(false);
                }
            }
            if (Down & HidNpadButton_ZR) {
                this->openSearchDialog();
            }
            const u64 navDownMask = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right;
            u64 dirKeys = Down & navDownMask;
            if (dirKeys == 0) {
                const bool dpadUp = (Held & HidNpadButton_Up) != 0;
                const bool dpadDown = (Held & HidNpadButton_Down) != 0;
                const bool dpadLeft = (Held & HidNpadButton_Left) != 0;
                const bool dpadRight = (Held & HidNpadButton_Right) != 0;
                const bool fromStick = !dpadUp && !dpadDown && !dpadLeft && !dpadRight;
                int dirX = dpadRight ? 1 : (dpadLeft ? -1 : 0);
                int dirY = dpadDown ? 1 : (dpadUp ? -1 : 0);
                if (fromStick) {
                    const auto left = mainApp->GetAnalogStick(0);
                    const auto right = mainApp->GetAnalogStick(1);
                    const auto strength = [](const HidAnalogStickState& stick) { return std::max(std::abs(stick.x), std::abs(stick.y)); };
                    const auto stick = strength(left) >= strength(right) ? left : right;
                    const int x = std::abs(stick.x), y = std::abs(stick.y);
                    // Engage deliberately; a lower release threshold prevents jitter around the dead zone.
                    const int threshold = JOYSTICK_MAX * (this->gridHoldFromStick && this->gridHoldStartTick != 0 ? 40 : 55) / 100;
                    if (std::max(x, y) >= threshold) {
                        bool horizontal = x > y;
                        // Keep the current axis near diagonals instead of generating repeated new presses.
                        if (this->gridHoldFromStick && this->gridHoldDirX != 0 && x * 5 >= y * 4) horizontal = true;
                        else if (this->gridHoldFromStick && this->gridHoldDirY != 0 && y * 5 >= x * 4) horizontal = false;
                        if (horizontal) dirX = stick.x > 0 ? 1 : -1;
                        else dirY = stick.y > 0 ? -1 : 1;
                    }
                }
                if (dirX == 0 && dirY == 0) {
                    this->gridHoldDirX = 0;
                    this->gridHoldDirY = 0;
                    this->gridHoldFromStick = false;
                    this->gridHoldStartTick = 0;
                    this->gridHoldLastTick = 0;
                } else {
                    u64 now = armGetSystemTick();
                    if (this->gridHoldDirX != dirX || this->gridHoldDirY != dirY || this->gridHoldFromStick != fromStick || this->gridHoldStartTick == 0) {
                        this->gridHoldDirX = dirX;
                        this->gridHoldDirY = dirY;
                        this->gridHoldFromStick = fromStick;
                        this->gridHoldStartTick = now;
                        this->gridHoldLastTick = now;
                        if (fromStick) {
                            if (dirY != 0)
                                dirKeys |= (dirY > 0) ? HidNpadButton_Down : HidNpadButton_Up;
                            else if (dirX != 0)
                                dirKeys |= (dirX > 0) ? HidNpadButton_Right : HidNpadButton_Left;
                        }
                    } else {
                        const u64 freq = armGetSystemTickFreq();
                        const u64 delayTicks = (freq * (fromStick ? 350 : 200)) / 1000;
                        const u64 repeatTicks = (freq * (fromStick ? 140 : 90)) / 1000;
                        if (now - this->gridHoldStartTick >= delayTicks && now - this->gridHoldLastTick >= repeatTicks) {
                            if (dirY != 0)
                                dirKeys |= (dirY > 0) ? HidNpadButton_Down : HidNpadButton_Up;
                            else if (dirX != 0)
                                dirKeys |= (dirX > 0) ? HidNpadButton_Right : HidNpadButton_Left;
                            this->gridHoldLastTick = now;
                        }
                    }
                }
            } else {
                this->gridHoldFromStick = false;
                this->gridHoldStartTick = 0;
                this->gridHoldLastTick = 0;
            }
            if ((Down & HidNpadButton_A) || (Up & TouchPseudoKey)) {
                if (!this->visibleItems.empty()) {
                    if (this->remoteGridIndex < 0)
                        this->remoteGridIndex = 0;
                    if (this->remoteGridIndex >= (int)this->visibleItems.size())
                        this->remoteGridIndex = (int)this->visibleItems.size() - 1;
                    if (this->isInstalledSection()) {
                        this->gridSelectedIndex = this->remoteGridIndex;
                        this->showInstalledDetails();
                    } else if (this->isSaveSyncSection()) {
                        this->handleSaveSyncAction(this->remoteGridIndex);
                    } else if (this->isCheatsSection()) {
                        this->handleCheatAction(this->remoteGridIndex);
                    } else {
                        this->selectTitle(this->remoteGridIndex);
                        if (this->visibleItems.size() == 1 && this->selectedItems.size() == 1) {
                            this->startInstall();
                        }
                    }
                }
            }
            if (!this->visibleItems.empty()) {
                int newIndex = this->remoteGridIndex;
                if (dirKeys & HidNpadButton_Up)
                    newIndex -= kGridCols;
                if (dirKeys & HidNpadButton_Down)
                    newIndex += kGridCols;
                if (dirKeys & HidNpadButton_Left)
                    newIndex -= 1;
                if (dirKeys & HidNpadButton_Right)
                    newIndex += 1;

                if (newIndex < 0)
                    newIndex = 0;
                if (newIndex >= (int)this->visibleItems.size())
                    newIndex = (int)this->visibleItems.size() - 1;

                if (newIndex != this->remoteGridIndex) {
                    const bool repeatedMove = ((dirKeys & navDownMask) != 0) && ((Down & navDownMask) == 0);
                    this->remoteGridIndex = newIndex;
                    if (repeatedMove)
                        inst::util::playNavigationClick();
                    if (this->isInstalledSection()) {
                        this->gridSelectedIndex = this->remoteGridIndex;
                        this->updateInstalledGrid();
                    } else {
                        this->updateRemoteGrid();
                    }
                }
            }
            if (!Pos.IsEmpty()) {
                const int gridW = kGridCols * kGridTileWidth + (kGridCols - 1) * kGridGap;
                const int gridH = kGridRows * kGridTileHeight + (kGridRows - 1) * kGridGap + 24;
                const bool inGrid = (Pos.X >= kGridStartX) && (Pos.X <= (kGridStartX + gridW)) && (Pos.Y >= kGridStartY) && (Pos.Y <= (kGridStartY + gridH));
                if (inGrid) {
                    const int relX = Pos.X - kGridStartX;
                    const int relY = Pos.Y - kGridStartY;
                    const int col = relX / (kGridTileWidth + kGridGap);
                    const int row = relY / (kGridTileHeight + kGridGap);
                    const int tileX = relX - (col * (kGridTileWidth + kGridGap));
                    const int tileY = relY - (row * (kGridTileHeight + kGridGap));
                    if (col >= 0 && col < kGridCols && row >= 0 && row < kGridRows && tileX < kGridTileWidth && tileY < kGridTileHeight + 24) {
                        int page = 0;
                        if (this->remoteGridIndex > 0)
                            page = this->remoteGridIndex / kGridItemsPerPage;
                        int pageStart = page * kGridItemsPerPage;
                        int index = pageStart + (row * kGridCols) + col;
                        if (index >= 0 && index < (int)this->visibleItems.size() && index != this->remoteGridIndex) {
                            this->remoteGridIndex = index;
                            if (this->isInstalledSection()) {
                                this->gridSelectedIndex = this->remoteGridIndex;
                                this->updateInstalledGrid();
                            } else {
                                this->updateRemoteGrid();
                            }
                        }
                        this->touchActive = true;
                        this->touchMoved = false;
                    }
                }
            } else if (this->touchActive) {
                if (!this->visibleItems.empty()) {
                    if (this->isInstalledSection()) {
                        this->gridSelectedIndex = this->remoteGridIndex;
                        this->showInstalledDetails();
                    } else if (this->isSaveSyncSection()) {
                        this->handleSaveSyncAction(this->remoteGridIndex);
                    } else if (this->isCheatsSection()) {
                        this->handleCheatAction(this->remoteGridIndex);
                    } else {
                        this->selectTitle(this->remoteGridIndex);
                        if (this->visibleItems.size() == 1 && this->selectedItems.size() == 1) {
                            this->startInstall();
                        }
                    }
                }
                this->touchActive = false;
                this->touchMoved = false;
            }
            this->updateDescriptionPanel();
            return;
        }
        if ((Down & HidNpadButton_A) || (Up & TouchPseudoKey)) {
            if (this->isInstalledSection()) {
                this->showInstalledDetails();
            } else if (this->isSaveSyncSection()) {
                this->handleSaveSyncAction(this->menu->GetSelectedIndex());
            } else if (this->isCheatsSection()) {
                this->handleCheatAction(this->menu->GetSelectedIndex());
            } else {
                this->selectTitle(this->menu->GetSelectedIndex());
                if (this->menu->GetItems().size() == 1 && this->selectedItems.size() == 1) {
                    this->startInstall();
                }
            }
        }
        if (Down & HidNpadButton_L) {
            if (this->remoteSections.size() > 1) {
                this->selectedSectionIndex = (this->selectedSectionIndex - 1 + (int)this->remoteSections.size()) % (int)this->remoteSections.size();
                this->searchQuery.clear();
                this->gridSelectedIndex = 0;
                this->gridPage = -1;
                this->remoteGridIndex = 0;
                this->remoteGridPage = -1;
                this->ensureSaveSyncSectionLoaded();
                this->updateSectionText();
                this->updateButtonsText();
                this->drawMenuItems(false);
            }
        }
        if (Down & HidNpadButton_R) {
            if (this->remoteSections.size() > 1) {
                this->selectedSectionIndex = (this->selectedSectionIndex + 1) % (int)this->remoteSections.size();
                this->searchQuery.clear();
                this->gridSelectedIndex = 0;
                this->gridPage = -1;
                this->remoteGridIndex = 0;
                this->remoteGridPage = -1;
                this->ensureSaveSyncSectionLoaded();
                this->updateSectionText();
                this->updateButtonsText();
                this->drawMenuItems(false);
            }
        }
        if (Down & HidNpadButton_ZR) {
            this->openSearchDialog();
        }
        if (Down & HidNpadButton_Y) {
            if (this->isCheatsSection()) {
                this->installAllCheats();
                return;
            } else if (!this->isInstalledSection() && !this->isSaveSyncSection()) {
                if (this->selectedItems.size() == this->menu->GetItems().size()) {
                    this->selectedItems.clear();
                    this->updateRememberedSelection();
                    this->refreshListSelectionIcons();
                } else {
                    std::unordered_set<std::string> selectedUrls;
                    selectedUrls.reserve(this->selectedItems.size());
                    for (const auto& selected : this->selectedItems) {
                        if (!selected.url.empty())
                            selectedUrls.insert(selected.url);
                    }
                    for (std::size_t i = 0; i < this->visibleItems.size(); i++) {
                        const auto& item = this->visibleItems[i];
                        if (item.url.empty() || selectedUrls.find(item.url) != selectedUrls.end())
                            continue;
                        this->selectedItems.push_back(item);
                        selectedUrls.insert(item.url);
                    }
                    this->updateRememberedSelection();
                    this->refreshListSelectionIcons();
                }
            }
        }
        if (Down & HidNpadButton_X) {
            this->startRemote(true);
        }
        if (Down & HidNpadButton_Plus) {
            if (!this->isInstalledSection() && !this->isSaveSyncSection() && !this->isCheatsSection()) {
                if (this->selectedItems.empty()) {
                    this->selectTitle(this->menu->GetSelectedIndex());
                }
                if (!this->selectedItems.empty()) this->startInstall();
            }
        }
        if (!this->remoteGridMode && !this->menu->GetItems().empty()) {
            const u64 holdMask = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_StickLUp | HidNpadButton_StickRUp | HidNpadButton_StickLDown | HidNpadButton_StickRDown;
            const bool heldUp = (Held & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) != 0;
            const bool heldDown = (Held & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)) != 0;
            const bool isHolding = (Held & holdMask) != 0;
            if (!isHolding || (heldUp && heldDown)) {
                this->holdDirection = 0;
                this->holdStartTick = 0;
                this->lastHoldTick = 0;
            } else {
                int direction = heldDown ? 1 : -1;
                u64 now = armGetSystemTick();
                if (this->holdDirection != direction || this->holdStartTick == 0) {
                    this->holdDirection = direction;
                    this->holdStartTick = now;
                    this->lastHoldTick = now;
                }
                const u64 freq = armGetSystemTickFreq();
                const u64 heldMs = (now - this->holdStartTick) * 1000 / freq;
                // Start with deliberate single-row repeats. Only a sustained
                // hold accelerates, and release/direction changes reset it.
                const u64 repeatMs = heldMs >= 3200 ? 16 : (heldMs >= 2200 ? 25 : (heldMs >= 1200 ? 45 : 90));
                if (heldMs >= 400 && !(Down & holdMask)
                    && now - this->lastHoldTick >= freq * repeatMs / 1000) {
                    const int current = this->menu->GetSelectedIndex();
                    const int count = static_cast<int>(this->menu->GetItems().size());
                    const int target = std::clamp(current + direction, 0, count - 1);
                    if (target != current) {
                        // Use ordinary one-row movement so the viewport scrolls
                        // naturally instead of snapping to a new top row.
                        this->menu->OnInput(direction > 0 ? HidNpadButton_Down : HidNpadButton_Up,
                            0, 0, pu::ui::Touch::Empty);
                        this->listVisibleTopIndex = this->menu->GetVisibleTopIndex();
                        if (heldMs < 1000) inst::util::playNavigationClick();
                    }
                    this->lastHoldTick = now;
                }
            }
        } else {
            this->holdDirection = 0;
            this->holdStartTick = 0;
            this->lastHoldTick = 0;
        }
        if (this->menu->IsVisible()) {
            if (!Pos.IsEmpty()) {
                const int menuX = this->menu->GetProcessedX();
                const int menuY = this->menu->GetProcessedY();
                const int menuW = this->menu->GetWidth();
                const int menuH = this->menu->GetHeight();
                const bool inMenu = (Pos.X >= menuX) && (Pos.X <= (menuX + menuW)) && (Pos.Y >= menuY) && (Pos.Y <= (menuY + menuH));
                if (!this->touchActive && inMenu) {
                    this->touchActive = true;
                    this->touchMoved = false;
                }
            } else if (this->touchActive) {
                if (!this->touchMoved && !this->menu->GetItems().empty()) {
                    if (this->isInstalledSection()) {
                        this->showInstalledDetails();
                    } else if (this->isSaveSyncSection()) {
                        this->handleSaveSyncAction(this->menu->GetSelectedIndex());
                    } else if (this->isCheatsSection()) {
                        this->handleCheatAction(this->menu->GetSelectedIndex());
                    } else {
                        this->selectTitle(this->menu->GetSelectedIndex());
                        if (this->menu->GetItems().size() == 1 && this->selectedItems.size() == 1) {
                            this->startInstall();
                        }
                    }
                }
                this->touchActive = false;
                this->touchMoved = false;
            }
        } else {
            this->touchActive = false;
            this->touchMoved = false;
        }
        if (this->remoteGridMode && this->isInstalledSection() && !this->visibleItems.empty()) {
            if (!Pos.IsEmpty()) {
                const int gridW = kGridCols * kGridTileWidth + (kGridCols - 1) * kGridGap;
                const int gridH = kGridRows * kGridTileHeight + (kGridRows - 1) * kGridGap + 24;
                const bool inGrid = (Pos.X >= kGridStartX) && (Pos.X <= (kGridStartX + gridW)) && (Pos.Y >= kGridStartY) && (Pos.Y <= (kGridStartY + gridH));
                if (!this->touchActive) {
                    if (inGrid) {
                        this->touchActive = true;
                        this->touchMoved = false;
                    }
                } else if (inGrid) {
                    const int relX = Pos.X - kGridStartX;
                    const int relY = Pos.Y - kGridStartY;
                    const int col = relX / (kGridTileWidth + kGridGap);
                    const int row = relY / (kGridTileHeight + kGridGap);
                    const int tileX = relX - (col * (kGridTileWidth + kGridGap));
                    const int tileY = relY - (row * (kGridTileHeight + kGridGap));
                    if (col >= 0 && col < kGridCols && row >= 0 && row < kGridRows && tileX < kGridTileWidth && tileY < kGridTileHeight + 24) {
                        int page = 0;
                        if (this->gridSelectedIndex > 0)
                            page = this->gridSelectedIndex / kGridItemsPerPage;
                        int pageStart = page * kGridItemsPerPage;
                        int index = pageStart + (row * kGridCols) + col;
                        if (index >= 0 && index < (int)this->visibleItems.size() && index != this->gridSelectedIndex) {
                            this->gridSelectedIndex = index;
                            this->updateInstalledGrid();
                        }
                    }
                }
            } else if (this->touchActive) {
                if (!this->touchMoved) {
                    this->showInstalledDetails();
                }
                this->touchActive = false;
                this->touchMoved = false;
            }
        }
        if (this->remoteGridMode) {
            if (this->isInstalledSection()) {
                this->gridSelectedIndex = this->remoteGridIndex;
                this->updateInstalledGrid();
            } else {
                this->updateRemoteGrid();
            }
        } else {
            this->updatePreview();
            this->updateListMarquee(false);
        }
        this->updateDescriptionPanel();
        this->updateDebug();
        this->updateBrowseDecorations();
    }

    void remoteInstPage::showInstalledDetails() {
        if (!this->isInstalledSection())
            return;
        int selectedIndex = this->remoteGridMode ? this->gridSelectedIndex : this->menu->GetSelectedIndex();
        if (selectedIndex < 0 || selectedIndex >= (int)this->visibleItems.size())
            return;
        const auto& item = this->visibleItems[selectedIndex];

        const char* typeLabel = "Base";
        if (item.appType == NcmContentMetaType_Patch)
            typeLabel = "Update";
        else if (item.appType == NcmContentMetaType_AddOnContent)
            typeLabel = "DLC";

        char titleIdBuf[32] = {0};
        if (item.hasTitleId)
            std::snprintf(titleIdBuf, sizeof(titleIdBuf), "%016lx", static_cast<unsigned long>(item.titleId));
        else
            std::snprintf(titleIdBuf, sizeof(titleIdBuf), "unknown");

        std::string body;
        body += "inst.remote.detail_type"_lang + std::string(typeLabel) + "\n";
        body += "inst.remote.detail_titleid"_lang + std::string(titleIdBuf) + "\n";
        if (item.hasAppVersion)
            body += "inst.remote.detail_version"_lang + std::to_string(item.appVersion);
        else
            body += "inst.remote.detail_version"_lang + "0";

        std::uint64_t baseTitleId = 0;
        if (DeriveBaseTitleId(item, baseTitleId)) {
            inst::offline::TitleMetadata meta;
            if (inst::offline::TryGetMetadata(baseTitleId, meta)) {
                if (!meta.publisher.empty())
                    body += "\nPublisher: " + meta.publisher;
                if (meta.hasReleaseDate)
                    body += "\nRelease: " + FormatReleaseDate(meta.releaseDate);
                if (meta.hasSize)
                    body += "\nSize: " + FormatSizeText(meta.size);
                if (meta.hasIsDemo)
                    body += "\nDemo: " + std::string(meta.isDemo ? "Yes" : "No");
            }
        }

        mainApp->CreateShowDialog(item.name, body, {"common.ok"_lang}, true);
    }

    void remoteInstPage::manageLocalCheats(const remoteInstStuff::RemoteItem& item)
    {
        std::string error;
        const auto locals = inst::cheats::List(item.cheatTitleId, error);
        if (!error.empty()) { mainApp->CreateShowDialog("Cheats", error, {"common.ok"_lang}, true); return; }
        std::vector<std::string> choices;
        for (const auto& local : locals) choices.push_back(local.buildId);
        choices.push_back("Create or edit build ID...");
        const int selected = mainApp->CreateShowDialog(item.name, "Local Atmosphere cheats for " + item.cheatTitleId, choices, false);
        if (selected < 0) return;
        std::string buildId = selected < (int)locals.size() ? locals[selected].buildId : inst::util::softwareKeyboard("Build ID (uppercase hex)", item.cheatBuildId, 64);
        if (!inst::cheats::IsValidBuildId(buildId)) { mainApp->CreateShowDialog("Cheats", "Build ID must be uppercase hexadecimal.", {"common.ok"_lang}, true); return; }
        std::string text;
        inst::cheats::Read(item.cheatTitleId, buildId, text, error); // A new file intentionally starts empty.
        const bool canAttemptAdminUpload = !inst::config::remoteUser.empty() || !inst::config::remotePass.empty();
        std::vector<std::string> localActions = {"View", "Edit", "Delete"};
        if (canAttemptAdminUpload)
            localActions.push_back("Upload to AeroFoil");
        localActions.push_back("common.cancel"_lang);
        const int action = mainApp->CreateShowDialog("Local cheat " + buildId, "View, edit, delete, or upload this Atmosphere cheat.", localActions, false);
        if (action == 0) { mainApp->CreateShowDialog("Cheat text", text.empty() ? "(empty)" : text, {"common.ok"_lang}, true); return; }
        if (action == 1) {
            const std::string edited = inst::util::softwareKeyboard("Cheat text (UTF-8)", text, 8192);
            if (!inst::cheats::WriteAtomically(item.cheatTitleId, buildId, edited, error)) mainApp->CreateShowDialog("Cheats", error, {"common.ok"_lang}, true);
            else mainApp->CreateShowDialog("Cheats", "Local cheat saved.", {"common.ok"_lang}, true);
            return;
        }
        if (action == 2) {
            if (mainApp->CreateShowDialog("Delete cheat?", buildId + ".txt will be removed from Atmosphere.", {"common.yes"_lang, "common.no"_lang}, false) == 0) {
                if (!inst::cheats::Remove(item.cheatTitleId, buildId, error)) mainApp->CreateShowDialog("Cheats", error, {"common.ok"_lang}, true);
                else mainApp->CreateShowDialog("Cheats", "Local cheat deleted.", {"common.ok"_lang}, true);
            }
            return;
        }
        if (canAttemptAdminUpload && action == 3) {
            const std::string note = inst::util::softwareKeyboard("Server note (optional)", item.cheatNote, 120);
            if (!remoteInstStuff::UploadCheatText(this->activeRemoteUrl, inst::config::remoteUser, inst::config::remotePass, item.cheatTitleId, buildId, note, text, error)) mainApp->CreateShowDialog("Cheats", error, {"common.ok"_lang}, true);
            else mainApp->CreateShowDialog("Cheats", "Cheat uploaded to AeroFoil.", {"common.ok"_lang}, true);
        }
    }

    void remoteInstPage::showCheatInstallProgress()
    {
        this->cheatInstallProgressVisible = true;
        this->menu->SetVisible(false);
        this->infoImage->SetVisible(false);
        this->previewImage->SetVisible(false);
        this->gridHighlight->SetVisible(false);
        this->gridTitleText->SetVisible(false);
        this->imageLoadingText->SetVisible(false);
        this->listMarqueeMaskRect->SetVisible(false);
        this->listMarqueeTintRect->SetVisible(false);
        this->listMarqueeOverlayText->SetVisible(false);
        this->listMarqueeClipEnabled = false;
        this->listMarqueeFadeRect->SetVisible(false);
        this->descriptionRect->SetVisible(false);
        this->descriptionText->SetVisible(false);
        for (auto& image : this->gridImages)
            image->SetVisible(false);
        for (auto& highlight : this->remoteGridSelectHighlights)
            highlight->SetVisible(false);
        for (auto& icon : this->remoteGridSelectIcons)
            icon->SetVisible(false);
    }

    void remoteInstPage::installAllCheats()
    {
        std::vector<remoteInstStuff::RemoteItem> allCheats;
        std::unordered_set<std::string> seenTargets;
        for (const auto& section : this->remoteSections) {
            if (section.id != "cheats") continue;
            for (const auto& candidate : section.items) {
                if (!candidate.isCheat) continue;
                const std::string targetKey = candidate.cheatTitleId + "/" + candidate.cheatBuildId;
                if (seenTargets.insert(targetKey).second)
                    allCheats.push_back(candidate);
            }
        }
        if (allCheats.empty()) { mainApp->CreateShowDialog("Cheats", "No valid cheats are available from this Remote.", {"common.ok"_lang}, true); return; }
        if (mainApp->CreateShowDialog("Install all cheats?", "Install " + std::to_string(allCheats.size()) + " available cheat file(s) for all titles?", {"common.yes"_lang, "common.no"_lang}, false) != 0)
            return;

        this->showCheatInstallProgress();
        int installedCount = 0;
        std::string firstError;
        for (std::size_t index = 0; index < allCheats.size(); ++index) {
            const auto& candidate = allCheats[index];
            const bool replacing = inst::cheats::IsInstalled(candidate.cheatTitleId, candidate.cheatBuildId);
            this->setLoadingProgressStage(std::string(replacing ? "Replacing" : "Installing") + " cheat " + std::to_string(index + 1) + "/" + std::to_string(allCheats.size()) + ": " + candidate.name);
            this->setLoadingProgress(static_cast<int>((index * 100) / allCheats.size()), true);
            mainApp->CallForRender();

            std::string text, installError;
            if (remoteInstStuff::DownloadCheatText(candidate, inst::config::remoteUser, inst::config::remotePass, text, installError) &&
                inst::cheats::WriteAtomically(candidate.cheatTitleId, candidate.cheatBuildId, text, installError)) {
                installedCount++;
            } else if (firstError.empty()) {
                firstError = installError;
            }
        }
        this->setLoadingProgressStage("Cheat installation complete.");
        this->setLoadingProgress(100, true);
        mainApp->CallForRender();
        std::string result = "Installed " + std::to_string(installedCount) + " of " + std::to_string(allCheats.size()) + " available cheat file(s).";
        if (!firstError.empty()) result += "\nFirst failure: " + firstError;
        mainApp->CreateShowDialog("Cheats", result, {"common.ok"_lang}, true);
        this->cheatInstallProgressVisible = false;
        this->setLoadingProgress(0, false);
        this->drawMenuItems(false);
    }

    void remoteInstPage::handleCheatAction(int selectedIndex)
    {
        if (selectedIndex < 0 || selectedIndex >= (int)this->visibleItems.size()) return;
        const auto& item = this->visibleItems[selectedIndex];
        if (!item.isCheat) { mainApp->CreateShowDialog("Cheats", "This server returned an invalid cheat entry.", {"common.ok"_lang}, true); return; }
        const bool installed = inst::cheats::IsInstalled(item.cheatTitleId, item.cheatBuildId);
        std::string details = "Title ID: " + item.cheatTitleId + "\nBuild ID: " + item.cheatBuildId + "\nInstalled: " + (installed ? "Yes" : "No");
        if (!item.cheatNote.empty()) details += "\nNote: " + item.cheatNote;
        const int action = mainApp->CreateShowDialog(item.name, details, {"Download / install", "Manage local cheats", "common.cancel"_lang}, false);
        if (action == 1) { this->manageLocalCheats(item); return; }
        if (action != 0) return;
        const bool replacing = inst::cheats::IsInstalled(item.cheatTitleId, item.cheatBuildId);
        this->showCheatInstallProgress();
        this->setLoadingProgressStage(std::string(replacing ? "Replacing" : "Installing") + " cheat: " + item.name);
        this->setLoadingProgress(0, true);
        mainApp->CallForRender();
        std::string text, error;
        if (!remoteInstStuff::DownloadCheatText(item, inst::config::remoteUser, inst::config::remotePass, text, error)) { this->cheatInstallProgressVisible = false; this->setLoadingProgress(0, false); mainApp->CreateShowDialog("Cheats", error, {"common.ok"_lang}, true); this->drawMenuItems(false); return; }
        this->setLoadingProgressStage(std::string(replacing ? "Replacing" : "Writing") + " cheat: " + item.name);
        this->setLoadingProgress(75, true);
        mainApp->CallForRender();
        if (!inst::cheats::WriteAtomically(item.cheatTitleId, item.cheatBuildId, text, error)) { this->cheatInstallProgressVisible = false; this->setLoadingProgress(0, false); mainApp->CreateShowDialog("Cheats", error, {"common.ok"_lang}, true); this->drawMenuItems(false); return; }
        this->setLoadingProgressStage("Cheat installation complete.");
        this->setLoadingProgress(100, true);
        mainApp->CallForRender();
        mainApp->CreateShowDialog("Cheats", "Installed to atmosphere/contents/" + item.cheatTitleId + "/cheats/" + item.cheatBuildId + ".txt", {"common.ok"_lang}, true);
        this->cheatInstallProgressVisible = false;
        this->setLoadingProgress(0, false);
        this->drawMenuItems(false);
    }

    bool remoteInstPage::tryGetCurrentDescription(std::string& outTitle, std::string& outDescription) const {
        if (this->visibleItems.empty())
            return false;

        int selectedIndex = this->remoteGridMode ? this->remoteGridIndex : this->menu->GetSelectedIndex();
        if (this->isInstalledSection() && this->remoteGridMode)
            selectedIndex = this->gridSelectedIndex;
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(this->visibleItems.size()))
            return false;

        const auto& item = this->visibleItems[selectedIndex];
        outTitle = item.name.empty() ? "Description" : inst::util::shortenString(item.name, 96, true);
        outDescription.clear();

        std::uint64_t baseTitleId = 0;
        if (DeriveBaseTitleId(item, baseTitleId)) {
            inst::offline::TitleMetadata meta;
            if (inst::offline::TryGetMetadata(baseTitleId, meta)) {
                if (!meta.description.empty())
                    outDescription = meta.description;
                else if (!meta.intro.empty())
                    outDescription = meta.intro;
            }
        }

        outDescription = NormalizeDescriptionWhitespace(outDescription);
        if (outDescription.empty())
            outDescription = "No description available for this title.";
        return true;
    }

    void remoteInstPage::showCurrentDescriptionDialog() {
        if (this->descriptionOverlayVisible) {
            this->closeDescriptionOverlay();
            return;
        }
        this->openDescriptionOverlay();
    }

    void remoteInstPage::openDescriptionOverlay() {
        std::string title;
        std::string description;
        if (!this->tryGetCurrentDescription(title, description))
            return;

        this->descriptionOverlayFullText = description;
        this->descriptionOverlayOffset = 0;
        this->descriptionOverlayVisible = true;
        this->descriptionGridTitleWasVisible = this->gridTitleText->IsVisible();
        this->gridTitleText->SetVisible(false);
        this->descriptionOverlayTitleText->SetText(title);
        this->descriptionOverlayRect->SetVisible(true);
        this->descriptionOverlayTitleText->SetVisible(true);
        this->descriptionOverlayBodyText->SetVisible(true);
        this->descriptionOverlayHintText->SetVisible(true);
        this->secondaryHintText->SetVisible(false);
        this->secondaryHintSegments.clear();
        this->bottomHintTouch = {};
        this->requestDescriptionMedia();
        this->reflowDescriptionOverlay();
    }

    void remoteInstPage::requestDescriptionMedia() {
        const auto generation = this->descriptionMediaGeneration.fetch_add(1) + 1;
        this->descriptionMediaPending = false;
        this->descriptionMediaStatus.clear();
        this->descriptionScreenshotIndex = 0;
        this->descriptionScreenshotPaths.clear();
        this->descriptionScreenshotFailed.clear();
        this->descriptionScreenshotDisplayedPath.clear();
        this->descriptionScreenshotTouch = {};
        // A server can expose the title-info API even when its catalog used the legacy fallback.
        if (this->activeRemoteUrl.empty()
            || this->isSaveSyncSection() || this->isCheatsSection() || this->descriptionMediaStop.load()) return;
        int index = this->remoteGridMode ? this->remoteGridIndex : this->menu->GetSelectedIndex();
        if (this->isInstalledSection() && this->remoteGridMode) index = this->gridSelectedIndex;
        DescriptionMediaRequest request;
        if (index < 0 || index >= static_cast<int>(this->visibleItems.size())
            || !DeriveBaseTitleId(this->visibleItems[index], request.titleId)) return;
        request.generation = generation;
        request.remoteUrl = this->activeRemoteUrl;
        request.user = inst::config::remoteUser;
        request.pass = inst::config::remotePass;
        request.cacheDir = inst::config::appDir + "/cache/screenshots";
        this->descriptionMediaPending = true;
        {
            std::lock_guard<std::mutex> lock(this->descriptionMediaMutex);
            this->descriptionMediaRequest = std::move(request);
            this->descriptionMediaRequestPending = true;
        }
        this->descriptionMediaCv.notify_one();
    }

    void remoteInstPage::descriptionMediaThreadMain(const std::string& cacheDir) {
        // Clear the previous session's screenshots once, before this worker loads any images.
        try {
            std::error_code error;
            std::filesystem::directory_iterator it(cacheDir, error), end;
            for (; !error && it != end && !this->descriptionMediaStop.load(); it.increment(error)) {
                std::error_code fileError;
                if (std::filesystem::is_regular_file(it->symlink_status(fileError)) && !fileError)
                    std::filesystem::remove(it->path(), fileError);
            }
        } catch (...) {}
        for (;;) {
            DescriptionMediaRequest request;
            {
                std::unique_lock<std::mutex> lock(this->descriptionMediaMutex);
                this->descriptionMediaCv.wait(lock, [this] { return this->descriptionMediaStop.load() || this->descriptionMediaRequestPending; });
                if (this->descriptionMediaStop.load()) return;
                request = std::move(this->descriptionMediaRequest);
                this->descriptionMediaRequestPending = false;
            }
            const auto canceled = [this, &request] {
                return this->descriptionMediaStop.load() || this->descriptionMediaGeneration.load() != request.generation;
            };
            DescriptionMediaResult result;
            result.generation = request.generation;
            const auto publish = [&] {
                if (canceled()) return;
                std::lock_guard<std::mutex> lock(this->descriptionMediaMutex);
                this->descriptionMediaResult = result;
                this->descriptionMediaDirty.store(true);
            };
            try {
                if (canceled()) continue;
                remoteInstStuff::RemoteTitleInfo info;
                if (!remoteInstStuff::FetchRemoteTitleInfo(request.remoteUrl, request.titleId, request.user, request.pass, info, result.status, canceled)) {
                    publish();
                    continue;
                }
                if (canceled()) continue;
                result.description = std::move(info.description);
                if (info.screenshots.empty()) result.status = "No screenshots available for this title.";
                result.paths.resize(info.screenshots.size());
                result.failed.resize(info.screenshots.size(), false);
                publish();
                std::error_code error;
                std::filesystem::create_directories(request.cacheDir, error);
                for (std::size_t i = 0; i < info.screenshots.size() && !canceled(); ++i) {
                    // The shop endpoint already returns a bounded JPEG. Cache it without codec work.
                    std::uint64_t hash = 14695981039346656037ULL;
                    for (unsigned char c : request.remoteUrl + "\n" + request.user + "\n" + info.screenshots[i]) {
                        hash ^= c;
                        hash *= 1099511628211ULL;
                    }
                    char key[32];
                    std::snprintf(key, sizeof(key), "native-%016llx.jpg", static_cast<unsigned long long>(hash));
                    const std::string path = request.cacheDir + "/" + key;
                    const std::string raw = path + ".download";
                    bool ready = std::filesystem::exists(path, error) && std::filesystem::file_size(path, error) > 0 && !error;
                    if (ready) {
                        const auto modified = std::filesystem::last_write_time(path, error);
                        const auto age = std::filesystem::file_time_type::clock::now() - modified;
                        ready = !error && age >= std::chrono::seconds(0) && age < std::chrono::hours(1);
                    }
                    int width = 0, height = 0;
                    if (ready) ready = ReadScreenshotSize(path, width, height);
                    if (!ready && remoteInstStuff::DownloadRemoteScreenshot(info.screenshots[i], request.remoteUrl, request.user, request.pass, raw, canceled)
                        && !canceled() && ReadScreenshotSize(raw, width, height)) {
                        std::filesystem::rename(raw, path, error);
                        ready = !error;
                    }
                    std::filesystem::remove(raw, error);
                    if (ready) result.paths[i] = path;
                    else result.failed[i] = true;
                    publish();
                }
            } catch (...) {
                result.status = "Unable to load screenshots. Close the description and try again.";
                for (std::size_t i = 0; i < result.failed.size(); ++i)
                    if (i >= result.paths.size() || result.paths[i].empty()) result.failed[i] = true;
                if (result.paths.size() != result.failed.size()) result.paths.clear();
                publish();
            }
        }
    }

    void remoteInstPage::updateDescriptionMedia() {
        if (!this->descriptionMediaDirty.exchange(false)) return;
        DescriptionMediaResult result;
        {
            std::lock_guard<std::mutex> lock(this->descriptionMediaMutex);
            result = this->descriptionMediaResult;
        }
        if (!this->descriptionOverlayVisible || result.generation != this->descriptionMediaGeneration.load()) return;
        const bool hadGallery = this->descriptionMediaPending || !this->descriptionScreenshotPaths.empty();
        const auto description = NormalizeDescriptionWhitespace(result.description);
        const bool changedText = !description.empty() && description != this->descriptionOverlayFullText;
        if (changedText) this->descriptionOverlayFullText = description;
        this->descriptionMediaPending = false;
        this->descriptionMediaStatus = std::move(result.status);
        this->descriptionScreenshotPaths = std::move(result.paths);
        this->descriptionScreenshotFailed = std::move(result.failed);
        if (this->descriptionScreenshotIndex >= static_cast<int>(this->descriptionScreenshotPaths.size())) this->descriptionScreenshotIndex = 0;
        if (changedText || hadGallery != !this->descriptionScreenshotPaths.empty()) this->reflowDescriptionOverlay();
        else {
            this->refreshDescriptionScreenshot();
            this->refreshDescriptionOverlayBody();
        }
    }

    void remoteInstPage::refreshDescriptionScreenshot() {
        const bool visible = this->descriptionOverlayVisible && (this->descriptionMediaPending || !this->descriptionScreenshotPaths.empty());
        this->descriptionScreenshotBack->SetVisible(visible);
        this->descriptionScreenshotStatus->SetVisible(this->descriptionOverlayVisible && (visible || !this->descriptionMediaStatus.empty()));
        this->descriptionScreenshotStatus->SetX(visible ? 600 : 46);
        this->descriptionScreenshotStatus->SetY(visible ? 548 : 580);
        this->descriptionScreenshotImage->SetVisible(false);
        if (!visible) {
            this->descriptionScreenshotStatus->SetText(ClipBrowseText(this->descriptionMediaStatus, 18, 1188));
            return;
        }
        if (this->descriptionMediaPending) {
            this->descriptionScreenshotStatus->SetText("Loading screenshots...");
            return;
        }
        const auto index = static_cast<std::size_t>(this->descriptionScreenshotIndex);
        const auto& path = this->descriptionScreenshotPaths[index];
        bool ready = false;
        if (!path.empty()) {
            if (path != this->descriptionScreenshotDisplayedPath) {
                this->descriptionScreenshotImage->SetImage(path);
                this->descriptionScreenshotDisplayedPath = path;
            }
            ready = this->descriptionScreenshotImage->IsImageValid();
        }
        this->descriptionScreenshotImage->SetVisible(ready);
        std::string status = "Screenshot " + std::to_string(index + 1) + " of " + std::to_string(this->descriptionScreenshotPaths.size());
        if (!ready) status += this->descriptionScreenshotFailed[index] || !path.empty() ? "    Unavailable; reopen to retry" : "    Loading...";
        this->descriptionScreenshotStatus->SetText(status);
    }

    void remoteInstPage::changeDescriptionScreenshot(int direction) {
        const int count = static_cast<int>(this->descriptionScreenshotPaths.size());
        if (count < 2) return;
        this->descriptionScreenshotIndex = (this->descriptionScreenshotIndex + direction + count) % count;
        this->refreshDescriptionScreenshot();
    }

    void remoteInstPage::reflowDescriptionOverlay() {
        const int width = this->descriptionMediaPending || !this->descriptionScreenshotPaths.empty() ? 524 : 1188;
        const auto font = pu::ui::render::LoadDefaultFont(19);
        const auto fallback = pu::ui::render::LoadSharedFont(pu::ui::render::SharedFont::NintendoExtended, 19);
        this->descriptionOverlayLines.clear();
        std::string remaining = this->descriptionOverlayFullText;
        while (!remaining.empty()) {
            const auto boundaries = BuildUtf8Boundaries(remaining);
            std::size_t low = 1, high = boundaries.size() - 1, best = 1;
            while (low <= high) {
                const auto mid = (low + high) / 2;
                const auto part = remaining.substr(0, boundaries[mid]);
                int measuredWidth = 0, measuredHeight = 0;
                const bool fits = font ? TTF_SizeUTF8(font, fallback, part.c_str(), &measuredWidth, &measuredHeight) == 0 && measuredWidth <= width
                    : mid <= static_cast<std::size_t>(width / 12);
                if (fits) { best = mid; low = mid + 1; }
                else high = mid - 1;
            }
            std::size_t end = boundaries[best];
            if (end < remaining.size()) {
                const auto space = remaining.rfind(' ', end);
                if (space != std::string::npos && space > 0) end = space;
            }
            this->descriptionOverlayLines.push_back(remaining.substr(0, end));
            remaining.erase(0, end);
            const auto next = remaining.find_first_not_of(' ');
            if (next == std::string::npos) remaining.clear();
            else remaining.erase(0, next);
        }
        if (this->descriptionOverlayLines.empty()) this->descriptionOverlayLines.push_back("No description available for this title.");
        this->descriptionOverlayOffset = std::min(this->descriptionOverlayOffset,
            std::max(0, static_cast<int>(this->descriptionOverlayLines.size()) - this->descriptionOverlayVisibleLines));
        this->refreshDescriptionScreenshot();
        this->refreshDescriptionOverlayBody();
    }

    void remoteInstPage::closeDescriptionOverlay() {
        this->descriptionMediaGeneration.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(this->descriptionMediaMutex);
            this->descriptionMediaRequestPending = false;
        }
        this->descriptionMediaPending = false;
        this->descriptionScreenshotPaths.clear();
        this->descriptionScreenshotFailed.clear();
        this->descriptionScreenshotDisplayedPath.clear();
        this->descriptionScreenshotImage->Clear();
        this->descriptionScreenshotBack->SetVisible(false);
        this->descriptionScreenshotStatus->SetVisible(false);
        this->descriptionScreenshotTouch = {};
        this->descriptionOverlayVisible = false;
        this->descriptionOverlayLines.clear();
        this->descriptionOverlayOffset = 0;
        this->descriptionOverlayRect->SetVisible(false);
        this->descriptionOverlayTitleText->SetVisible(false);
        this->descriptionOverlayBodyText->SetVisible(false);
        this->descriptionOverlayHintText->SetVisible(false);
        this->gridTitleText->SetVisible(this->descriptionGridTitleWasVisible);
        this->descriptionGridTitleWasVisible = false;
        this->bottomHintTouch = {};
        this->updateButtonsText();
    }

    void remoteInstPage::scrollDescriptionOverlay(int delta) {
        if (!this->descriptionOverlayVisible || delta == 0)
            return;
        const int lineCount = static_cast<int>(this->descriptionOverlayLines.size());
        if (lineCount <= this->descriptionOverlayVisibleLines)
            return;
        const int maxOffset = lineCount - this->descriptionOverlayVisibleLines;
        int nextOffset = this->descriptionOverlayOffset + delta;
        if (nextOffset < 0)
            nextOffset = 0;
        if (nextOffset > maxOffset)
            nextOffset = maxOffset;
        if (nextOffset == this->descriptionOverlayOffset)
            return;
        this->descriptionOverlayOffset = nextOffset;
        this->refreshDescriptionOverlayBody();
    }

    void remoteInstPage::refreshDescriptionOverlayBody() {
        if (!this->descriptionOverlayVisible)
            return;

        const int lineCount = static_cast<int>(this->descriptionOverlayLines.size());
        if (lineCount <= 0) {
            this->descriptionOverlayBodyText->SetText("");
            this->descriptionOverlayHintText->SetVisible(false);
            this->butText->SetText(" Close");
            this->bottomHintSegments = BuildBottomHintSegments(" Close", this->butText->GetX(), 18);
            return;
        }

        int start = this->descriptionOverlayOffset;
        if (start < 0)
            start = 0;
        if (start >= lineCount)
            start = lineCount - 1;
        int end = start + this->descriptionOverlayVisibleLines;
        if (end > lineCount)
            end = lineCount;

        std::string body;
        for (int i = start; i < end; i++) {
            if (!body.empty())
                body.push_back('\n');
            body += this->descriptionOverlayLines[i];
        }
        this->descriptionOverlayBodyText->SetText(body);

        const bool canScroll = lineCount > this->descriptionOverlayVisibleLines;
        this->descriptionOverlayHintText->SetVisible(canScroll);
        if (canScroll)
            this->descriptionOverlayHintText->SetText("Description · Lines " + std::to_string(start + 1) + "–" + std::to_string(end) + " of " + std::to_string(lineCount));
        std::string hints = " Close";
        if (canScroll) hints += "    ↑/↓ Scroll description";
        if (this->descriptionScreenshotPaths.size() > 1)
            hints += "     Previous screenshot     Next screenshot";
        this->butText->SetFontSize(18);
        this->butText->SetText(hints);
        this->secondaryHintText->SetVisible(false);
        this->secondaryHintSegments.clear();
        this->bottomHintSegments = BuildBottomHintSegments(hints, this->butText->GetX(), 18);
    }

    void remoteInstPage::updateDescriptionPanel() {
        if (!this->descriptionVisible) {
            this->descriptionRect->SetVisible(false);
            this->descriptionText->SetVisible(false);
            return;
        }

        if (this->visibleItems.empty()) {
            this->descriptionRect->SetVisible(false);
            this->descriptionText->SetVisible(false);
            return;
        }

        int selectedIndex = this->remoteGridMode ? this->remoteGridIndex : this->menu->GetSelectedIndex();
        if (this->isInstalledSection() && this->remoteGridMode)
            selectedIndex = this->gridSelectedIndex;
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(this->visibleItems.size())) {
            this->descriptionRect->SetVisible(false);
            this->descriptionText->SetVisible(false);
            return;
        }

        const auto& item = this->visibleItems[selectedIndex];
        std::string description;
        std::uint64_t baseTitleId = 0;
        if (DeriveBaseTitleId(item, baseTitleId)) {
            inst::offline::TitleMetadata meta;
            if (inst::offline::TryGetMetadata(baseTitleId, meta)) {
                if (!meta.description.empty())
                    description = meta.description;
                else if (!meta.intro.empty())
                    description = meta.intro;
            }
        }

        description = NormalizeDescriptionWhitespace(description);
        if (description.empty())
            description = "No description available for this title.";

        const std::string wrapped = WrapDescriptionText(description, 118, 5);
        std::string title = inst::util::shortenString(item.name, 92, true);
        this->descriptionText->SetText(title + "\n" + wrapped);
        this->descriptionRect->SetVisible(true);
        this->descriptionText->SetVisible(true);
    }

    void remoteInstPage::setButtonsText(const std::string& text) {
        this->secondaryHintText->SetVisible(false);
        this->secondaryHintSegments.clear();
        if (this->suppressBottomHints) {
            this->butText->SetText("");
            this->bottomHintSegments.clear();
            return;
        }
        std::string full = text;
        if (!this->saveVersionSelectorVisible && !this->descriptionOverlayVisible)
            full += "     Details";
        std::string first, second;
        std::size_t start = 0;
        bool secondLine = false;
        this->butText->SetFontSize(18);
        while (start < full.size()) {
            const auto end = full.find("    ", start);
            const auto token = full.substr(start, end == std::string::npos ? end : end - start);
            const auto segments = BuildBottomHintSegments(token, 0, 18);
            // Section controls now have their own persistent navigation strip.
            const bool sectionToken = !segments.empty() && segments.front().primary == HidNpadButton_L;
            if (!token.empty() && !sectionToken) {
                const auto candidate = first.empty() ? token : first + "    " + token;
                this->butText->SetText(candidate);
                if (!secondLine && this->butText->GetTextWidth() > 1200) secondLine = true;
                auto& line = secondLine ? second : first;
                if (!line.empty()) line += "    ";
                line += token;
            }
            if (end == std::string::npos) break;
            start = end + 4;
        }
        this->butText->SetText(first);
        this->secondaryHintText->SetText(second);
        this->secondaryHintText->SetVisible(!second.empty());
        this->bottomHintSegments = BuildBottomHintSegments(first, 24, 18);
        this->secondaryHintSegments = BuildBottomHintSegments(second, 24, 16);
    }
}


