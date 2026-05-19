#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <SDL2/SDL.h>
#include <switch.h>
#include "ui/MainApplication.hpp"
#include "ui/shopInstPage.hpp"
#include "ui/overflowText.hpp"
#include "util/config.hpp"
#include "util/curl.hpp"
#include "util/install_queue.hpp"
#include "util/lang.hpp"
#include "util/offline_title_db.hpp"
#include "util/save_sync.hpp"
#include "util/title_util.hpp"
#include "util/util.hpp"
#include "ui/bottomHint.hpp"

#define COLOR(hex) pu::ui::Color::FromHex(hex)
#define ShopDlcTrace(...) ((void)0)
#define ResetShopDlcTrace() ((void)0)

namespace {
    constexpr int kGridCols = 10;
    constexpr int kGridRows = 4;
    constexpr int kGridTileWidth = 120;
    constexpr int kGridTileHeight = 120;
    constexpr int kGridGap = 6;
    constexpr int kGridWidth = (kGridCols * kGridTileWidth) + ((kGridCols - 1) * kGridGap);
    constexpr int kGridStartX = (1280 - kGridWidth) / 2;
    constexpr int kGridStartY = 120;
    constexpr int kGridItemsPerPage = kGridCols * kGridRows;
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

    int ComputeListNameLimit(const std::string& suffix)
    {
        int nameLimit = 56;
        if (!suffix.empty()) {
            int maxSuffix = static_cast<int>(suffix.size()) + 1;
            if (nameLimit > maxSuffix)
                nameLimit -= maxSuffix;
        }
        if (nameLimit < 8)
            nameLimit = 8;
        return nameLimit;
    }

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

    bool TryGetItemSortDateKey(const shopInstStuff::ShopItem& item, std::uint64_t& out)
    {
        if (item.saveCreatedTs > 0) {
            out = item.saveCreatedTs;
            return true;
        }
        if (!item.saveCreatedAt.empty())
            return TryParseDateSortKey(item.saveCreatedAt, out);
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

    bool TryResolveBaseTitleId(const shopInstStuff::ShopItem& item, std::uint64_t& outBaseId);

    bool DeriveBaseTitleId(const shopInstStuff::ShopItem& item, std::uint64_t& out)
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

    bool TryInferNormalizedAppType(const shopInstStuff::ShopItem& item, std::int32_t& outType)
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

    bool TryResolveBaseTitleId(const shopInstStuff::ShopItem& item, std::uint64_t& outBaseId)
    {
        // Some shops publish title_id as the base title even for UPDATE/DLC entries.
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

    bool IsBaseItem(const shopInstStuff::ShopItem& item)
    {
        std::int32_t normalizedType = -1;
        return TryInferNormalizedAppType(item, normalizedType) && normalizedType == NcmContentMetaType_Application;
    }

    bool IsUpdateItem(const shopInstStuff::ShopItem& item)
    {
        std::int32_t normalizedType = -1;
        return TryInferNormalizedAppType(item, normalizedType) && normalizedType == NcmContentMetaType_Patch;
    }

    bool IsDlcItem(const shopInstStuff::ShopItem& item)
    {
        std::int32_t normalizedType = -1;
        return TryInferNormalizedAppType(item, normalizedType) && normalizedType == NcmContentMetaType_AddOnContent;
    }

    std::string BuildItemIdentityKey(const shopInstStuff::ShopItem& item)
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

    bool TryGetOfflineIconBaseId(const shopInstStuff::ShopItem& item, std::uint64_t& outBaseId)
    {
        return TryResolveBaseTitleId(item, outBaseId);
    }

    bool HasOfflineIconForItem(const shopInstStuff::ShopItem& item, std::uint64_t* outBaseId = nullptr)
    {
        std::uint64_t baseId = 0;
        if (!TryGetOfflineIconBaseId(item, baseId))
            return false;
        if (outBaseId)
            *outBaseId = baseId;
        return inst::offline::HasIcon(baseId);
    }

    bool TryLoadOfflineIconForItem(const shopInstStuff::ShopItem& item, std::vector<std::uint8_t>& outData)
    {
        std::uint64_t baseId = 0;
        if (!TryGetOfflineIconBaseId(item, baseId))
            return false;
        return inst::offline::TryGetIconData(baseId, outData);
    }

    std::string GetShopGridIconCachePath(const shopInstStuff::ShopItem& item)
    {
        if (!item.hasIconUrl)
            return "";

        std::string cacheDir = inst::config::appDir + "/shop_icons";
        if (!std::filesystem::exists(cacheDir))
            std::filesystem::create_directory(cacheDir);

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
        return cacheDir + "/" + fileName + ext;
    }

    bool IsBaseTitleCurrentlyInstalled(u64 baseTitleId)
    {
        s32 metaCount = 0;
        if (R_FAILED(nsCountApplicationContentMeta(baseTitleId, &metaCount)) || metaCount <= 0)
            return false;
        return tin::util::IsTitleInstalled(baseTitleId);
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

    std::string BuildGridTitleWithSize(const shopInstStuff::ShopItem& item)
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
    extern MainApplication *mainApp;

    shopInstPage::shopInstPage() : Layout::Layout() {
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
        this->topRect = Rectangle::New(0, 0, 1280, 74, topColor);
        this->infoRect = Rectangle::New(0, 75, 1280, 60, infoColor);
        this->botRect = Rectangle::New(0, 660, 1280, 60, botColor);
        if (inst::config::gayMode) {
            this->titleImage = Image::New(-113, -8, "romfs:/images/logo.png");
            this->appVersionText = TextBlock::New(367, 29, "v" + inst::config::appVersion + (inst::config::appGitMeta.empty() ? "" : ("\n" + inst::config::appGitMeta)), 22);
        }
        else {
            this->titleImage = Image::New(0, -8, "romfs:/images/logo.png");
            this->appVersionText = TextBlock::New(480, 29, "v" + inst::config::appVersion + (inst::config::appGitMeta.empty() ? "" : ("\n" + inst::config::appGitMeta)), 22);
        }
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
        this->pageInfoText = TextBlock::New(10, 81, "", 34);
        this->pageInfoText->SetColor(COLOR("#FFFFFFFF"));
        this->loadingProgressText = TextBlock::New(0, 504, "", 24);
        this->loadingProgressText->SetColor(COLOR("#FFFFFFFF"));
        this->loadingProgressText->SetVisible(false);
        this->loadingBarBack = Rectangle::New(260, 540, 760, 14, COLOR("#FFFFFF33"));
        this->loadingBarBack->SetVisible(false);
        this->loadingBarFill = Rectangle::New(260, 540, 0, 14, COLOR("#34C759FF"));
        this->loadingBarFill->SetVisible(false);
        this->loadingStagesBack = Rectangle::New(220, 208, 840, 276, inst::config::oledMode ? COLOR("#101010CC") : COLOR("#170909CC"));
        this->loadingStagesBack->SetVisible(false);
        this->loadingStagesText = TextBlock::New(252, 230, "", 24);
        this->loadingStagesText->SetColor(COLOR("#E9FFF2FF"));
        this->loadingStagesText->SetVisible(false);
        this->searchInfoText = TextBlock::New(0, 91, "", 20);
        this->searchInfoText->SetColor(COLOR("#FFFFFFFF"));
        this->searchInfoText->SetVisible(false);
        this->butText = TextBlock::New(10, 678, "", 20);
        this->butText->SetColor(COLOR("#FFFFFFFF"));
        this->queueStatusText = TextBlock::New(0, 678, "", 18);
        this->queueStatusText->SetColor(COLOR("#4CD964FF"));
        this->queueStatusText->SetVisible(false);
        this->setButtonsText("inst.shop.buttons_loading"_lang);
        this->menu = pu::ui::elm::Menu::New(0, 136, 1280, COLOR("#FFFFFF00"), 36, 14, 22);
        if (inst::config::oledMode) {
            this->menu->SetOnFocusColor(COLOR("#FFFFFF33"));
            this->menu->SetScrollbarColor(COLOR("#FFFFFF66"));
        } else {
            this->menu->SetOnFocusColor(COLOR("#00000033"));
            this->menu->SetScrollbarColor(COLOR("#17090980"));
        }
        this->infoImage = Image::New(34, 90, "romfs:/images/icons/eshop-connection-waiting.png");
        this->previewImage = Image::New(900, 230, "romfs:/images/icons/title-placeholder.png");
        this->previewImage->SetWidth(320);
        this->previewImage->SetHeight(320);
        auto highlightColor = inst::config::oledMode ? COLOR("#FFFFFF66") : COLOR("#FFFFFF33");
        this->gridHighlight = Rectangle::New(0, 0, kGridTileWidth + 8, kGridTileHeight + 8, highlightColor);
        this->gridHighlight->SetVisible(false);
        this->gridImages.reserve(kGridItemsPerPage);
        for (int i = 0; i < kGridItemsPerPage; i++) {
            auto img = Image::New(0, 0, "romfs:/images/icons/title-placeholder.png");
            img->SetWidth(kGridTileWidth);
            img->SetHeight(kGridTileHeight);
            img->SetVisible(false);
            this->gridImages.push_back(img);
        }
        auto selectedColor = COLOR("#34C75966");
        this->shopGridSelectHighlights.reserve(kGridItemsPerPage);
        for (int i = 0; i < kGridItemsPerPage; i++) {
            auto highlight = Rectangle::New(0, 0, kGridTileWidth + 8, kGridTileHeight + 8, selectedColor);
            highlight->SetVisible(false);
            this->shopGridSelectHighlights.push_back(highlight);
        }
        this->shopGridSelectIcons.reserve(kGridItemsPerPage);
        for (int i = 0; i < kGridItemsPerPage; i++) {
            auto icon = Image::New(0, 0, "romfs:/images/icons/title_selected.png");
            icon->SetWidth(120);
            icon->SetHeight(120);
            icon->SetVisible(false);
            this->shopGridSelectIcons.push_back(icon);
        }
        this->gridTitleText = TextBlock::New(10, 649, "", 18);
        this->gridTitleText->SetColor(COLOR("#FFFFFFFF"));
        this->gridTitleText->SetVisible(false);
        this->imageLoadingText = TextBlock::New(0, 98, "Fetching images...", 18);
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
        this->descriptionOverlayHintText = TextBlock::New(46, 618, "B Close    Up/Down Scroll", 18);
        this->descriptionOverlayHintText->SetColor(COLOR("#FFFFFFFF"));
        this->descriptionOverlayHintText->SetVisible(false);
        this->saveVersionSelectorRect = Rectangle::New(90, 96, 1100, 548, inst::config::oledMode ? COLOR("#000000EE") : COLOR("#170909EE"));
        this->saveVersionSelectorRect->SetVisible(false);
        this->saveVersionSelectorTitleText = TextBlock::New(114, 112, "", 24);
        this->saveVersionSelectorTitleText->SetColor(COLOR("#FFFFFFFF"));
        this->saveVersionSelectorTitleText->SetVisible(false);
        this->saveVersionSelectorMenu = pu::ui::elm::Menu::New(114, 152, 1052, COLOR("#FFFFFF00"), 44, 8, 20);
        if (inst::config::oledMode) {
            this->saveVersionSelectorMenu->SetOnFocusColor(COLOR("#FFFFFF33"));
            this->saveVersionSelectorMenu->SetScrollbarColor(COLOR("#FFFFFF66"));
        } else {
            this->saveVersionSelectorMenu->SetOnFocusColor(COLOR("#00000033"));
            this->saveVersionSelectorMenu->SetScrollbarColor(COLOR("#17090980"));
        }
        this->saveVersionSelectorMenu->SetVisible(false);
        this->saveVersionSelectorDetailText = TextBlock::New(114, 524, "", 18);
        this->saveVersionSelectorDetailText->SetColor(COLOR("#FFFFFFFF"));
        this->saveVersionSelectorDetailText->SetVisible(false);
        this->saveVersionSelectorHintText = TextBlock::New(114, 614, "A Download    B Back", 18);
        this->saveVersionSelectorHintText->SetColor(COLOR("#FFFFFFFF"));
        this->saveVersionSelectorHintText->SetVisible(false);
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
        this->Add(this->queueStatusText);
        this->Add(this->pageInfoText);
        this->Add(this->loadingProgressText);
        this->Add(this->loadingBarBack);
        this->Add(this->loadingBarFill);
        this->Add(this->loadingStagesBack);
        this->Add(this->loadingStagesText);
        this->Add(this->searchInfoText);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        this->Add(this->menu);
#pragma GCC diagnostic pop
        this->Add(this->infoImage);
        this->Add(this->previewImage);
        for (auto& highlight : this->shopGridSelectHighlights)
            this->Add(highlight);
        for (auto& img : this->gridImages)
            this->Add(img);
        for (auto& icon : this->shopGridSelectIcons)
            this->Add(icon);
        this->Add(this->gridHighlight);
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
        this->Add(this->saveVersionSelectorRect);
        this->Add(this->saveVersionSelectorTitleText);
        this->Add(this->saveVersionSelectorMenu);
        this->Add(this->saveVersionSelectorDetailText);
        this->Add(this->saveVersionSelectorHintText);
        this->iconDownloadThread = std::thread([this]() {
            this->iconDownloadThreadMain();
        });
    }

    shopInstPage::~shopInstPage() {
        this->iconDownloadStopRequested.store(true);
        this->iconDownloadCv.notify_all();
        if (this->iconDownloadThread.joinable())
            this->iconDownloadThread.join();
    }

    void shopInstPage::resetIconDownloadState() {
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

    void shopInstPage::queueIconDownload(const shopInstStuff::ShopItem& item, const std::string& filePath) {
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

    void shopInstPage::refreshImageLoadingText(bool showCompleted) {
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
        this->imageLoadingText->SetX(10);

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

    void shopInstPage::iconDownloadThreadMain() {
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

                ok = inst::curl::downloadImageWithAuth(request.iconUrl, tempPath.c_str(), inst::config::shopUser, inst::config::shopPass, 8000);
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

    bool shopInstPage::isAllSection() const {
        if (this->shopSections.empty())
            return false;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= (int)this->shopSections.size())
            return false;
        return this->shopSections[this->selectedSectionIndex].id == "all";
    }

    bool shopInstPage::isInstalledSection() const {
        if (this->shopSections.empty())
            return false;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= (int)this->shopSections.size())
            return false;
        return this->shopSections[this->selectedSectionIndex].id == "installed";
    }

    bool shopInstPage::isSaveSyncSection() const {
        if (!this->saveSyncEnabled)
            return false;
        if (this->shopSections.empty())
            return false;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= (int)this->shopSections.size())
            return false;
        const std::string id = this->shopSections[this->selectedSectionIndex].id;
        return id == "saves" || id == "save";
    }

    const std::vector<shopInstStuff::ShopItem>& shopInstPage::getCurrentItems() const {
        static const std::vector<shopInstStuff::ShopItem> empty;
        if (this->shopSections.empty())
            return empty;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= (int)this->shopSections.size())
            return empty;
        return this->shopSections[this->selectedSectionIndex].items;
    }

    void shopInstPage::applyAllSectionSort() {
        if (this->shopSections.empty())
            return;

        auto it = std::find_if(this->shopSections.begin(), this->shopSections.end(), [](const auto& section) {
            return section.id == "all";
        });
        if (it == this->shopSections.end())
            return;

        auto byNameAsc = [](const shopInstStuff::ShopItem& a, const shopInstStuff::ShopItem& b) {
            return inst::util::ignoreCaseCompare(a.name, b.name);
        };

        auto byDateAsc = [&](const shopInstStuff::ShopItem& a, const shopInstStuff::ShopItem& b) {
            if (a.hasReleaseDate != b.hasReleaseDate)
                return a.hasReleaseDate;
            if (a.hasReleaseDate && b.hasReleaseDate && a.releaseDate != b.releaseDate)
                return a.releaseDate < b.releaseDate;
            return byNameAsc(a, b);
        };

        auto byDateDesc = [&](const shopInstStuff::ShopItem& a, const shopInstStuff::ShopItem& b) {
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

    std::string shopInstPage::getAllSortModeLabel() const {
        switch (this->allSortMode) {
            case 1:
                return "Name Z-A";
            case 2:
                return "Release Date Old-New";
            case 3:
                return "Release Date New-Old";
            default:
                return "Name A-Z";
        }
    }

    void shopInstPage::updateSectionText() {
        this->pageInfoText->SetVisible(true);
        if (this->shopSections.empty()) {
            this->pageInfoText->SetText("inst.shop.loading"_lang);
            this->pageInfoText->SetY(96);
            CenterTextX(this->pageInfoText);
            this->searchInfoText->SetVisible(false);
            return;
        }
        const auto& section = this->shopSections[this->selectedSectionIndex];
        this->pageInfoText->SetText(section.title);
        this->pageInfoText->SetY(81);
        CenterTextX(this->pageInfoText);
        std::string rightInfo;
        if (!this->searchQuery.empty()) {
            std::string query = inst::util::shortenString(this->searchQuery, 28, true);
            rightInfo = "Search: " + query;
        }
        if (this->isAllSection()) {
            if (!rightInfo.empty())
                rightInfo += " | ";
            rightInfo += "Sort: " + this->getAllSortModeLabel();
        } else if (this->browseSortMode != BrowseSortMode::Default) {
            if (!rightInfo.empty())
                rightInfo += " | ";
            rightInfo += "Sort: ";
            rightInfo += this->getBrowseSortLabel();
        }

        if (!rightInfo.empty()) {
            this->searchInfoText->SetText(rightInfo);
            int x = 1280 - this->searchInfoText->GetTextWidth() - 12;
            if (x < 0)
                x = 0;
            this->searchInfoText->SetX(x);
            this->searchInfoText->SetVisible(true);
        } else {
            this->searchInfoText->SetVisible(false);
        }
    }

    void shopInstPage::refreshLoadingStagesText()
    {
        static const char* kLoadingStages[] = {
            "Connect to Shop",
            "Fetch Catalog",
            "Parse Response",
            "Build Sections",
            "Filter and Sort",
            "Sync Save Data",
            "Finalize"
        };
        static const char* kSpinnerFrames[] = {
            "●···",
            "·●··",
            "··●·",
            "···●"
        };
        const int spinnerCount = static_cast<int>(sizeof(kSpinnerFrames) / sizeof(kSpinnerFrames[0]));
        const int spinnerIndex = (spinnerCount > 0) ? (this->loadingSpinnerFrame % spinnerCount) : 0;
        const char* spinner = (spinnerCount > 0) ? kSpinnerFrames[spinnerIndex] : "|";

        std::string text = "inst.shop.loading"_lang + "\n";
        text += "--------------------------------\n";
        for (int i = 0; i < static_cast<int>(sizeof(kLoadingStages) / sizeof(kLoadingStages[0])); i++) {
            if (i < this->loadingStageIndex) {
                text += "[\xEE\x85\x8B] ";
            } else if (i == this->loadingStageIndex) {
                text += "[";
                text += spinner;
                text += "] ";
            } else {
                text += "[○] ";
            }

            if (i + 1 < 10)
                text += "0";
            text += std::to_string(i + 1);
            text += "  ";
            text += kLoadingStages[i];
            if (i + 1 < static_cast<int>(sizeof(kLoadingStages) / sizeof(kLoadingStages[0])))
                text += "\n";
        }
        this->loadingStagesText->SetText(text);
    }

    int shopInstPage::mapLoadingStageToIndex(const std::string& stage) const
    {
        if (stage.find("Preparing shop") != std::string::npos)
            return 0;
        if (stage.find("Fetching shop list") != std::string::npos
            || stage.find("Finishing catalog transfer") != std::string::npos)
            return 1;
        if (stage.find("Parsing shop response") != std::string::npos)
            return 2;
        if (stage.find("Scanning shop sections") != std::string::npos
            || stage.find("Preparing sections") != std::string::npos
            || stage.find("Preparing updates section") != std::string::npos
            || stage.find("Preparing DLC section") != std::string::npos
            || stage.find("Preparing installed section") != std::string::npos
            || stage.find("Preparing owned sections") != std::string::npos)
            return 3;
        if (stage.find("Checking available updates") != std::string::npos
            || stage.find("Filtering sections") != std::string::npos
            || stage.find("Sorting titles") != std::string::npos)
            return 4;
        if (stage.find("Preparing save sync") != std::string::npos
            || stage.find("Loading save sync list") != std::string::npos)
            return 5;
        if (stage.find("Finalizing shop") != std::string::npos
            || stage.find("Shop ready") != std::string::npos)
            return 6;
        return -1;
    }

    void shopInstPage::setLoadingProgressStage(const std::string& stage)
    {
        this->loadingStageLabel = stage;
        this->loadingStageIndex = this->mapLoadingStageToIndex(stage);
        this->refreshLoadingStagesText();
    }

    void shopInstPage::setLoadingProgress(int percent, bool visible)
    {
        if (percent < 0)
            percent = 0;
        if (percent > 100)
            percent = 100;

        this->loadingProgressText->SetVisible(visible);
        this->loadingBarBack->SetVisible(visible);
        this->loadingBarFill->SetVisible(visible);
        this->loadingStagesBack->SetVisible(visible);
        this->loadingStagesText->SetVisible(visible);
        if (!visible) {
            this->loadingStageIndex = -1;
            this->loadingSpinnerFrame = 0;
            this->loadingSpinnerLastTick = 0;
            this->refreshLoadingStagesText();
            return;
        }

        std::string label = this->loadingStageLabel;
        if (label.empty())
            label = "inst.shop.loading"_lang;
        this->loadingProgressText->SetText(label + " " + std::to_string(percent) + "%");
        CenterTextX(this->loadingProgressText);
        this->loadingBarFill->SetWidth((760 * percent) / 100);
        this->refreshLoadingStagesText();
    }

    const char* shopInstPage::getBrowseSortLabel() const
    {
        switch (this->browseSortMode) {
            case BrowseSortMode::DateDesc:
                return "Date";
            case BrowseSortMode::NameAsc:
                return "Name";
            default:
                return "Shop Order";
        }
    }

    void shopInstPage::applyBrowseSort()
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

    void shopInstPage::openSearchDialog()
    {
        std::string query = inst::util::softwareKeyboard("inst.shop.search_hint"_lang, this->searchQuery, 60);
        if (query == this->searchQuery)
            return;

        this->searchQuery = query;
        this->shopGridPage = -1;
        this->gridPage = -1;
        this->updateSectionText();
        this->drawMenuItems(false);
    }

    void shopInstPage::openSortDialog()
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
                "Sort by Date",
                "Sort by Name",
                "Use Shop Order"
            };
        }
        options.push_back("common.cancel"_lang);

        const int choice = mainApp->CreateShowDialog("Sort Shop", details, options, false);
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

        this->shopGridPage = -1;
        this->gridPage = -1;
        this->updateSectionText();
        this->drawMenuItems(false);
    }

    void shopInstPage::getListTextBounds(int& textX, int& textWidth) const
    {
        textX = 0;
        textWidth = 0;
        if (this->menu == nullptr)
            return;

        const int menuX = this->menu->GetProcessedX();
        const int menuW = this->menu->GetWidth();
        textX = menuX + 25;
        if (!this->isInstalledSection() && !this->isSaveSyncSection())
            textX = menuX + 76;
        int maxRowRight = menuX + menuW - 28;
        const int previewSafeRight = menuX + 860;
        if (maxRowRight > previewSafeRight)
            maxRowRight = previewSafeRight;
        if (maxRowRight < textX)
            maxRowRight = textX;
        textWidth = maxRowRight - textX;
    }

    static std::string BuildSingleLineGridTitle(const std::string& title)
    {
        const std::string normalized = OverflowText::NormalizeSingleLineText(title);
        return ClipSingleLineByMenuRender(normalized, 18, 1260, 26, nullptr);
    }

    std::string shopInstPage::buildListMenuLabel(const shopInstStuff::ShopItem& item)
    {
        const std::string normalizedName = OverflowText::NormalizeSingleLineText(item.name);
        std::string sizeText = FormatSizeText(item.size);
        std::string suffix = sizeText.empty() ? "" : (" [" + sizeText + "]");
        const int nameLimit = ComputeListNameLimit(suffix);
        return inst::util::shortenString(normalizedName, nameLimit, true) + suffix;
    }

    void shopInstPage::updateListMarquee(bool force)
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

        if (this->shopGridMode || !this->menu->IsVisible()) {
            hideMarquee();
            this->listPrevSelectedIndex = -1;
            this->listMarqueeIndex = -1;
            return;
        }
        auto& items = this->menu->GetItems();
        if (items.empty() || this->visibleItems.empty()) {
            hideMarquee();
            this->listPrevSelectedIndex = -1;
            this->listMarqueeIndex = -1;
            return;
        }

        int selectedIndex = this->menu->GetSelectedIndex();
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(this->visibleItems.size())) {
            hideMarquee();
            this->listPrevSelectedIndex = -1;
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

        const int itemCount = static_cast<int>(items.size());
        int visibleCount = this->menu->GetNumberOfItemsToShow();
        if (visibleCount < 1)
            visibleCount = 1;
        int maxTopIndex = itemCount - visibleCount;
        if (maxTopIndex < 0)
            maxTopIndex = 0;

        if (force || this->listPrevSelectedIndex < 0) {
            if (selectedIndex >= itemCount - visibleCount)
                this->listVisibleTopIndex = maxTopIndex;
            else if (selectedIndex < visibleCount)
                this->listVisibleTopIndex = 0;
            else
                this->listVisibleTopIndex = selectedIndex;
        } else if (selectedIndex > this->listPrevSelectedIndex) {
            if (selectedIndex >= (this->listVisibleTopIndex + visibleCount))
                this->listVisibleTopIndex = selectedIndex - visibleCount + 1;
        } else if (selectedIndex < this->listPrevSelectedIndex) {
            if (selectedIndex < this->listVisibleTopIndex)
                this->listVisibleTopIndex = selectedIndex;
        }

        if (this->listVisibleTopIndex < 0)
            this->listVisibleTopIndex = 0;
        if (this->listVisibleTopIndex > maxTopIndex)
            this->listVisibleTopIndex = maxTopIndex;
        this->listPrevSelectedIndex = selectedIndex;

        const auto& item = this->visibleItems[static_cast<std::size_t>(selectedIndex)];
        const std::string normalizedName = OverflowText::NormalizeSingleLineText(item.name);
        std::string sizeText = FormatSizeText(item.size);
        std::string suffix = sizeText.empty() ? "" : (" [" + sizeText + "]");

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

        constexpr int kShopListMenuFontSize = 22;
        constexpr int kRenderNoWrapWidth = 1279;
        std::string label = ClipSingleLinePrefixSuffixByMenuRender(
            normalizedName, suffix, kShopListMenuFontSize, kRenderNoWrapWidth, itemHeight - 2);
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
        const pu::ui::Color marqueeHighlightColor = COLOR("#303030FF");
        const pu::ui::Color marqueeResolvedColor = BlendOverOpaque(marqueeBaseColor, marqueeHighlightColor);
        pu::ui::Color marqueeTextColor = ShouldUseDarkText(marqueeResolvedColor) ? COLOR("#000000FF") : COLOR("#FFFFFFFF");
        const pu::ui::Color currentMarqueeTextColor = this->listMarqueeOverlayText->GetColor();
        if (currentMarqueeTextColor.R != marqueeTextColor.R || currentMarqueeTextColor.G != marqueeTextColor.G
            || currentMarqueeTextColor.B != marqueeTextColor.B || currentMarqueeTextColor.A != marqueeTextColor.A) {
            this->listMarqueeOverlayText->SetColor(marqueeTextColor);
        }
        this->listMarqueeMaskRect->SetColor(marqueeBaseColor);
        this->listMarqueeMaskRect->SetX(textX);
        this->listMarqueeMaskRect->SetY(rowY);
        this->listMarqueeMaskRect->SetWidth(maskWidth);
        this->listMarqueeMaskRect->SetHeight(itemHeight);
        this->listMarqueeMaskRect->SetVisible(true);
        this->listMarqueeTintRect->SetColor(marqueeHighlightColor);
        this->listMarqueeTintRect->SetX(textX);
        this->listMarqueeTintRect->SetY(rowY);
        this->listMarqueeTintRect->SetWidth(maskWidth);
        this->listMarqueeTintRect->SetHeight(itemHeight);
        this->listMarqueeTintRect->SetVisible(true);
        this->listMarqueeClipEnabled = true;
        this->listMarqueeClipX = textX;
        this->listMarqueeClipY = rowY;
        this->listMarqueeClipW = maskWidth;
        this->listMarqueeClipH = itemHeight;
        this->listMarqueeOverlayText->SetX(textX - this->listMarqueeOffset);
        this->listMarqueeOverlayText->SetY(textY);
        this->listMarqueeOverlayText->SetVisible(true);

        if (this->listMarqueeFadeAlpha > 0) {
            pu::ui::Color fadeColor = marqueeResolvedColor;
            fadeColor.A = static_cast<u8>(this->listMarqueeFadeAlpha);
            this->listMarqueeFadeRect->SetColor(fadeColor);
            this->listMarqueeFadeRect->SetX(textX);
            this->listMarqueeFadeRect->SetY(rowY);
            this->listMarqueeFadeRect->SetWidth(maskWidth);
            this->listMarqueeFadeRect->SetHeight(itemHeight);
            this->listMarqueeFadeRect->SetVisible(true);
        } else {
            this->listMarqueeFadeRect->SetVisible(false);
        }
    }

    void shopInstPage::updateButtonsText() {
        if (this->saveVersionSelectorVisible) {
            if (this->saveVersionSelectorDeleteMode)
                this->setButtonsText(" Delete Backup    / Select Version     Back");
            else
                this->setButtonsText(" Download    / Select Version     Back");
        }
        else if (this->isSaveSyncSection())
            this->setButtonsText(" Manage Save     Refresh    / Section     Search    \xEE\x83\x85 Sort     Cancel");
        else if (this->isInstalledSection())
            this->setButtonsText(" Details     Refresh    / Section     Search    \xEE\x83\x85 Sort     View     Cancel");
        else {
            std::string buttonsText = "inst.shop.buttons_all"_lang;
            buttonsText += "    \xEE\x83\x85 Sort";
            this->setButtonsText(buttonsText);
        }
    }

    void shopInstPage::buildInstalledSection() {
        (void)this->ensureInstalledSectionBuilt();
    }

    bool shopInstPage::buildInstalledSnapshot() {
        if (this->installedSnapshot.ready)
            return true;

        this->installedSnapshot = {};
        Result rc = nsInitialize();
        if (R_FAILED(rc))
            return false;

        const s32 chunk = 64;
        s32 offset = 0;
        while (true) {
            NsApplicationRecord records[chunk];
            s32 outCount = 0;
            rc = nsListApplicationRecord(records, chunk, offset, &outCount);
            if (R_FAILED(rc) || outCount <= 0)
                break;

            for (s32 i = 0; i < outCount; i++) {
                const u64 baseId = records[i].application_id;
                const bool installed = IsBaseTitleCurrentlyInstalled(baseId);
                this->installedSnapshot.baseInstalled[baseId] = installed;
                if (!installed)
                    continue;

                this->installedSnapshot.installedBaseIds.push_back(baseId);

                s32 metaCount = 0;
                if (R_SUCCEEDED(nsCountApplicationContentMeta(baseId, &metaCount)) && metaCount > 0) {
                    std::vector<NsApplicationContentMetaStatus> list(static_cast<std::size_t>(metaCount));
                    s32 metaOut = 0;
                    if (R_SUCCEEDED(nsListApplicationContentMetaStatus(baseId, 0, list.data(), metaCount, &metaOut)) && metaOut > 0) {
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
                }
            }

            offset += outCount;
        }

        nsExit();
        this->installedSnapshot.ready = true;
        return true;
    }

    void shopInstPage::ensureInstalledSectionPlaceholder() {
        if (inst::config::shopHideInstalledSection)
            return;

        auto it = std::find_if(this->shopSections.begin(), this->shopSections.end(), [](const auto& section) {
            return section.id == "installed";
        });
        if (it != this->shopSections.end())
            return;

        shopInstStuff::ShopSection installedSection;
        installedSection.id = "installed";
        installedSection.title = "Installed";
        this->shopSections.insert(this->shopSections.begin(), std::move(installedSection));
    }

    bool shopInstPage::ensureInstalledSectionBuilt() {
        this->ensureInstalledSectionPlaceholder();
        if (this->installedSnapshot.installedSectionBuilt)
            return true;
        if (!this->buildInstalledSnapshot())
            return false;

        auto sectionIt = std::find_if(this->shopSections.begin(), this->shopSections.end(), [](const auto& section) {
            return section.id == "installed";
        });
        if (sectionIt == this->shopSections.end())
            return false;

        Result rc = nsInitialize();
        if (R_FAILED(rc))
            return false;

        std::vector<shopInstStuff::ShopItem> installedItems;
        installedItems.reserve(this->installedSnapshot.installedBaseIds.size() * 2);

        for (const auto baseId : this->installedSnapshot.installedBaseIds) {
            shopInstStuff::ShopItem baseItem;
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
                        shopInstStuff::ShopItem item;
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

    void shopInstPage::ensureSaveSyncSectionLoaded() {
        if (!this->saveSyncEnabled || this->saveSyncLoaded)
            return;
        if (this->shopSections.empty())
            return;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= static_cast<int>(this->shopSections.size()))
            return;

        const std::string id = this->shopSections[static_cast<std::size_t>(this->selectedSectionIndex)].id;
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
        for (auto& highlight : this->shopGridSelectHighlights)
            highlight->SetVisible(false);
        for (auto& icon : this->shopGridSelectIcons)
            icon->SetVisible(false);

        const int previousSectionIndex = this->selectedSectionIndex;
        this->setLoadingProgressStage("Loading save sync list...");
        this->setLoadingProgress(92, true);
        mainApp->CallForRender();
        this->buildSaveSyncSection(this->activeShopUrl);
        this->saveSyncLoaded = true;

        int saveSectionIndex = -1;
        for (std::size_t i = 0; i < this->shopSections.size(); i++) {
            const std::string& sectionId = this->shopSections[i].id;
            if (sectionId == "saves" || sectionId == "save") {
                saveSectionIndex = static_cast<int>(i);
                break;
            }
        }

        if (saveSectionIndex >= 0) {
            this->selectedSectionIndex = saveSectionIndex;
        } else if (this->shopSections.empty()) {
            this->selectedSectionIndex = 0;
        } else {
            int clampedIndex = previousSectionIndex;
            if (clampedIndex >= static_cast<int>(this->shopSections.size()))
                clampedIndex = static_cast<int>(this->shopSections.size()) - 1;
            if (clampedIndex < 0)
                clampedIndex = 0;
            this->selectedSectionIndex = clampedIndex;
        }

        this->setLoadingProgress(0, false);
        this->setLoadingProgressStage("");
    }

    void shopInstPage::buildSaveSyncSection(const std::string& shopUrl) {
        this->saveSyncLoaded = true;
        this->saveSyncEntries.clear();
        std::unordered_map<std::uint64_t, std::string> saveIconUrlByTitleId;

        auto normalizeSectionId = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        };

        auto buildDefaultShopIconUrl = [&](std::uint64_t titleId) -> std::string {
            if (titleId == 0 || shopUrl.empty())
                return std::string();
            std::string baseUrl = shopUrl;
            while (!baseUrl.empty() && baseUrl.back() == '/')
                baseUrl.pop_back();
            char titleIdHex[17] = {};
            std::snprintf(titleIdHex, sizeof(titleIdHex), "%016lX", titleId);
            return baseUrl + "/api/shop/icon/" + std::string(titleIdHex);
        };

        std::vector<shopInstStuff::ShopItem> remoteSaveItems;
        std::vector<shopInstStuff::ShopSection> retainedSections;
        retainedSections.reserve(this->shopSections.size());
        for (auto& section : this->shopSections) {
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
        this->shopSections = std::move(retainedSections);

        std::vector<shopInstStuff::ShopItem> apiRemoteSaveItems;
        std::string remoteFetchWarning;
        if (!inst::save_sync::FetchRemoteSaveItems(shopUrl, inst::config::shopUser, inst::config::shopPass, apiRemoteSaveItems, remoteFetchWarning)) {
            if (!remoteFetchWarning.empty())
                ShopDlcTrace("save sync remote list warning: %s", remoteFetchWarning.c_str());
            // Keep save sync enabled so local saves can still be shown and uploaded.
        }
        if (!apiRemoteSaveItems.empty()) {
            remoteSaveItems.insert(remoteSaveItems.end(), apiRemoteSaveItems.begin(), apiRemoteSaveItems.end());
        }

        std::string warning;
        inst::save_sync::BuildEntries(remoteSaveItems, this->saveSyncEntries, warning);
        if (!warning.empty())
            ShopDlcTrace("save sync warning: %s", warning.c_str());
        if (this->saveSyncEntries.empty())
            return;

        shopInstStuff::ShopSection saveSection;
        saveSection.id = "saves";
        saveSection.title = "Saves";
        saveSection.items.reserve(this->saveSyncEntries.size());
        for (const auto& entry : this->saveSyncEntries) {
            shopInstStuff::ShopItem item;
            item.name = entry.titleName;
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
                item.iconUrl = buildDefaultShopIconUrl(entry.titleId);
                item.hasIconUrl = !item.iconUrl.empty();
            }
            saveSection.items.push_back(std::move(item));
        }

        this->shopSections.push_back(std::move(saveSection));
    }

    void shopInstPage::refreshSaveSyncSection(std::uint64_t selectedTitleId, int previousSectionIndex) {
        this->buildSaveSyncSection(this->activeShopUrl);

        int saveSectionIndex = -1;
        for (std::size_t i = 0; i < this->shopSections.size(); i++) {
            const std::string& id = this->shopSections[i].id;
            if (id == "saves" || id == "save") {
                saveSectionIndex = static_cast<int>(i);
                break;
            }
        }

        if (saveSectionIndex >= 0) {
            this->selectedSectionIndex = saveSectionIndex;
        } else if (this->shopSections.empty()) {
            this->selectedSectionIndex = 0;
        } else {
            if (previousSectionIndex >= static_cast<int>(this->shopSections.size()))
                previousSectionIndex = static_cast<int>(this->shopSections.size()) - 1;
            if (previousSectionIndex < 0)
                previousSectionIndex = 0;
            this->selectedSectionIndex = previousSectionIndex;
        }

        this->shopGridPage = -1;
        this->gridPage = -1;
        this->updateSectionText();
        this->updateButtonsText();
        this->drawMenuItems(false);

        if (this->isSaveSyncSection() && !this->visibleItems.empty()) {
            int restoredIndex = 0;
            for (std::size_t i = 0; i < this->visibleItems.size(); i++) {
                if (this->visibleItems[i].hasTitleId && this->visibleItems[i].titleId == selectedTitleId) {
                    restoredIndex = static_cast<int>(i);
                    break;
                }
            }

            if (this->shopGridMode) {
                this->shopGridIndex = restoredIndex;
                this->updateShopGrid();
            } else {
                this->menu->SetSelectedIndex(restoredIndex);
                this->updatePreview();
                this->updateDescriptionPanel();
            }
        } else if (this->shopGridMode) {
            this->updateShopGrid();
        } else {
            this->updatePreview();
            this->updateDescriptionPanel();
        }
    }

    bool shopInstPage::openSaveVersionSelector(const inst::save_sync::SaveSyncEntry& entry, int previousSectionIndex, bool deleteMode) {
        this->saveVersionSelectorVersions.clear();
        for (const auto& version : entry.remoteVersions) {
            if (!version.downloadUrl.empty() || !version.saveId.empty())
                this->saveVersionSelectorVersions.push_back(version);
        }
        if (this->saveVersionSelectorVersions.size() <= 1)
            return false;

        this->saveVersionSelectorTitleId = entry.titleId;
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

    void shopInstPage::closeSaveVersionSelector(bool refreshList) {
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
        if (this->shopGridMode) {
            this->updateShopGrid();
        } else {
            this->updatePreview();
        }
        this->updateDescriptionPanel();
    }

    void shopInstPage::refreshSaveVersionSelectorDetailText() {
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

    bool shopInstPage::handleSaveVersionSelectorInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
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
                return entry.titleId == this->saveVersionSelectorTitleId;
            });
            if (it == this->saveSyncEntries.end()) {
                this->closeSaveVersionSelector(true);
                mainApp->CreateShowDialog("Save Sync", "Unable to resolve save entry.", {"common.ok"_lang}, true);
                return true;
            }

            if (this->saveVersionSelectorLocalAvailable) {
                if (!this->saveVersionSelectorDeleteMode) {
                    const int overwriteChoice = mainApp->CreateShowDialog(
                        "Save Sync",
                        "Replace local save data with the server copy?",
                        {"common.yes"_lang, "common.no"_lang},
                        false);
                    if (overwriteChoice != 0)
                        return true;
                }
            }

            const auto& selectedVersion = this->saveVersionSelectorVersions[static_cast<std::size_t>(selectedIndex)];
            std::string error;
            bool ok = false;
            if (this->saveVersionSelectorDeleteMode) {
                std::string saveIdText = selectedVersion.saveId.empty() ? "unknown" : selectedVersion.saveId;
                const int confirmDelete = mainApp->CreateShowDialog(
                    "Save Sync",
                    "Delete selected server backup?\nID: " + inst::util::shortenString(saveIdText, 52, false),
                    {"Delete", "common.cancel"_lang},
                    false);
                if (confirmDelete != 0)
                    return true;

                ok = inst::save_sync::DeleteSaveFromServer(
                    this->activeShopUrl,
                    inst::config::shopUser,
                    inst::config::shopPass,
                    *it,
                    &selectedVersion,
                    error);
            } else {
                ok = inst::save_sync::DownloadSaveToConsole(
                    this->activeShopUrl,
                    inst::config::shopUser,
                    inst::config::shopPass,
                    *it,
                    &selectedVersion,
                    error);
            }
            if (!ok) {
                if (error.empty())
                    error = "Save sync failed.";
                mainApp->CreateShowDialog("Save Sync", error, {"common.ok"_lang}, true);
                return true;
            }

            mainApp->CreateShowDialog("Save Sync", this->saveVersionSelectorDeleteMode ? "Save backup deleted successfully." : "Save downloaded successfully.", {"common.ok"_lang}, true);
            const std::uint64_t selectedTitleId = this->saveVersionSelectorTitleId;
            const int previousSectionIndex = this->saveVersionSelectorPreviousSectionIndex;
            this->closeSaveVersionSelector(false);
            this->refreshSaveSyncSection(selectedTitleId, previousSectionIndex);
            return true;
        }

        this->refreshSaveVersionSelectorDetailText();
        return true;
    }

    void shopInstPage::handleSaveSyncAction(int selectedIndex) {
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(this->visibleItems.size()))
            return;
        const auto& selectedItem = this->visibleItems[selectedIndex];
        if (!selectedItem.hasTitleId)
            return;

        auto it = std::find_if(this->saveSyncEntries.begin(), this->saveSyncEntries.end(), [&](const auto& entry) {
            return entry.titleId == selectedItem.titleId;
        });
        if (it == this->saveSyncEntries.end()) {
            mainApp->CreateShowDialog("Save Sync", "Unable to resolve save entry.", {"common.ok"_lang}, true);
            return;
        }

        const std::uint64_t selectedTitleId = it->titleId;
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
            mainApp->CreateShowDialog("Save Sync", "No local or remote save is available for this title.", {"common.ok"_lang}, true);
            return;
        }
        options.push_back("common.cancel"_lang);

        int choice = mainApp->CreateShowDialog("Save Sync", it->titleName, options, false);
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
                "Save Sync",
                "Replace local save data with the server copy?",
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
                "Save Sync",
                "Delete server backup?\nID: " + inst::util::shortenString(saveIdText, 52, false),
                {"Delete", "common.cancel"_lang},
                false);
            if (confirmDelete != 0)
                return;
        }

        std::string error;
        bool ok = false;
        if (actions[choice] == 1) {
            ok = inst::save_sync::UploadSaveToServer(this->activeShopUrl, inst::config::shopUser, inst::config::shopPass, *it, uploadNote, error);
        } else if (actions[choice] == 2) {
            ok = inst::save_sync::DownloadSaveToConsole(this->activeShopUrl, inst::config::shopUser, inst::config::shopPass, *it, selectedRemoteVersion, error);
        } else if (actions[choice] == 3) {
            ok = inst::save_sync::DeleteSaveFromServer(this->activeShopUrl, inst::config::shopUser, inst::config::shopPass, *it, selectedRemoteVersion, error);
        }

        if (!ok) {
            if (error.empty())
                error = "Save sync failed.";
            mainApp->CreateShowDialog("Save Sync", error, {"common.ok"_lang}, true);
            return;
        }

        std::string successMessage = "Save sync completed successfully.";
        if (actions[choice] == 1)
            successMessage = "Save uploaded successfully.";
        else if (actions[choice] == 2)
            successMessage = "Save downloaded successfully.";
        else if (actions[choice] == 3)
            successMessage = "Save backup deleted successfully.";
        mainApp->CreateShowDialog("Save Sync", successMessage, {"common.ok"_lang}, true);
        this->refreshSaveSyncSection(selectedTitleId, previousSectionIndex);
    }

    void shopInstPage::buildLegacyOwnedSections() {
        if (this->shopSections.empty())
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
        for (std::size_t i = 0; i < this->shopSections.size(); i++) {
            const std::string id = normalizeSectionId(this->shopSections[i].id);
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

        std::vector<shopInstStuff::ShopItem> updates;
        std::vector<shopInstStuff::ShopItem> dlcs;
        std::unordered_set<std::string> seenUpdateKeys;
        std::unordered_set<std::string> seenDlcKeys;

        for (const auto& section : this->shopSections) {
            const std::string id = normalizeSectionId(section.id);
            if (id == "installed" || id == "updates" || id == "update" || id == "dlc" || id == "addon" || id == "add-on" || id == "add_ons")
                continue;

            for (const auto& item : section.items) {
                if (item.url.empty())
                    continue;

                std::uint64_t baseTitleId = 0;
                if (!DeriveBaseTitleId(item, baseTitleId))
                    continue;
                if (!isBaseInstalled(baseTitleId))
                    continue;

                if (!hasUpdatesSection && IsUpdateItem(item)) {
                    const std::string key = BuildItemIdentityKey(item);
                    if (!key.empty() && !seenUpdateKeys.insert(key).second)
                        continue;
                    updates.push_back(item);
                    continue;
                }

                if (!hasDlcSection && IsDlcItem(item)) {
                    if (item.hasTitleId && this->installedSnapshot.installedDlcIds.count(item.titleId))
                        continue;
                    const std::string key = BuildItemIdentityKey(item);
                    if (!key.empty() && !seenDlcKeys.insert(key).second)
                        continue;
                    dlcs.push_back(item);
                }
            }
        }

        auto sortByName = [](std::vector<shopInstStuff::ShopItem>& items) {
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                return inst::util::ignoreCaseCompare(a.name, b.name);
            });
        };
        sortByName(updates);
        sortByName(dlcs);

        int insertIndex = (allSectionIndex >= 0) ? (allSectionIndex + 1) : static_cast<int>(this->shopSections.size());
        if (!hasUpdatesSection && !updates.empty()) {
            shopInstStuff::ShopSection updatesSection;
            updatesSection.id = "updates";
            updatesSection.title = "Updates";
            updatesSection.items = std::move(updates);
            this->shopSections.insert(this->shopSections.begin() + insertIndex, std::move(updatesSection));
            insertIndex++;
        }
        if (!hasDlcSection && !dlcs.empty()) {
            shopInstStuff::ShopSection dlcSection;
            dlcSection.id = "dlc";
            dlcSection.title = "DLC";
            dlcSection.items = std::move(dlcs);
            this->shopSections.insert(this->shopSections.begin() + insertIndex, std::move(dlcSection));
        }
    }

    void shopInstPage::cacheAvailableUpdates() {
        this->availableUpdates.clear();
        std::unordered_set<std::string> seenKeys;
        for (const auto& section : this->shopSections) {
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

    void shopInstPage::filterOwnedSections() {
        if (this->shopSections.empty())
            return;
        if (!this->buildInstalledSnapshot()) {
            ShopDlcTrace("filterOwnedSections buildInstalledSnapshot failed");
            return;
        }
        ShopDlcTrace("filterOwnedSections begin sections=%llu snapshotBases=%llu snapshotDlcs=%llu",
            static_cast<unsigned long long>(this->shopSections.size()),
            static_cast<unsigned long long>(this->installedSnapshot.baseInstalled.size()),
            static_cast<unsigned long long>(this->installedSnapshot.installedDlcIds.size()));
        const bool enforceBaseInstallForDlcSection = true;
        ShopDlcTrace("filter mode nativeDlcSectionPresent=%d enforceBaseInstallForDlcSection=%d",
            this->nativeDlcSectionPresent ? 1 : 0,
            enforceBaseInstallForDlcSection ? 1 : 0);

        auto looksLikeDlcTitleId = [](std::uint64_t titleId) {
            const std::uint64_t suffix = titleId & 0xFFFULL;
            return suffix != 0x000ULL && suffix != 0x800ULL;
        };

        auto resolveItemDlcTitleId = [&](const shopInstStuff::ShopItem& item, std::uint64_t& outTitleId) {
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

        auto isBaseInstalled = [&](const shopInstStuff::ShopItem& item, std::uint32_t& outVersion) {
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

        auto isDlcInstalled = [&](const shopInstStuff::ShopItem& item) {
            if (!IsDlcItem(item))
                return false;

            std::uint64_t dlcTitleId = 0;
            if (!resolveItemDlcTitleId(item, dlcTitleId)) {
                ShopDlcTrace("dlc resolve failed name='%s' appType=%d hasTitleId=%d titleId='%s' hasAppId=%d appId='%s'",
                    TraceNamePreview(item.name).c_str(), item.appType, item.hasTitleId ? 1 : 0,
                    item.hasTitleId ? FormatTitleIdHex(item.titleId).c_str() : "none",
                    item.hasAppId ? 1 : 0, item.hasAppId ? item.appId.c_str() : "none");
                return false;
            }
            if (isDlcInstalledByTitleId(dlcTitleId)) {
                ShopDlcTrace("dlc installed yes dlcId=%s name='%s'", FormatTitleIdHex(dlcTitleId).c_str(), TraceNamePreview(item.name).c_str());
                return true;
            }
            ShopDlcTrace("dlc installed no dlcId=%s name='%s'", FormatTitleIdHex(dlcTitleId).c_str(), TraceNamePreview(item.name).c_str());
            return false;
        };

        for (auto& section : this->shopSections) {
            if (section.items.empty())
                continue;
            if (section.id != "updates" && section.id != "dlc")
                continue;

            std::vector<shopInstStuff::ShopItem> filtered;
            filtered.reserve(section.items.size());
            for (const auto& item : section.items) {
                std::uint32_t installedVersion = 0;
                std::uint64_t baseTitleId = 0;
                DeriveBaseTitleId(item, baseTitleId);
                bool baseIsInstalled = true;
                if (section.id == "updates" || IsUpdateItem(item) || enforceBaseInstallForDlcSection)
                    baseIsInstalled = isBaseInstalled(item, installedVersion);
                if ((section.id == "updates" || IsUpdateItem(item) || enforceBaseInstallForDlcSection) && !baseIsInstalled) {
                    if (section.id == "dlc")
                        ShopDlcTrace("dlc drop reason=base_not_installed name='%s'", TraceNamePreview(item.name).c_str());
                    continue;
                }
                if (section.id == "updates" || IsUpdateItem(item)) {
                    if (!item.hasAppVersion || item.appVersion > installedVersion)
                        filtered.push_back(item);
                } else {
                    if (isDlcInstalled(item)) {
                        if (section.id == "dlc")
                            ShopDlcTrace("dlc drop reason=already_installed name='%s'", TraceNamePreview(item.name).c_str());
                        continue;
                    }
                    if (section.id == "dlc")
                        ShopDlcTrace("dlc keep name='%s'", TraceNamePreview(item.name).c_str());
                    filtered.push_back(item);
                }
            }
            section.items = std::move(filtered);
        }

        for (auto& section : this->shopSections) {
            if (section.items.empty())
                continue;
            if (section.id == "all" || section.id == "installed")
                continue;
            if (section.id == "updates" || section.id == "dlc")
                continue;

            std::vector<shopInstStuff::ShopItem> filtered;
            filtered.reserve(section.items.size());
            for (const auto& item : section.items) {
                if (!IsDlcItem(item)) {
                    filtered.push_back(item);
                    continue;
                }
                std::uint32_t installedVersion = 0;
                if (isDlcInstalled(item))
                    continue;
                if (isBaseInstalled(item, installedVersion))
                    filtered.push_back(item);
            }
            section.items = std::move(filtered);
        }

        if (inst::config::shopHideInstalled) {
            for (auto& section : this->shopSections) {
                if (section.items.empty())
                    continue;
                if (section.id == "installed" || section.id == "updates" || section.id == "update" ||
                    (this->saveSyncEnabled && (section.id == "saves" || section.id == "save")))
                    continue;

                std::vector<shopInstStuff::ShopItem> filtered;
                filtered.reserve(section.items.size());
                for (const auto& item : section.items) {
                    bool hideInstalledItem = false;
                    std::uint32_t installedVersion = 0;
                    if (IsBaseItem(item)) {
                        hideInstalledItem = isBaseInstalled(item, installedVersion);
                    } else if (IsUpdateItem(item)) {
                        if (isBaseInstalled(item, installedVersion) && item.hasAppVersion && item.appVersion <= installedVersion)
                            hideInstalledItem = true;
                    } else if (IsDlcItem(item)) {
                        hideInstalledItem = isDlcInstalled(item);
                    }
                    if (!hideInstalledItem) {
                        filtered.push_back(item);
                    }
                }
                section.items = std::move(filtered);
            }
        }

        auto hasSuffix = [](const std::string& text, const std::string& suffix) {
            if (text.size() < suffix.size())
                return false;
            return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        };

        auto appendTypeLabels = [&](shopInstStuff::ShopSection& section) {
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

        for (auto& section : this->shopSections) {
            if (section.items.empty())
                continue;
            appendTypeLabels(section);
        }

    }

    void shopInstPage::updatePreview() {
        if (this->shopGridMode) {
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
            this->previewImage->SetX(900);
            this->previewImage->SetY(230);
            this->previewImage->SetWidth(320);
            this->previewImage->SetHeight(320);
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
            this->previewImage->SetImage("romfs:/images/icons/title-placeholder.png");
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
            const std::string filePath = GetShopGridIconCachePath(item);
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

        this->previewImage->SetImage("romfs:/images/icons/title-placeholder.png");
        applyPreviewLayout();
        this->previewImage->SetVisible(true);
        this->refreshImageLoadingText();
    }


    void shopInstPage::updateDebug() {
        if (!this->debugVisible) {
            this->debugText->SetVisible(false);
            return;
        }
        if (this->visibleItems.empty()) {
            std::string text = "debug: no items";
            if (!this->shopSections.empty() && this->selectedSectionIndex >= 0 && this->selectedSectionIndex < (int)this->shopSections.size()) {
                const auto& section = this->shopSections[this->selectedSectionIndex];
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

        int selectedIndex = this->shopGridMode ? this->shopGridIndex : this->menu->GetSelectedIndex();
        if (this->isInstalledSection() && this->shopGridMode)
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

    void shopInstPage::drawMenuItems(bool clearItems) {
        std::string previousSelectionKey;
        int previousSelectionIndex = 0;
        if (!this->visibleItems.empty()) {
            int currentIndex = this->shopGridMode ? this->shopGridIndex : this->menu->GetSelectedIndex();
            if (this->isInstalledSection() && this->shopGridMode)
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

        if (this->isAllSection() && inst::config::shopAllBaseOnly) {
            std::vector<shopInstStuff::ShopItem> baseOnlyItems;
            baseOnlyItems.reserve(this->visibleItems.size());
            for (const auto& item : this->visibleItems) {
                if (IsBaseItem(item))
                    baseOnlyItems.push_back(item);
            }
            this->visibleItems = std::move(baseOnlyItems);
        }

        if (!this->isAllSection())
            this->applyBrowseSort();

        if (!this->shopSections.empty() && this->selectedSectionIndex >= 0 && this->selectedSectionIndex < static_cast<int>(this->shopSections.size()) && this->visibleItems.empty()) {
            const auto &section = this->shopSections[this->selectedSectionIndex];
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
            }
        }

        if (this->isInstalledSection() && this->shopGridMode) {
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

        if (this->shopGridMode) {
            this->listMarqueeMaskRect->SetVisible(false);
            this->listMarqueeTintRect->SetVisible(false);
            this->listMarqueeOverlayText->SetVisible(false);
            this->listMarqueeClipEnabled = false;
            this->listMarqueeFadeRect->SetVisible(false);
            this->updateShopGrid();
            this->updateDescriptionPanel();
            return;
        }

        for (auto& img : this->gridImages)
            img->SetVisible(false);
        this->gridHighlight->SetVisible(false);
        this->gridTitleText->SetVisible(false);
        for (auto& icon : this->shopGridSelectIcons)
            icon->SetVisible(false);
        this->menu->SetVisible(true);

        const bool installedSection = this->isInstalledSection();
        const bool saveSyncSection = this->isSaveSyncSection();
        std::unordered_set<std::string> selectedUrls;
        if (!installedSection && !saveSyncSection && !this->selectedItems.empty()) {
            selectedUrls.reserve(this->selectedItems.size());
            for (const auto& selected : this->selectedItems) {
                if (!selected.url.empty())
                    selectedUrls.insert(selected.url);
            }
        }
        const bool useFastListLabels = this->visibleItems.size() > 400;
        for (std::size_t i = 0; i < this->visibleItems.size(); i++) {
            const auto& item = this->visibleItems[i];
            std::string itm;
            if (useFastListLabels) {
                itm = OverflowText::NormalizeSingleLineText(item.name);
                std::string sizeText = FormatSizeText(item.size);
                if (!sizeText.empty())
                    itm += " [" + sizeText + "]";
            } else {
                itm = this->buildListMenuLabel(item);
            }
            auto entry = pu::ui::elm::MenuItem::New(itm);
            entry->SetColor(COLOR("#FFFFFFFF"));
            if (!installedSection && !saveSyncSection) {
                entry->SetIcon("romfs:/images/icons/checkbox-blank-outline.png");
                if (!item.url.empty() && selectedUrls.find(item.url) != selectedUrls.end())
                    entry->SetIcon("romfs:/images/icons/check-box-outline.png");
            }
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
        this->shopGridIndex = restoredIndex;
        this->gridSelectedIndex = restoredIndex;
        this->listMarqueeIndex = -1;
        this->listVisibleTopIndex = 0;
        this->listPrevSelectedIndex = -1;
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

    void shopInstPage::refreshListSelectionIcons() {
        if (this->menu->GetItems().empty())
            return;
        if (this->isInstalledSection() || this->isSaveSyncSection())
            return;

        std::unordered_set<std::string> selectedUrls;
        selectedUrls.reserve(this->selectedItems.size());
        for (const auto& selected : this->selectedItems) {
            if (!selected.url.empty())
                selectedUrls.insert(selected.url);
        }

        auto& menuItems = this->menu->GetItems();
        const std::size_t count = std::min(menuItems.size(), this->visibleItems.size());
        for (std::size_t i = 0; i < count; i++) {
            const auto& item = this->visibleItems[i];
            const bool isSelected = !item.url.empty() && selectedUrls.find(item.url) != selectedUrls.end();
            menuItems[i]->SetIcon(isSelected ? "romfs:/images/icons/check-box-outline.png"
                                             : "romfs:/images/icons/checkbox-blank-outline.png");
        }
    }

    void shopInstPage::updateInstalledGrid() {
        this->listMarqueeMaskRect->SetVisible(false);
        this->listMarqueeTintRect->SetVisible(false);
        this->listMarqueeOverlayText->SetVisible(false);
        this->listMarqueeClipEnabled = false;
        this->listMarqueeFadeRect->SetVisible(false);
        if (!this->isInstalledSection() || !this->shopGridMode) {
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
                    this->gridImages[i]->SetImage("romfs:/images/icons/title-placeholder.png");
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

    void shopInstPage::updateShopGrid() {
        this->listMarqueeMaskRect->SetVisible(false);
        this->listMarqueeTintRect->SetVisible(false);
        this->listMarqueeOverlayText->SetVisible(false);
        this->listMarqueeClipEnabled = false;
        this->listMarqueeFadeRect->SetVisible(false);
        if (!this->shopGridMode || this->visibleItems.empty()) {
            for (auto& img : this->gridImages)
                img->SetVisible(false);
            this->gridHighlight->SetVisible(false);
            this->gridTitleText->SetVisible(false);
            for (auto& highlight : this->shopGridSelectHighlights)
                highlight->SetVisible(false);
            for (auto& icon : this->shopGridSelectIcons)
                icon->SetVisible(false);
            this->imageLoadingText->SetVisible(false);
            this->shopGridPage = -1;
            this->updateDescriptionPanel();
            return;
        }

        this->menu->SetVisible(false);
        this->previewImage->SetVisible(false);

        if (this->shopGridIndex < 0)
            this->shopGridIndex = 0;
        if (this->shopGridIndex >= (int)this->visibleItems.size())
            this->shopGridIndex = (int)this->visibleItems.size() - 1;

        int page = this->shopGridIndex / kGridItemsPerPage;
        int pageStart = page * kGridItemsPerPage;
        int maxIndex = (int)this->visibleItems.size();
        if (page != this->shopGridPage) {
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
                    std::string filePath = GetShopGridIconCachePath(item);
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
                    this->gridImages[i]->SetImage("romfs:/images/icons/title-placeholder.png");
                    this->gridImages[i]->SetWidth(kGridTileWidth);
                    this->gridImages[i]->SetHeight(kGridTileHeight);
                }
                this->gridImages[i]->SetVisible(true);
            }

            this->shopGridPage = page;
        }

        if (this->iconDownloadUiDirty.exchange(false)) {
            for (int i = 0; i < kGridItemsPerPage; i++) {
                const int itemIndex = pageStart + i;
                if (itemIndex < 0 || itemIndex >= maxIndex)
                    continue;

                const auto& item = this->visibleItems[itemIndex];
                const std::string filePath = GetShopGridIconCachePath(item);
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
                this->shopGridSelectHighlights[i]->SetVisible(false);
                this->shopGridSelectIcons[i]->SetVisible(false);
                continue;
            }

            const auto& item = this->visibleItems[itemIndex];
            bool isSelected = false;
            if (!this->selectedItems.empty() && !item.url.empty()) {
                isSelected = std::any_of(this->selectedItems.begin(), this->selectedItems.end(), [&](const auto& entry) {
                    return entry.url == item.url;
                });
            }
            const int iconSize = 120;
            this->shopGridSelectIcons[i]->SetX(x + (kGridTileWidth - iconSize) / 2);
            this->shopGridSelectIcons[i]->SetY(y + (kGridTileHeight - iconSize) / 2);
            this->shopGridSelectIcons[i]->SetVisible(isSelected);
            this->shopGridSelectHighlights[i]->SetX(x - 4);
            this->shopGridSelectHighlights[i]->SetY(y - 4);
            this->shopGridSelectHighlights[i]->SetWidth(kGridTileWidth + 8);
            this->shopGridSelectHighlights[i]->SetHeight(kGridTileHeight + 8);
            this->shopGridSelectHighlights[i]->SetVisible(isSelected);
        }

        int slot = this->shopGridIndex - pageStart;
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

        if (this->shopGridIndex >= 0 && this->shopGridIndex < (int)this->visibleItems.size()) {
            std::string title = BuildGridTitleWithSize(this->visibleItems[this->shopGridIndex]);
            this->gridTitleText->SetText(BuildSingleLineGridTitle(title));
            this->gridTitleText->SetVisible(true);
        } else {
            this->gridTitleText->SetVisible(false);
        }
        this->updateDescriptionPanel();
    }

    void shopInstPage::selectTitle(int selectedIndex) {
        if (this->isSaveSyncSection())
            return;
        if (selectedIndex < 0 || selectedIndex >= (int)this->visibleItems.size())
            return;
        const auto& item = this->visibleItems[selectedIndex];
        if (item.url.empty())
            return;
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
        if (this->shopGridMode) {
            if (this->isInstalledSection()) {
                this->gridSelectedIndex = this->shopGridIndex;
                this->updateInstalledGrid();
            } else {
                this->updateShopGrid();
            }
            return;
        }

        this->drawMenuItems(false);
    }

    void shopInstPage::updateRememberedSelection() {
    }

    void shopInstPage::startShop(bool forceRefresh) {
        ResetShopDlcTrace();
        ShopDlcTrace("startShop begin forceRefresh=%d shopHideInstalled=%d hideInstalledSection=%d", forceRefresh ? 1 : 0, inst::config::shopHideInstalled ? 1 : 0, inst::config::shopHideInstalledSection ? 1 : 0);
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
        this->setButtonsText("inst.shop.buttons_loading"_lang);
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
        for (auto& highlight : this->shopGridSelectHighlights)
            highlight->SetVisible(false);
        for (auto& icon : this->shopGridSelectIcons)
            icon->SetVisible(false);
        this->selectedItems.clear();
        this->visibleItems.clear();
        this->shopSections.clear();
        this->availableUpdates.clear();
        this->saveSyncEntries.clear();
        if (forceRefresh) {
            this->installedSnapshot = {};
        } else {
            // Reuse installed-state caches across normal reloads to avoid rescanning NS metadata.
            // The per-section installed list is tied to shopSections and must be rebuilt each time.
            this->installedSnapshot.installedSectionBuilt = false;
        }
        this->activeShopUrl.clear();
        this->searchQuery.clear();
        this->previewKey.clear();
        this->pageInfoText->SetText("");
        this->pageInfoText->SetVisible(false);
        this->infoImage->SetX((1280 - this->infoImage->GetWidth()) / 2);
        this->infoImage->SetY(86);
        this->setLoadingProgressStage("Preparing shop...");
        this->setLoadingProgress(0, true);
        mainApp->LoadLayout(mainApp->shopinstPage);
        mainApp->CallForRender();

        std::string shopUrl = inst::config::shopUrl;
        if (shopUrl.empty()) {
            std::vector<inst::config::ShopProfile> shops = inst::config::LoadShops();
            if (!shops.empty() && inst::config::SetActiveShop(shops.front(), true))
                shopUrl = inst::config::shopUrl;
        }
        if (shopUrl.empty()) {
            shopUrl = inst::util::softwareKeyboard("options.shop.url_hint"_lang, "http://", 200);
            if (shopUrl.empty()) {
                mainApp->LoadLayout(mainApp->mainPage);
                return;
            }
            inst::config::shopUrl = shopUrl;
            inst::config::setConfig();
        }
        this->activeShopUrl = shopUrl;

        std::string error;
        bool usedLegacyFallback = false;
        int loadingPercent = 5;
        auto updateLoadingProgress = [&](int percent, const char* stage = nullptr, bool forceRender = false) {
            if (stage != nullptr)
                this->setLoadingProgressStage(stage);
            if (percent > 100)
                percent = 100;
            if (percent < loadingPercent)
                return;
            if (!forceRender && percent == loadingPercent)
                return;
            loadingPercent = percent;
            this->setLoadingProgress(loadingPercent, true);
            mainApp->CallForRender();
        };
        this->setLoadingProgressStage("Fetching shop list...");
        this->setLoadingProgress(loadingPercent, true);
        mainApp->CallForRender();

        const std::string cacheKey = shopUrl + "\n" + inst::config::shopUser + "\n" + inst::config::shopPass + "\n" + (inst::config::shopLegacyMode ? "1" : "0");
        const bool canUseCatalogCache = !forceRefresh
            && this->catalogCacheValid
            && this->catalogCacheKey == cacheKey
            && !this->catalogCacheSections.empty();

        if (canUseCatalogCache) {
            updateLoadingProgress(89, "Using cached catalog...", true);
            this->shopSections = this->catalogCacheSections;
            usedLegacyFallback = this->catalogCacheUsedLegacyFallback;
        } else {
            std::atomic<bool> fetchDone{false};
            std::atomic<std::uint64_t> fetchDownloaded{0};
            std::atomic<std::uint64_t> fetchTotal{0};
            std::vector<shopInstStuff::ShopSection> fetchedSections;
            std::string fetchError;
            bool fetchUsedLegacyFallback = false;

            std::thread fetchThread([&]() {
                auto fetchProgressCb = [&](std::uint64_t downloaded, std::uint64_t total) {
                    fetchDownloaded.store(downloaded, std::memory_order_relaxed);
                    fetchTotal.store(total, std::memory_order_relaxed);
                };
                fetchedSections = shopInstStuff::FetchShopSections(
                    shopUrl,
                    inst::config::shopUser,
                    inst::config::shopPass,
                    fetchError,
                    &fetchUsedLegacyFallback,
                    fetchProgressCb
                );
                fetchDone.store(true, std::memory_order_release);
            });

            const u64 spinnerStepTicks = (armGetSystemTickFreq() * 140) / 1000;
            while (!fetchDone.load(std::memory_order_acquire)) {
                const u64 now = armGetSystemTick();
                const std::uint64_t downloaded = fetchDownloaded.load(std::memory_order_relaxed);
                const std::uint64_t total = fetchTotal.load(std::memory_order_relaxed);

                int mappedPercent = loadingPercent;
                const char* stageLabel = "Fetching shop list...";
                if (total > 0) {
                    int fetchPercent = static_cast<int>((downloaded * 100ULL) / total);
                    if (fetchPercent > 100)
                        fetchPercent = 100;

                    // Keep room for post-download parsing/preparation.
                    mappedPercent = 5 + ((fetchPercent * 84) / 100); // 5..89
                    if (fetchPercent >= 96)
                        stageLabel = "Finishing catalog transfer...";
                } else {
                    // Some servers do not provide content-length; map received bytes to coarse milestones.
                    if (downloaded >= (64ULL * 1024ULL))
                        mappedPercent = std::max(mappedPercent, 10);
                    if (downloaded >= (256ULL * 1024ULL))
                        mappedPercent = std::max(mappedPercent, 16);
                    if (downloaded >= (768ULL * 1024ULL))
                        mappedPercent = std::max(mappedPercent, 24);
                    if (downloaded >= (2ULL * 1024ULL * 1024ULL))
                        mappedPercent = std::max(mappedPercent, 34);
                    if (downloaded >= (4ULL * 1024ULL * 1024ULL))
                        mappedPercent = std::max(mappedPercent, 45);
                    if (downloaded >= (8ULL * 1024ULL * 1024ULL))
                        mappedPercent = std::max(mappedPercent, 57);
                    if (downloaded >= (12ULL * 1024ULL * 1024ULL))
                        mappedPercent = std::max(mappedPercent, 68);
                    if (downloaded >= (16ULL * 1024ULL * 1024ULL))
                        mappedPercent = std::max(mappedPercent, 78);
                    if (downloaded >= (24ULL * 1024ULL * 1024ULL))
                        mappedPercent = std::max(mappedPercent, 86);
                    if (downloaded >= (32ULL * 1024ULL * 1024ULL))
                        mappedPercent = std::max(mappedPercent, 89);
                    if (downloaded >= (24ULL * 1024ULL * 1024ULL))
                        stageLabel = "Finishing catalog transfer...";
                }

                const bool progressAdvanced = (mappedPercent > loadingPercent);
                const bool stageChanged = (this->loadingStageLabel != stageLabel);
                if (progressAdvanced || stageChanged) {
                    updateLoadingProgress(mappedPercent, stageLabel, progressAdvanced);
                } else {
                    if (this->loadingSpinnerLastTick == 0)
                        this->loadingSpinnerLastTick = now;
                    if (now - this->loadingSpinnerLastTick >= spinnerStepTicks) {
                        this->loadingSpinnerLastTick = now;
                        this->loadingSpinnerFrame = (this->loadingSpinnerFrame + 1) % 4;
                        this->refreshLoadingStagesText();
                        mainApp->CallForRender();
                    }
                }

                svcSleepThread(16'000'000ULL);
            }

            if (fetchThread.joinable())
                fetchThread.join();

            this->shopSections = std::move(fetchedSections);
            error = fetchError;
            usedLegacyFallback = fetchUsedLegacyFallback;
            if (error.empty() && !this->shopSections.empty()) {
                this->catalogCacheValid = true;
                this->catalogCacheKey = cacheKey;
                this->catalogCacheUsedLegacyFallback = usedLegacyFallback;
                this->catalogCacheSections = this->shopSections;
            }
        }
        updateLoadingProgress(90, "Parsing shop response...");
        this->saveSyncEnabled = !usedLegacyFallback && !inst::config::shopLegacyMode;
        ShopDlcTrace("FetchShopSections done sections=%llu errorLen=%llu", static_cast<unsigned long long>(this->shopSections.size()), static_cast<unsigned long long>(error.size()));
        ShopDlcTrace(
            "save sync eligibility legacyFallback=%d tinfoilMode=%d enabled=%d",
            usedLegacyFallback ? 1 : 0,
            inst::config::shopLegacyMode ? 1 : 0,
            this->saveSyncEnabled ? 1 : 0
        );
        if (!error.empty()) {
            ShopDlcTrace("FetchShopSections error: %s", error.c_str());
            mainApp->CreateShowDialog("inst.shop.failed"_lang, error, {"common.ok"_lang}, true);
            mainApp->LoadLayout(mainApp->mainPage);
            return;
        }
        if (this->shopSections.empty()) {
            ShopDlcTrace("FetchShopSections returned empty sections");
            mainApp->CreateShowDialog("inst.shop.empty"_lang, "", {"common.ok"_lang}, true);
            mainApp->LoadLayout(mainApp->mainPage);
            return;
        }
        updateLoadingProgress(91, "Scanning shop sections...");

        for (std::size_t i = 0; i < this->shopSections.size(); i++) {
            const auto& section = this->shopSections[i];
            if (section.id == "updates" || section.id == "update")
                this->nativeUpdatesSectionPresent = true;
            if (section.id == "dlc" || section.id == "addon" || section.id == "add-on" || section.id == "add_ons")
                this->nativeDlcSectionPresent = true;
            ShopDlcTrace("section-before id='%s' title='%s' items=%llu", section.id.c_str(), section.title.c_str(), static_cast<unsigned long long>(section.items.size()));

            if (!this->shopSections.empty()) {
                const int scanProgress = 91 + static_cast<int>(((i + 1) * 3ULL) / this->shopSections.size()); // 91..94
                updateLoadingProgress(scanProgress);
            }
        }
        ShopDlcTrace("native sections updates=%d dlc=%d", this->nativeUpdatesSectionPresent ? 1 : 0, this->nativeDlcSectionPresent ? 1 : 0);
        updateLoadingProgress(95, "Preparing sections...");

        if ((this->nativeUpdatesSectionPresent || this->nativeDlcSectionPresent) && !this->shopSections.empty()) {
            auto normalizeSectionId = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return value;
            };

            int allIndex = -1;
            int updatesIndex = -1;
            int dlcIndex = -1;
            for (std::size_t i = 0; i < this->shopSections.size(); i++) {
                const std::string id = normalizeSectionId(this->shopSections[i].id);
                if (id == "all")
                    allIndex = static_cast<int>(i);
                else if (id == "updates" || id == "update")
                    updatesIndex = static_cast<int>(i);
                else if (id == "dlc" || id == "addon" || id == "add-on" || id == "add_ons")
                    dlcIndex = static_cast<int>(i);
            }

            if (allIndex >= 0) {
                auto augmentSectionFromAll = [&](int targetIndex, bool (*predicate)(const shopInstStuff::ShopItem&)) {
                    if (targetIndex < 0 || targetIndex >= static_cast<int>(this->shopSections.size()))
                        return;
                    if (targetIndex == allIndex)
                        return;

                    auto& targetItems = this->shopSections[targetIndex].items;
                    const auto& allItems = this->shopSections[allIndex].items;
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
                ShopDlcTrace("augment skipped: no all section present");
                updateLoadingProgress(97, "Preparing sections...");
            }
        }

        updateLoadingProgress(98, "Preparing installed section...");
        this->ensureInstalledSectionPlaceholder();
        ShopDlcTrace("after ensureInstalledSectionPlaceholder sections=%llu", static_cast<unsigned long long>(this->shopSections.size()));
        updateLoadingProgress(99, "Preparing owned sections...");
        this->buildLegacyOwnedSections();
        ShopDlcTrace("after buildLegacyOwnedSections sections=%llu", static_cast<unsigned long long>(this->shopSections.size()));
        updateLoadingProgress(99, "Checking available updates...");
        this->cacheAvailableUpdates();
        ShopDlcTrace("after cacheAvailableUpdates availableUpdates=%llu", static_cast<unsigned long long>(this->availableUpdates.size()));
        updateLoadingProgress(99, "Filtering sections...");
        this->filterOwnedSections();
        updateLoadingProgress(99, "Sorting titles...");
        this->applyAllSectionSort();
        updateLoadingProgress(99, "Preparing save sync...");
        if (this->saveSyncEnabled) {
            const bool hasSaveSection = std::any_of(this->shopSections.begin(), this->shopSections.end(), [](const auto& section) {
                return section.id == "saves" || section.id == "save";
            });
            if (!hasSaveSection) {
                shopInstStuff::ShopSection saveSection;
                saveSection.id = "saves";
                saveSection.title = "Saves";
                this->shopSections.push_back(std::move(saveSection));
            }
        }
        updateLoadingProgress(99, "Finalizing shop...");
        this->setLoadingProgressStage("Shop ready");
        this->setLoadingProgress(100, true);
        mainApp->CallForRender();

        this->selectedSectionIndex = 0;
        for (size_t i = 0; i < this->shopSections.size(); i++) {
            if (this->shopSections[i].id == "new") {
                this->selectedSectionIndex = static_cast<int>(i);
                break;
            }
        }
        this->shopGridMode = inst::config::shopStartGridMode;
        this->shopGridIndex = 0;
        this->shopGridPage = -1;
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
        if (!this->shopGridMode) {
            this->menu->SetSelectedIndex(0);
            this->menu->SetVisible(true);
            this->updatePreview();
        }
        this->pendingMotdFetch = true;
    }

    void shopInstPage::refreshAfterInstall() {
        // Clear finished queue items
        inst::queue::InstallQueue::Instance().ClearFinished();

        if (this->shopSections.empty() || this->activeShopUrl.empty()) {
            this->startShop(true);
            return;
        }

        mainApp->LoadLayout(mainApp->shopinstPage);

        const bool wasGridMode = this->shopGridMode;
        const int previousSectionIndex = this->selectedSectionIndex;
        std::string previousSectionId;
        if (previousSectionIndex >= 0 && previousSectionIndex < static_cast<int>(this->shopSections.size()))
            previousSectionId = this->shopSections[static_cast<std::size_t>(previousSectionIndex)].id;

        this->installedSnapshot = {};
        this->ensureInstalledSectionPlaceholder();
        (void)this->ensureInstalledSectionBuilt();
        this->cacheAvailableUpdates();
        this->filterOwnedSections();
        this->applyAllSectionSort();

        int nextSectionIndex = 0;
        if (!previousSectionId.empty()) {
            for (std::size_t i = 0; i < this->shopSections.size(); i++) {
                if (this->shopSections[i].id == previousSectionId) {
                    nextSectionIndex = static_cast<int>(i);
                    break;
                }
            }
        } else if (!this->shopSections.empty()) {
            nextSectionIndex = std::clamp(previousSectionIndex, 0, static_cast<int>(this->shopSections.size()) - 1);
        }

        this->selectedSectionIndex = nextSectionIndex;
        this->shopGridMode = wasGridMode;
        this->shopGridIndex = 0;
        this->shopGridPage = -1;
        this->gridSelectedIndex = 0;
        this->gridPage = -1;
        this->selectedItems.clear();
        this->previewKey.clear();
        this->updateSectionText();
        this->updateButtonsText();
        this->drawMenuItems(false);
        this->infoImage->SetVisible(false);
        if (!this->shopGridMode) {
            this->menu->SetSelectedIndex(0);
            this->menu->SetVisible(true);
            this->updatePreview();
        }
        this->updateDescriptionPanel();
        mainApp->CallForRender();
    }

    void shopInstPage::startInstall() {
        std::vector<shopInstStuff::ShopItem> autoAddedItems;
        if (!this->selectedItems.empty()) {
            auto isAlreadySelected = [&](const shopInstStuff::ShopItem& candidate) {
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

            std::vector<shopInstStuff::ShopItem> updatesToAdd;
            std::unordered_map<std::uint64_t, shopInstStuff::ShopItem> latestUpdates;
            auto shouldReplaceUpdateCandidate = [](const shopInstStuff::ShopItem& current, const shopInstStuff::ShopItem& candidate) {
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
                int res = mainApp->CreateShowDialog("inst.shop.update_prompt_title"_lang,
                    "inst.shop.update_prompt_desc"_lang + std::to_string(updatesToAdd.size()),
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
                std::vector<shopInstStuff::ShopItem> dlcToAdd;
                std::unordered_map<std::string, bool> seenDlcKeys;

                for (const auto& section : this->shopSections) {
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
                    int res = mainApp->CreateShowDialog("inst.shop.dlc_prompt_title"_lang,
                        "inst.shop.dlc_prompt_desc"_lang + std::to_string(dlcToAdd.size()),
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
            dialogResult = mainApp->CreateShowDialog("inst.target.desc0"_lang + name + "inst.target.desc1"_lang, "common.cancel_desc"_lang, {"inst.target.opt0"_lang, "inst.target.opt1"_lang, "Queue (SD)", "Queue (NAND)"}, false);
        } else {
            dialogResult = mainApp->CreateShowDialog("inst.target.desc00"_lang + std::to_string(this->selectedItems.size()) + "inst.target.desc01"_lang, "common.cancel_desc"_lang, {"inst.target.opt0"_lang, "inst.target.opt1"_lang, "Queue (SD)", "Queue (NAND)"}, false);
        }
        if (dialogResult == -1) {
            if (!autoAddedItems.empty()) {
                auto matchesItem = [](const shopInstStuff::ShopItem& lhs, const shopInstStuff::ShopItem& rhs) {
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

                if (this->shopGridMode)
                    this->updateShopGrid();
                else
                    this->drawMenuItems(false);
            }
            return;
        }

        this->updateRememberedSelection();
        auto& queue = inst::queue::InstallQueue::Instance();

        if (dialogResult == 2 || dialogResult == 3) {
            // "Queue (SD)" = 2, "Queue (NAND)" = 3 - add to queue, stay in shop
            int queueStorage = (dialogResult == 3) ? 1 : 0;
            queue.Enqueue(this->selectedItems, queueStorage, "inst.shop.source_string"_lang);
            this->selectedItems.clear();
            if (this->shopGridMode)
                this->updateShopGrid();
            else
                this->drawMenuItems(false);
            return;
        }

        // dialogResult 0 = SD, 1 = NAND - start install immediately
        queue.Enqueue(this->selectedItems, dialogResult, "inst.shop.source_string"_lang);
        this->selectedItems.clear();

        // Pop and start the batch
        std::vector<shopInstStuff::ShopItem> batch;
        int batchStorage = 0;
        std::string batchSource;
        if (queue.PopNextBatch(batch, batchStorage, batchSource)) {
            shopInstStuff::installTitleShop(batch, batchStorage, batchSource);
            this->refreshAfterInstall();
            return;
        }

        // Fallback: refresh shop view
        if (this->shopGridMode)
            this->updateShopGrid();
        else
            this->drawMenuItems(false);
    }

    void shopInstPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
        // Update queue status indicator
        {
            auto& queue = inst::queue::InstallQueue::Instance();
            if (queue.ConsumeUiDirtyFlag() || queue.HasPending()) {
                std::string status = queue.GetStatusText();
                if (!status.empty()) {
                    if (queue.HasPending() && !queue.IsRunning())
                        status += "  [+] Start";
                    this->queueStatusText->SetText(status);
                    this->queueStatusText->SetX(1280 - 10 - this->queueStatusText->GetTextWidth());
                    this->queueStatusText->SetVisible(true);
                } else {
                    this->queueStatusText->SetVisible(false);
                }
            }
        }

        int bottomTapX = 0;
        if (DetectBottomHintTap(Pos, this->bottomHintTouch, 668, 52, bottomTapX)) {
            Down |= FindBottomHintButton(this->bottomHintSegments, bottomTapX);
        }
        inst::util::playNavigationClickIfNeeded(Down);
        if (this->descriptionOverlayVisible) {
            if (Down & (HidNpadButton_B | HidNpadButton_ZL)) {
                this->closeDescriptionOverlay();
                return;
            }

            int delta = 0;
            if (Down & (HidNpadButton_Up | HidNpadButton_StickLUp))
                delta = -1;
            else if (Down & (HidNpadButton_Down | HidNpadButton_StickLDown))
                delta = 1;
            else if (Down & HidNpadButton_Left)
                delta = -(this->descriptionOverlayVisibleLines / 2);
            else if (Down & HidNpadButton_Right)
                delta = (this->descriptionOverlayVisibleLines / 2);

            if (delta != 0) {
                this->scrollDescriptionOverlay(delta);
                return;
            }
            return;
        }
        if (this->handleSaveVersionSelectorInput(Down, Up, Held, Pos))
            return;
        if (this->pendingMotdFetch && Down == 0 && Up == 0 && Held == 0 && Pos.IsEmpty()) {
            this->pendingMotdFetch = false;
            std::string motd = shopInstStuff::FetchShopMotd(this->activeShopUrl, inst::config::shopUser, inst::config::shopPass);
            if (!motd.empty())
                mainApp->CreateShowDialog("inst.shop.motd_title"_lang, motd, {"common.ok"_lang}, true);
            return;
        }
        if (Down & HidNpadButton_B) {
            this->updateRememberedSelection();
            mainApp->LoadLayout(mainApp->mainPage);
        }
        if (Down & HidNpadButton_Minus) {
            this->shopGridMode = !this->shopGridMode;
            this->touchActive = false;
            this->touchMoved = false;
            if (this->shopGridMode) {
                this->listMarqueeMaskRect->SetVisible(false);
                this->listMarqueeTintRect->SetVisible(false);
                this->listMarqueeOverlayText->SetVisible(false);
                this->listMarqueeClipEnabled = false;
                this->listMarqueeFadeRect->SetVisible(false);
                this->shopGridIndex = this->menu->GetSelectedIndex();
                if (this->shopGridIndex < 0)
                    this->shopGridIndex = 0;
                this->shopGridPage = -1;
                this->gridPage = -1;
                if (this->isInstalledSection()) {
                    this->gridSelectedIndex = this->shopGridIndex;
                    this->updateInstalledGrid();
                } else {
                    this->updateShopGrid();
                }
            } else {
                // Ensure list mode visibility is fully applied even when we skip full list rebuild.
                for (auto& img : this->gridImages)
                    img->SetVisible(false);
                this->gridHighlight->SetVisible(false);
                this->gridTitleText->SetVisible(false);
                for (auto& highlight : this->shopGridSelectHighlights)
                    highlight->SetVisible(false);
                for (auto& icon : this->shopGridSelectIcons)
                    icon->SetVisible(false);
                this->menu->SetVisible(true);
                if (!this->menu->GetItems().empty()) {
                    int sel = this->shopGridIndex;
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
                this->updatePreview();
                this->updateListMarquee(true);
            }
            this->updateDescriptionPanel();
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
        if (this->shopGridMode) {
            if (Down & HidNpadButton_Plus) {
                if (!this->isInstalledSection() && !this->isSaveSyncSection() && !this->visibleItems.empty() && this->selectedItems.empty()) {
                    // If no items selected but queue has pending items, start the queue
                    auto& queue = inst::queue::InstallQueue::Instance();
                    if (queue.HasPending()) {
                        std::vector<shopInstStuff::ShopItem> batch;
                        int batchStorage = 0;
                        std::string batchSource;
                        if (queue.PopNextBatch(batch, batchStorage, batchSource)) {
                            shopInstStuff::installTitleShop(batch, batchStorage, batchSource);
                            this->refreshAfterInstall();
                            return;
                        }
                    } else {
                        this->selectTitle(this->shopGridIndex);
                    }
                }
                if (!this->isInstalledSection() && !this->isSaveSyncSection() && !this->selectedItems.empty())
                    this->startInstall();
            }
            if (Down & HidNpadButton_Y) {
                if (!this->isInstalledSection() && !this->isSaveSyncSection()) {
                    if (this->selectedItems.size() == this->visibleItems.size()) {
                        this->selectedItems.clear();
                        this->updateRememberedSelection();
                        if (this->menu->GetItems().size() == this->visibleItems.size())
                            this->refreshListSelectionIcons();
                        else
                            this->drawMenuItems(false);
                        this->updateShopGrid();
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
                        this->updateShopGrid();
                    }
                }
            }
            if (Down & HidNpadButton_X) {
                this->startShop(true);
            }
            if (Down & HidNpadButton_L) {
                if (this->shopSections.size() > 1) {
                    this->selectedSectionIndex = (this->selectedSectionIndex - 1 + (int)this->shopSections.size()) % (int)this->shopSections.size();
                    this->searchQuery.clear();
                    this->shopGridIndex = 0;
                    this->shopGridPage = -1;
                    this->gridSelectedIndex = 0;
                    this->gridPage = -1;
                    this->ensureSaveSyncSectionLoaded();
                    this->updateSectionText();
                    this->updateButtonsText();
                    this->drawMenuItems(false);
                }
            }
            if (Down & HidNpadButton_R) {
                if (this->shopSections.size() > 1) {
                    this->selectedSectionIndex = (this->selectedSectionIndex + 1) % (int)this->shopSections.size();
                    this->searchQuery.clear();
                    this->shopGridIndex = 0;
                    this->shopGridPage = -1;
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
                const bool stickUp = (Held & HidNpadButton_StickLUp) != 0;
                const bool stickDown = (Held & HidNpadButton_StickLDown) != 0;
                const bool stickLeft = (Held & HidNpadButton_StickLLeft) != 0;
                const bool stickRight = (Held & HidNpadButton_StickLRight) != 0;
                const bool heldUp = dpadUp || stickUp;
                const bool heldDown = dpadDown || stickDown;
                const bool heldLeft = dpadLeft || stickLeft;
                const bool heldRight = dpadRight || stickRight;
                if (!heldUp && !heldDown && !heldLeft && !heldRight) {
                    this->gridHoldDirX = 0;
                    this->gridHoldDirY = 0;
                    this->gridHoldStartTick = 0;
                    this->gridHoldLastTick = 0;
                } else {
                    int dirX = heldRight ? 1 : (heldLeft ? -1 : 0);
                    int dirY = heldDown ? 1 : (heldUp ? -1 : 0);
                    u64 now = armGetSystemTick();
                    if (this->gridHoldDirX != dirX || this->gridHoldDirY != dirY || this->gridHoldStartTick == 0) {
                        this->gridHoldDirX = dirX;
                        this->gridHoldDirY = dirY;
                        this->gridHoldStartTick = now;
                        this->gridHoldLastTick = now;
                        if (!dpadUp && !dpadDown && !dpadLeft && !dpadRight) {
                            if (dirY != 0)
                                dirKeys |= (dirY > 0) ? HidNpadButton_Down : HidNpadButton_Up;
                            else if (dirX != 0)
                                dirKeys |= (dirX > 0) ? HidNpadButton_Right : HidNpadButton_Left;
                        }
                    } else {
                        const u64 freq = armGetSystemTickFreq();
                        const u64 delayTicks = (freq * 200) / 1000;
                        const u64 repeatTicks = (freq * 90) / 1000;
                        if (now - this->gridHoldStartTick >= delayTicks && now - this->gridHoldLastTick >= repeatTicks) {
                            if (dirY != 0)
                                dirKeys |= (dirY > 0) ? HidNpadButton_Down : HidNpadButton_Up;
                            else if (dirX != 0)
                                dirKeys |= (dirX > 0) ? HidNpadButton_Right : HidNpadButton_Left;
                            this->gridHoldLastTick = now;
                        }
                    }
                }
            }
            if ((Down & HidNpadButton_A) || (Up & TouchPseudoKey)) {
                if (!this->visibleItems.empty()) {
                    if (this->shopGridIndex < 0)
                        this->shopGridIndex = 0;
                    if (this->shopGridIndex >= (int)this->visibleItems.size())
                        this->shopGridIndex = (int)this->visibleItems.size() - 1;
                    if (this->isInstalledSection()) {
                        this->gridSelectedIndex = this->shopGridIndex;
                        this->showInstalledDetails();
                    } else if (this->isSaveSyncSection()) {
                        this->handleSaveSyncAction(this->shopGridIndex);
                    } else {
                        this->selectTitle(this->shopGridIndex);
                        if (this->visibleItems.size() == 1 && this->selectedItems.size() == 1) {
                            this->startInstall();
                        }
                    }
                }
            }
            if (!this->visibleItems.empty()) {
                int newIndex = this->shopGridIndex;
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

                if (newIndex != this->shopGridIndex) {
                    const bool repeatedMove = ((dirKeys & navDownMask) != 0) && ((Down & navDownMask) == 0);
                    this->shopGridIndex = newIndex;
                    if (repeatedMove)
                        inst::util::playNavigationClick();
                    if (this->isInstalledSection()) {
                        this->gridSelectedIndex = this->shopGridIndex;
                        this->updateInstalledGrid();
                    } else {
                        this->updateShopGrid();
                    }
                }
            }
            if (!Pos.IsEmpty()) {
                const int gridW = kGridCols * kGridTileWidth + (kGridCols - 1) * kGridGap;
                const int gridH = kGridRows * kGridTileHeight + (kGridRows - 1) * kGridGap;
                const bool inGrid = (Pos.X >= kGridStartX) && (Pos.X <= (kGridStartX + gridW)) && (Pos.Y >= kGridStartY) && (Pos.Y <= (kGridStartY + gridH));
                if (inGrid) {
                    const int relX = Pos.X - kGridStartX;
                    const int relY = Pos.Y - kGridStartY;
                    const int col = relX / (kGridTileWidth + kGridGap);
                    const int row = relY / (kGridTileHeight + kGridGap);
                    const int tileX = relX - (col * (kGridTileWidth + kGridGap));
                    const int tileY = relY - (row * (kGridTileHeight + kGridGap));
                    if (col >= 0 && col < kGridCols && row >= 0 && row < kGridRows && tileX <= kGridTileWidth && tileY <= kGridTileHeight) {
                        int page = 0;
                        if (this->shopGridIndex > 0)
                            page = this->shopGridIndex / kGridItemsPerPage;
                        int pageStart = page * kGridItemsPerPage;
                        int index = pageStart + (row * kGridCols) + col;
                        if (index >= 0 && index < (int)this->visibleItems.size() && index != this->shopGridIndex) {
                            this->shopGridIndex = index;
                            if (this->isInstalledSection()) {
                                this->gridSelectedIndex = this->shopGridIndex;
                                this->updateInstalledGrid();
                            } else {
                                this->updateShopGrid();
                            }
                        }
                        this->touchActive = true;
                        this->touchMoved = false;
                    }
                }
            } else if (this->touchActive) {
                if (!this->visibleItems.empty()) {
                    if (this->isInstalledSection()) {
                        this->gridSelectedIndex = this->shopGridIndex;
                        this->showInstalledDetails();
                    } else if (this->isSaveSyncSection()) {
                        this->handleSaveSyncAction(this->shopGridIndex);
                    } else {
                        this->selectTitle(this->shopGridIndex);
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
            } else {
                this->selectTitle(this->menu->GetSelectedIndex());
                if (this->menu->GetItems().size() == 1 && this->selectedItems.size() == 1) {
                    this->startInstall();
                }
            }
        }
        if (Down & HidNpadButton_L) {
            if (this->shopSections.size() > 1) {
                this->selectedSectionIndex = (this->selectedSectionIndex - 1 + (int)this->shopSections.size()) % (int)this->shopSections.size();
                this->searchQuery.clear();
                this->gridSelectedIndex = 0;
                this->gridPage = -1;
                this->shopGridIndex = 0;
                this->shopGridPage = -1;
                this->ensureSaveSyncSectionLoaded();
                this->updateSectionText();
                this->updateButtonsText();
                this->drawMenuItems(false);
            }
        }
        if (Down & HidNpadButton_R) {
            if (this->shopSections.size() > 1) {
                this->selectedSectionIndex = (this->selectedSectionIndex + 1) % (int)this->shopSections.size();
                this->searchQuery.clear();
                this->gridSelectedIndex = 0;
                this->gridPage = -1;
                this->shopGridIndex = 0;
                this->shopGridPage = -1;
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
            if (!this->isInstalledSection() && !this->isSaveSyncSection()) {
                if (this->selectedItems.size() == this->menu->GetItems().size()) {
                    this->selectedItems.clear();
                    this->updateRememberedSelection();
                    this->drawMenuItems(false);
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
                    this->drawMenuItems(false);
                }
            }
        }
        if (Down & HidNpadButton_X) {
            this->startShop(true);
        }
        if (Down & HidNpadButton_Plus) {
            if (!this->isInstalledSection() && !this->isSaveSyncSection()) {
                if (this->selectedItems.empty()) {
                    // If no items selected but queue has pending items, start the queue
                    auto& queue = inst::queue::InstallQueue::Instance();
                    if (queue.HasPending()) {
                        std::vector<shopInstStuff::ShopItem> batch;
                        int batchStorage = 0;
                        std::string batchSource;
                        if (queue.PopNextBatch(batch, batchStorage, batchSource)) {
                            shopInstStuff::installTitleShop(batch, batchStorage, batchSource);
                            this->refreshAfterInstall();
                            return;
                        }
                    } else {
                        this->selectTitle(this->menu->GetSelectedIndex());
                    }
                }
                if (!this->selectedItems.empty()) this->startInstall();
            }
        }
        if (!this->shopGridMode && !this->menu->GetItems().empty()) {
            const u64 holdMask = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_StickLUp | HidNpadButton_StickLDown;
            const bool heldUp = (Held & (HidNpadButton_Up | HidNpadButton_StickLUp)) != 0;
            const bool heldDown = (Held & (HidNpadButton_Down | HidNpadButton_StickLDown)) != 0;
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
                if ((int)this->menu->GetItems().size() > this->menu->GetNumberOfItemsToShow()) {
                    const u64 freq = armGetSystemTickFreq();
                    const u64 delayTicks = (freq * 300) / 1000;
                    const u64 repeatTicks = (freq * 70) / 1000;
                    if (now - this->holdStartTick >= delayTicks && now - this->lastHoldTick >= repeatTicks) {
                        int currentIndex = this->menu->GetSelectedIndex();
                        int maxIndex = static_cast<int>(this->menu->GetItems().size()) - 1;
                        bool moved = false;
                        if (direction > 0) {
                            if (currentIndex < maxIndex) {
                                this->menu->OnInput(HidNpadButton_AnyDown, 0, 0, pu::ui::Touch::Empty);
                                moved = true;
                            }
                        } else {
                            if (currentIndex > 0) {
                                this->menu->OnInput(HidNpadButton_AnyUp, 0, 0, pu::ui::Touch::Empty);
                                moved = true;
                            }
                        }
                        if (moved)
                            inst::util::playNavigationClick();
                        this->lastHoldTick = now;
                    }
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
        if (this->shopGridMode && this->isInstalledSection() && !this->visibleItems.empty()) {
            if (!Pos.IsEmpty()) {
                const int gridW = kGridCols * kGridTileWidth + (kGridCols - 1) * kGridGap;
                const int gridH = kGridRows * kGridTileHeight + (kGridRows - 1) * kGridGap;
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
                    if (col >= 0 && col < kGridCols && row >= 0 && row < kGridRows && tileX <= kGridTileWidth && tileY <= kGridTileHeight) {
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
        if (this->shopGridMode) {
            if (this->isInstalledSection()) {
                this->gridSelectedIndex = this->shopGridIndex;
                this->updateInstalledGrid();
            } else {
                this->updateShopGrid();
            }
        } else {
            this->updatePreview();
            this->updateListMarquee(false);
        }
        this->updateDescriptionPanel();
        this->updateDebug();
    }

    void shopInstPage::showInstalledDetails() {
        if (!this->isInstalledSection())
            return;
        int selectedIndex = this->shopGridMode ? this->gridSelectedIndex : this->menu->GetSelectedIndex();
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
        body += "inst.shop.detail_type"_lang + std::string(typeLabel) + "\n";
        body += "inst.shop.detail_titleid"_lang + std::string(titleIdBuf) + "\n";
        if (item.hasAppVersion)
            body += "inst.shop.detail_version"_lang + std::to_string(item.appVersion);
        else
            body += "inst.shop.detail_version"_lang + "0";

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

    bool shopInstPage::tryGetCurrentDescription(std::string& outTitle, std::string& outDescription) const {
        if (this->visibleItems.empty())
            return false;

        int selectedIndex = this->shopGridMode ? this->shopGridIndex : this->menu->GetSelectedIndex();
        if (this->isInstalledSection() && this->shopGridMode)
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

    void shopInstPage::showCurrentDescriptionDialog() {
        if (this->descriptionOverlayVisible) {
            this->closeDescriptionOverlay();
            return;
        }
        this->openDescriptionOverlay();
    }

    void shopInstPage::openDescriptionOverlay() {
        std::string title;
        std::string description;
        if (!this->tryGetCurrentDescription(title, description))
            return;

        this->descriptionOverlayLines = WrapDescriptionLines(description, 102);
        if (this->descriptionOverlayLines.empty())
            this->descriptionOverlayLines.push_back("No description available for this title.");
        this->descriptionOverlayOffset = 0;
        this->descriptionOverlayVisible = true;
        this->descriptionOverlayTitleText->SetText(title);
        this->descriptionOverlayRect->SetVisible(true);
        this->descriptionOverlayTitleText->SetVisible(true);
        this->descriptionOverlayBodyText->SetVisible(true);
        this->descriptionOverlayHintText->SetVisible(true);
        const std::string overlayButtonsText = "B Close    Up/Down Scroll";
        this->butText->SetText(overlayButtonsText);
        this->bottomHintSegments = BuildBottomHintSegments(overlayButtonsText, 10, 20);
        this->refreshDescriptionOverlayBody();
    }

    void shopInstPage::closeDescriptionOverlay() {
        this->descriptionOverlayVisible = false;
        this->descriptionOverlayLines.clear();
        this->descriptionOverlayOffset = 0;
        this->descriptionOverlayRect->SetVisible(false);
        this->descriptionOverlayTitleText->SetVisible(false);
        this->descriptionOverlayBodyText->SetVisible(false);
        this->descriptionOverlayHintText->SetVisible(false);
        this->updateButtonsText();
    }

    void shopInstPage::scrollDescriptionOverlay(int delta) {
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

    void shopInstPage::refreshDescriptionOverlayBody() {
        if (!this->descriptionOverlayVisible)
            return;

        const int lineCount = static_cast<int>(this->descriptionOverlayLines.size());
        if (lineCount <= 0) {
            this->descriptionOverlayBodyText->SetText("");
            this->descriptionOverlayHintText->SetText("B Close");
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

        if (lineCount > this->descriptionOverlayVisibleLines) {
            const int shownStart = start + 1;
            const int shownEnd = end;
            this->descriptionOverlayHintText->SetText(
                "B Close    Up/Down Scroll    " + std::to_string(shownStart) + "-" + std::to_string(shownEnd) + "/" + std::to_string(lineCount));
        } else {
            this->descriptionOverlayHintText->SetText("B Close");
        }
    }

    void shopInstPage::updateDescriptionPanel() {
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

        int selectedIndex = this->shopGridMode ? this->shopGridIndex : this->menu->GetSelectedIndex();
        if (this->isInstalledSection() && this->shopGridMode)
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

    void shopInstPage::setButtonsText(const std::string& text) {
        if (this->suppressBottomHints) {
            this->butText->SetText("");
            this->bottomHintSegments.clear();
            return;
        }
        std::string fullText = text;
        int hintFontSize = 18;
        this->butText->SetFontSize(hintFontSize);
        const std::string descHint = "     Show Desc";
        auto segments = BuildBottomHintSegments(fullText + descHint, 10, 20);
        if (!segments.empty()) {
            const auto& last = segments.back();
            if ((last.x + last.width) <= 1270)
                fullText += descHint;
        }
        this->butText->SetText(fullText);
        constexpr int kHintMaxWidth = 1260;
        if (this->butText->GetTextWidth() > kHintMaxWidth) {
            hintFontSize = 16;
            this->butText->SetFontSize(hintFontSize);
            this->butText->SetText(fullText);
            if (this->butText->GetTextWidth() > kHintMaxWidth) {
                hintFontSize = 14;
                this->butText->SetFontSize(hintFontSize);
                this->butText->SetText(fullText);
                if (this->butText->GetTextWidth() > kHintMaxWidth) {
                    hintFontSize = 12;
                    this->butText->SetFontSize(hintFontSize);
                    this->butText->SetText(fullText);
                    if (this->butText->GetTextWidth() > kHintMaxWidth) {
                        hintFontSize = 10;
                        this->butText->SetFontSize(hintFontSize);
                        this->butText->SetText(fullText);
                    }
                }
            }
        }
        this->bottomHintSegments = BuildBottomHintSegments(fullText, 10, hintFontSize);
    }
}


