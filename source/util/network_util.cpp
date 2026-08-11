/*
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "util/network_util.hpp"

#include <switch.h>
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <limits>
#include "util/curl.hpp"
#include "util/error.hpp"
#include "util/hauth.hpp"
#include "util/uid.hpp"
#include "util/lang.hpp"
#include "util/config.hpp"
#include "ui/instPage.hpp"
#include "ui/MainApplication.hpp"

namespace inst::ui {
    extern MainApplication *mainApp;
}

namespace tin::network
{
    static std::string g_basic_auth_user;
    static std::string g_basic_auth_pass;
    static bool g_basic_auth_set = false;

    static void ApplyBasicAuth(CURL* curl, std::string& authValue)
    {
        if (!g_basic_auth_set)
            return;

        authValue = g_basic_auth_user + ":" + g_basic_auth_pass;
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERPWD, authValue.c_str());
    }

    static std::string TrimCopy(const std::string& in)
    {
        size_t start = 0;
        while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start])))
            start++;

        size_t end = in.size();
        while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1])))
            end--;

        return in.substr(start, end - start);
    }

    static bool IsDigitsOnly(const std::string& in)
    {
        if (in.empty())
            return false;
        for (unsigned char c : in) {
            if (c < '0' || c > '9')
                return false;
        }
        return true;
    }

    static std::string StripUrlFragment(const std::string& in)
    {
        const auto pos = in.find('#');
        if (pos == std::string::npos)
            return in;
        return in.substr(0, pos);
    }

    static bool StartsWithNoCase(const std::string& text, const char* prefix)
    {
        size_t i = 0;
        while (prefix[i] != '\0') {
            if (i >= text.size())
                return false;
            const unsigned char a = static_cast<unsigned char>(text[i]);
            const unsigned char b = static_cast<unsigned char>(prefix[i]);
            if (std::tolower(a) != std::tolower(b))
                return false;
            i++;
        }
        return true;
    }

    static bool ParseUnsignedSize(const std::string& text, size_t& out)
    {
        if (text.empty())
            return false;

        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
        if (end == text.c_str() || (end && *end != '\0'))
            return false;
        if (parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max()))
            return false;

        out = static_cast<size_t>(parsed);
        return true;
    }

    static void BuildVersionAndRevision(std::string& outVersion, std::string& outRevision)
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

    static std::string UrlDecode(const std::string& value)
    {
        CURL* curl = curl_easy_init();
        if (!curl)
            return value;

        int outLength = 0;
        char* decoded = curl_easy_unescape(curl, value.c_str(), value.size(), &outLength);
        std::string result = decoded ? std::string(decoded, outLength) : value;
        if (decoded)
            curl_free(decoded);
        curl_easy_cleanup(curl);
        return result;
    }

    struct StreamCallbackContext
    {
        std::function<size_t (u8* bytes, size_t size)>* streamFunc = nullptr;
        bool hadException = false;
    };

    static size_t ParseHTMLDataCallback(char* bytes, size_t size, size_t numItems, void* userData)
    {
        auto* ctx = reinterpret_cast<StreamCallbackContext*>(userData);
        if (!ctx || !ctx->streamFunc)
            return 0;

        if (inst::ui::instPage::isInstallCancelRequested())
            return 0;

        const size_t numBytes = size * numItems;
        try {
            if (*ctx->streamFunc != nullptr)
                return (*ctx->streamFunc)((u8*)bytes, numBytes);
            return numBytes;
        } catch (...) {
            ctx->hadException = true;
            return 0;
        }
    }

    static int StreamHttpRangeForUrl(const std::string& url, size_t offset, size_t size,
        const std::function<size_t (u8* bytes, size_t size)>& streamFunc)
    {
        if (size == 0)
            return 0;

        const std::string requestUrl = TrimCopy(StripUrlFragment(url));
        auto writeDataFunc = streamFunc;
        StreamCallbackContext callbackCtx;
        callbackCtx.streamFunc = &writeDataFunc;
        CURL* curl = curl_easy_init();
        if (!curl)
            THROW_FORMAT("Failed to initialize curl\n");

        std::stringstream ss;
        ss << offset << "-" << (offset + size - 1);
        const auto range = ss.str();

        curl_easy_setopt(curl, CURLOPT_URL, requestUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        const std::string& userAgent = inst::curl::getUserAgent();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
        curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &callbackCtx);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &ParseHTMLDataCallback);

        // Larger receive buffer (throughput)
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024L);

        // Stall at ~0 B/s → abort this attempt (~15s), then StreamDataRange retries
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 30000L);

        // New TCP each range attempt (don't reuse a half-dead connection)
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        // Do NOT set TCP_KEEPIDLE / TCP_KEEPINTVL

        std::string authValue;
        ApplyBasicAuth(curl, authValue);

        struct curl_slist* headerList = nullptr;
        std::string versionValue;
        std::string revisionValue;
        BuildVersionAndRevision(versionValue, revisionValue);
        const std::string themeHeader = "Theme: 0000000000000000000000000000000000000000000000000000000000000000";
        const std::string uidHeader = "UID: " + inst::util::ComputeUidFromMmcCid();
        const std::string versionHeader = "Version: " + versionValue;
        const std::string revisionHeader = "Revision: " + revisionValue;
        const std::string languageHeader = "Language: " + Language::GetRemoteHeaderLanguage();
        const std::string hauthHeader = "HAUTH: " + inst::util::ComputeHauthFromUrl(requestUrl);
        const std::string uauthHeader = "UAUTH: " + inst::util::ComputeUauthFromUrl(
            requestUrl,
            g_basic_auth_set ? g_basic_auth_user : "",
            g_basic_auth_set ? g_basic_auth_pass : "");
        headerList = curl_slist_append(headerList, themeHeader.c_str());
        headerList = curl_slist_append(headerList, languageHeader.c_str());
        headerList = curl_slist_append(headerList, hauthHeader.c_str());
        headerList = curl_slist_append(headerList, uidHeader.c_str());
        headerList = curl_slist_append(headerList, versionHeader.c_str());
        headerList = curl_slist_append(headerList, revisionHeader.c_str());
        headerList = curl_slist_append(headerList, uauthHeader.c_str());
        if (headerList)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

        const CURLcode rc = curl_easy_perform(curl);
        u64 httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        if (headerList)
            curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);

        if (callbackCtx.hadException)
            return 1999;

        if (rc == CURLE_OK && httpCode == 206)
            return 0;

        LOG_DEBUG("Range request failed url=%s range=%s http=%lu curl=%d (%s)\n",
            requestUrl.c_str(), range.c_str(), httpCode, (int)rc, curl_easy_strerror(rc));

        if (httpCode != 0 && httpCode != 206)
            return static_cast<int>(httpCode);

        // e.g. CURLE_OPERATION_TIMEDOUT from LOW_SPEED → retriable in StreamDataRange
        return 1000 + static_cast<int>(rc);
    }

    HTTPHeader::HTTPHeader(std::string url) :
        m_url(url)
    {
    }

    size_t HTTPHeader::ParseHTMLHeader(char* bytes, size_t size, size_t numItems, void* userData)
    {
        HTTPHeader* header = reinterpret_cast<HTTPHeader*>(userData);
        size_t numBytes = size * numItems;
        std::string line(bytes, numBytes);

        line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

        if (!line.empty())
        {
            auto keyEnd = line.find(": ");

            if (keyEnd != 0)
            {
                std::string key = line.substr(0, keyEnd);
                std::string value = line.substr(keyEnd + 2);

                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                header->m_values[key] = value;
            }
        }

        return numBytes;
    }

    void HTTPHeader::PerformRequest()
    {
        m_values.clear();

        CURL* curl = curl_easy_init();
        CURLcode rc = (CURLcode)0;

        if (!curl)
        {
            THROW_FORMAT("Failed to initialize curl\n");
        }

        curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, true);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
        const std::string& userAgent = inst::curl::getUserAgent();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &tin::network::HTTPHeader::ParseHTMLHeader);
        std::string authValue;
        ApplyBasicAuth(curl, authValue);

        rc = curl_easy_perform(curl);
        if (rc != CURLE_OK)
        {
            THROW_FORMAT("Failed to retrieve HTTP Header: %s\n", curl_easy_strerror(rc));
        }

        u64 httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(curl);

        if (httpCode != 200 && httpCode != 204)
        {
            THROW_FORMAT("Unexpected HTTP response code when retrieving header: %lu\n", httpCode);
        }
    }

    bool HTTPHeader::HasValue(std::string key)
    {
        return m_values.count(key);
    }

    std::string HTTPHeader::GetValue(std::string key)
    {
        return m_values[key];
    }

    HTTPDownload::HTTPDownload(std::string url) :
        m_url(url), m_header(url)
    {
        m_url = TrimCopy(m_url);
        const bool isJbod = StartsWithNoCase(m_url, "jbod:");
        if (isJbod) {
            m_isJbod = true;

            const std::string payload = m_url.substr(5);
            std::stringstream ss(payload);
            std::string token;
            std::vector<std::string> tokens;
            while (std::getline(ss, token, '/')) {
                if (!token.empty())
                    tokens.push_back(token);
            }

            if (tokens.size() < 2)
                THROW_FORMAT("Invalid JBOD URL format\n");

            size_t defaultChunkSize = 0;
            if (!ParseUnsignedSize(tokens[0], defaultChunkSize) || defaultChunkSize == 0)
                THROW_FORMAT("Invalid JBOD chunk size\n");

            size_t runningOffset = 0;
            bool sawAnyUrl = false;
            bool hasPendingSizeOverride = false;
            size_t pendingSizeOverride = 0;
            for (size_t i = 1; i < tokens.size(); i++) {
                if (IsDigitsOnly(tokens[i])) {
                    if (!ParseUnsignedSize(tokens[i], pendingSizeOverride) || pendingSizeOverride == 0)
                        THROW_FORMAT("Invalid JBOD part size override\n");
                    hasPendingSizeOverride = true;
                    continue;
                }

                std::string partUrl = UrlDecode(tokens[i]);
                partUrl = TrimCopy(StripUrlFragment(partUrl));
                if (partUrl.rfind("http://", 0) != 0 && partUrl.rfind("https://", 0) != 0)
                    THROW_FORMAT("Invalid JBOD part URL\n");

                const bool explicitSize = hasPendingSizeOverride;
                const size_t partSize = explicitSize ? pendingSizeOverride : defaultChunkSize;
                if (runningOffset > std::numeric_limits<size_t>::max() - partSize)
                    THROW_FORMAT("JBOD part sizes overflow\n");

                m_jbodSegments.push_back({partUrl, runningOffset, partSize, false});
                runningOffset += partSize;
                sawAnyUrl = true;
                hasPendingSizeOverride = false;
                pendingSizeOverride = 0;
            }

            if (!sawAnyUrl)
                THROW_FORMAT("Invalid JBOD URL format (no parts)\n");
            if (hasPendingSizeOverride)
                THROW_FORMAT("Invalid JBOD URL format (dangling size override)\n");

            m_jbodSize = runningOffset;
            if (!m_jbodSegments.empty()) {
                m_jbodSegments.back().openEnded = true;
                m_jbodSize = std::numeric_limits<size_t>::max();
            }
            m_rangesSupported = true;
            return;
        }

        m_rangesSupported = true;
    }

    size_t HTTPDownload::ParseHTMLData(char* bytes, size_t size, size_t numItems, void* userData)
    {
        auto streamFunc = *reinterpret_cast<std::function<size_t (u8* bytes, size_t size)>*>(userData);
        const size_t numBytes = size * numItems;
        try {
            if (streamFunc != nullptr)
                return streamFunc((u8*)bytes, numBytes);
            return numBytes;
        } catch (...) {
            return 0;
        }
    }

    void HTTPDownload::BufferDataRange(void* buffer, size_t offset, size_t size, std::function<void (size_t sizeRead)> progressFunc)
    {
        size_t sizeRead = 0;

        auto streamFunc = [&](u8* streamBuf, size_t streamBufSize) -> size_t
        {
            if (sizeRead + streamBufSize > size)
            {
                LOG_DEBUG("New read size 0x%lx would exceed total expected size 0x%lx\n", sizeRead + streamBufSize, size);
                return 0;
            }

            if (progressFunc != nullptr)
                progressFunc(sizeRead);

            memcpy(reinterpret_cast<u8*>(buffer) + sizeRead, streamBuf, streamBufSize);
            sizeRead += streamBufSize;
            return streamBufSize;
        };

        const int rc = this->StreamDataRange(offset, size, streamFunc);
        if (rc != 0 || sizeRead != size)
        {
            THROW_FORMAT("HTTP range read failed (rc=%d)\n", rc);
        }
    }

    int HTTPDownload::StreamDataRange(size_t offset, size_t size, std::function<size_t (u8* bytes, size_t size)> streamFunc, std::function<bool()> retryConfirmFunc)
    {
        if (size == 0)
            return 0;

        if (!m_rangesSupported)
            THROW_FORMAT("Attempted range request when ranges aren't supported!\n");

        static constexpr int kMaxRetries = 15;
        static constexpr u64 kRetryDelayNs = 2500000000ULL;

        auto streamWithRetry = [&](const std::string& url, size_t requestOffset, size_t requestSize) -> int
        {
            size_t bytesReceived = 0;

            auto trackingFunc = [&](u8* buf, size_t sz) -> size_t {
                size_t written = streamFunc(buf, sz);
                bytesReceived += written;
                return written;
            };

            while (true)
            {
                for (int attempt = 0; attempt <= kMaxRetries; attempt++)
                {
                    if (attempt > 0)
                    {
                        LOG_DEBUG("StreamDataRange: retry %d/%d, resuming at offset %zu+%zu\n",
                            attempt, kMaxRetries, requestOffset, bytesReceived);
                        svcSleepThread(kRetryDelayNs);
                    }

                    const size_t currentOffset = requestOffset + bytesReceived;
                    const size_t remaining = requestSize - bytesReceived;

                    if (remaining == 0)
                        return 0;

                    const int rc = StreamHttpRangeForUrl(url, currentOffset, remaining, trackingFunc);
                    if (rc == 0)
                        return 0;

                    const bool fatal =
                        rc == 1999 ||
                        rc == 401 ||
                        rc == 403 ||
                        rc == 404 ||
                        rc == 416 ||
                        rc == 1000 + CURLE_WRITE_ERROR;

                    if (fatal)
                    {
                        LOG_DEBUG("StreamDataRange: fatal error, aborting (url=%s rc=%d)\n",
                            url.c_str(), rc);
                        return 1;
                    }

                    LOG_DEBUG("StreamDataRange: retriable error (url=%s rc=%d), %d retries left\n",
                        url.c_str(), rc, kMaxRetries - attempt);
                }

                LOG_DEBUG("StreamDataRange: auto-retries exhausted for %s\n", url.c_str());
                if (retryConfirmFunc && retryConfirmFunc())
                {
                    LOG_DEBUG("StreamDataRange: user requested another retry cycle for %s\n", url.c_str());
                    continue;
                }
                break;
            }

            return 1;
        };

        if (!m_isJbod)
            return streamWithRetry(m_url, offset, size);

        size_t globalOffset = offset;
        size_t remaining = size;

        while (remaining > 0) {
            auto it = std::find_if(m_jbodSegments.begin(), m_jbodSegments.end(),
                [globalOffset](const JbodSegment& seg) {
                    if (globalOffset < seg.offset)
                        return false;
                    if (seg.openEnded)
                        return true;
                    return globalOffset < (seg.offset + seg.size);
                });
            if (it == m_jbodSegments.end()) {
                if (!m_jbodSegments.empty()) {
                    auto last = m_jbodSegments.end() - 1;
                    if (last->openEnded && globalOffset >= last->offset) {
                        it = last;
                    }
                }
                if (it == m_jbodSegments.end())
                    THROW_FORMAT("JBOD segment lookup failed\n");
            }

            const size_t localOffset = globalOffset - it->offset;
            size_t chunkRemaining = 0;
            if (it->openEnded) {
                chunkRemaining = remaining;
            } else {
                if (localOffset >= it->size)
                    THROW_FORMAT("JBOD segment offset out of range\n");
                chunkRemaining = it->size - localOffset;
            }
            const size_t readNow = std::min(remaining, chunkRemaining);

            const int rc = streamWithRetry(it->url, localOffset, readNow);
            if (rc != 0)
                return rc;

            globalOffset += readNow;
            remaining -= readNow;
        }

        return 0;
    }

    void SetBasicAuth(const std::string& user, const std::string& pass)
    {
        g_basic_auth_user = user;
        g_basic_auth_pass = pass;
        g_basic_auth_set = true;
    }

    void ClearBasicAuth()
    {
        g_basic_auth_user.clear();
        g_basic_auth_pass.clear();
        g_basic_auth_set = false;
    }

    size_t WaitReceiveNetworkData(int sockfd, void* buf, size_t len)
    {
        int ret = 0;
        size_t read = 0;
        u64 lastRenderTick = armGetSystemTick();
        const u64 renderInterval = armGetSystemTickFreq() / 4;

        while ((((ret = recv(sockfd, (u8*)buf + read, len - read, 0)) > 0 && (read += ret) < len) || errno == EAGAIN))
        {
            errno = 0;
            inst::ui::mainApp->RefreshInputDevice();
            const u64 now = armGetSystemTick();
            if (now - lastRenderTick >= renderInterval) {
                lastRenderTick = now;
                inst::ui::mainApp->CallForRender();
            }
        }

        return read;
    }

    size_t WaitSendNetworkData(int sockfd, void* buf, size_t len)
    {
        int ret = 0;
        size_t written = 0;

        while (written < len)
        {
            inst::ui::mainApp->RefreshInputDevice();
            inst::ui::mainApp->UpdateButtons();
            u64 kDown = inst::ui::mainApp->GetButtonsDown();
            if (kDown & HidNpadButton_B)
                break;

            errno = 0;
            ret = send(sockfd, (u8*)buf + written, len - written, 0);

            if (ret < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    sleep(5);
                    continue;
                }
                break;
            }

            written += ret;
        }

        return written;
    }

    void NSULDrop(std::string url)
    {
        CURL* curl = curl_easy_init();

        if (!curl)
        {
            THROW_FORMAT("Failed to initialize curl\n");
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DROP");
        const std::string& userAgent = inst::curl::getUserAgent();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 50);

        curl_easy_perform(curl);

        curl_easy_cleanup(curl);
    }
}

