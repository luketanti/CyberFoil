#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <switch.h>

#include "remoteInstall.hpp"

namespace inst::save_sync {
    struct SaveSyncRemoteVersion {
        std::string saveId;
        std::string saveType = "unknown";
        std::string note;
        std::string createdAt;
        std::uint64_t createdTs = 0;
        std::string downloadUrl;
        std::uint64_t size = 0;
    };

    struct SaveSyncEntry {
        std::uint64_t titleId = 0;
        std::string titleName;
        bool localAvailable = false;
        bool deviceSave = false;
        std::string saveType = "unknown";
        bool remoteAvailable = false;
        std::string remoteDownloadUrl;
        std::uint64_t remoteSize = 0;
        std::vector<SaveSyncRemoteVersion> remoteVersions;
    };

    bool FetchRemoteSaveItems(const std::string& remoteUrl, const std::string& user, const std::string& pass, std::vector<remoteInstStuff::RemoteItem>& outItems, std::string& warning);
    bool BuildEntriesForUser(const std::vector<remoteInstStuff::RemoteItem>& remoteItems, const AccountUid* uid, std::vector<SaveSyncEntry>& outEntries, std::string& warning);
    bool BuildEntries(const std::vector<remoteInstStuff::RemoteItem>& remoteItems, std::vector<SaveSyncEntry>& outEntries, std::string& warning);
    bool UploadSaveToServerForUser(const std::string& remoteUrl, const std::string& user, const std::string& pass, const AccountUid* uid, const SaveSyncEntry& entry, const std::string& note, std::string& error);
    bool UploadSaveToServer(const std::string& remoteUrl, const std::string& user, const std::string& pass, const SaveSyncEntry& entry, const std::string& note, std::string& error);
    bool DownloadSaveToConsole(const std::string& remoteUrl, const std::string& user, const std::string& pass, const SaveSyncEntry& entry, const SaveSyncRemoteVersion* remoteVersion, std::string& error);
    bool DeleteSaveFromServer(const std::string& remoteUrl, const std::string& user, const std::string& pass, const SaveSyncEntry& entry, const SaveSyncRemoteVersion* remoteVersion, std::string& error);
}
