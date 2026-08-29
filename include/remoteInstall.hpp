#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace remoteInstStuff {
    using RemoteFetchProgressCallback = std::function<void(std::uint64_t downloaded, std::uint64_t total)>;

    struct RemoteItem {
        std::string name;
        std::string url;
        std::string indexSourceUrl;
        // Headers declared by a legacy Tinfoil index for the file request.
        std::vector<std::string> requestHeaders;
        std::string iconUrl;
        std::string appId;
        std::string saveId;
        std::string saveNote;
        std::string saveCreatedAt;
        std::string cheatTitleId;
        std::string cheatBuildId;
        std::string cheatNote;
        std::uint64_t saveCreatedTs = 0;
        std::uint64_t size;
        std::uint64_t titleId = 0;
        std::uint32_t appVersion = 0;
        std::uint32_t releaseDate = 0;
        std::int32_t appType = -1;
        bool hasTitleId = false;
        bool hasAppVersion = false;
        bool hasReleaseDate = false;
        bool hasIconUrl = false;
        bool hasAppId = false;
        bool googleDriveWithoutApiKey = false;
        bool isCheat = false;
        // Cached base/update/DLC classification; 0 means "not classified yet".
        // Filled once while sections are built, see ClassifyItem() in ui/remoteInstPage.cpp.
        std::uint8_t typeFlags = 0;
    };

    struct RemoteSection {
        std::string id;
        std::string title;
        std::vector<RemoteItem> items;
    };

    std::vector<RemoteItem> FetchRemote(const std::string& remoteUrl, const std::string& user, const std::string& pass, std::string& error, const RemoteFetchProgressCallback& progressCb = RemoteFetchProgressCallback());
    std::vector<RemoteSection> FetchRemoteSections(const std::string& remoteUrl, const std::string& user, const std::string& pass, std::string& error, bool* outUsedLegacyFallback = nullptr, const RemoteFetchProgressCallback& progressCb = RemoteFetchProgressCallback());
    std::string FetchRemoteMotd(const std::string& remoteUrl, const std::string& user, const std::string& pass);
    std::string GetRemoteApiPrefix();
    bool DownloadCheatText(const RemoteItem& item, const std::string& user, const std::string& pass, std::string& text, std::string& error);
    bool UploadCheatText(const std::string& remoteUrl, const std::string& user, const std::string& pass, const std::string& titleId, const std::string& buildId, const std::string& note, const std::string& text, std::string& error);
    void installTitleRemote(const std::vector<RemoteItem>& items, int storage, const std::string& sourceLabel);
}
