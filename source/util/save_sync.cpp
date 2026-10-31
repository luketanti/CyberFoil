#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <switch.h>
#include <zlib.h>

#include "util/save_sync.hpp"
#include "util/save_data_policy.hpp"

#include "util/config.hpp"
#include "util/curl.hpp"
#include "util/hauth.hpp"
#include "util/json.hpp"
#include "util/lang.hpp"
#include "util/title_util.hpp"
#include "util/uid.hpp"
#include "util/util.hpp"
#include "ui/instPage.hpp"

namespace {
    constexpr const char* kSaveMountName = "svsync";
    constexpr double kSaveSyncUiTransferWeight = 70.0; // keep room for zip/mount phases around transfers
    constexpr std::uint64_t kSaveSyncCommitEveryBytes = 8ULL * 1024ULL * 1024ULL;
    constexpr std::size_t kSaveSyncCommitEveryFiles = 64;
    constexpr std::uint64_t kSaveSyncUiUpdateBytesStep = 256ULL * 1024ULL;
    constexpr auto kSaveSyncUiUpdateMinInterval = std::chrono::milliseconds(120);
    constexpr std::size_t kSaveSyncZipIoBufferSize = 256U * 1024U;

    struct UiProgressThrottle {
        int lastPercent = -1;
        std::uint64_t lastNow = 0;
        std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::time_point::min();
    };
    using SaveSyncFileProgressCallback = std::function<void(std::size_t index, std::size_t total, const std::string& relativePath)>;
    bool IsSafeZipRelativePath(const std::string& rawPath);

    bool ShouldEmitUiProgress(UiProgressThrottle& throttle, int percent, std::uint64_t now)
    {
        const auto current = std::chrono::steady_clock::now();
        if (percent >= 0 && percent != throttle.lastPercent) {
            throttle.lastPercent = percent;
            throttle.lastNow = now;
            throttle.lastUpdate = current;
            return true;
        }
        if ((now >= throttle.lastNow) && ((now - throttle.lastNow) >= kSaveSyncUiUpdateBytesStep)) {
            throttle.lastNow = now;
            throttle.lastUpdate = current;
            return true;
        }
        if (throttle.lastUpdate == std::chrono::steady_clock::time_point::min() ||
            (current - throttle.lastUpdate) >= kSaveSyncUiUpdateMinInterval) {
            throttle.lastNow = now;
            throttle.lastUpdate = current;
            return true;
        }
        return false;
    }

    std::string FormatResultHex(Result rc)
    {
        char buf[16] = {0};
        std::snprintf(buf, sizeof(buf), "0x%08X", rc);
        return std::string(buf);
    }

    std::string FormatTitleIdHex(std::uint64_t titleId)
    {
        char buf[32] = {0};
        std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(titleId));
        return std::string(buf);
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
        if (text.empty())
            return false;

        bool hasHexLetters = false;
        bool allDigits = true;
        for (unsigned char c : text) {
            if (!std::isxdigit(c))
                return false;
            if (!std::isdigit(c))
                hasHexLetters = true;
            if (!std::isdigit(c))
                allDigits = false;
        }

        if (hasHexLetters || text.size() == 16) {
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(text.c_str(), &end, 16);
            if (end == text.c_str() || (end && *end != '\0'))
                return false;
            out = static_cast<std::uint64_t>(parsed);
            return true;
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

    std::string FormatSizeForUi(std::uint64_t bytes)
    {
        static const char* units[] = {"B", "KB", "MB", "GB"};
        double value = static_cast<double>(bytes);
        std::size_t unit = 0;
        while (value >= 1024.0 && unit + 1 < (sizeof(units) / sizeof(units[0]))) {
            value /= 1024.0;
            unit++;
        }
        char buf[64] = {};
        if (unit == 0)
            std::snprintf(buf, sizeof(buf), "%llu %s", static_cast<unsigned long long>(bytes), units[unit]);
        else
            std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
        return std::string(buf);
    }

    std::string NormalizeRemoteUrl(std::string url)
    {
        url = TrimAscii(url);
        if (url.empty())
            return url;
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
            url = "http://" + url;
        if (!url.empty() && url.back() == '/')
            url.pop_back();
        return url;
    }

    std::string NormalizePathForArchive(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        while (path.find("//") != std::string::npos)
            path.erase(path.find("//"), 1);
        return path;
    }

    bool BuildArchiveRelativePath(const std::filesystem::path& sourceRoot, const std::filesystem::path& filePath, std::string& outRel)
    {
        outRel.clear();
        std::string root = NormalizePathForArchive(sourceRoot.string());
        std::string full = NormalizePathForArchive(filePath.string());
        if (root.empty() || full.empty())
            return false;

        if (root.back() != '/')
            root.push_back('/');

        if (full.rfind(root, 0) == 0) {
            outRel = full.substr(root.size());
        } else {
            std::error_code relEc;
            outRel = std::filesystem::relative(filePath, sourceRoot, relEc).generic_string();
            if (relEc)
                return false;
        }

        outRel = NormalizePathForArchive(outRel);
        while (!outRel.empty() && outRel.front() == '/')
            outRel.erase(outRel.begin());
        return IsSafeZipRelativePath(outRel);
    }

    std::string BuildUploadUrl(const std::string& remoteUrl, std::uint64_t titleId)
    {
        const std::string baseUrl = NormalizeRemoteUrl(remoteUrl);
        if (baseUrl.empty())
            return std::string();
        return baseUrl + "/api/saves/upload/" + FormatTitleIdHex(titleId);
    }

    std::string NormalizeSaveIdToken(std::string value)
    {
        value = TrimAscii(value);
        if (value.empty())
            return std::string();
        if (value.size() > 96)
            value.resize(96);

        std::string normalized;
        normalized.reserve(value.size());
        for (unsigned char c : value) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
                normalized.push_back(static_cast<char>(c));
        }
        return normalized;
    }

    std::string BuildDownloadUrl(const std::string& remoteUrl, std::uint64_t titleId, const std::string& saveId = std::string())
    {
        const std::string baseUrl = NormalizeRemoteUrl(remoteUrl);
        if (baseUrl.empty())
            return std::string();
        const std::string normalizedSaveId = NormalizeSaveIdToken(saveId);
        if (!normalizedSaveId.empty())
            return baseUrl + "/api/saves/download/" + FormatTitleIdHex(titleId) + "/" + normalizedSaveId + ".zip";
        return baseUrl + "/api/saves/download/" + FormatTitleIdHex(titleId) + ".zip";
    }

    std::string BuildDeleteUrl(const std::string& remoteUrl, std::uint64_t titleId, const std::string& saveId = std::string())
    {
        const std::string baseUrl = NormalizeRemoteUrl(remoteUrl);
        if (baseUrl.empty())
            return std::string();
        const std::string normalizedSaveId = NormalizeSaveIdToken(saveId);
        if (!normalizedSaveId.empty())
            return baseUrl + "/api/saves/delete/" + FormatTitleIdHex(titleId) + "/" + normalizedSaveId;
        return baseUrl + "/api/saves/delete/" + FormatTitleIdHex(titleId);
    }

    std::string BuildRemoteListUrl(const std::string& remoteUrl)
    {
        const std::string baseUrl = NormalizeRemoteUrl(remoteUrl);
        if (baseUrl.empty())
            return std::string();
        return baseUrl + "/api/saves/list";
    }

    std::string ScopeSaveUrl(const std::string& url, const std::string& saveType)
    {
        if (url.find("save_type=") != std::string::npos) return url;
        return url + (url.find('?') == std::string::npos ? "?" : "&") + "save_type=" + saveType;
    }

    std::string BuildFullUrl(const std::string& baseUrl, const std::string& pathOrUrl)
    {
        if (pathOrUrl.empty())
            return std::string();
        if (pathOrUrl.rfind("http://", 0) == 0 || pathOrUrl.rfind("https://", 0) == 0)
            return pathOrUrl;
        if (baseUrl.empty())
            return pathOrUrl;
        if (pathOrUrl.front() == '/')
            return baseUrl + pathOrUrl;
        return baseUrl + "/" + pathOrUrl;
    }

    bool ResolveRemoteTitleId(const remoteInstStuff::RemoteItem& item, std::uint64_t& outTitleId)
    {
        if (item.hasTitleId) {
            outTitleId = item.titleId;
            return true;
        }
        if (item.hasAppId && TryParseTitleIdText(item.appId, outTitleId))
            return true;
        return false;
    }

    bool ResolveActiveUser(AccountUid& outUid, std::string& error)
    {
        error.clear();
        Result rc = accountInitialize(AccountServiceType_Application);
        if (R_FAILED(rc)) {
            error = "Failed to initialize account service (" + FormatResultHex(rc) + ").";
            return false;
        }

        AccountUid uid = {};
        if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid)) {
            outUid = uid;
            accountExit();
            return true;
        }
        if (R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid)) {
            outUid = uid;
            accountExit();
            return true;
        }

        AccountUid users[ACC_USER_LIST_SIZE] = {};
        s32 total = 0;
        if (R_SUCCEEDED(accountListAllUsers(users, ACC_USER_LIST_SIZE, &total)) && total > 0 && accountUidIsValid(&users[0])) {
            outUid = users[0];
            accountExit();
            return true;
        }

        accountExit();
        error = "No valid user account found for save-data access.";
        return false;
    }

    bool ResolveRequestedOrActiveUser(const AccountUid* requestedUid, AccountUid& outUid, std::string& error)
    {
        error.clear();
        if (requestedUid && accountUidIsValid(requestedUid)) {
            outUid = *requestedUid;
            return true;
        }
        return ResolveActiveUser(outUid, error);
    }

    bool EnumerateLocalSaves(const AccountUid& uid, std::map<inst::save_sync::SaveEntryKey, inst::save_sync::SaveSyncEntry>& entries, std::string& warning)
    {
        warning.clear();
        FsSaveDataInfoReader reader = {};
        Result rc = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);
        if (R_FAILED(rc)) {
            warning = "Failed to enumerate local saves (" + FormatResultHex(rc) + ").";
            return false;
        }

        const bool nsReady = R_SUCCEEDED(nsInitialize());
        while (true) {
            FsSaveDataInfo infoBuf[64] = {};
            s64 outCount = 0;
            rc = fsSaveDataInfoReaderRead(&reader, infoBuf, 64, &outCount);
            if (R_FAILED(rc)) {
                warning = "Failed while reading local saves (" + FormatResultHex(rc) + ").";
                break;
            }
            if (outCount <= 0)
                break;

            for (s64 i = 0; i < outCount; i++) {
                const FsSaveDataInfo& info = infoBuf[i];
                const bool deviceSave = info.save_data_type == FsSaveDataType_Device;
                if (!inst::save_sync::IsBackupSave(info, uid))
                    continue;

                const std::string saveType = deviceSave ? "device" : "account";
                auto& entry = entries[{info.application_id, saveType}];
                entry.titleId = info.application_id;
                entry.localAvailable = true;
                entry.deviceSave = deviceSave;
                entry.saveType = saveType;
                if (entry.titleName.empty())
                    entry.titleName = tin::util::GetTitleName(info.application_id, NcmContentMetaType_Application);
            }
        }

        if (nsReady)
            nsExit();
        fsSaveDataInfoReaderClose(&reader);
        return true;
    }

    bool CreateZipFromDirectory(const std::filesystem::path& sourceRoot, const std::filesystem::path& zipPath, std::string& error, const SaveSyncFileProgressCallback& progressCb = {})
    {
        error.clear();
        zipFile zf = zipOpen64(zipPath.string().c_str(), APPEND_STATUS_CREATE);
        if (!zf) {
            error = "Failed to create save archive.";
            return false;
        }

        std::vector<std::filesystem::path> files;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(sourceRoot, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator end;
        while (!ec && it != end) {
            if (it->is_regular_file())
                files.push_back(it->path());
            it.increment(ec);
        }
        if (ec) {
            zipClose(zf, nullptr);
            error = "Failed while collecting save files for archive.";
            return false;
        }

        if (files.empty()) {
            zip_fileinfo zi = {};
            if (zipOpenNewFileInZip64(zf, ".empty", &zi, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_BEST_SPEED, 1) != ZIP_OK) {
                zipClose(zf, nullptr);
                error = "Failed to build save archive.";
                return false;
            }
            zipCloseFileInZip(zf);
            zipClose(zf, nullptr);
            return true;
        }

        std::vector<char> buffer(kSaveSyncZipIoBufferSize);
        std::size_t fileIndex = 0;
        for (const auto& file : files) {
            std::string rel;
            if (!BuildArchiveRelativePath(sourceRoot, file, rel) || rel.empty()) {
                zipClose(zf, nullptr);
                error = "Failed to resolve save archive relative path.";
                return false;
            }
            fileIndex++;
            if (progressCb)
                progressCb(fileIndex, files.size(), rel);

            zip_fileinfo zi = {};
            if (zipOpenNewFileInZip64(zf, rel.c_str(), &zi, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_BEST_SPEED, 1) != ZIP_OK) {
                zipClose(zf, nullptr);
                error = "Failed to open file in save archive.";
                return false;
            }

            std::ifstream in(file, std::ios::binary);
            if (!in.is_open()) {
                zipCloseFileInZip(zf);
                zipClose(zf, nullptr);
                error = "Failed to read local save file for archive.";
                return false;
            }

            while (in.good()) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize read = in.gcount();
                if (read > 0) {
                    if (zipWriteInFileInZip(zf, buffer.data(), static_cast<unsigned int>(read)) != ZIP_OK) {
                        zipCloseFileInZip(zf);
                        zipClose(zf, nullptr);
                        error = "Failed while writing save archive.";
                        return false;
                    }
                }
            }

            if (zipCloseFileInZip(zf) != ZIP_OK) {
                zipClose(zf, nullptr);
                error = "Failed to finalize save archive entry.";
                return false;
            }
        }

        if (zipClose(zf, nullptr) != ZIP_OK) {
            error = "Failed to finalize save archive.";
            return false;
        }
        return true;
    }

    bool IsSafeZipRelativePath(const std::string& rawPath)
    {
        if (rawPath.empty())
            return false;
        if (rawPath.front() == '/' || rawPath.front() == '\\')
            return false;
        if (rawPath.find(':') != std::string::npos)
            return false;

        std::filesystem::path p(rawPath);
        for (const auto& part : p) {
            const std::string token = part.string();
            if (token == "..")
                return false;
        }
        return true;
    }

    bool ExtractZipToMountedSaveWithCommits(const std::filesystem::path& archivePath, const std::string& mountedPath, const char* mountName, std::string& error, const SaveSyncFileProgressCallback& progressCb = {})
    {
        error.clear();
        unzFile unz = unzOpen64(archivePath.string().c_str());
        if (!unz) {
            error = "Failed to open downloaded save archive.";
            return false;
        }

        int code = unzGoToFirstFile(unz);
        bool sawEntry = false;
        std::vector<char> buffer(kSaveSyncZipIoBufferSize);
        std::uint64_t bytesSinceCommit = 0;
        std::size_t filesSinceCommit = 0;
        std::size_t totalFiles = 0;
        if (code == UNZ_OK) {
            int countCode = code;
            while (countCode == UNZ_OK) {
                unz_file_info64 countInfo = {};
                if (unzGetCurrentFileInfo64(unz, &countInfo, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK) {
                    unzClose(unz);
                    error = "Failed to scan save archive metadata.";
                    return false;
                }
                std::string countRelPath;
                if (countInfo.size_filename > 0) {
                    std::vector<char> pathBuf(static_cast<size_t>(countInfo.size_filename) + 1, '\0');
                    if (unzGetCurrentFileInfo64(unz, &countInfo, pathBuf.data(), static_cast<uLong>(pathBuf.size()), nullptr, 0, nullptr, 0) != UNZ_OK) {
                        unzClose(unz);
                        error = "Failed to scan save archive filename.";
                        return false;
                    }
                    countRelPath.assign(pathBuf.data());
                }
                std::replace(countRelPath.begin(), countRelPath.end(), '\\', '/');
                const bool countIsDirectory = !countRelPath.empty() && countRelPath.back() == '/';
                if (!countRelPath.empty() && countRelPath != ".empty" && !countIsDirectory)
                    totalFiles++;
                countCode = unzGoToNextFile(unz);
            }
            if (countCode != UNZ_END_OF_LIST_OF_FILE) {
                unzClose(unz);
                error = "Failed while scanning downloaded save archive.";
                return false;
            }
            code = unzGoToFirstFile(unz);
        }
        std::size_t extractedFiles = 0;
        while (code == UNZ_OK) {
            sawEntry = true;
            unz_file_info64 info = {};
            if (unzGetCurrentFileInfo64(unz, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK) {
                unzClose(unz);
                error = "Failed to read save archive metadata.";
                return false;
            }

            std::string relPath;
            if (info.size_filename > 0) {
                std::vector<char> pathBuf(static_cast<size_t>(info.size_filename) + 1, '\0');
                if (unzGetCurrentFileInfo64(unz, &info, pathBuf.data(), static_cast<uLong>(pathBuf.size()), nullptr, 0, nullptr, 0) != UNZ_OK) {
                    unzClose(unz);
                    error = "Failed to read save archive filename.";
                    return false;
                }
                relPath.assign(pathBuf.data());
            }
            std::replace(relPath.begin(), relPath.end(), '\\', '/');

            const bool isDirectory = !relPath.empty() && relPath.back() == '/';
            if (!relPath.empty() && relPath != ".empty") {
                if (!IsSafeZipRelativePath(relPath)) {
                    unzClose(unz);
                    error = "Save archive contains an invalid path entry.";
                    return false;
                }

                const std::filesystem::path outPath = std::filesystem::path(mountedPath) / std::filesystem::path(relPath);
                std::error_code ec;
                if (isDirectory) {
                    std::filesystem::create_directories(outPath, ec);
                    if (ec) {
                        unzClose(unz);
                        error = "Failed to prepare save directories.";
                        return false;
                    }
                } else {
                    extractedFiles++;
                    if (progressCb)
                        progressCb(extractedFiles, totalFiles, relPath);
                    std::filesystem::create_directories(outPath.parent_path(), ec);
                    if (ec) {
                        unzClose(unz);
                        error = "Failed to prepare save file directories.";
                        return false;
                    }

                    if (unzOpenCurrentFile(unz) != UNZ_OK) {
                        unzClose(unz);
                        error = "Failed to open file in save archive.";
                        return false;
                    }

                    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
                    if (!out.is_open()) {
                        unzCloseCurrentFile(unz);
                        unzClose(unz);
                        error = "Failed to create file in save data.";
                        return false;
                    }

                    while (true) {
                        const int read = unzReadCurrentFile(unz, buffer.data(), static_cast<unsigned int>(buffer.size()));
                        if (read < 0) {
                            out.close();
                            unzCloseCurrentFile(unz);
                            unzClose(unz);
                            error = "Failed while extracting save archive.";
                            return false;
                        }
                        if (read == 0)
                            break;
                        out.write(buffer.data(), read);
                        bytesSinceCommit += static_cast<std::uint64_t>(read);
                        if (!out.good()) {
                            out.close();
                            unzCloseCurrentFile(unz);
                            unzClose(unz);
                            error = "Failed while writing save data.";
                            return false;
                        }
                    }

                    out.flush();
                    out.close();
                    if (unzCloseCurrentFile(unz) != UNZ_OK) {
                        unzClose(unz);
                        error = "Failed to finalize save file extraction.";
                        return false;
                    }

                    filesSinceCommit++;
                    if (bytesSinceCommit >= kSaveSyncCommitEveryBytes || filesSinceCommit >= kSaveSyncCommitEveryFiles) {
                        Result commitRc = fsdevCommitDevice(mountName);
                        if (R_FAILED(commitRc)) {
                            unzClose(unz);
                            error = "Failed to commit imported save data (" + FormatResultHex(commitRc) + ").";
                            return false;
                        }
                        bytesSinceCommit = 0;
                        filesSinceCommit = 0;
                    }
                }
            }

            code = unzGoToNextFile(unz);
        }

        unzClose(unz);
        if (!sawEntry) {
            error = "Downloaded save archive is empty.";
            return false;
        }
        if (code != UNZ_END_OF_LIST_OF_FILE) {
            error = "Failed while reading downloaded save archive.";
            return false;
        }
        return true;
    }

    size_t WriteToString(char* ptr, size_t size, size_t numItems, void* userdata)
    {
        auto* out = reinterpret_cast<std::string*>(userdata);
        const size_t total = size * numItems;
        out->append(ptr, total);
        return total;
    }

    void BuildVersionAndRevision(std::string& outVersion, std::string& outRevision)
    {
        const std::string raw = inst::config::remoteLegacyMode ? "20.0.2" : inst::config::appVersion;
        outVersion = raw.empty() ? "0.0" : raw;
        outRevision = "0";

        const std::size_t firstDot = raw.find('.');
        if (firstDot == std::string::npos)
            return;

        const std::size_t secondDot = raw.find('.', firstDot + 1);
        if (secondDot == std::string::npos) {
            outVersion = raw;
            return;
        }

        outVersion = raw.substr(0, secondDot);
        const std::string revisionToken = raw.substr(secondDot + 1);
        if (revisionToken.empty())
            return;

        std::size_t digitsEnd = 0;
        while (digitsEnd < revisionToken.size()) {
            const char c = revisionToken[digitsEnd];
            if (c < '0' || c > '9')
                break;
            digitsEnd++;
        }
        if (digitsEnd > 0)
            outRevision = revisionToken.substr(0, digitsEnd);
    }

    std::vector<std::string> BuildRemoteHeaders(const std::string& requestUrl, const std::string& user, const std::string& pass)
    {
        std::string themeHeader = "Theme: 0000000000000000000000000000000000000000000000000000000000000000";
        std::string versionValue;
        std::string revisionValue;
        BuildVersionAndRevision(versionValue, revisionValue);
        std::string versionHeader = "Version: " + versionValue;
        std::string revisionHeader = "Revision: " + revisionValue;
        std::string languageHeader = "Language: " + Language::GetRemoteHeaderLanguage();
        std::string hauthHeader = "HAUTH: " + inst::util::ComputeHauthFromUrl(requestUrl);
        std::string uauthHeader = "UAUTH: " + inst::util::ComputeUauthFromUrl(requestUrl, user, pass);
        std::string uidHeader = "UID: " + inst::util::ComputeUidFromMmcCid();
        return {
            themeHeader,
            uidHeader,
            versionHeader,
            revisionHeader,
            languageHeader,
            hauthHeader,
            uauthHeader
        };
    }

    bool HttpGetWithAuth(const std::string& url, const std::string& user, const std::string& pass, long& outCode, std::string& outBody, std::string& error)
    {
        outBody.clear();
        error.clear();
        outCode = 0;

        if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
            error = "Failed to initialize HTTP client.";
            return false;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            error = "Failed to initialize HTTP request.";
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        const std::string& userAgent = inst::curl::getUserAgent();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);

        struct curl_slist* headerList = nullptr;
        const auto headers = BuildRemoteHeaders(url, user, pass);
        for (const auto& header : headers)
            headerList = curl_slist_append(headerList, header.c_str());
        if (headerList)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

        std::string authValue;
        if (!user.empty() || !pass.empty()) {
            authValue = user + ":" + pass;
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERPWD, authValue.c_str());
        }

        const CURLcode rc = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &outCode);
        if (headerList)
            curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            error = "Failed to query remote saves: " + std::string(curl_easy_strerror(rc));
            return false;
        }
        if (outCode < 200 || outCode >= 300) {
            if (outCode == 401 || outCode == 403) {
                error = "Save sync is not permitted for this account (backup access required).";
            } else if (outCode == 404) {
                error = "Server does not provide save sync endpoints.";
            } else {
                std::ostringstream ss;
                ss << "Remote save list request returned HTTP " << outCode << ".";
                error = ss.str();
            }
            return false;
        }
        return true;
    }

    bool HttpDeleteWithAuth(const std::string& url, const std::string& user, const std::string& pass, long& outCode, std::string& outBody, std::string& error)
    {
        outBody.clear();
        error.clear();
        outCode = 0;

        if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
            error = "Failed to initialize HTTP client.";
            return false;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            error = "Failed to initialize HTTP request.";
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        const std::string& userAgent = inst::curl::getUserAgent();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);

        struct curl_slist* headerList = nullptr;
        const auto headers = BuildRemoteHeaders(url, user, pass);
        for (const auto& header : headers)
            headerList = curl_slist_append(headerList, header.c_str());
        if (headerList)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

        std::string authValue;
        if (!user.empty() || !pass.empty()) {
            authValue = user + ":" + pass;
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERPWD, authValue.c_str());
        }

        const CURLcode rc = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &outCode);
        if (headerList)
            curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            error = "Failed to delete remote save: " + std::string(curl_easy_strerror(rc));
            return false;
        }
        return true;
    }

    bool HttpDownloadFileWithAuthAndProgress(const std::string& url, const std::string& outputPath, const std::string& user, const std::string& pass, long timeoutMs, const std::string& expectedSaveType, std::string& error)
    {
        error.clear();
        if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
            error = "Failed to initialize HTTP client.";
            return false;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            error = "Failed to initialize HTTP request.";
            return false;
        }

        FILE* file = std::fopen(outputPath.c_str(), "wb");
        if (!file) {
            curl_easy_cleanup(curl);
            error = "Failed to open save archive output file.";
            return false;
        }

        UiProgressThrottle throttle{};
        std::string responseSaveType;
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, +[](char* data, size_t size, size_t count, void* context) -> size_t {
            const size_t length = size * count;
            auto& saveType = *static_cast<std::string*>(context);
            const std::string line(data, length);
            if (line.rfind("HTTP/", 0) == 0) saveType.clear(); // Discard redirect/interim response headers.
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name == "x-save-type") saveType = TrimAscii(line.substr(colon + 1));
            }
            return length;
        });
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseSaveType);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullptr);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        const std::string& userAgent = inst::curl::getUserAgent();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) -> int {
            auto* throttle = static_cast<UiProgressThrottle*>(clientp);
            if (!throttle)
                return 0;
            std::uint64_t total = dltotal > 0 ? static_cast<std::uint64_t>(dltotal) : 0;
            std::uint64_t now = dlnow > 0 ? static_cast<std::uint64_t>(dlnow) : 0;
            if (total > 0) {
                const int transferPercent = static_cast<int>((now * 100ULL) / total);
                if (!ShouldEmitUiProgress(*throttle, transferPercent, now))
                    return 0;
                const int uiPercent = 10 + static_cast<int>((kSaveSyncUiTransferWeight * transferPercent) / 100.0);
                inst::ui::instPage::setInstBarPerc(uiPercent);
                inst::ui::instPage::setProgressDetailText(
                    "inst.remote.save_sync.progress.download_fmt"_lang +
                    std::to_string(transferPercent) + "% (" + FormatSizeForUi(now) + " / " + FormatSizeForUi(total) + ")");
            } else if (now > 0) {
                if (!ShouldEmitUiProgress(*throttle, -1, now))
                    return 0;
                inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.download_bytes_fmt"_lang + FormatSizeForUi(now));
            }
            return 0;
        });
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &throttle);

        struct curl_slist* headerList = nullptr;
        const auto headers = BuildRemoteHeaders(url, user, pass);
        for (const auto& header : headers)
            headerList = curl_slist_append(headerList, header.c_str());
        if (headerList)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

        std::string authValue;
        if (!user.empty() || !pass.empty()) {
            authValue = user + ":" + pass;
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERPWD, authValue.c_str());
        }

        const CURLcode rc = curl_easy_perform(curl);
        long responseCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
        if (headerList)
            curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        std::fclose(file);

        if (rc != CURLE_OK) {
            error = "Failed to download save archive: " + std::string(curl_easy_strerror(rc));
            return false;
        }
        if (responseCode < 200 || responseCode >= 300) {
            if (responseCode == 401 || responseCode == 403) {
                error = "Save download is not permitted for this account (backup access required).";
            } else if (responseCode == 404) {
                error = "Remote save backup was not found.";
            } else {
                std::ostringstream ss;
                ss << "Save download failed with HTTP " << responseCode;
                error = ss.str();
            }
            return false;
        }
        if (!inst::save_sync::CanRestoreSaveType(responseSaveType, expectedSaveType)) {
            std::remove(outputPath.c_str());
            error = "The server did not confirm the expected save type. No local save data was changed.";
            return false;
        }
        return true;
    }

    bool TryGetTitleIdFromJson(const nlohmann::json& obj, std::uint64_t& outTitleId)
    {
        static const char* keys[] = {"title_id", "titleId", "app_id", "appId", "id"};
        for (const auto* key : keys) {
            if (!obj.contains(key))
                continue;
            const auto& value = obj[key];
            if (value.is_number_unsigned()) {
                outTitleId = value.get<std::uint64_t>();
                return true;
            }
            if (value.is_number_integer()) {
                const auto parsed = value.get<long long>();
                if (parsed >= 0) {
                    outTitleId = static_cast<std::uint64_t>(parsed);
                    return true;
                }
            }
            if (value.is_string()) {
                if (TryParseTitleIdText(value.get<std::string>(), outTitleId))
                    return true;
            }
        }
        return false;
    }

    bool TryGetStringByKeys(const nlohmann::json& obj, const std::vector<const char*>& keys, std::string& out)
    {
        out.clear();
        for (const auto* key : keys) {
            if (!obj.contains(key))
                continue;
            const auto& value = obj[key];
            if (!value.is_string())
                continue;
            out = TrimAscii(value.get<std::string>());
            if (!out.empty())
                return true;
        }
        return false;
    }

    bool TryGetU64ByKeys(const nlohmann::json& obj, const std::vector<const char*>& keys, std::uint64_t& out)
    {
        out = 0;
        for (const auto* key : keys) {
            if (!obj.contains(key))
                continue;
            const auto& value = obj[key];
            if (value.is_number_unsigned()) {
                out = value.get<std::uint64_t>();
                return true;
            }
            if (value.is_number_integer()) {
                const auto parsed = value.get<long long>();
                if (parsed >= 0) {
                    out = static_cast<std::uint64_t>(parsed);
                    return true;
                }
            }
            if (value.is_string()) {
                const std::string text = TrimAscii(value.get<std::string>());
                if (text.empty())
                    continue;
                bool allDigits = true;
                for (unsigned char c : text) {
                    if (!std::isdigit(c)) {
                        allDigits = false;
                        break;
                    }
                }
                if (allDigits) {
                    char* end = nullptr;
                    unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
                    if (end != text.c_str() && end && *end == '\0') {
                        out = static_cast<std::uint64_t>(parsed);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool UploadZipMultipart(const std::string& url, const std::string& zipPath, const std::string& user, const std::string& pass, std::uint64_t titleId, const std::string& saveType, const std::string& note, std::string& error)
    {
        error.clear();
        if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
            error = "Failed to initialize HTTP client.";
            return false;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            error = "Failed to initialize HTTP request.";
            return false;
        }

        UiProgressThrottle throttle{};
        std::string responseBody;
        const std::string titleIdText = FormatTitleIdHex(titleId);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        const std::string& userAgent = inst::curl::getUserAgent();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 60000L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> int {
            auto* throttle = static_cast<UiProgressThrottle*>(clientp);
            if (!throttle)
                return 0;
            std::uint64_t total = ultotal > 0 ? static_cast<std::uint64_t>(ultotal) : 0;
            std::uint64_t now = ulnow > 0 ? static_cast<std::uint64_t>(ulnow) : 0;
            if (total > 0) {
                const int transferPercent = static_cast<int>((now * 100ULL) / total);
                if (!ShouldEmitUiProgress(*throttle, transferPercent, now))
                    return 0;
                const int uiPercent = 30 + static_cast<int>((65.0 * transferPercent) / 100.0); // 30..95
                inst::ui::instPage::setInstBarPerc(uiPercent);
                inst::ui::instPage::setProgressDetailText(
                    "inst.remote.save_sync.progress.upload_archive_fmt"_lang +
                    std::to_string(transferPercent) + "% (" + FormatSizeForUi(now) + " / " + FormatSizeForUi(total) + ")");
            } else if (now > 0) {
                if (!ShouldEmitUiProgress(*throttle, -1, now))
                    return 0;
                inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.upload_archive_bytes_fmt"_lang + FormatSizeForUi(now));
            }
            if (dltotal > 0 && dlnow >= 0) {
                const int responsePercent = static_cast<int>((static_cast<double>(dlnow) / static_cast<double>(dltotal)) * 100.0);
                inst::ui::instPage::setProgressDetailText(
                    "inst.remote.save_sync.progress.upload_wait_response_fmt"_lang + std::to_string(responsePercent) + "%");
            }
            return 0;
        });
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &throttle);

        struct curl_slist* headerList = nullptr;
        const auto headers = BuildRemoteHeaders(url, user, pass);
        for (const auto& header : headers)
            headerList = curl_slist_append(headerList, header.c_str());
        if (headerList)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

        std::string authValue;
        if (!user.empty() || !pass.empty()) {
            authValue = user + ":" + pass;
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERPWD, authValue.c_str());
        }

        curl_mime* mime = curl_mime_init(curl);
        if (!mime) {
            if (headerList)
                curl_slist_free_all(headerList);
            curl_easy_cleanup(curl);
            error = "Failed to prepare upload payload.";
            return false;
        }

        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "file");
        curl_mime_filedata(part, zipPath.c_str());
        curl_mime_filename(part, (titleIdText + ".zip").c_str());

        part = curl_mime_addpart(mime);
        curl_mime_name(part, "title_id");
        curl_mime_data(part, titleIdText.c_str(), CURL_ZERO_TERMINATED);

        part = curl_mime_addpart(mime);
        curl_mime_name(part, "save_type");
        curl_mime_data(part, saveType.c_str(), CURL_ZERO_TERMINATED);

        part = curl_mime_addpart(mime);
        curl_mime_name(part, "application_id");
        curl_mime_data(part, titleIdText.c_str(), CURL_ZERO_TERMINATED);

        const std::string trimmedNote = TrimAscii(note);
        if (!trimmedNote.empty()) {
            part = curl_mime_addpart(mime);
            curl_mime_name(part, "note");
            curl_mime_data(part, trimmedNote.c_str(), CURL_ZERO_TERMINATED);
        }

        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

        const CURLcode rc = curl_easy_perform(curl);
        long responseCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

        curl_mime_free(mime);
        if (headerList)
            curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            error = "Upload failed: " + std::string(curl_easy_strerror(rc));
            return false;
        }
        if (responseCode < 200 || responseCode >= 300) {
            if (responseCode == 401 || responseCode == 403) {
                error = "Save upload is not permitted for this account (backup access required).";
            } else if (responseCode == 404) {
                error = "Server does not provide save upload endpoints.";
            } else {
                std::ostringstream ss;
                ss << "Upload failed with HTTP " << responseCode;
                if (!responseBody.empty())
                    ss << ": " << responseBody;
                error = ss.str();
            }
            return false;
        }
        return true;
    }

    bool ClearDirectoryContents(const std::filesystem::path& root, std::string& error)
    {
        error.clear();
        std::error_code ec;
        if (!std::filesystem::exists(root, ec))
            return true;
        for (const auto& entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                error = "Failed while reading existing save data.";
                return false;
            }
            std::filesystem::remove_all(entry.path(), ec);
            if (ec) {
                error = "Failed while clearing existing save data.";
                return false;
            }
        }
        return true;
    }

    bool MountSaveDataForTitle(const AccountUid& uid, std::uint64_t titleId, bool deviceSave, bool readOnly, std::string& error)
    {
        error.clear();
        fsdevUnmountDevice(kSaveMountName);
        Result rc;
        if (deviceSave) {
            // Device saves have no account UID. Prefer a read-only filesystem
            // for backups, as with account saves.
            FsFileSystem fs = {};
            FsSaveDataAttribute attr = {};
            attr.application_id = titleId;
            attr.save_data_type = FsSaveDataType_Device;
            rc = readOnly ? fsOpenReadOnlySaveDataFileSystem(&fs, FsSaveDataSpaceId_User, &attr)
                          : fsOpen_DeviceSaveData(&fs, titleId);
            if (readOnly && R_FAILED(rc)) rc = fsOpen_DeviceSaveData(&fs, titleId);
            if (R_SUCCEEDED(rc) && fsdevMountDevice(kSaveMountName, fs) == -1)
                rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        } else {
            rc = readOnly ? fsdevMountSaveDataReadOnly(kSaveMountName, titleId, uid) : fsdevMountSaveData(kSaveMountName, titleId, uid);
        }
        if (!deviceSave && readOnly && R_FAILED(rc))
            rc = fsdevMountSaveData(kSaveMountName, titleId, uid);
        if (R_FAILED(rc)) {
            error = "Failed to mount save data (" + FormatResultHex(rc) + ").";
            return false;
        }
        return true;
    }

}

namespace inst::save_sync {
    bool FetchRemoteSaveItems(const std::string& remoteUrl, const std::string& user, const std::string& pass, std::vector<remoteInstStuff::RemoteItem>& outItems, std::string& warning)
    {
        outItems.clear();
        warning.clear();

        const std::string baseUrl = NormalizeRemoteUrl(remoteUrl);
        const std::string listUrl = BuildRemoteListUrl(remoteUrl).empty() ? "" : BuildRemoteListUrl(remoteUrl) + "?save_types=1";
        if (listUrl.empty())
            return true;

        long responseCode = 0;
        std::string body;
        if (!HttpGetWithAuth(listUrl, user, pass, responseCode, body, warning))
            return false;

        try {
            nlohmann::json root = nlohmann::json::parse(body);
            const nlohmann::json* saves = nullptr;
            if (root.is_array())
                saves = &root;
            else if (root.is_object() && root.contains("saves") && root["saves"].is_array())
                saves = &root["saves"];
            if (!saves)
                return true;

            for (const auto& save : *saves) {
                if (!save.is_object())
                    continue;

                std::uint64_t titleId = 0;
                if (!TryGetTitleIdFromJson(save, titleId))
                    continue;

                std::string name;
                if (save.contains("name") && save["name"].is_string())
                    name = save["name"].get<std::string>();
                if (name.empty())
                    name = tin::util::GetTitleName(titleId, NcmContentMetaType_Application);
                if (name.empty())
                    name = FormatTitleIdHex(titleId);

                std::string saveId;
                if (TryGetStringByKeys(save, {"save_id", "saveId", "version_id", "versionId"}, saveId))
                    saveId = NormalizeSaveIdToken(saveId);

                std::string note;
                TryGetStringByKeys(save, {"note", "save_note", "saveNote"}, note);

                std::string createdAt;
                TryGetStringByKeys(save, {"created_at", "createdAt", "date", "timestamp"}, createdAt);

                std::uint64_t createdTs = 0;
                TryGetU64ByKeys(save, {"created_ts", "createdTs", "timestamp_unix", "timestampUnix"}, createdTs);

                std::string downloadUrl;
                if (save.contains("download_url") && save["download_url"].is_string())
                    downloadUrl = save["download_url"].get<std::string>();
                else if (save.contains("downloadUrl") && save["downloadUrl"].is_string())
                    downloadUrl = save["downloadUrl"].get<std::string>();
                else if (save.contains("url") && save["url"].is_string())
                    downloadUrl = save["url"].get<std::string>();
                downloadUrl = BuildFullUrl(baseUrl, downloadUrl);
                if (downloadUrl.empty())
                    downloadUrl = BuildDownloadUrl(remoteUrl, titleId, saveId);

                std::uint64_t size = 0;
                TryGetU64ByKeys(save, {"size", "archive_size", "archiveSize"}, size);

                remoteInstStuff::RemoteItem item;
                item.name = name;
                item.url = downloadUrl;
                item.size = size;
                item.titleId = titleId;
                item.hasTitleId = true;
                item.saveId = saveId;
                std::string saveType;
                TryGetStringByKeys(save, {"save_type", "saveType"}, saveType);
                item.saveType = NormalizeBackupSaveType(saveType);
                item.saveNote = note;
                item.saveCreatedAt = createdAt;
                item.saveCreatedTs = createdTs;
                outItems.push_back(std::move(item));
            }
        } catch (...) {
            warning = "Failed to parse remote save list.";
            return false;
        }

        return true;
    }

    bool BuildEntriesForUser(const std::vector<remoteInstStuff::RemoteItem>& remoteItems, const AccountUid* uid, std::vector<SaveSyncEntry>& outEntries, std::string& warning)
    {
        warning.clear();
        outEntries.clear();

        std::map<SaveEntryKey, SaveSyncEntry> entriesByTitleId;

        AccountUid targetUid = {};
        std::string userError;
        if (!ResolveRequestedOrActiveUser(uid, targetUid, userError))
            warning = userError;
        // Shared device saves remain discoverable even without a user account.
        std::string localWarning;
        EnumerateLocalSaves(targetUid, entriesByTitleId, localWarning);
        if (!localWarning.empty()) warning = localWarning;

        for (const auto& item : remoteItems) {
            std::uint64_t titleId = 0;
            if (!ResolveRemoteTitleId(item, titleId))
                continue;

            const std::string saveType = NormalizeBackupSaveType(item.saveType);
            auto& entry = entriesByTitleId[{titleId, saveType}];
            entry.titleId = titleId;
            entry.saveType = saveType;
            entry.deviceSave = saveType == "device";
            if (!item.name.empty())
                entry.titleName = item.name;

            SaveSyncRemoteVersion version;
            version.saveId = NormalizeSaveIdToken(item.saveId);
            version.saveType = saveType;
            version.note = TrimAscii(item.saveNote);
            version.createdAt = TrimAscii(item.saveCreatedAt);
            version.createdTs = item.saveCreatedTs;
            version.size = item.size;
            version.downloadUrl = item.url;

            bool duplicate = false;
            for (const auto& existing : entry.remoteVersions) {
                if (existing.saveId == version.saveId &&
                    existing.downloadUrl == version.downloadUrl &&
                    existing.createdTs == version.createdTs &&
                    existing.size == version.size) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                entry.remoteVersions.push_back(std::move(version));
        }

        for (auto& it : entriesByTitleId) {
            auto& entry = it.second;
            std::sort(entry.remoteVersions.begin(), entry.remoteVersions.end(), [](const auto& a, const auto& b) {
                if (a.createdTs != b.createdTs)
                    return a.createdTs > b.createdTs;
                if (a.createdAt != b.createdAt)
                    return a.createdAt > b.createdAt;
                return a.saveId > b.saveId;
            });
            entry.remoteAvailable = !entry.remoteVersions.empty();
            if (entry.remoteAvailable) {
                if (entry.remoteDownloadUrl.empty())
                    entry.remoteDownloadUrl = entry.remoteVersions.front().downloadUrl;
                if (entry.remoteSize == 0)
                    entry.remoteSize = entry.remoteVersions.front().size;
            }

            if (entry.titleName.empty())
                entry.titleName = tin::util::GetTitleName(entry.titleId, NcmContentMetaType_Application);
            if (entry.titleName.empty())
                entry.titleName = FormatTitleIdHex(entry.titleId);
            outEntries.push_back(entry);
        }

        std::sort(outEntries.begin(), outEntries.end(), [](const auto& a, const auto& b) {
            return inst::util::ignoreCaseCompare(a.titleName, b.titleName);
        });

        return true;
    }

    bool BuildEntries(const std::vector<remoteInstStuff::RemoteItem>& remoteItems, std::vector<SaveSyncEntry>& outEntries, std::string& warning)
    {
        return BuildEntriesForUser(remoteItems, nullptr, outEntries, warning);
    }

    bool UploadSaveToServerForUser(const std::string& remoteUrl, const std::string& user, const std::string& pass, const AccountUid* uid, const SaveSyncEntry& entry, const std::string& note, std::string& error)
    {
        error.clear();
        if (entry.titleId == 0) {
            error = "Invalid title ID for save upload.";
            return false;
        }
        if (!entry.localAvailable) {
            error = "No local save data exists for this title and user. Launch the game once to create save data, then retry.";
            return false;
        }

        if (!CanRestoreSaveType(entry.saveType, entry.deviceSave ? "device" : "account")) {
            error = "Select an account or device save before uploading.";
            return false;
        }
        // Probe before packaging: older servers would silently store typed
        // uploads as unknown and could mix their latest-save directories.
        long capabilityCode = 0;
        std::string capabilityBody;
        if (!HttpGetWithAuth(NormalizeRemoteUrl(remoteUrl) + "/api/saves/capabilities", user, pass,
                capabilityCode, capabilityBody, error)) {
            if (capabilityCode == 404) error = "Update AeroFoil to a version with account/device save-type support before uploading.";
            return false;
        }
        const auto capabilities = nlohmann::json::parse(capabilityBody, nullptr, false);
        if (!capabilities.is_object() || !capabilities.contains("save_types") || capabilities["save_types"] != true) {
            error = "This server does not support account/device save types. Update AeroFoil before uploading.";
            return false;
        }

        AccountUid targetUid = {};
        if (!entry.deviceSave && !ResolveRequestedOrActiveUser(uid, targetUid, error))
            return false;

        const std::string tempRoot = inst::config::appDir + "/save_sync_tmp";
        const std::filesystem::path archivePath = std::filesystem::path(tempRoot) / (FormatTitleIdHex(entry.titleId) + ".zip");
        std::error_code ec;
        std::filesystem::remove_all(tempRoot, ec);
        std::filesystem::create_directories(tempRoot, ec);

        const std::string mountedPath = std::string(kSaveMountName) + ":/";

        if (!MountSaveDataForTitle(targetUid, entry.titleId, entry.deviceSave, true, error))
            return false;

        inst::ui::instPage::setTopInstInfoText("inst.remote.save_sync.title"_lang);
        inst::ui::instPage::setInstInfoText("inst.remote.save_sync.status.packaging"_lang);
        inst::ui::instPage::setInstBarPerc(2);
        inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.scanning_files"_lang);
        auto zipProgressCb = [](std::size_t index, std::size_t total, const std::string& relativePath) {
            const int percent = (total > 0) ? static_cast<int>((index * 100ULL) / total) : 0;
            const int uiPercent = 2 + static_cast<int>((28.0 * percent) / 100.0); // 2..30
            inst::ui::instPage::setInstBarPerc(uiPercent);
            inst::ui::instPage::setProgressDetailText(
                "inst.remote.save_sync.progress.compressing_fmt"_lang +
                std::to_string(index) + "/" + std::to_string(total) + ": " + inst::util::shortenString(relativePath, 72, false));
        };
        if (!CreateZipFromDirectory(mountedPath, archivePath, error, zipProgressCb)) {
            fsdevUnmountDevice(kSaveMountName);
            return false;
        }

        fsdevUnmountDevice(kSaveMountName);
        inst::ui::instPage::setInstInfoText("inst.remote.save_sync.status.uploading_archive"_lang);
        inst::ui::instPage::setInstBarPerc(30);

        const std::string uploadUrl = BuildUploadUrl(remoteUrl, entry.titleId);
        if (uploadUrl.empty()) {
            error = "Remote URL is not configured.";
            return false;
        }

        if (!UploadZipMultipart(uploadUrl, archivePath.string(), user, pass, entry.titleId, entry.saveType, note, error))
            return false;

        std::filesystem::remove_all(tempRoot, ec);
        inst::ui::instPage::setInstBarPerc(100);
        inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.upload_complete"_lang);
        return true;
    }

    bool UploadSaveToServer(const std::string& remoteUrl, const std::string& user, const std::string& pass, const SaveSyncEntry& entry, const std::string& note, std::string& error)
    {
        return UploadSaveToServerForUser(remoteUrl, user, pass, nullptr, entry, note, error);
    }

    bool DownloadSaveToConsole(const std::string& remoteUrl, const std::string& user, const std::string& pass, const SaveSyncEntry& entry, const SaveSyncRemoteVersion* remoteVersion, std::string& error)
    {
        error.clear();
        if (entry.saveType == "unknown") {
            error = "This backup has no save type. Download it from AeroFoil and reupload it with Account or Device selected before restoring.";
            return false;
        }
        if (entry.titleId == 0) {
            error = "Invalid title ID for save download.";
            return false;
        }
        if (!entry.localAvailable) {
            error = "No local save container exists for this title and user. Launch the game once to create save data, then retry download.";
            return false;
        }

        const SaveSyncRemoteVersion* selectedRemoteVersion = remoteVersion;
        if (!selectedRemoteVersion && !entry.remoteVersions.empty())
            selectedRemoteVersion = &entry.remoteVersions.front();

        if (selectedRemoteVersion && !CanRestoreSaveType(selectedRemoteVersion->saveType, entry.saveType)) {
            error = "The backup save type does not match the local save. Restore canceled.";
            return false;
        }

        std::string selectedSaveId;
        std::string downloadUrl = entry.remoteDownloadUrl;
        if (selectedRemoteVersion) {
            if (!selectedRemoteVersion->saveId.empty())
                selectedSaveId = selectedRemoteVersion->saveId;
            if (!selectedRemoteVersion->downloadUrl.empty())
                downloadUrl = selectedRemoteVersion->downloadUrl;
        }
        if (downloadUrl.empty())
            downloadUrl = BuildDownloadUrl(remoteUrl, entry.titleId, selectedSaveId);
        if (downloadUrl.empty())
            downloadUrl = BuildDownloadUrl(remoteUrl, entry.titleId);
        if (downloadUrl.empty()) {
            error = "No download URL available for this save.";
            return false;
        }
        downloadUrl = ScopeSaveUrl(downloadUrl, entry.saveType);

        const std::string tempRoot = inst::config::appDir + "/save_sync_tmp";
        const std::filesystem::path archivePath = std::filesystem::path(tempRoot) / (FormatTitleIdHex(entry.titleId) + ".zip");
        std::error_code ec;
        std::filesystem::remove_all(tempRoot, ec);
        std::filesystem::create_directories(tempRoot, ec);

        inst::ui::instPage::setInstBarPerc(10);
        inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.starting_download"_lang);
        if (!HttpDownloadFileWithAuthAndProgress(downloadUrl, archivePath.string(), user, pass, 60000, entry.saveType, error)) {
            if (error.empty())
                error = "Failed to download save archive from server.";
            return false;
        }

        AccountUid uid = {};
        if (!entry.deviceSave && !ResolveActiveUser(uid, error))
            return false;

        if (!MountSaveDataForTitle(uid, entry.titleId, entry.deviceSave, false, error))
            return false;

        const std::string mountedPath = std::string(kSaveMountName) + ":/";
        inst::ui::instPage::setInstInfoText("inst.remote.save_sync.status.preparing_restore"_lang);
        inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.clearing_existing_files"_lang);
        if (!ClearDirectoryContents(mountedPath, error)) {
            fsdevUnmountDevice(kSaveMountName);
            return false;
        }
        inst::ui::instPage::setInstBarPerc(85);
        inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.applying_save_data"_lang);
        Result clearCommitRc = fsdevCommitDevice(kSaveMountName);
        if (R_FAILED(clearCommitRc)) {
            fsdevUnmountDevice(kSaveMountName);
            error = "Failed to commit cleared save data (" + FormatResultHex(clearCommitRc) + ").";
            return false;
        }
        fsdevUnmountDevice(kSaveMountName);

        // Match JKSV restore flow: remount fresh after wipe+commit, then import.
        if (!MountSaveDataForTitle(uid, entry.titleId, entry.deviceSave, false, error))
            return false;

        auto extractProgressCb = [](std::size_t index, std::size_t total, const std::string& relativePath) {
            const int percent = (total > 0) ? static_cast<int>((index * 100ULL) / total) : 0;
            const int uiPercent = 85 + static_cast<int>((14.0 * percent) / 100.0); // 85..99
            inst::ui::instPage::setInstBarPerc(uiPercent);
            inst::ui::instPage::setProgressDetailText(
                "inst.remote.save_sync.progress.restoring_fmt"_lang +
                std::to_string(index) + "/" + std::to_string(total) + ": " + inst::util::shortenString(relativePath, 72, false));
        };
        if (!ExtractZipToMountedSaveWithCommits(archivePath, mountedPath, kSaveMountName, error, extractProgressCb)) {
            fsdevUnmountDevice(kSaveMountName);
            return false;
        }

        Result rc = fsdevCommitDevice(kSaveMountName);
        fsdevUnmountDevice(kSaveMountName);
        if (R_FAILED(rc)) {
            error = "Failed to commit imported save data (" + FormatResultHex(rc) + ").";
            return false;
        }

        std::filesystem::remove_all(tempRoot, ec);
        inst::ui::instPage::setInstBarPerc(100);
        inst::ui::instPage::setProgressDetailText("inst.remote.save_sync.progress.complete"_lang);
        return true;
    }

    bool DeleteSaveFromServer(const std::string& remoteUrl, const std::string& user, const std::string& pass, const SaveSyncEntry& entry, const SaveSyncRemoteVersion* remoteVersion, std::string& error)
    {
        error.clear();
        if (entry.titleId == 0) {
            error = "Invalid title ID for save delete.";
            return false;
        }
        if (!entry.remoteAvailable && entry.remoteVersions.empty()) {
            error = "No remote save is available for this title.";
            return false;
        }

        const SaveSyncRemoteVersion* selectedRemoteVersion = remoteVersion;
        if (!selectedRemoteVersion && !entry.remoteVersions.empty())
            selectedRemoteVersion = &entry.remoteVersions.front();

        std::string selectedSaveId;
        if (selectedRemoteVersion && !selectedRemoteVersion->saveId.empty())
            selectedSaveId = selectedRemoteVersion->saveId;

        if (selectedRemoteVersion && NormalizeBackupSaveType(selectedRemoteVersion->saveType) != entry.saveType) {
            error = "The selected backup belongs to a different save type. Delete canceled.";
            return false;
        }
        std::string deleteUrl = BuildDeleteUrl(remoteUrl, entry.titleId, selectedSaveId);
        if (deleteUrl.empty())
            deleteUrl = BuildDeleteUrl(remoteUrl, entry.titleId);
        if (deleteUrl.empty()) {
            error = "No delete URL available for this save.";
            return false;
        }
        deleteUrl = ScopeSaveUrl(deleteUrl, entry.saveType);

        long responseCode = 0;
        std::string responseBody;
        if (!HttpDeleteWithAuth(deleteUrl, user, pass, responseCode, responseBody, error))
            return false;

        if (responseCode < 200 || responseCode >= 300) {
            if (responseCode == 401 || responseCode == 403) {
                error = "Save delete is not permitted for this account (backup access required).";
            } else if (responseCode == 404) {
                error = "Remote save backup was not found.";
            } else {
                std::ostringstream ss;
                ss << "Save delete failed with HTTP " << responseCode;
                if (!responseBody.empty())
                    ss << ": " << responseBody;
                error = ss.str();
            }
            return false;
        }

        return true;
    }
}


