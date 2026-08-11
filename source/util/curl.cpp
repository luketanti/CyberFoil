#include <curl/curl.h>
#include <string>
#include <sstream>
#include <iostream>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>
#include "util/curl.hpp"
#include "util/config.hpp"
#include "util/error.hpp"
#include "util/hauth.hpp"
#include "util/lang.hpp"
#include "util/uid.hpp"
#include "ui/instPage.hpp"

static size_t writeDataFile(void *ptr, size_t size, size_t nmemb, void *stream) {
  size_t written = fwrite(ptr, size, nmemb, (FILE *)stream);
  return written;
}

static bool isLikelyImageFile(const char *path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    unsigned char buf[12] = {};
    in.read(reinterpret_cast<char *>(buf), sizeof(buf));
    std::streamsize read = in.gcount();
    if (read >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF)
        return true;
    if (read >= 8 &&
        buf[0] == 0x89 && buf[1] == 0x50 && buf[2] == 0x4E && buf[3] == 0x47 &&
        buf[4] == 0x0D && buf[5] == 0x0A && buf[6] == 0x1A && buf[7] == 0x0A)
        return true;
    if (read >= 12 &&
        buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' &&
        buf[8] == 'W' && buf[9] == 'E' && buf[10] == 'B' && buf[11] == 'P')
        return true;
    return false;
}

namespace inst::curl {
    const std::string& getDefaultUserAgent() {
        static const std::string kDefaultUserAgent = "cyberfoil";
        return kDefaultUserAgent;
    }

    const std::string& getEmptyUserAgent() {
        static const std::string kEmptyUserAgent;
        return kEmptyUserAgent;
    }

    const std::string& getDownloadUserAgent() {
        static const std::string kChromeUserAgent =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";
        static const std::string kSafariUserAgent =
            "Mozilla/5.0 (iPhone; CPU iPhone OS 17_3 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.3 Mobile/15E148 Safari/604.1";
        static const std::string kFirefoxUserAgent =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:123.0) Gecko/20100101 Firefox/123.0";

        const std::string mode = inst::config::NormalizeHttpUserAgentMode(inst::config::httpUserAgentMode);
        if (mode == "chrome")
            return kChromeUserAgent;
        if (mode == "safari")
            return kSafariUserAgent;
        if (mode == "tinfoil")
            return getEmptyUserAgent();
        if (mode == "firefox")
            return kFirefoxUserAgent;
        if (mode == "custom")
            return inst::config::httpUserAgent;
        return getDefaultUserAgent();
    }

    const std::string& getUserAgent() {
        return getDownloadUserAgent();
    }
}

static void buildVersionAndRevision(std::string& outVersion, std::string& outRevision)
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

static std::vector<std::string> buildRemoteHeaders(const std::string& requestUrl, const std::string& user, const std::string& pass)
{
    std::string themeHeader = "Theme: 0000000000000000000000000000000000000000000000000000000000000000";
    std::string versionValue;
    std::string revisionValue;
    buildVersionAndRevision(versionValue, revisionValue);
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

size_t writeDataBuffer(char *ptr, size_t size, size_t nmemb, void *userdata) {
    std::ostringstream *stream = (std::ostringstream*)userdata;
    size_t count = size * nmemb;
    stream->write(ptr, count);
    return count;
}

int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    if (ultotal) {
        int uploadProgress = (int)(((double)ulnow / (double)ultotal) * 100.0);
        inst::ui::instPage::setInstBarPerc(uploadProgress);
    } else if (dltotal) {
        int downloadProgress = (int)(((double)dlnow / (double)dltotal) * 100.0);
        inst::ui::instPage::setInstBarPerc(downloadProgress);
    }
    return 0;
}

struct DownloadProgressContext {
    const inst::curl::DownloadProgressCallback* cb = nullptr;
    curl_off_t lastNow = -1;
    curl_off_t lastTotal = -1;
};

struct WriteAtOffsetContext {
    FILE* file = nullptr;
};

int progress_callback_file(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<DownloadProgressContext*>(clientp);
    if (ctx == nullptr || ctx->cb == nullptr || !(*ctx->cb)) {
        return 0;
    }

    if (ctx->lastNow == dlnow && ctx->lastTotal == dltotal) {
        return 0;
    }
    ctx->lastNow = dlnow;
    ctx->lastTotal = dltotal;

    const std::uint64_t now = (dlnow > 0) ? static_cast<std::uint64_t>(dlnow) : 0;
    const std::uint64_t total = (dltotal > 0) ? static_cast<std::uint64_t>(dltotal) : 0;
    (*ctx->cb)(now, total);
    return 0;
}

static size_t writeDataFileAtOffset(void *ptr, size_t size, size_t nmemb, void *stream) {
    auto* ctx = static_cast<WriteAtOffsetContext*>(stream);
    if (ctx == nullptr || ctx->file == nullptr)
        return 0;
    return fwrite(ptr, size, nmemb, ctx->file);
}

static constexpr long kDefaultConnectTimeoutMs = 30000;
static constexpr long kLowSpeedLimitBytesPerSec = 1;
static constexpr long kLowSpeedTimeSeconds = 15;
static constexpr long kFileDownloadCurlBufferSize = 512L * 1024L;
static constexpr std::size_t kFileDownloadIoBufferSize = 1024U * 1024U;

static bool ensureCurlGlobalInit() {
    static std::once_flag initFlag;
    static bool initOk = false;
    std::call_once(initFlag, []() {
        initOk = (curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
    });
    return initOk;
}

static void removeFileIfExistsNoThrow(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

static void applyCommonCurlOptions(CURL *curl_handle, const std::string& url, long timeout, bool writeProgress, bool forceNewConnection = false) {
    curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
    const std::string& userAgent = inst::curl::getDownloadUserAgent();
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_BUFFERSIZE, kFileDownloadCurlBufferSize);
    curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);
    // Do NOT set TCP_KEEPIDLE / TCP_KEEPINTVL (speed regression)

    if (forceNewConnection) {
        curl_easy_setopt(curl_handle, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(curl_handle, CURLOPT_FORBID_REUSE, 1L);
    }

    if (writeProgress) {
        curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, progress_callback);
    } else {
        curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);
    }

    if (timeout > 0) {
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, timeout);
        curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, timeout);
    } else {
        curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, kDefaultConnectTimeoutMs);
        curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedLimitBytesPerSec);
        curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, kLowSpeedTimeSeconds);
    }
}

static void applyBufferedFileIo(FILE* file) {
    if (file != nullptr)
        setvbuf(file, nullptr, _IOFBF, kFileDownloadIoBufferSize);
}

namespace inst::curl {
    bool downloadFile (const std::string ourUrl, const char *pagefilename, long timeout, bool writeProgress) {
        if (!ensureCurlGlobalInit()) {
            LOG_DEBUG("curl global init failed\n");
            return false;
        }

        CURL *curl_handle = curl_easy_init();
        if (curl_handle == nullptr) {
            LOG_DEBUG("curl_easy_init failed\n");
            return false;
        }

        applyCommonCurlOptions(curl_handle, ourUrl, timeout, writeProgress);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, writeDataFile);
        curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1L);

        FILE *pagefile = fopen(pagefilename, "wb");
        if (pagefile == nullptr) {
            LOG_DEBUG("Failed to open download output file: %s\n", pagefilename);
            curl_easy_cleanup(curl_handle);
            return false;
        }
        applyBufferedFileIo(pagefile);

        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, pagefile);
        const CURLcode result = curl_easy_perform(curl_handle);
        long responseCode = 0;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &responseCode);

        fclose(pagefile);
        curl_easy_cleanup(curl_handle);

        const bool ok = (result == CURLE_OK) && (responseCode >= 200 && responseCode < 300);
        if (ok)
            return true;

        removeFileIfExistsNoThrow(pagefilename);

        LOG_DEBUG("downloadFile failed rc=%s http=%ld url=%s\n", curl_easy_strerror(result), responseCode, ourUrl.c_str());
        return false;
    }

    bool downloadFileWithProgress(const std::string ourUrl, const char *pagefilename, long timeout, const DownloadProgressCallback& progressCb) {
        if (!ensureCurlGlobalInit()) {
            LOG_DEBUG("curl global init failed\n");
            return false;
        }

        CURL *curl_handle = curl_easy_init();
        if (curl_handle == nullptr) {
            LOG_DEBUG("curl_easy_init failed\n");
            return false;
        }

        applyCommonCurlOptions(curl_handle, ourUrl, timeout, false);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, writeDataFile);
        curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);

        DownloadProgressContext progressCtx{};
        progressCtx.cb = &progressCb;
        progressCtx.lastNow = -1;
        progressCtx.lastTotal = -1;
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, progress_callback_file);
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, &progressCtx);

        FILE *pagefile = fopen(pagefilename, "wb");
        if (pagefile == nullptr) {
            LOG_DEBUG("Failed to open download output file: %s\n", pagefilename);
            curl_easy_cleanup(curl_handle);
            return false;
        }
        applyBufferedFileIo(pagefile);

        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, pagefile);
        const CURLcode result = curl_easy_perform(curl_handle);
        long responseCode = 0;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &responseCode);

        fclose(pagefile);
        curl_easy_cleanup(curl_handle);

        const bool ok = (result == CURLE_OK) && (responseCode >= 200 && responseCode < 300);
        if (ok) {
            if (progressCb) {
                progressCb(progressCtx.lastNow > 0 ? static_cast<std::uint64_t>(progressCtx.lastNow) : 0,
                           progressCtx.lastTotal > 0 ? static_cast<std::uint64_t>(progressCtx.lastTotal) : 0);
            }
            return true;
        }

        removeFileIfExistsNoThrow(pagefilename);

        LOG_DEBUG("downloadFileWithProgress failed rc=%s http=%ld url=%s\n", curl_easy_strerror(result), responseCode, ourUrl.c_str());
        return false;
    }

    bool downloadFileRangeWithProgress(const std::string ourUrl, const char *pagefilename, std::uint64_t start, std::uint64_t endInclusive, long timeout, const DownloadProgressCallback& progressCb) {
        if (!ensureCurlGlobalInit()) {
            LOG_DEBUG("curl global init failed\n");
            return false;
        }

        if (endInclusive < start) {
            LOG_DEBUG("downloadFileRangeWithProgress invalid range start=%llu end=%llu\n",
                static_cast<unsigned long long>(start), static_cast<unsigned long long>(endInclusive));
            return false;
        }

        CURL *curl_handle = curl_easy_init();
        if (curl_handle == nullptr) {
            LOG_DEBUG("curl_easy_init failed\n");
            return false;
        }

        applyCommonCurlOptions(curl_handle, ourUrl, timeout, false);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, writeDataFile);
        curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);

        DownloadProgressContext progressCtx{};
        progressCtx.cb = &progressCb;
        progressCtx.lastNow = -1;
        progressCtx.lastTotal = -1;
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, progress_callback_file);
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, &progressCtx);

        const std::string range = std::to_string(start) + "-" + std::to_string(endInclusive);
        curl_easy_setopt(curl_handle, CURLOPT_RANGE, range.c_str());

        FILE *pagefile = fopen(pagefilename, "wb");
        if (pagefile == nullptr) {
            LOG_DEBUG("Failed to open ranged download output file: %s\n", pagefilename);
            curl_easy_cleanup(curl_handle);
            return false;
        }
        applyBufferedFileIo(pagefile);

        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, pagefile);
        const CURLcode result = curl_easy_perform(curl_handle);
        long responseCode = 0;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &responseCode);

        fclose(pagefile);
        curl_easy_cleanup(curl_handle);

        const bool ok = (result == CURLE_OK) && (responseCode == 206);
        if (ok) {
            if (progressCb) {
                progressCb(progressCtx.lastNow > 0 ? static_cast<std::uint64_t>(progressCtx.lastNow) : 0,
                    progressCtx.lastTotal > 0 ? static_cast<std::uint64_t>(progressCtx.lastTotal) : 0);
            }
            return true;
        }

        removeFileIfExistsNoThrow(pagefilename);

        LOG_DEBUG("downloadFileRangeWithProgress failed rc=%s http=%ld url=%s range=%s\n",
            curl_easy_strerror(result), responseCode, ourUrl.c_str(), range.c_str());
        return false;
    }

    bool downloadFileRangeToOffsetWithProgress(const std::string ourUrl, const char *pagefilename, std::uint64_t fileOffset, std::uint64_t start, std::uint64_t endInclusive, long timeout, const DownloadProgressCallback& progressCb) {
        if (!ensureCurlGlobalInit()) {
            LOG_DEBUG("curl global init failed\n");
            return false;
        }

        if (endInclusive < start) {
            LOG_DEBUG("downloadFileRangeToOffsetWithProgress invalid range start=%llu end=%llu\n",
                static_cast<unsigned long long>(start), static_cast<unsigned long long>(endInclusive));
            return false;
        }

        CURL *curl_handle = curl_easy_init();
        if (curl_handle == nullptr) {
            LOG_DEBUG("curl_easy_init failed\n");
            return false;
        }

        applyCommonCurlOptions(curl_handle, ourUrl, timeout, false);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, writeDataFileAtOffset);
        curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);

        DownloadProgressContext progressCtx{};
        progressCtx.cb = &progressCb;
        progressCtx.lastNow = -1;
        progressCtx.lastTotal = -1;
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, progress_callback_file);
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, &progressCtx);

        const std::string range = std::to_string(start) + "-" + std::to_string(endInclusive);
        curl_easy_setopt(curl_handle, CURLOPT_RANGE, range.c_str());

        FILE *pagefile = fopen(pagefilename, "r+b");
        if (pagefile == nullptr) {
            LOG_DEBUG("Failed to open ranged offset output file: %s\n", pagefilename);
            curl_easy_cleanup(curl_handle);
            return false;
        }
        applyBufferedFileIo(pagefile);

#if defined(_WIN32)
        if (_fseeki64(pagefile, static_cast<__int64>(fileOffset), SEEK_SET) != 0) {
#else
        if (fseeko(pagefile, static_cast<off_t>(fileOffset), SEEK_SET) != 0) {
#endif
            fclose(pagefile);
            curl_easy_cleanup(curl_handle);
            return false;
        }

        WriteAtOffsetContext writeCtx{};
        writeCtx.file = pagefile;
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &writeCtx);
        const CURLcode result = curl_easy_perform(curl_handle);
        long responseCode = 0;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &responseCode);

        fflush(pagefile);
        fclose(pagefile);
        curl_easy_cleanup(curl_handle);

        const bool ok = (result == CURLE_OK) && (responseCode == 206);
        if (ok) {
            if (progressCb) {
                progressCb(progressCtx.lastNow > 0 ? static_cast<std::uint64_t>(progressCtx.lastNow) : 0,
                    progressCtx.lastTotal > 0 ? static_cast<std::uint64_t>(progressCtx.lastTotal) : 0);
            }
            return true;
        }

        LOG_DEBUG("downloadFileRangeToOffsetWithProgress failed rc=%s http=%ld url=%s range=%s\n",
            curl_easy_strerror(result), responseCode, ourUrl.c_str(), range.c_str());
        return false;
    }

    bool downloadFileWithAuth(const std::string ourUrl, const char *pagefilename, const std::string& user, const std::string& pass, long timeout) {
        if (!ensureCurlGlobalInit()) {
            LOG_DEBUG("curl global init failed\n");
            return false;
        }

        CURL *curl_handle = curl_easy_init();
        if (curl_handle == nullptr) {
            LOG_DEBUG("curl_easy_init failed\n");
            return false;
        }

        applyCommonCurlOptions(curl_handle, ourUrl, timeout, false);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, writeDataFile);
        curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1L);

        struct curl_slist* headerList = nullptr;
        const auto headers = buildRemoteHeaders(ourUrl, user, pass);
        for (const auto& header : headers)
            headerList = curl_slist_append(headerList, header.c_str());
        if (headerList)
            curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headerList);

        if (!user.empty() || !pass.empty()) {
            std::string authValue = user + ":" + pass;
            curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl_handle, CURLOPT_USERPWD, authValue.c_str());
        }

        FILE *pagefile = fopen(pagefilename, "wb");
        if (pagefile == nullptr) {
            LOG_DEBUG("Failed to open download output file: %s\n", pagefilename);
            curl_easy_cleanup(curl_handle);
            return false;
        }

        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, pagefile);
        const CURLcode result = curl_easy_perform(curl_handle);
        long responseCode = 0;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &responseCode);

        fclose(pagefile);
        if (headerList)
            curl_slist_free_all(headerList);
        curl_easy_cleanup(curl_handle);

        const bool ok = (result == CURLE_OK) && (responseCode >= 200 && responseCode < 300);
        if (ok)
            return true;

        removeFileIfExistsNoThrow(pagefilename);

        LOG_DEBUG("downloadFileWithAuth failed rc=%s http=%ld url=%s\n", curl_easy_strerror(result), responseCode, ourUrl.c_str());
        return false;
    }

    bool downloadImageWithAuth(const std::string ourUrl, const char *pagefilename, const std::string& user, const std::string& pass, long timeout) {
        if (!ensureCurlGlobalInit()) {
            LOG_DEBUG("curl global init failed\n");
            return false;
        }

        CURL *curl_handle = curl_easy_init();
        if (curl_handle == nullptr) {
            LOG_DEBUG("curl_easy_init failed\n");
            return false;
        }

        applyCommonCurlOptions(curl_handle, ourUrl, timeout, false);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, writeDataFile);
        curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1L);

        struct curl_slist* headerList = nullptr;
        const auto headers = buildRemoteHeaders(ourUrl, user, pass);
        for (const auto& header : headers)
            headerList = curl_slist_append(headerList, header.c_str());
        if (headerList)
            curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headerList);

        FILE *pagefile = fopen(pagefilename, "wb");
        if (pagefile == nullptr) {
            LOG_DEBUG("Failed to open image output file: %s\n", pagefilename);
            if (headerList)
                curl_slist_free_all(headerList);
            curl_easy_cleanup(curl_handle);
            return false;
        }
        applyBufferedFileIo(pagefile);

        long responseCode = 0;
        char* contentType = nullptr;

        if (!user.empty() || !pass.empty()) {
            std::string authValue = user + ":" + pass;
            curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl_handle, CURLOPT_USERPWD, authValue.c_str());
        }

        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, pagefile);
        const CURLcode result = curl_easy_perform(curl_handle);
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &responseCode);
        curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &contentType);

        fclose(pagefile);
        if (headerList)
            curl_slist_free_all(headerList);
        curl_easy_cleanup(curl_handle);

        bool ok = (result == CURLE_OK) && (responseCode >= 200 && responseCode < 300);
        if (ok) {
            bool typeOk = (contentType != nullptr) && (std::strncmp(contentType, "image/", 6) == 0);
            if (!typeOk)
                typeOk = isLikelyImageFile(pagefilename);
            ok = typeOk;
        }
        if (!ok)
            removeFileIfExistsNoThrow(pagefilename);
        if (!ok)
            LOG_DEBUG(curl_easy_strerror(result));
        return ok;
    }

    std::string downloadToBuffer (const std::string ourUrl, int firstRange, int secondRange, long timeout) {
        if (!ensureCurlGlobalInit()) {
            LOG_DEBUG("curl global init failed\n");
            return "";
        }

        CURL *curl_handle = curl_easy_init();
        if (curl_handle == nullptr) {
            LOG_DEBUG("curl_easy_init failed\n");
            return "";
        }

        std::ostringstream stream;
        applyCommonCurlOptions(curl_handle, ourUrl, timeout, false);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, writeDataBuffer);
        std::string ourRange;
        if (firstRange && secondRange) {
            ourRange = std::to_string(firstRange) + "-" + std::to_string(secondRange);
            curl_easy_setopt(curl_handle, CURLOPT_RANGE, ourRange.c_str());
        }
        
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &stream);
        const CURLcode result = curl_easy_perform(curl_handle);
        long responseCode = 0;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &responseCode);
        
        curl_easy_cleanup(curl_handle);

        if ((result == CURLE_OK) && (responseCode >= 200 && responseCode < 300))
            return stream.str();

        LOG_DEBUG("downloadToBuffer failed rc=%s http=%ld url=%s\n", curl_easy_strerror(result), responseCode, ourUrl.c_str());
        return "";
    }
}

