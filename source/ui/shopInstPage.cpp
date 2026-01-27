#include <algorithm>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <cctype>
#include <cstdlib>
#include <switch.h>
#include "ui/MainApplication.hpp"
#include "ui/shopInstPage.hpp"
#include "util/config.hpp"
#include "util/curl.hpp"
#include "util/lang.hpp"
#include "util/title_util.hpp"
#include "util/util.hpp"

#define COLOR(hex) pu::ui::Color::FromHex(hex)

namespace {
    constexpr int kGridCols = 8;
    constexpr int kGridRows = 3;
    constexpr int kGridTileWidth = 140;
    constexpr int kGridTileHeight = 140;
    constexpr int kGridGap = 6;
    constexpr int kGridWidth = (kGridCols * kGridTileWidth) + ((kGridCols - 1) * kGridGap);
    constexpr int kGridStartX = (1280 - kGridWidth) / 2;
    constexpr int kGridStartY = 170;
    constexpr int kGridItemsPerPage = kGridCols * kGridRows;

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

    bool DeriveBaseTitleId(const shopInstStuff::ShopItem& item, std::uint64_t& out)
    {
        if (item.hasTitleId) {
            out = item.titleId;
            return true;
        }
        if (!item.hasAppId)
            return false;
        std::string appId = NormalizeHex(item.appId);
        if (appId.size() < 16)
            return false;
        std::string baseId;
        if (item.appType == NcmContentMetaType_Patch) {
            baseId = appId.substr(0, appId.size() - 3) + "000";
        } else if (item.appType == NcmContentMetaType_AddOnContent) {
            std::string basePart = appId.substr(0, appId.size() - 3);
            if (basePart.empty())
                return false;
            char* end = nullptr;
            unsigned long long baseValue = std::strtoull(basePart.c_str(), &end, 16);
            if (end == basePart.c_str() || (end && *end != '\0') || baseValue == 0)
                return false;
            baseValue -= 1;
            char buf[17] = {0};
            std::snprintf(buf, sizeof(buf), "%0*llx", (int)basePart.size(), baseValue);
            baseId = std::string(buf) + "000";
        } else {
            baseId = appId;
        }
        return TryParseHexU64(baseId, out);
    }

    bool IsBaseItem(const shopInstStuff::ShopItem& item)
    {
        if (item.appType == NcmContentMetaType_Application)
            return true;
        if (item.hasAppId) {
            std::string appId = NormalizeHex(item.appId);
            return appId.size() >= 3 && appId.rfind("000") == appId.size() - 3;
        }
        if (item.hasTitleId) {
            return (item.titleId & 0xFFF) == 0;
        }
        return false;
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
        this->topRect = Rectangle::New(0, 0, 1280, 94, topColor);
        this->infoRect = Rectangle::New(0, 95, 1280, 60, infoColor);
        this->botRect = Rectangle::New(0, 660, 1280, 60, botColor);
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
        this->butText = TextBlock::New(10, 678, "", 24);
        this->butText->SetColor(COLOR("#FFFFFFFF"));
        this->menu = pu::ui::elm::Menu::New(0, 156, 1280, COLOR("#FFFFFF00"), 84, (506 / 84));
        if (inst::config::oledMode) {
            this->menu->SetOnFocusColor(COLOR("#FFFFFF33"));
            this->menu->SetScrollbarColor(COLOR("#FFFFFF66"));
        } else {
            this->menu->SetOnFocusColor(COLOR("#00000033"));
            this->menu->SetScrollbarColor(COLOR("#17090980"));
        }
        this->infoImage = Image::New(453, 292, "romfs:/images/icons/lan-connection-waiting.png");
        this->previewImage = Image::New(900, 230, "romfs:/images/awoos/7d8a05cddfef6da4901b20d2698d5a71.png");
        this->previewImage->SetWidth(320);
        this->previewImage->SetHeight(320);
        auto highlightColor = inst::config::oledMode ? COLOR("#FFFFFF66") : COLOR("#FFFFFF33");
        this->gridHighlight = Rectangle::New(0, 0, kGridTileWidth + 8, kGridTileHeight + 8, highlightColor);
        this->gridHighlight->SetVisible(false);
        this->gridImages.reserve(kGridItemsPerPage);
        for (int i = 0; i < kGridItemsPerPage; i++) {
            auto img = Image::New(0, 0, "romfs:/images/awoos/7d8a05cddfef6da4901b20d2698d5a71.png");
            img->SetWidth(kGridTileWidth);
            img->SetHeight(kGridTileHeight);
            img->SetVisible(false);
            this->gridImages.push_back(img);
        }
        this->gridTitleText = TextBlock::New(10, 634, "", 24);
        this->gridTitleText->SetColor(COLOR("#FFFFFFFF"));
        this->gridTitleText->SetVisible(false);
        this->debugText = TextBlock::New(10, 620, "", 18);
        this->debugText->SetColor(COLOR("#FFFFFFFF"));
        this->debugText->SetVisible(false);
        this->Add(this->topRect);
        this->Add(this->infoRect);
        this->Add(this->botRect);
        this->Add(this->titleImage);
        this->Add(this->appVersionText);
        this->Add(this->butText);
        this->Add(this->pageInfoText);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        this->Add(this->menu);
#pragma GCC diagnostic pop
        this->Add(this->infoImage);
        this->Add(this->previewImage);
        for (auto& img : this->gridImages)
            this->Add(img);
        this->Add(this->gridHighlight);
        this->Add(this->gridTitleText);
        this->Add(this->debugText);
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

    const std::vector<shopInstStuff::ShopItem>& shopInstPage::getCurrentItems() const {
        static const std::vector<shopInstStuff::ShopItem> empty;
        if (this->shopSections.empty())
            return empty;
        if (this->selectedSectionIndex < 0 || this->selectedSectionIndex >= (int)this->shopSections.size())
            return empty;
        return this->shopSections[this->selectedSectionIndex].items;
    }

    void shopInstPage::updateSectionText() {
        if (this->shopSections.empty()) {
            this->pageInfoText->SetText("inst.shop.top_info"_lang);
            return;
        }
        const auto& section = this->shopSections[this->selectedSectionIndex];
        std::string label = "inst.shop.top_info"_lang + " " + section.title;
        if (this->isAllSection() && !this->searchQuery.empty()) {
            label += " (" + this->searchQuery + ")";
        }
        this->pageInfoText->SetText(label);
    }

    void shopInstPage::updateButtonsText() {
        if (this->isInstalledSection())
            this->butText->SetText("inst.shop.buttons_installed"_lang);
        else if (this->isAllSection())
            this->butText->SetText("inst.shop.buttons_all"_lang);
        else
            this->butText->SetText("inst.shop.buttons"_lang);
    }

    void shopInstPage::buildInstalledSection() {
        std::vector<shopInstStuff::ShopItem> installedItems;
        Result rc = nsInitialize();
        if (R_FAILED(rc))
            return;
        rc = ncmInitialize();
        if (R_FAILED(rc)) {
            nsExit();
            return;
        }

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
                if (!IsBaseTitleCurrentlyInstalled(baseId))
                    continue;
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
                    std::vector<NsApplicationContentMetaStatus> list(metaCount);
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
            offset += outCount;
        }

        nsExit();

        if (installedItems.empty())
            return;

        std::sort(installedItems.begin(), installedItems.end(), [](const auto& a, const auto& b) {
            return inst::util::ignoreCaseCompare(a.name, b.name);
        });

        shopInstStuff::ShopSection installedSection;
        installedSection.id = "installed";
        installedSection.title = "Installed";
        installedSection.items = std::move(installedItems);
        this->shopSections.insert(this->shopSections.begin(), std::move(installedSection));
    }

    void shopInstPage::cacheAvailableUpdates() {
        this->availableUpdates.clear();
        for (const auto& section : this->shopSections) {
            if (section.id == "updates") {
                this->availableUpdates = section.items;
                break;
            }
        }
    }

    void shopInstPage::filterOwnedSections() {
        if (this->shopSections.empty())
            return;

        Result rc = nsInitialize();
        if (R_FAILED(rc))
            return;

        std::unordered_map<std::uint64_t, std::uint32_t> installedUpdateVersion;
        std::unordered_map<std::uint64_t, bool> baseInstalled;

        const s32 chunk = 64;
        s32 offset = 0;
        while (true) {
            NsApplicationRecord records[chunk];
            s32 outCount = 0;
            if (R_FAILED(nsListApplicationRecord(records, chunk, offset, &outCount)) || outCount <= 0)
                break;
            for (s32 i = 0; i < outCount; i++) {
                const auto titleId = records[i].application_id;
                baseInstalled[titleId] = IsBaseTitleCurrentlyInstalled(titleId);
            }
            offset += outCount;
        }

        auto isBaseInstalled = [&](const shopInstStuff::ShopItem& item, std::uint32_t& outVersion) {
            std::uint64_t baseTitleId = 0;
            if (!DeriveBaseTitleId(item, baseTitleId))
                return false;
            auto baseIt = baseInstalled.find(baseTitleId);
            if (baseIt != baseInstalled.end()) {
                if (baseIt->second) {
                    auto verIt = installedUpdateVersion.find(baseTitleId);
                    if (verIt != installedUpdateVersion.end()) {
                        outVersion = verIt->second;
                    } else {
                        tin::util::GetInstalledUpdateVersion(baseTitleId, outVersion);
                        if (outVersion == 0)
                            TryGetInstalledUpdateVersionNcm(baseTitleId, outVersion);
                        installedUpdateVersion[baseTitleId] = outVersion;
                    }
                }
                return baseIt->second;
            }
            bool installed = IsBaseTitleCurrentlyInstalled(baseTitleId);
            if (installed) {
                tin::util::GetInstalledUpdateVersion(baseTitleId, outVersion);
                if (outVersion == 0)
                    TryGetInstalledUpdateVersionNcm(baseTitleId, outVersion);
            }
            baseInstalled[baseTitleId] = installed;
            installedUpdateVersion[baseTitleId] = outVersion;
            return installed;
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
                if (!isBaseInstalled(item, installedVersion))
                    continue;
                if (section.id == "updates" || item.appType == NcmContentMetaType_Patch) {
                    if (!item.hasAppVersion)
                        continue;
                    if (item.appVersion > installedVersion)
                        filtered.push_back(item);
                } else {
                    if (item.hasTitleId && tin::util::IsTitleInstalled(item.titleId))
                        continue;
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
                if (item.appType != NcmContentMetaType_AddOnContent) {
                    filtered.push_back(item);
                    continue;
                }
                std::uint32_t installedVersion = 0;
                if (item.hasTitleId && tin::util::IsTitleInstalled(item.titleId))
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
                if (section.id == "all" || section.id == "installed" || section.id == "updates")
                    continue;

                std::vector<shopInstStuff::ShopItem> filtered;
                filtered.reserve(section.items.size());
                for (const auto& item : section.items) {
                    std::uint32_t installedVersion = 0;
                    if (!IsBaseItem(item) || !item.hasTitleId || !isBaseInstalled(item, installedVersion)) {
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

        ncmExit();
        nsExit();
    }

    void shopInstPage::updatePreview() {
        if (this->isInstalledSection()) {
            this->previewImage->SetVisible(false);
            this->previewKey.clear();
            return;
        }
        if (this->visibleItems.empty()) {
            this->previewImage->SetVisible(false);
            this->previewKey.clear();
            return;
        }

        int selectedIndex = this->menu->GetSelectedIndex();
        if (selectedIndex < 0 || selectedIndex >= (int)this->visibleItems.size())
            return;
        const auto& item = this->visibleItems[selectedIndex];

        std::string key;
        if (item.url.empty()) {
            key = "installed:" + std::to_string(item.titleId);
        } else if (item.hasIconUrl) {
            key = item.iconUrl;
        } else {
            key = item.url;
        }

        if (key == this->previewKey)
            return;
        this->previewKey = key;

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
            this->previewImage->SetImage("romfs:/images/awoos/7d8a05cddfef6da4901b20d2698d5a71.png");
            applyPreviewLayout();
            this->previewImage->SetVisible(true);
            return;
        }

        if (item.hasIconUrl) {
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
            std::string filePath = cacheDir + "/" + fileName + ext;

            if (!std::filesystem::exists(filePath)) {
                bool ok = inst::curl::downloadImageWithAuth(item.iconUrl, filePath.c_str(), inst::config::shopUser, inst::config::shopPass, 8000);
                if (!ok) {
                    if (std::filesystem::exists(filePath))
                        std::filesystem::remove(filePath);
                }
            }

            if (std::filesystem::exists(filePath)) {
                this->previewImage->SetImage(filePath);
                applyPreviewLayout();
                this->previewImage->SetVisible(true);
                return;
            }
        }

        this->previewImage->SetImage("romfs:/images/awoos/7d8a05cddfef6da4901b20d2698d5a71.png");
        applyPreviewLayout();
        this->previewImage->SetVisible(true);
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

        int selectedIndex = this->isInstalledSection() ? this->gridSelectedIndex : this->menu->GetSelectedIndex();
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
        if (clearItems) this->selectedItems.clear();
        this->menu->ClearItems();
        this->visibleItems.clear();
        const auto& items = this->getCurrentItems();
        if (this->isAllSection() && !this->searchQuery.empty()) {
            for (const auto& item : items) {
                std::string name = item.name;
                std::string query = this->searchQuery;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                std::transform(query.begin(), query.end(), query.begin(), ::tolower);
                if (name.find(query) != std::string::npos)
                    this->visibleItems.push_back(item);
            }
        } else {
            this->visibleItems = items;
        }

        if (this->isInstalledSection()) {
            this->menu->SetVisible(false);
            this->previewImage->SetVisible(false);
            if (this->gridSelectedIndex >= (int)this->visibleItems.size())
                this->gridSelectedIndex = 0;
            this->updateInstalledGrid();
            return;
        }

        for (auto& img : this->gridImages)
            img->SetVisible(false);
        this->gridHighlight->SetVisible(false);
        this->menu->SetVisible(true);

        for (const auto& item : this->visibleItems) {
            std::string itm = inst::util::shortenString(item.name, 56, true);
            auto entry = pu::ui::elm::MenuItem::New(itm);
            entry->SetColor(COLOR("#FFFFFFFF"));
            entry->SetIcon("romfs:/images/icons/checkbox-blank-outline.png");
            for (const auto& selected : this->selectedItems) {
                if (selected.url == item.url) {
                    entry->SetIcon("romfs:/images/icons/check-box-outline.png");
                    break;
                }
            }
            this->menu->AddItem(entry);
        }

        if (!this->menu->GetItems().empty()) {
            int sel = this->menu->GetSelectedIndex();
            if (sel < 0 || sel >= (int)this->menu->GetItems().size())
                this->menu->SetSelectedIndex(0);
        }
    }

    void shopInstPage::updateInstalledGrid() {
        if (!this->isInstalledSection()) {
            for (auto& img : this->gridImages)
                img->SetVisible(false);
            this->gridHighlight->SetVisible(false);
            this->gridTitleText->SetVisible(false);
            this->gridPage = -1;
            return;
        }

        if (this->visibleItems.empty()) {
            for (auto& img : this->gridImages)
                img->SetVisible(false);
            this->gridHighlight->SetVisible(false);
            this->gridTitleText->SetVisible(false);
            this->gridPage = -1;
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

                if (!applied) {
                    this->gridImages[i]->SetImage("romfs:/images/awoos/7d8a05cddfef6da4901b20d2698d5a71.png");
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
            std::string title = inst::util::shortenString(this->visibleItems[this->gridSelectedIndex].name, 70, true);
            this->gridTitleText->SetText(title);
            this->gridTitleText->SetVisible(true);
        } else {
            this->gridTitleText->SetVisible(false);
        }
    }

    void shopInstPage::selectTitle(int selectedIndex) {
        if (selectedIndex < 0 || selectedIndex >= (int)this->visibleItems.size())
            return;
        const auto& item = this->visibleItems[selectedIndex];
        if (item.url.empty())
            return;
        auto selected = std::find_if(this->selectedItems.begin(), this->selectedItems.end(), [&](const auto& entry) {
            return entry.url == item.url;
        });
        if (selected != this->selectedItems.end())
            this->selectedItems.erase(selected);
        else
            this->selectedItems.push_back(item);
        this->updateRememberedSelection();
        this->drawMenuItems(false);
    }

    void shopInstPage::updateRememberedSelection() {
    }

    void shopInstPage::startShop(bool forceRefresh) {
        this->butText->SetText("inst.shop.buttons_loading"_lang);
        this->menu->SetVisible(false);
        this->menu->ClearItems();
        this->infoImage->SetVisible(true);
        this->previewImage->SetVisible(false);
        this->pageInfoText->SetText("inst.shop.loading"_lang);
        mainApp->LoadLayout(mainApp->shopinstPage);
        mainApp->CallForRender();

        std::string shopUrl = inst::config::shopUrl;
        if (shopUrl.empty()) {
            shopUrl = inst::util::softwareKeyboard("options.shop.url_hint"_lang, "http://", 200);
            if (shopUrl.empty()) {
                mainApp->LoadLayout(mainApp->mainPage);
                return;
            }
            inst::config::shopUrl = shopUrl;
            inst::config::setConfig();
        }

        std::string error;
        this->shopSections = shopInstStuff::FetchShopSections(shopUrl, inst::config::shopUser, inst::config::shopPass, error, !forceRefresh);
        if (!error.empty()) {
            mainApp->CreateShowDialog("inst.shop.failed"_lang, error, {"common.ok"_lang}, true);
            mainApp->LoadLayout(mainApp->mainPage);
            return;
        }
        if (this->shopSections.empty()) {
            mainApp->CreateShowDialog("inst.shop.empty"_lang, "", {"common.ok"_lang}, true);
            mainApp->LoadLayout(mainApp->mainPage);
            return;
        }

        std::string motd = shopInstStuff::FetchShopMotd(shopUrl, inst::config::shopUser, inst::config::shopPass);
        if (!motd.empty())
            mainApp->CreateShowDialog("inst.shop.motd_title"_lang, motd, {"common.ok"_lang}, true);

        if (!inst::config::shopHideInstalledSection)
            this->buildInstalledSection();
        this->cacheAvailableUpdates();
        this->filterOwnedSections();

        this->selectedSectionIndex = 0;
        for (size_t i = 0; i < this->shopSections.size(); i++) {
            if (this->shopSections[i].id == "recommended") {
                this->selectedSectionIndex = static_cast<int>(i);
                break;
            }
        }
        this->gridSelectedIndex = 0;
        this->gridPage = -1;
        this->updateSectionText();
        this->updateButtonsText();
        this->selectedItems.clear();
        this->drawMenuItems(false);
        this->menu->SetSelectedIndex(0);
        this->infoImage->SetVisible(false);
        this->menu->SetVisible(true);
        this->updatePreview();
    }

    void shopInstPage::startInstall() {
        if (!this->selectedItems.empty()) {
            std::vector<shopInstStuff::ShopItem> updatesToAdd;
            std::unordered_map<std::uint64_t, shopInstStuff::ShopItem> latestUpdates;
            for (const auto& update : this->availableUpdates) {
                if (update.appType != NcmContentMetaType_Patch || !update.hasAppVersion)
                    continue;
                std::uint64_t baseTitleId = 0;
                if (!DeriveBaseTitleId(update, baseTitleId))
                    continue;
                auto it = latestUpdates.find(baseTitleId);
                if (it == latestUpdates.end() || update.appVersion > it->second.appVersion)
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
                bool alreadySelected = std::any_of(this->selectedItems.begin(), this->selectedItems.end(), [&](const auto& entry) {
                    return entry.url == updateIt->second.url;
                });
                if (!alreadySelected && !updateIt->second.url.empty())
                    updatesToAdd.push_back(updateIt->second);
            }

            if (!updatesToAdd.empty()) {
                int res = mainApp->CreateShowDialog("inst.shop.update_prompt_title"_lang,
                    "inst.shop.update_prompt_desc"_lang + std::to_string(updatesToAdd.size()),
                    {"common.yes"_lang, "common.no"_lang}, false);
                if (res == 0) {
                    for (const auto& update : updatesToAdd)
                        this->selectedItems.push_back(update);
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
        if (dialogResult == -1)
            return;

        this->updateRememberedSelection();
        shopInstStuff::installTitleShop(this->selectedItems, dialogResult, "inst.shop.source_string"_lang);
    }

    void shopInstPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) {
        if (Down & HidNpadButton_B) {
            this->updateRememberedSelection();
            mainApp->LoadLayout(mainApp->mainPage);
        }
        if ((Down & HidNpadButton_A) || (Up & TouchPseudoKey)) {
            if (this->isInstalledSection()) {
                this->showInstalledDetails();
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
                this->updateSectionText();
                this->updateButtonsText();
                this->drawMenuItems(false);
            }
        }
        if (Down & HidNpadButton_ZR) {
            if (this->isAllSection()) {
                std::string query = inst::util::softwareKeyboard("inst.shop.search_hint"_lang, this->searchQuery, 60);
                this->searchQuery = query;
                this->updateSectionText();
                this->drawMenuItems(false);
            }
        }
        if (this->isInstalledSection() && !this->visibleItems.empty()) {
            int newIndex = this->gridSelectedIndex;
            u64 dirKeys = Down & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right);
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

            if (newIndex != this->gridSelectedIndex) {
                this->gridSelectedIndex = newIndex;
                this->updateInstalledGrid();
            }
        }
        if (Down & HidNpadButton_ZL) {
            this->debugVisible = !this->debugVisible;
            this->updateDebug();
        }
        if (Down & HidNpadButton_Y) {
            if (!this->isInstalledSection()) {
                if (this->selectedItems.size() == this->menu->GetItems().size()) {
                    this->drawMenuItems(true);
                } else {
                    for (long unsigned int i = 0; i < this->menu->GetItems().size(); i++) {
                        if (this->menu->GetItems()[i]->GetIcon() == "romfs:/images/icons/check-box-outline.png") continue;
                        this->selectTitle(i);
                    }
                    this->drawMenuItems(false);
                }
            }
        }
        if (Down & HidNpadButton_X) {
            this->startShop(true);
        }
        if (Down & HidNpadButton_Plus) {
            if (!this->isInstalledSection()) {
                if (this->selectedItems.empty()) {
                    this->selectTitle(this->menu->GetSelectedIndex());
                }
                if (!this->selectedItems.empty()) this->startInstall();
            }
        }
        this->updatePreview();
        this->updateInstalledGrid();
        this->updateDebug();
    }

    void shopInstPage::showInstalledDetails() {
        if (!this->isInstalledSection())
            return;
        if (this->gridSelectedIndex < 0 || this->gridSelectedIndex >= (int)this->visibleItems.size())
            return;
        const auto& item = this->visibleItems[this->gridSelectedIndex];

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

        mainApp->CreateShowDialog(item.name, body, {"common.ok"_lang}, true);
    }
}
