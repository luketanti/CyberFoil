#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <filesystem>
#include <limits>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <zlib.h>
#include <zstd.h>
#include <mbedtls/aes.h>
#include "shopInstall.hpp"
#include "install/http_nsp.hpp"
#include "install/http_xci.hpp"
#include "install/install.hpp"
#include "install/install_nsp.hpp"
#include "install/install_xci.hpp"
#include "nx/nca_writer.h"
#include "util/file_util.hpp"
#include "util/offline_title_db.hpp"
#include "util/title_util.hpp"
#include "ui/MainApplication.hpp"
#include "ui/instPage.hpp"
#include "util/config.hpp"
#include "util/curl.hpp"
#include "util/error.hpp"
#include "util/hauth.hpp"
#include "util/install_diagnostics.hpp"
#include "util/install_queue.hpp"
#include "util/json.hpp"
#include "util/lang.hpp"
#include "util/network_util.hpp"
#include "util/uid.hpp"
#include "util/util.hpp"

#ifndef HAVE_LIB_BLOB
#define HAVE_LIB_BLOB 0
#endif

extern "C" {
    bool z9f1(const void* wrappedKey, std::size_t wrappedLen, void* outAesKey16) __attribute__((weak));
}

namespace inst::ui {
    extern MainApplication *mainApp;
}

namespace {
    std::string FormatOneDecimal(double value)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", value);
        return std::string(buf);
    }

    std::string FormatEta(std::uint64_t totalSeconds)
    {
        const std::uint64_t h = totalSeconds / 3600;
        const std::uint64_t m = (totalSeconds % 3600) / 60;
        const std::uint64_t s = totalSeconds % 60;
        char buf[32];
        if (h > 0) {
            std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                static_cast<unsigned long long>(h),
                static_cast<unsigned long long>(m),
                static_cast<unsigned long long>(s));
        } else {
            std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                static_cast<unsigned long long>(m),
                static_cast<unsigned long long>(s));
        }
        return std::string(buf);
    }

    size_t WriteToString(char* ptr, size_t size, size_t numItems, void* userdata)
    {
        auto out = reinterpret_cast<std::string*>(userdata);
        out->append(ptr, size * numItems);
        return size * numItems;
    }

    std::string NormalizeShopUrl(std::string url)
    {
        url.erase(0, url.find_first_not_of(" \t\r\n"));
        url.erase(url.find_last_not_of(" \t\r\n") + 1);
        if (url.empty())
            return url;
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
            url = "http://" + url;
        if (!url.empty() && url.back() == '/')
            url.pop_back();
        return url;
    }

    int HexNibble(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F')
            return 10 + (c - 'A');
        return -1;
    }

    std::string DecodeUrlSegment(const std::string& value)
    {
        std::string out;
        out.reserve(value.size());

        for (std::size_t i = 0; i < value.size(); i++) {
            const char c = value[i];
            if (c == '%' && (i + 2) < value.size()) {
                const int hi = HexNibble(value[i + 1]);
                const int lo = HexNibble(value[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }

            // Keep compatibility with common form-style encoding for display names.
            if (c == '+') {
                out.push_back(' ');
                continue;
            }

            out.push_back(c);
        }

        return out;
    }

    bool IsLikelyLegacyPayloadAt(const std::string& body, std::size_t offset)
    {
        constexpr std::size_t kHeaderSize = 0x110;
        constexpr std::uint8_t kCompressionNone = 0x00;
        constexpr std::uint8_t kCompressionZstd = 0x0D;
        constexpr std::uint8_t kCompressionZlib = 0x0E;

        if (offset > body.size())
            return false;
        if ((body.size() - offset) < kHeaderSize)
            return false;
        if (body.compare(offset, 7, "TINFOIL") != 0)
            return false;

        const std::uint8_t info = static_cast<std::uint8_t>(body[offset + 7]);
        const std::uint8_t compression = info & 0x0F;
        if (compression != kCompressionNone &&
            compression != kCompressionZstd &&
            compression != kCompressionZlib) {
            return false;
        }

        // Header looks structurally valid enough to treat as legacy payload.
        return true;
    }

    bool FindLegacyPayloadOffset(const std::string& body, std::size_t& outOffset)
    {
        outOffset = std::string::npos;
        if (body.empty())
            return false;

        if (IsLikelyLegacyPayloadAt(body, 0)) {
            outOffset = 0;
            return true;
        }

        // Some servers/proxies prepend BOM/whitespace/NUL bytes before legacy payload.
        std::size_t start = 0;
        if (body.size() >= 3 &&
            static_cast<unsigned char>(body[0]) == 0xEF &&
            static_cast<unsigned char>(body[1]) == 0xBB &&
            static_cast<unsigned char>(body[2]) == 0xBF) {
            start = 3;
        }

        while (start < body.size()) {
            const char c = body[start];
            if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                start++;
                continue;
            }
            break;
        }

        if (start < body.size() && IsLikelyLegacyPayloadAt(body, start)) {
            outOffset = start;
            return true;
        }

        return false;
    }

    void BuildVersionAndRevision(std::string& outVersion, std::string& outRevision)
    {
        const std::string raw = inst::config::shopLegacyMode ? "20.0.2" : inst::config::appVersion;
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

    std::vector<std::string> BuildLegacyHeaders(const std::string& requestUrl, const std::string& user, const std::string& pass)
    {
        if (!inst::util::HasLegacyAuthSupport())
            return {};

        std::string themeHeader = "Theme: 0000000000000000000000000000000000000000000000000000000000000000";
        std::string versionValue;
        std::string revisionValue;
        BuildVersionAndRevision(versionValue, revisionValue);
        std::string versionHeader = "Version: " + versionValue;
        std::string revisionHeader = "Revision: " + revisionValue;
        std::string languageHeader = "Language: " + Language::GetShopHeaderLanguage();
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

    constexpr std::size_t kLegacyHeaderSize = 0x110;
    constexpr std::size_t kLegacyWrappedKeySize = 0x100;
    constexpr std::uint8_t kLegacyEncryptedFlag = 0xF0;
    constexpr std::uint8_t kLegacyCompressionNone = 0x00;
    constexpr std::uint8_t kLegacyCompressionZstd = 0x0D;
    constexpr std::uint8_t kLegacyCompressionZlib = 0x0E;

    std::uint64_t ReadLeU64(const std::string& body, std::size_t offset)
    {
        if (offset + 8 > body.size())
            return 0;
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; i++)
            value |= (static_cast<std::uint64_t>(static_cast<std::uint8_t>(body[offset + i])) << (i * 8));
        return value;
    }

    bool InflatePayloadWithWindowBits(const std::vector<std::uint8_t>& input, int windowBits, std::vector<std::uint8_t>& output)
    {
        z_stream stream{};
        stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
        stream.avail_in = static_cast<uInt>(input.size());

        if (inflateInit2(&stream, windowBits) != Z_OK)
            return false;

        std::vector<std::uint8_t> decoded;
        decoded.reserve(input.size() * 2);
        std::uint8_t chunk[8192];
        int status = Z_OK;

        while (status == Z_OK) {
            stream.next_out = reinterpret_cast<Bytef*>(chunk);
            stream.avail_out = static_cast<uInt>(sizeof(chunk));
            status = inflate(&stream, Z_NO_FLUSH);
            const std::size_t produced = sizeof(chunk) - stream.avail_out;
            if (produced > 0)
                decoded.insert(decoded.end(), chunk, chunk + produced);
        }

        inflateEnd(&stream);
        if (status != Z_STREAM_END)
            return false;

        output = std::move(decoded);
        return true;
    }

    bool TryPkcs7Unpad(const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& output)
    {
        output.clear();
        if (input.empty())
            return false;

        const std::uint8_t pad = input.back();
        if (pad == 0 || pad > 16 || pad > input.size())
            return false;
        for (std::size_t i = input.size() - pad; i < input.size(); i++) {
            if (input[i] != pad)
                return false;
        }

        output.assign(input.begin(), input.end() - pad);
        return true;
    }

    std::vector<std::uint8_t> TrimTrailingZeros(const std::vector<std::uint8_t>& input)
    {
        std::vector<std::uint8_t> out = input;
        while (!out.empty() && out.back() == 0)
            out.pop_back();
        return out;
    }

    bool DecompressZstdFlexible(const std::vector<std::uint8_t>& input, std::uint64_t expectedSize, std::vector<std::uint8_t>& output)
    {
        output.clear();
        std::size_t srcOffset = 0;
        const std::size_t total = input.size();

        while (srcOffset < total) {
            while (srcOffset < total && input[srcOffset] == 0)
                srcOffset++;
            if (srcOffset >= total)
                break;

            const std::size_t frameSize = ZSTD_findFrameCompressedSize(input.data() + srcOffset, total - srcOffset);
            if (ZSTD_isError(frameSize) || frameSize == 0 || (srcOffset + frameSize) > total)
                return false;

            const unsigned long long frameContentSize = ZSTD_getFrameContentSize(input.data() + srcOffset, frameSize);
            if (frameContentSize == ZSTD_CONTENTSIZE_ERROR || frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN)
                return false;
            if (frameContentSize > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
                return false;

            const std::size_t writeOffset = output.size();
            output.resize(writeOffset + static_cast<std::size_t>(frameContentSize));
            const std::size_t rc = ZSTD_decompress(
                output.data() + writeOffset,
                static_cast<std::size_t>(frameContentSize),
                input.data() + srcOffset,
                frameSize);
            if (ZSTD_isError(rc) || rc != frameContentSize)
                return false;

            srcOffset += frameSize;
        }

        if (expectedSize > 0 && output.empty())
            return false;
        return true;
    }

    bool TryUnwrapLegacyAesKey(const std::uint8_t* wrappedKey, std::vector<std::uint8_t>& outAesKey)
    {
        outAesKey.clear();
        if (wrappedKey == nullptr)
            return false;
        if (z9f1 == nullptr)
            return false;

        std::array<std::uint8_t, 16> unwrapped{};
        const bool ok = z9f1(wrappedKey, kLegacyWrappedKeySize, unwrapped.data());
        if (!ok) {
            inst::util::SecureWipe(unwrapped.data(), unwrapped.size());
            return false;
        }

        outAesKey.assign(unwrapped.begin(), unwrapped.end());
        inst::util::SecureWipe(unwrapped.data(), unwrapped.size());
        return true;
    }

    bool DecodeLegacyPayload(const std::string& body, std::string& outDecoded, std::string& outError)
    {
        if (body.rfind("TINFOIL", 0) != 0) {
            outDecoded = body;
            return true;
        }

        if (body.size() < kLegacyHeaderSize) {
            outError = "Encrypted shop response is truncated.";
            return false;
        }

        const std::uint8_t info = static_cast<std::uint8_t>(body[7]);
        const bool encrypted = (info & kLegacyEncryptedFlag) == kLegacyEncryptedFlag;
        const std::uint8_t compression = info & 0x0F;
        const std::uint64_t plainSize = ReadLeU64(body, 0x108);

        std::vector<std::uint8_t> payload(body.begin() + kLegacyHeaderSize, body.end());

        if (encrypted) {
#if !HAVE_LIB_BLOB
            outError = "This build was made without prebuilt/lib.a; encrypted shop responses are not supported.";
            return false;
#else
            std::vector<std::uint8_t> aesKey;
            if (!TryUnwrapLegacyAesKey(reinterpret_cast<const std::uint8_t*>(body.data() + 0x8), aesKey)) {
                outError = "Encrypted shop response could not be decrypted.";
                return false;
            }

            mbedtls_aes_context aesCtx;
            mbedtls_aes_init(&aesCtx);
            if (mbedtls_aes_setkey_dec(&aesCtx, aesKey.data(), 128) != 0) {
                mbedtls_aes_free(&aesCtx);
                if (!aesKey.empty())
                    inst::util::SecureWipe(aesKey.data(), aesKey.size());
                outError = "Failed to initialize AES decryption for encrypted shop response.";
                return false;
            }
            if ((payload.size() % 16) != 0) {
                mbedtls_aes_free(&aesCtx);
                if (!aesKey.empty())
                    inst::util::SecureWipe(aesKey.data(), aesKey.size());
                outError = "Encrypted shop payload is not AES block aligned.";
                return false;
            }
            for (std::size_t off = 0; off < payload.size(); off += 16)
                mbedtls_aes_crypt_ecb(&aesCtx, MBEDTLS_AES_DECRYPT, payload.data() + off, payload.data() + off);
            mbedtls_aes_free(&aesCtx);
            if (!aesKey.empty())
                inst::util::SecureWipe(aesKey.data(), aesKey.size());
            aesKey.clear();
#endif
        }

        std::vector<std::vector<std::uint8_t>> candidates;
        candidates.push_back(payload);
        std::vector<std::uint8_t> unpadded;
        if (TryPkcs7Unpad(payload, unpadded) && !unpadded.empty() && unpadded != payload)
            candidates.push_back(unpadded);

        std::vector<std::uint8_t> decoded;
        if (compression == kLegacyCompressionNone) {
            const std::vector<std::uint8_t>& candidate = candidates.front();
            if (plainSize > 0) {
                if (candidate.size() < plainSize) {
                    outError = "Payload shorter than expected plain size.";
                    return false;
                }
                decoded.assign(candidate.begin(), candidate.begin() + static_cast<std::size_t>(plainSize));
            } else {
                decoded = candidate;
            }
        } else if (compression == kLegacyCompressionZlib) {
            bool ok = false;
            for (const auto& cand : candidates) {
                if (InflatePayloadWithWindowBits(cand, MAX_WBITS, decoded) ||
                    InflatePayloadWithWindowBits(cand, MAX_WBITS | 16, decoded) ||
                    InflatePayloadWithWindowBits(cand, -MAX_WBITS, decoded)) {
                    ok = true;
                    break;
                }
                const auto trimmed = TrimTrailingZeros(cand);
                if (!trimmed.empty() && trimmed != cand &&
                    (InflatePayloadWithWindowBits(trimmed, MAX_WBITS, decoded) ||
                     InflatePayloadWithWindowBits(trimmed, MAX_WBITS | 16, decoded) ||
                     InflatePayloadWithWindowBits(trimmed, -MAX_WBITS, decoded))) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                outError = "Failed to decompress zlib-compressed shop payload.";
                return false;
            }
        } else if (compression == kLegacyCompressionZstd) {
            bool ok = false;
            for (const auto& cand : candidates) {
                if (DecompressZstdFlexible(cand, plainSize, decoded)) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                outError = "Failed to decompress zstd-compressed shop payload.";
                return false;
            }
        } else {
            outError = "Unsupported compressed shop payload format.";
            return false;
        }

        outDecoded.assign(decoded.begin(), decoded.end());
        while (!outDecoded.empty() && outDecoded.back() == '\0')
            outDecoded.pop_back();
        return true;
    }

    bool IsXciExtension(const std::string& name)
    {
        std::string ext = std::filesystem::path(name).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".xci" || ext == ".xcz";
    }

    bool IsXciMagic(const std::string& url)
    {
        try {
            tin::network::HTTPDownload download(url);
            u32 magic = 0;
            download.BufferDataRange(&magic, 0xF000, sizeof(magic), nullptr);
            if (magic == 0x30534648)
                return true;
            magic = 0;
            download.BufferDataRange(&magic, 0x10000, sizeof(magic), nullptr);
            return magic == 0x30534648;
        } catch (...) {
            return false;
        }
    }

    bool ContainsHtml(const std::string& body)
    {
        std::string lower = body;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        return lower.find("<!doctype html") != std::string::npos || lower.find("<html") != std::string::npos;
    }

    bool IsLoginUrl(const char* effectiveUrl)
    {
        if (!effectiveUrl)
            return false;
        std::string url = effectiveUrl;
        return url.find("/login") != std::string::npos;
    }

    std::string BuildFullUrl(const std::string& baseUrl, const std::string& urlPath)
    {
        if (urlPath.rfind("http://", 0) == 0 || urlPath.rfind("https://", 0) == 0)
            return urlPath;
        if (urlPath.size() >= 5) {
            std::string prefix = urlPath.substr(0, 5);
            std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return std::tolower(c); });
            if (prefix == "jbod:")
                return urlPath;
        }
        if (!urlPath.empty() && urlPath[0] == '/')
            return baseUrl + urlPath;
        return baseUrl + "/" + urlPath;
    }

    std::uint64_t GetOfflineLookupTitleId(const shopInstStuff::ShopItem& item);

    std::string GetShopIconCachePath(const shopInstStuff::ShopItem& item)
    {
        if (!item.hasIconUrl)
            return "";
        std::string cacheDir = inst::config::appDir + "/shop_icons";
        std::error_code ec;
        if (!std::filesystem::exists(cacheDir, ec)) {
            std::filesystem::create_directory(cacheDir, ec);
            if (ec)
                return "";
        }

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

    void UpdateInstallIcon(const shopInstStuff::ShopItem& item)
    {
        if (item.hasTitleId) {
            const std::uint64_t lookupTitleId = GetOfflineLookupTitleId(item);
            if (lookupTitleId != 0) {
                std::vector<std::uint8_t> iconData;
                if (inst::offline::TryGetIconData(lookupTitleId, iconData) && !iconData.empty()) {
                    inst::ui::instPage::setInstallIconData(iconData.data(), static_cast<std::uint32_t>(iconData.size()));
                    return;
                }
            }
        }

        if (!item.hasIconUrl) {
            inst::ui::instPage::clearInstallIcon();
            return;
        }

        std::string filePath = GetShopIconCachePath(item);
        if (filePath.empty()) {
            inst::ui::instPage::clearInstallIcon();
            return;
        }

        if (!std::filesystem::exists(filePath)) {
            bool ok = inst::curl::downloadImageWithAuth(item.iconUrl, filePath.c_str(), inst::config::shopUser, inst::config::shopPass, 8000);
            if (!ok && std::filesystem::exists(filePath))
                std::filesystem::remove(filePath);
        }

        if (std::filesystem::exists(filePath))
            inst::ui::instPage::setInstallIcon(filePath);
        else
            inst::ui::instPage::clearInstallIcon();
    }

    bool TryParseTitleId(const nlohmann::json& entry, std::uint64_t& out);
    bool TryParseAppVersion(const nlohmann::json& entry, std::uint32_t& out);
    bool TryParseAppType(const nlohmann::json& entry, std::int32_t& out);
    bool InferAppTypeFromTitleId(std::uint64_t titleId, std::int32_t& out);

    bool TryParseHexU64(const std::string& value, std::uint64_t& out)
    {
        if (value.empty())
            return false;
        char* end = nullptr;
        unsigned long long parsed = std::strtoull(value.c_str(), &end, 16);
        if (end == value.c_str() || (end && *end != '\0'))
            return false;
        out = static_cast<std::uint64_t>(parsed);
        return true;
    }

    bool TryParseTitleIdText(const std::string& value, std::uint64_t& out)
    {
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return false;
        const auto end = value.find_last_not_of(" \t\r\n");
        std::string text = value.substr(start, (end - start) + 1);

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

        if (hasHexLetters || text.size() == 16)
            return TryParseHexU64(text, out);

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

    bool TryParseTitleId(const nlohmann::json& entry, std::uint64_t& out)
    {
        if (!entry.contains("title_id"))
            return false;
        const auto& value = entry["title_id"];
        if (value.is_number_unsigned()) {
            out = value.get<std::uint64_t>();
            return true;
        }
        if (value.is_string()) {
            return TryParseTitleIdText(value.get<std::string>(), out);
        }
        return false;
    }

    bool TryParseAppVersion(const nlohmann::json& entry, std::uint32_t& out)
    {
        if (!entry.contains("app_version"))
            return false;
        const auto& value = entry["app_version"];
        if (value.is_number_unsigned()) {
            out = value.get<std::uint32_t>();
            return true;
        }
        if (value.is_number_integer()) {
            int parsed = value.get<int>();
            if (parsed < 0)
                return false;
            out = static_cast<std::uint32_t>(parsed);
            return true;
        }
        if (value.is_string()) {
            std::string text = value.get<std::string>();
            text.erase(0, text.find_first_not_of(" \t\r\n"));
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
            if (end == nullptr || *end != '\0')
                return false;
            out = static_cast<std::uint32_t>(parsed);
            return true;
        }
        return false;
    }

    bool TryParseReleaseDateText(const std::string& text, std::uint32_t& out)
    {
        std::string digits;
        digits.reserve(text.size());
        for (unsigned char c : text) {
            if (std::isdigit(c))
                digits.push_back(static_cast<char>(c));
        }
        if (digits.size() >= 8) {
            const std::string yyyymmdd = digits.substr(0, 8);
            out = static_cast<std::uint32_t>(std::strtoul(yyyymmdd.c_str(), nullptr, 10));
            return out != 0;
        }
        return false;
    }

    bool TryParseReleaseDate(const nlohmann::json& entry, std::uint32_t& out)
    {
        static const char* kKeys[] = {
            "release_date",
            "releaseDate",
            "release",
            "date"
        };

        for (const char* key : kKeys) {
            if (!entry.contains(key))
                continue;
            const auto& value = entry[key];
            if (value.is_number_unsigned()) {
                const auto parsed = value.get<std::uint64_t>();
                if (parsed == 0)
                    continue;
                if (parsed >= 10000101ULL) {
                    out = static_cast<std::uint32_t>(parsed);
                    return true;
                }
                continue;
            }
            if (value.is_number_integer()) {
                const auto parsed = value.get<long long>();
                if (parsed <= 0)
                    continue;
                if (parsed >= 10000101LL) {
                    out = static_cast<std::uint32_t>(parsed);
                    return true;
                }
                continue;
            }
            if (value.is_string()) {
                std::uint32_t parsed = 0;
                if (TryParseReleaseDateText(value.get<std::string>(), parsed)) {
                    out = parsed;
                    return true;
                }
            }
        }
        return false;
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

    bool TryParseAppType(const nlohmann::json& entry, std::int32_t& out)
    {
        if (!entry.contains("app_type"))
            return false;
        const auto& value = entry["app_type"];
        if (value.is_number_integer()) {
            const std::int32_t parsed = value.get<std::int32_t>();
            return NormalizeAppTypeValue(parsed, out);
        }
        if (value.is_number_unsigned()) {
            const std::int32_t parsed = static_cast<std::int32_t>(value.get<std::uint32_t>());
            return NormalizeAppTypeValue(parsed, out);
        }
        if (value.is_string()) {
            std::string type = value.get<std::string>();
            type.erase(0, type.find_first_not_of(" \t\r\n"));
            type.erase(type.find_last_not_of(" \t\r\n") + 1);
            std::transform(type.begin(), type.end(), type.begin(), ::tolower);
            if (type == "base") {
                out = NcmContentMetaType_Application;
                return true;
            }
            if (type == "upd" || type == "update" || type == "patch") {
                out = NcmContentMetaType_Patch;
                return true;
            }
            if (type == "dlc" || type == "addon") {
                out = NcmContentMetaType_AddOnContent;
                return true;
            }
            char* end = nullptr;
            long parsed = std::strtol(type.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && parsed >= std::numeric_limits<std::int32_t>::min() && parsed <= std::numeric_limits<std::int32_t>::max()) {
                return NormalizeAppTypeValue(static_cast<std::int32_t>(parsed), out);
            }
        }
        return false;
    }

    bool InferAppTypeFromSectionId(const std::string& sectionId, std::int32_t& out)
    {
        if (sectionId.empty())
            return false;
        std::string id = sectionId;
        std::transform(id.begin(), id.end(), id.begin(), ::tolower);
        if (id == "updates" || id == "update") {
            out = NcmContentMetaType_Patch;
            return true;
        }
        if (id == "dlc" || id == "addon" || id == "add-on" || id == "add_ons") {
            out = NcmContentMetaType_AddOnContent;
            return true;
        }
        if (id == "base") {
            out = NcmContentMetaType_Application;
            return true;
        }
        return false;
    }

    std::string NormalizeHexString(const std::string& value)
    {
        std::string out;
        out.reserve(value.size());
        for (unsigned char c : value) {
            if (std::isxdigit(c))
                out.push_back(static_cast<char>(std::tolower(c)));
        }
        return out;
    }

    bool InferAppTypeFromAppId(const std::string& appId, std::int32_t& out)
    {
        std::uint64_t parsedTitleId = 0;
        if (!TryParseTitleIdText(appId, parsedTitleId))
            return false;
        return InferAppTypeFromTitleId(parsedTitleId, out);
    }

    bool InferAppTypeFromTitleId(std::uint64_t titleId, std::int32_t& out)
    {
        const std::uint64_t suffix = titleId & 0xFFFULL;
        if (suffix == 0x800ULL) {
            out = NcmContentMetaType_Patch;
            return true;
        }
        if (suffix == 0x000ULL) {
            out = NcmContentMetaType_Application;
            return true;
        }
        out = NcmContentMetaType_AddOnContent;
        return true;
    }

    bool TryParseTitleIdFromAppId(const std::string& appId, std::uint64_t& out)
    {
        return TryParseTitleIdText(appId, out);
    }

    std::string TrimAscii(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return std::string();
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(start, (end - start) + 1);
    }

    bool TryExtractHexTitleIdToken(const std::string& token, std::string& outHex)
    {
        std::string text = TrimAscii(token);
        if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0)
            text = text.substr(2);
        if (text.size() != 16)
            return false;
        for (unsigned char c : text) {
            if (!std::isxdigit(c))
                return false;
        }
        outHex = NormalizeHexString(text);
        return true;
    }

    void CollectHexTitleIdCandidates(const std::string& text, std::vector<std::string>& out)
    {
        auto isHexAt = [&](std::size_t idx) {
            return std::isxdigit(static_cast<unsigned char>(text[idx])) != 0;
        };

        for (std::size_t i = 0; i < text.size();) {
            if (!isHexAt(i)) {
                i++;
                continue;
            }

            std::size_t j = i;
            while (j < text.size() && isHexAt(j))
                j++;

            const std::size_t runLen = j - i;
            if (runLen >= 16) {
                // Prefer exact 16-char tokens; fallback to the first 16 chars of longer runs.
                const std::size_t candidateCount = runLen / 16;
                for (std::size_t k = 0; k < candidateCount; k++) {
                    std::string candidate = text.substr(i + (k * 16), 16);
                    std::string normalized = NormalizeHexString(candidate);
                    if (normalized.size() == 16)
                        out.push_back(normalized);
                }
            }

            i = j;
        }
    }

    std::string ChooseLegacyTitleIdToken(const std::vector<std::string>& hexTokens, std::int32_t appType)
    {
        if (hexTokens.empty())
            return std::string();

        auto endsWith = [](const std::string& value, const char* suffix) -> bool {
            const std::size_t suffixLen = std::char_traits<char>::length(suffix);
            return value.size() >= suffixLen && value.compare(value.size() - suffixLen, suffixLen, suffix) == 0;
        };

        if (appType == NcmContentMetaType_Patch) {
            for (const auto& token : hexTokens) {
                if (endsWith(token, "800"))
                    return token;
            }
        } else if (appType == NcmContentMetaType_AddOnContent) {
            for (const auto& token : hexTokens) {
                if (!endsWith(token, "000") && !endsWith(token, "800"))
                    return token;
            }
        } else if (appType == NcmContentMetaType_Application) {
            for (const auto& token : hexTokens) {
                if (endsWith(token, "000"))
                    return token;
            }
        }

        return hexTokens.front();
    }

    std::string FormatTitleIdHexUpper(std::uint64_t titleId)
    {
        char buf[17] = {0};
        std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(titleId));
        return std::string(buf);
    }

    void ApplyLegacyMetadataFromName(const std::string& name, shopInstStuff::ShopItem& item)
    {
        std::vector<std::string> hexTokens;
        std::size_t pos = 0;
        while (true) {
            const auto open = name.find('[', pos);
            if (open == std::string::npos)
                break;
            const auto close = name.find(']', open + 1);
            if (close == std::string::npos)
                break;
            std::string token = TrimAscii(name.substr(open + 1, close - open - 1));
            std::string lower = token;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == "base") {
                item.appType = NcmContentMetaType_Application;
            } else if (lower == "upd" || lower == "update" || lower == "patch") {
                item.appType = NcmContentMetaType_Patch;
            } else if (lower == "dlc" || lower == "addon") {
                item.appType = NcmContentMetaType_AddOnContent;
            } else if (lower.size() >= 2 && lower[0] == 'v') {
                const std::string number = lower.substr(1);
                if (!number.empty() && std::all_of(number.begin(), number.end(), ::isdigit)) {
                    item.appVersion = static_cast<std::uint32_t>(std::strtoull(number.c_str(), nullptr, 10));
                    item.hasAppVersion = true;
                }
            } else {
                std::string hexToken;
                if (TryExtractHexTitleIdToken(token, hexToken))
                    hexTokens.push_back(hexToken);
            }
            pos = close + 1;
        }

        if (hexTokens.empty())
            CollectHexTitleIdCandidates(name, hexTokens);

        if (!hexTokens.empty()) {
            const std::string chosen = ChooseLegacyTitleIdToken(hexTokens, item.appType);
            if (!chosen.empty()) {
                item.appId = chosen;
                item.hasAppId = true;
                std::uint64_t parsedTitleId = 0;
                if (TryParseHexU64(chosen, parsedTitleId)) {
                    item.titleId = parsedTitleId;
                    item.hasTitleId = true;
                }
            }
        }

        if (item.appType < 0 && item.hasAppId)
            InferAppTypeFromAppId(item.appId, item.appType);
        if (item.appType < 0 && item.hasTitleId)
            InferAppTypeFromTitleId(item.titleId, item.appType);
    }

    bool TryResolveBaseTitleId(const shopInstStuff::ShopItem& item, std::uint64_t& outBaseId)
    {
        // Some shops publish title_id as the base title even for UPDATE/DLC entries.
        if (item.hasTitleId && ((item.titleId & 0xFFFULL) == 0x000ULL)) {
            outBaseId = item.titleId;
            return true;
        }

        std::uint64_t parsedAppId = 0;
        const bool hasParsedAppId = item.hasAppId && TryParseTitleIdFromAppId(item.appId, parsedAppId);
        if (hasParsedAppId) {
            std::int32_t inferredFromApp = -1;
            InferAppTypeFromTitleId(parsedAppId, inferredFromApp);
            NcmContentMetaType metaType = NcmContentMetaType_Application;
            if (inferredFromApp >= 0)
                metaType = static_cast<NcmContentMetaType>(inferredFromApp);
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
        if (inferredType < 0)
            InferAppTypeFromTitleId(item.titleId, inferredType);

        NcmContentMetaType metaType = NcmContentMetaType_Application;
        if (inferredType >= 0)
            metaType = static_cast<NcmContentMetaType>(inferredType);
        outBaseId = tin::util::GetBaseTitleId(item.titleId, metaType);
        return outBaseId != 0;
    }

    std::uint64_t GetOfflineLookupTitleId(const shopInstStuff::ShopItem& item)
    {
        std::uint64_t baseTitleId = 0;
        if (!TryResolveBaseTitleId(item, baseTitleId))
            return 0;
        return baseTitleId;
    }

    void ApplyOfflineDataToItem(shopInstStuff::ShopItem& item, bool hasExplicitName)
    {
        const std::uint64_t lookupTitleId = GetOfflineLookupTitleId(item);
        if (lookupTitleId == 0)
            return;

        inst::offline::TitleMetadata meta;
        if (inst::offline::TryGetMetadata(lookupTitleId, meta)) {
            if ((!hasExplicitName || item.name.empty()) && !meta.name.empty())
                item.name = meta.name;
            if (item.size == 0 && meta.hasSize)
                item.size = meta.size;
            if (!item.hasAppVersion && meta.hasVersion) {
                item.appVersion = meta.version;
                item.hasAppVersion = true;
            }
            if (!item.hasReleaseDate && meta.hasReleaseDate && meta.releaseDate > 0) {
                item.releaseDate = meta.releaseDate;
                item.hasReleaseDate = true;
            }
        }

    }

    std::vector<shopInstStuff::ShopSection> ParseShopSectionsBody(const std::string& body, const std::string& baseUrl, std::string& error)
    {
        std::vector<shopInstStuff::ShopSection> sections;
        try {
            nlohmann::json shop = nlohmann::json::parse(body);
            if (shop.contains("error") && shop["error"].is_string()) {
                error = "Shop login failed. " + shop["error"].get<std::string>();
                return sections;
            }
            if (!shop.contains("sections") || !shop["sections"].is_array()) {
                std::string lower = body;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
                if (lower.find("unauthorized") != std::string::npos || lower.find("login") != std::string::npos) {
                    error = "Shop login failed. Check username/password or enable public shop.";
                } else {
                    error = "Shop response missing sections.";
                }
                return sections;
            }

            for (const auto& section : shop["sections"]) {
                if (!section.contains("items") || !section["items"].is_array())
                    continue;
                shopInstStuff::ShopSection parsed;
                parsed.id = section.value("id", "all");
                parsed.title = section.value("title", "All");
                for (const auto& entry : section["items"]) {
                    if (!entry.contains("url"))
                        continue;
                    std::string url = entry["url"].get<std::string>();
                    std::uint64_t size = 0;
                    if (entry.contains("size") && entry["size"].is_number()) {
                        size = entry["size"].get<std::uint64_t>();
                    }

                    std::string fragment;
                    std::string urlPath = url;
                    auto hashPos = urlPath.find('#');
                    if (hashPos != std::string::npos) {
                        fragment = urlPath.substr(hashPos + 1);
                        urlPath = urlPath.substr(0, hashPos);
                    }

                    std::string fullUrl = BuildFullUrl(baseUrl, urlPath);

                    std::string name;
                    const bool hasExplicitName = entry.contains("name");
                    if (hasExplicitName) {
                        name = entry["name"].get<std::string>();
                    } else if (!fragment.empty()) {
                        name = DecodeUrlSegment(fragment);
                    } else {
                        name = inst::util::formatUrlString(fullUrl);
                    }

                    if (!fullUrl.empty() && !name.empty()) {
                        shopInstStuff::ShopItem item;
                        item.name = name;
                        item.url = fullUrl;
                        item.size = size;
                        std::uint64_t titleId = 0;
                        std::uint32_t appVersion = 0;
                        std::int32_t appType = -1;
                        if (TryParseTitleId(entry, titleId)) {
                            item.titleId = titleId;
                            item.hasTitleId = true;
                        }
                        if (TryParseAppVersion(entry, appVersion)) {
                            item.appVersion = appVersion;
                            item.hasAppVersion = true;
                        }
                        std::uint32_t releaseDate = 0;
                        if (TryParseReleaseDate(entry, releaseDate)) {
                            item.releaseDate = releaseDate;
                            item.hasReleaseDate = true;
                        }
                        if (TryParseAppType(entry, appType))
                            item.appType = appType;
                        if (entry.contains("app_id") && entry["app_id"].is_string()) {
                            item.appId = entry["app_id"].get<std::string>();
                            item.hasAppId = !item.appId.empty();
                            if (!item.hasTitleId) {
                                std::uint64_t parsedAppId = 0;
                                if (TryParseTitleIdFromAppId(item.appId, parsedAppId)) {
                                    item.titleId = parsedAppId;
                                    item.hasTitleId = true;
                                }
                            }
                        }
                        if (item.appType < 0) {
                            if (item.hasAppId)
                                InferAppTypeFromAppId(item.appId, item.appType);
                            if (item.appType < 0 && item.hasTitleId)
                                InferAppTypeFromTitleId(item.titleId, item.appType);
                            if (item.appType < 0)
                                InferAppTypeFromSectionId(parsed.id, item.appType);
                        }
                        if (entry.contains("icon_url") && entry["icon_url"].is_string()) {
                            std::string iconUrl = entry["icon_url"].get<std::string>();
                            if (!iconUrl.empty()) {
                                item.iconUrl = BuildFullUrl(baseUrl, iconUrl);
                                item.hasIconUrl = true;
                            }
                        } else if (entry.contains("iconUrl") && entry["iconUrl"].is_string()) {
                            std::string iconUrl = entry["iconUrl"].get<std::string>();
                            if (!iconUrl.empty()) {
                                item.iconUrl = BuildFullUrl(baseUrl, iconUrl);
                                item.hasIconUrl = true;
                            }
                        }
                        if (entry.contains("save_id") && entry["save_id"].is_string())
                            item.saveId = entry["save_id"].get<std::string>();
                        else if (entry.contains("saveId") && entry["saveId"].is_string())
                            item.saveId = entry["saveId"].get<std::string>();
                        if (entry.contains("note") && entry["note"].is_string())
                            item.saveNote = entry["note"].get<std::string>();
                        else if (entry.contains("save_note") && entry["save_note"].is_string())
                            item.saveNote = entry["save_note"].get<std::string>();
                        else if (entry.contains("saveNote") && entry["saveNote"].is_string())
                            item.saveNote = entry["saveNote"].get<std::string>();
                        if (entry.contains("created_at") && entry["created_at"].is_string())
                            item.saveCreatedAt = entry["created_at"].get<std::string>();
                        else if (entry.contains("createdAt") && entry["createdAt"].is_string())
                            item.saveCreatedAt = entry["createdAt"].get<std::string>();
                        if (entry.contains("created_ts")) {
                            if (entry["created_ts"].is_number_unsigned())
                                item.saveCreatedTs = entry["created_ts"].get<std::uint64_t>();
                            else if (entry["created_ts"].is_number_integer()) {
                                const auto parsedCreatedTs = entry["created_ts"].get<long long>();
                                if (parsedCreatedTs > 0)
                                    item.saveCreatedTs = static_cast<std::uint64_t>(parsedCreatedTs);
                            }
                        } else if (entry.contains("createdTs")) {
                            if (entry["createdTs"].is_number_unsigned())
                                item.saveCreatedTs = entry["createdTs"].get<std::uint64_t>();
                            else if (entry["createdTs"].is_number_integer()) {
                                const auto parsedCreatedTs = entry["createdTs"].get<long long>();
                                if (parsedCreatedTs > 0)
                                    item.saveCreatedTs = static_cast<std::uint64_t>(parsedCreatedTs);
                            }
                        }
                        ApplyOfflineDataToItem(item, hasExplicitName);
                        parsed.items.push_back(item);
                    }
                }

                if (!parsed.items.empty())
                    sections.push_back(parsed);
            }
        }
        catch (...) {
            error = "Invalid shop response.";
            return {};
        }

        return sections;
    }
}

namespace shopInstStuff {
    namespace {
        struct ShopFetchProgressContext {
            const ShopFetchProgressCallback* cb = nullptr;
            curl_off_t lastNow = -1;
            curl_off_t lastTotal = -1;
        };

        int ShopFetchProgressHandler(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
        {
            auto* ctx = static_cast<ShopFetchProgressContext*>(clientp);
            if (ctx == nullptr || ctx->cb == nullptr || !(*ctx->cb))
                return 0;

            if (ctx->lastNow == dlnow && ctx->lastTotal == dltotal)
                return 0;

            ctx->lastNow = dlnow;
            ctx->lastTotal = dltotal;

            const std::uint64_t now = (dlnow > 0) ? static_cast<std::uint64_t>(dlnow) : 0;
            const std::uint64_t total = (dltotal > 0) ? static_cast<std::uint64_t>(dltotal) : 0;
            (*ctx->cb)(now, total);
            return 0;
        }
    }

    struct FetchResult {
        std::string body;
        long responseCode = 0;
        std::string effectiveUrl;
        std::string contentType;
        std::string error;
        std::string decodeError;
        CURLcode curlCode = CURLE_OK;
    };

    namespace {
        constexpr long kShopRequestTimeoutMs = 30000L;
        constexpr long kShopConnectTimeoutMs = 10000L;
        constexpr int kShopFetchMaxAttempts = 4;

        bool IsRetriableHttpCode(long responseCode)
        {
            return responseCode == 408 ||
                responseCode == 425 ||
                responseCode == 429 ||
                responseCode == 500 ||
                responseCode == 502 ||
                responseCode == 503 ||
                responseCode == 504;
        }

        bool IsRetriableCurlCode(CURLcode code)
        {
            switch (code) {
                case CURLE_COULDNT_RESOLVE_HOST:
                case CURLE_COULDNT_RESOLVE_PROXY:
                case CURLE_COULDNT_CONNECT:
                case CURLE_OPERATION_TIMEDOUT:
                case CURLE_SEND_ERROR:
                case CURLE_RECV_ERROR:
                case CURLE_GOT_NOTHING:
                case CURLE_SSL_CONNECT_ERROR:
                    return true;
                default:
                    return false;
            }
        }

        bool ShouldRetryShopFetch(const FetchResult& result)
        {
            if (result.curlCode != CURLE_OK)
                return IsRetriableCurlCode(result.curlCode);
            return IsRetriableHttpCode(result.responseCode);
        }

        std::uint32_t ShopRetryDelayMs(int attemptIndex)
        {
            // attemptIndex is 0-based for retries after the first try.
            static constexpr std::uint32_t kBackoffMs[kShopFetchMaxAttempts - 1] = {450, 1000, 1800};
            if (attemptIndex < 0)
                return kBackoffMs[0];
            if (attemptIndex >= static_cast<int>(sizeof(kBackoffMs) / sizeof(kBackoffMs[0])))
                return kBackoffMs[(sizeof(kBackoffMs) / sizeof(kBackoffMs[0])) - 1];
            return kBackoffMs[attemptIndex];
        }
    }

    FetchResult FetchShopResponse(const std::string& url, const std::string& user, const std::string& pass, const ShopFetchProgressCallback& progressCb = ShopFetchProgressCallback())
    {
        FetchResult lastResult;

        for (int attempt = 0; attempt < kShopFetchMaxAttempts; attempt++) {
            FetchResult result;
            CURL* curl = curl_easy_init();
            if (!curl) {
                result.error = "Failed to initialize curl.";
                return result;
            }

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            const std::string userAgent = inst::config::shopLegacyMode ? std::string() : inst::curl::getDefaultUserAgent();
            curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kShopRequestTimeoutMs);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kShopConnectTimeoutMs);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

            ShopFetchProgressContext progressCtx{};
            if (progressCb) {
                progressCtx.cb = &progressCb;
                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ShopFetchProgressHandler);
                curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressCtx);
            }

            struct curl_slist* headerList = nullptr;
            const auto headers = BuildLegacyHeaders(url, user, pass);
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
            result.curlCode = rc;

            long responseCode = 0;
            char* effectiveUrl = nullptr;
            char* contentType = nullptr;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
            curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
            curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentType);
            if (headerList)
                curl_slist_free_all(headerList);
            curl_easy_cleanup(curl);

            result.responseCode = responseCode;
            result.effectiveUrl = effectiveUrl ? effectiveUrl : "";
            result.contentType = contentType ? contentType : "";

            if (rc != CURLE_OK) {
                result.error = std::string(curl_easy_strerror(rc)) + " (curl=" + std::to_string(static_cast<int>(rc)) + ")";
            } else if (progressCb) {
                const std::uint64_t bodySize = static_cast<std::uint64_t>(result.body.size());
                progressCb(bodySize, bodySize);
            }

            std::size_t legacyOffset = std::string::npos;
            if (result.error.empty() && FindLegacyPayloadOffset(result.body, legacyOffset)) {
                std::string decodedBody;
                std::string decodeError;
                const std::string legacyBody = (legacyOffset == 0) ? result.body : result.body.substr(legacyOffset);
                if (DecodeLegacyPayload(legacyBody, decodedBody, decodeError))
                    result.body = std::move(decodedBody);
                else
                    result.decodeError = std::move(decodeError);
            }

            const bool canRetry = (attempt + 1) < kShopFetchMaxAttempts && ShouldRetryShopFetch(result);
            if (!canRetry)
                return result;

            lastResult = result;
            std::this_thread::sleep_for(std::chrono::milliseconds(ShopRetryDelayMs(attempt)));
        }

        return lastResult;
    }

    bool ValidateShopResponse(const FetchResult& fetch, std::string& error)
    {
        if (!fetch.error.empty()) {
            error = fetch.error;
            return false;
        }
        if (fetch.responseCode == 401 || fetch.responseCode == 403) {
            if (!inst::util::HasLegacyAuthSupport()) {
                error = "Shop requires legacy HAUTH/UAUTH signing, but this build does not support it.";
            } else {
                error = "Shop requires authentication. Check credentials or enable public shop in eShop.";
            }
            return false;
        }
        if (!fetch.decodeError.empty()) {
            error = fetch.decodeError;
            return false;
        }
        std::size_t legacyOffset = std::string::npos;
        if (FindLegacyPayloadOffset(fetch.body, legacyOffset)) {
            error = "Encrypted shop response could not be decoded.";
            return false;
        }
        if (IsLoginUrl(fetch.effectiveUrl.c_str()) || (!fetch.contentType.empty() && fetch.contentType.find("text/html") != std::string::npos) || ContainsHtml(fetch.body)) {
            if (inst::config::shopLegacyMode && !inst::util::HasLegacyAuthSupport()) {
                error = "This build does not support this shop";
            } else {
                error = "eShop returned the login page. Check shop URL, username, and password, or enable public shop.";
            }
            return false;
        }
        return true;
    }

    namespace {
        bool AppendShopItemFromEntry(const nlohmann::json& entry, const std::string& baseUrl,
            std::vector<ShopItem>& items, std::unordered_set<std::string>& seenItemUrls)
        {
            std::string rawUrl;
            if (entry.is_string()) {
                rawUrl = entry.get<std::string>();
            } else if (entry.is_object()) {
                if (entry.contains("url") && entry["url"].is_string())
                    rawUrl = entry["url"].get<std::string>();
                else if (entry.contains("path") && entry["path"].is_string())
                    rawUrl = entry["path"].get<std::string>();
                else if (entry.contains("file") && entry["file"].is_string())
                    rawUrl = entry["file"].get<std::string>();
                else if (entry.contains("download_url") && entry["download_url"].is_string())
                    rawUrl = entry["download_url"].get<std::string>();
                else if (entry.contains("downloadUrl") && entry["downloadUrl"].is_string())
                    rawUrl = entry["downloadUrl"].get<std::string>();
            } else {
                return false;
            }

            if (rawUrl.empty())
                return false;

            std::uint64_t size = 0;
            if (entry.is_object() && entry.contains("size")) {
                if (entry["size"].is_number_unsigned())
                    size = entry["size"].get<std::uint64_t>();
                else if (entry["size"].is_number_integer()) {
                    const auto parsedSize = entry["size"].get<long long>();
                    if (parsedSize > 0)
                        size = static_cast<std::uint64_t>(parsedSize);
                }
            }

            std::string fragment;
            std::string urlPath = rawUrl;
            const auto hashPos = urlPath.find('#');
            if (hashPos != std::string::npos) {
                fragment = urlPath.substr(hashPos + 1);
                urlPath = urlPath.substr(0, hashPos);
            }

            const std::string fullUrl = BuildFullUrl(baseUrl, urlPath);
            if (fullUrl.empty())
                return false;
            if (!seenItemUrls.insert(fullUrl).second)
                return false;

            std::string name;
            if (entry.is_object() && entry.contains("name") && entry["name"].is_string() &&
                !TrimAscii(entry["name"].get<std::string>()).empty()) {
                name = TrimAscii(entry["name"].get<std::string>());
            } else if (!fragment.empty()) {
                name = DecodeUrlSegment(fragment);
            } else {
                name = inst::util::formatUrlString(fullUrl);
            }
            if (name.empty())
                return false;

            ShopItem item;
            item.name = name;
            item.url = fullUrl;
            item.size = size;
            ApplyLegacyMetadataFromName(name, item);

            std::uint32_t releaseDate = 0;
            if (entry.is_object() && TryParseReleaseDate(entry, releaseDate)) {
                item.releaseDate = releaseDate;
                item.hasReleaseDate = true;
            }

            if (entry.is_object()) {
                if (entry.contains("icon_url") && entry["icon_url"].is_string()) {
                    std::string iconUrl = entry["icon_url"].get<std::string>();
                    if (!iconUrl.empty()) {
                        item.iconUrl = BuildFullUrl(baseUrl, iconUrl);
                        item.hasIconUrl = true;
                    }
                } else if (entry.contains("iconUrl") && entry["iconUrl"].is_string()) {
                    std::string iconUrl = entry["iconUrl"].get<std::string>();
                    if (!iconUrl.empty()) {
                        item.iconUrl = BuildFullUrl(baseUrl, iconUrl);
                        item.hasIconUrl = true;
                    }
                }
                if (entry.contains("save_id") && entry["save_id"].is_string())
                    item.saveId = entry["save_id"].get<std::string>();
                else if (entry.contains("saveId") && entry["saveId"].is_string())
                    item.saveId = entry["saveId"].get<std::string>();
                if (entry.contains("note") && entry["note"].is_string())
                    item.saveNote = entry["note"].get<std::string>();
                else if (entry.contains("save_note") && entry["save_note"].is_string())
                    item.saveNote = entry["save_note"].get<std::string>();
                else if (entry.contains("saveNote") && entry["saveNote"].is_string())
                    item.saveNote = entry["saveNote"].get<std::string>();
                if (entry.contains("created_at") && entry["created_at"].is_string())
                    item.saveCreatedAt = entry["created_at"].get<std::string>();
                else if (entry.contains("createdAt") && entry["createdAt"].is_string())
                    item.saveCreatedAt = entry["createdAt"].get<std::string>();
                if (entry.contains("created_ts")) {
                    if (entry["created_ts"].is_number_unsigned())
                        item.saveCreatedTs = entry["created_ts"].get<std::uint64_t>();
                    else if (entry["created_ts"].is_number_integer()) {
                        const auto parsedCreatedTs = entry["created_ts"].get<long long>();
                        if (parsedCreatedTs > 0)
                            item.saveCreatedTs = static_cast<std::uint64_t>(parsedCreatedTs);
                    }
                } else if (entry.contains("createdTs")) {
                    if (entry["createdTs"].is_number_unsigned())
                        item.saveCreatedTs = entry["createdTs"].get<std::uint64_t>();
                    else if (entry["createdTs"].is_number_integer()) {
                        const auto parsedCreatedTs = entry["createdTs"].get<long long>();
                        if (parsedCreatedTs > 0)
                            item.saveCreatedTs = static_cast<std::uint64_t>(parsedCreatedTs);
                    }
                }
            }

            if (!item.hasIconUrl && !inst::config::shopLegacyMode) {
                std::uint64_t baseTitleId = 0;
                if (TryResolveBaseTitleId(item, baseTitleId) && baseTitleId != 0) {
                    item.iconUrl = BuildFullUrl(baseUrl, "/api/shop/icon/" + FormatTitleIdHexUpper(baseTitleId));
                    item.hasIconUrl = true;
                }
            }

            items.push_back(std::move(item));
            return true;
        }

        bool AppendLegacyFilesFromJson(const nlohmann::json& files, const std::string& baseUrl,
            std::vector<ShopItem>& items, std::unordered_set<std::string>& seenItemUrls)
        {
            if (files.is_array()) {
                bool any = false;
                for (const auto& entry : files)
                    any = AppendShopItemFromEntry(entry, baseUrl, items, seenItemUrls) || any;
                return any;
            }

            if (!files.is_object())
                return false;

            bool any = false;
            for (auto it = files.begin(); it != files.end(); ++it) {
                const std::string key = it.key();
                const auto& value = it.value();
                if (value.is_object()) {
                    nlohmann::json normalized = value;
                    if ((!normalized.contains("name") || !normalized["name"].is_string()) && !key.empty())
                        normalized["name"] = key;
                    any = AppendShopItemFromEntry(normalized, baseUrl, items, seenItemUrls) || any;
                } else if (value.is_string()) {
                    nlohmann::json normalized = {
                        {"name", key},
                        {"url", value.get<std::string>()}
                    };
                    any = AppendShopItemFromEntry(normalized, baseUrl, items, seenItemUrls) || any;
                } else if (value.is_array()) {
                    for (const auto& sub : value) {
                        if (!sub.is_string())
                            continue;
                        nlohmann::json normalized = {
                            {"name", key},
                            {"url", sub.get<std::string>()}
                        };
                        any = AppendShopItemFromEntry(normalized, baseUrl, items, seenItemUrls) || any;
                    }
                }
            }
            return any;
        }

        std::string GetDirectoryEntryUrl(const nlohmann::json& entry)
        {
            if (entry.is_string())
                return entry.get<std::string>();
            if (!entry.is_object())
                return "";

            static const char* candidates[] = {"url", "path", "directory", "href", "location"};
            for (const char* key : candidates) {
                if (entry.contains(key) && entry[key].is_string())
                    return entry[key].get<std::string>();
            }
            return "";
        }

        bool CollectShopItemsFromJson(const nlohmann::json& shop, const std::string& baseUrl,
            const std::string& user, const std::string& pass, std::vector<ShopItem>& items,
            std::unordered_set<std::string>& seenItemUrls, std::unordered_set<std::string>& seenManifestUrls,
            std::string& error, const ShopFetchProgressCallback& progressCb)
        {
            if (!shop.is_object()) {
                error = "Invalid shop response.";
                return false;
            }
            if (shop.contains("error") && shop["error"].is_string()) {
                error = shop["error"].get<std::string>();
                return false;
            }

            bool handled = false;

            if (shop.contains("sections") && shop["sections"].is_array()) {
                std::string parseError;
                std::vector<ShopSection> parsedSections = ParseShopSectionsBody(shop.dump(), baseUrl, parseError);
                if (!parsedSections.empty()) {
                    for (const auto& section : parsedSections) {
                        for (const auto& sectionItem : section.items) {
                            if (!seenItemUrls.insert(sectionItem.url).second)
                                continue;
                            items.push_back(sectionItem);
                        }
                    }
                    handled = true;
                }
            }

            if (shop.contains("files")) {
                if (AppendLegacyFilesFromJson(shop["files"], baseUrl, items, seenItemUrls))
                    handled = true;
            }
            if (shop.contains("paths")) {
                if (AppendLegacyFilesFromJson(shop["paths"], baseUrl, items, seenItemUrls))
                    handled = true;
            }

            if (shop.contains("directories") && shop["directories"].is_array()) {
                handled = true;
                for (const auto& directoryEntry : shop["directories"]) {
                    const std::string directoryPath = GetDirectoryEntryUrl(directoryEntry);
                    if (directoryPath.empty())
                        continue;

                    const std::string directoryUrl = BuildFullUrl(baseUrl, directoryPath);
                    if (directoryUrl.empty())
                        continue;
                    if (!seenManifestUrls.insert(directoryUrl).second)
                        continue;

                    FetchResult directoryFetch = FetchShopResponse(directoryUrl, user, pass, progressCb);
                    if (!ValidateShopResponse(directoryFetch, error))
                        return false;

                    nlohmann::json directoryJson;
                    try {
                        directoryJson = nlohmann::json::parse(directoryFetch.body);
                    } catch (...) {
                        error = "Invalid shop response.";
                        return false;
                    }

                    if (!CollectShopItemsFromJson(directoryJson, baseUrl, user, pass, items, seenItemUrls, seenManifestUrls, error, progressCb))
                        return false;
                }
            }

            if (!handled) {
                error = "Shop response missing file list.";
                return false;
            }

            return true;
        }
    }

    std::vector<ShopItem> FetchShop(const std::string& shopUrl, const std::string& user, const std::string& pass, std::string& error, const ShopFetchProgressCallback& progressCb)
    {
        std::vector<ShopItem> items;
        error.clear();

        std::string baseUrl = NormalizeShopUrl(shopUrl);
        if (baseUrl.empty()) {
            error = "Shop URL is empty.";
            return items;
        }

        FetchResult fetch = FetchShopResponse(baseUrl, user, pass, progressCb);
        if (!ValidateShopResponse(fetch, error))
            return items;

        try {
            nlohmann::json shop = nlohmann::json::parse(fetch.body);
            std::unordered_set<std::string> seenItemUrls;
            std::unordered_set<std::string> seenManifestUrls;
            seenManifestUrls.insert(baseUrl);
            if (!CollectShopItemsFromJson(shop, baseUrl, user, pass, items, seenItemUrls, seenManifestUrls, error, progressCb))
                return items;
        }
        catch (...) {
            error = "Invalid shop response.";
            return {};
        }

        std::sort(items.begin(), items.end(), [](const ShopItem& a, const ShopItem& b) {
            return inst::util::ignoreCaseCompare(a.name, b.name);
        });
        return items;
    }

    std::vector<ShopSection> FetchShopSections(const std::string& shopUrl, const std::string& user, const std::string& pass, std::string& error, bool* outUsedLegacyFallback, const ShopFetchProgressCallback& progressCb)
    {
        std::vector<ShopSection> sections;
        error.clear();
        if (outUsedLegacyFallback)
            *outUsedLegacyFallback = false;

        std::string baseUrl = NormalizeShopUrl(shopUrl);
        if (baseUrl.empty()) {
            error = "Shop URL is empty.";
            return sections;
        }

        auto tryLegacyFallback = [&]() -> bool {
            std::string legacyError;
            std::vector<ShopItem> items = FetchShop(shopUrl, user, pass, legacyError, progressCb);
            if (items.empty()) {
                if (!legacyError.empty())
                    error = legacyError;
                return false;
            }

            if (outUsedLegacyFallback)
                *outUsedLegacyFallback = true;
            error.clear();
            sections.push_back({"all", "All", items});
            return true;
        };

        if (inst::config::shopLegacyMode) {
            tryLegacyFallback();
            return sections;
        }

        std::string sectionsUrl = baseUrl + "/api/shop/sections";
        FetchResult fetch = FetchShopResponse(sectionsUrl, user, pass, progressCb);
        if (fetch.responseCode == 404) {
            tryLegacyFallback();
            return sections;
        }

        if (!ValidateShopResponse(fetch, error)) {
            if (!fetch.error.empty()) {
                error = "inst.shop.unreachable"_lang + "\n" + fetch.error;
                if (fetch.responseCode > 0)
                    error += "\nHTTP " + std::to_string(fetch.responseCode);
                return sections;
            }
            if (tryLegacyFallback())
                return sections;
            return sections;
        }

        sections = ParseShopSectionsBody(fetch.body, baseUrl, error);
        if (sections.empty() && !error.empty() && tryLegacyFallback())
            return sections;
        return sections;
    }

    namespace {
        class StreamInstallHelper final : public tin::install::Install {
        public:
            StreamInstallHelper(NcmStorageId dest_storage, bool ignore_req)
                : Install(dest_storage, ignore_req) {}

            void AddContentMeta(const nx::ncm::ContentMeta& meta, const NcmContentInfo& info) {
                m_contentMeta.push_back(meta);
                m_cnmt_infos.push_back(info);
            }

            void CommitLatest() {
                if (m_contentMeta.empty()) return;
                const size_t idx = m_contentMeta.size() - 1;
                tin::data::ByteBuffer install_buf;
                m_contentMeta[idx].GetInstallContentMeta(install_buf, m_cnmt_infos[idx], m_ignoreReqFirmVersion);
                InstallContentMetaRecords(install_buf, idx);
                InstallApplicationRecord(idx);
            }

            void CommitAll() {
                for (size_t i = 0; i < m_contentMeta.size(); i++) {
                    tin::data::ByteBuffer install_buf;
                    m_contentMeta[i].GetInstallContentMeta(install_buf, m_cnmt_infos[i], m_ignoreReqFirmVersion);
                    InstallContentMetaRecords(install_buf, i);
                    InstallApplicationRecord(i);
                }
            }

        private:
            std::vector<NcmContentInfo> m_cnmt_infos;
            std::vector<std::tuple<nx::ncm::ContentMeta, NcmContentInfo>> ReadCNMT() override { return {}; }
            void InstallTicketCert() override {}
            void InstallNCA(const NcmContentId& /*ncaId*/) override {}
        };

        class HttpStreamSource {
        public:
            explicit HttpStreamSource(tin::network::HTTPDownload& download) : m_download(download) {}

            Result Read(void* buf, s64 off, s64 size, u64* bytes_read) {
                if (off < 0 || size <= 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
                const size_t req_size = static_cast<size_t>(size);
                const size_t req_off = static_cast<size_t>(off);

                if (!m_cache.empty() && req_off >= m_cache_start && (req_off + req_size) <= m_cache_end) {
                    const size_t rel = req_off - m_cache_start;
                    std::memcpy(buf, m_cache.data() + rel, req_size);
                    *bytes_read = static_cast<u64>(size);
                    return 0;
                }

                const size_t fetch_size = std::min(req_size, kReadAheadSize);
                m_cache.resize(fetch_size);
                m_download.BufferDataRange(m_cache.data(), req_off, fetch_size, nullptr);
                m_cache_start = req_off;
                m_cache_end = req_off + fetch_size;

                std::memcpy(buf, m_cache.data(), req_size);
                *bytes_read = static_cast<u64>(size);
                return 0;
            }

        private:
            static constexpr size_t kReadAheadSize = 16 * 1024 * 1024;
            tin::network::HTTPDownload& m_download;
            std::vector<std::uint8_t> m_cache;
            size_t m_cache_start = 0;
            size_t m_cache_end = 0;
        };

        struct StreamHfs0Header {
            u32 magic;
            u32 total_files;
            u32 string_table_size;
            u32 padding;
        };

        struct StreamHfs0FileTableEntry {
            u64 data_offset;
            u64 data_size;
            u32 name_offset;
            u32 hash_size;
            u64 padding;
            u8 hash[0x20];
        };

        struct StreamHfs0 {
            StreamHfs0Header header{};
            std::vector<StreamHfs0FileTableEntry> file_table{};
            std::vector<std::string> string_table{};
            s64 data_offset{};
        };

        struct StreamCollectionEntry {
            std::string name;
            u64 offset{};
            u64 size{};
        };

        static bool ReadHfs0Partition(HttpStreamSource& source, s64 off, StreamHfs0& out) {
            u64 bytes_read = 0;
            if (R_FAILED(source.Read(&out.header, off, sizeof(out.header), &bytes_read))) return false;
            if (out.header.magic != 0x30534648) return false;
            if (out.header.total_files == 0 || out.header.total_files > 0x4000) return false;
            if (out.header.string_table_size > (256 * 1024)) return false;
            off += bytes_read;

            out.file_table.resize(out.header.total_files);
            const auto file_table_size = static_cast<s64>(out.file_table.size() * sizeof(StreamHfs0FileTableEntry));
            if (file_table_size > (4 * 1024 * 1024)) return false;
            if (R_FAILED(source.Read(out.file_table.data(), off, file_table_size, &bytes_read))) return false;
            off += bytes_read;

            std::vector<char> string_table(out.header.string_table_size);
            if (R_FAILED(source.Read(string_table.data(), off, string_table.size(), &bytes_read))) return false;
            off += bytes_read;

            out.string_table.clear();
            out.string_table.reserve(out.header.total_files);
            for (u32 i = 0; i < out.header.total_files; i++) {
                out.string_table.emplace_back(string_table.data() + out.file_table[i].name_offset);
            }

            out.data_offset = off;
            return true;
        }

        static bool GetXciCollections(HttpStreamSource& source, std::vector<StreamCollectionEntry>& out) {
            StreamHfs0 root{};
            s64 root_offset = 0xF000;
            if (!ReadHfs0Partition(source, root_offset, root)) {
                root_offset = 0x10000;
                if (!ReadHfs0Partition(source, root_offset, root)) {
                    return false;
                }
            }

            for (u32 i = 0; i < root.header.total_files; i++) {
                if (root.string_table[i] != "secure") continue;

                StreamHfs0 secure{};
                const auto secure_offset = root.data_offset + static_cast<s64>(root.file_table[i].data_offset);
                if (!ReadHfs0Partition(source, secure_offset, secure)) return false;

                out.clear();
                out.reserve(secure.header.total_files);
                for (u32 j = 0; j < secure.header.total_files; j++) {
                    StreamCollectionEntry entry;
                    entry.name = secure.string_table[j];
                    entry.offset = static_cast<u64>(secure.data_offset + static_cast<s64>(secure.file_table[j].data_offset));
                    entry.size = secure.file_table[j].data_size;
                    out.emplace_back(std::move(entry));
                }
                return true;
            }

            return false;
        }

        static bool InstallXciHttpStream(const std::string& url, NcmStorageId dest_storage) {
            tin::network::HTTPDownload download(url);
            HttpStreamSource source(download);

            std::vector<StreamCollectionEntry> collections;
            if (!GetXciCollections(source, collections)) return false;

            u64 totalBytes = 0;
            for (const auto& c : collections) {
                totalBytes += c.size;
            }
            u64 processedBytes = 0;
            u64 lastTick = armGetSystemTick();
            u64 lastProcessed = 0;
            const u64 freq = armGetSystemTickFreq();
            inst::ui::instPage::setInstInfoText("inst.info_page.preparing"_lang);
            inst::ui::instPage::setInstBarPerc(0);

            std::sort(collections.begin(), collections.end(), [](const auto& a, const auto& b) {
                return a.offset < b.offset;
            });

            struct EntryState {
                std::string name;
                NcmContentId nca_id{};
                std::uint64_t size = 0;
                std::uint64_t written = 0;
                bool started = false;
                bool complete = false;
                bool is_nca = false;
                bool is_cnmt = false;
                std::shared_ptr<nx::ncm::ContentStorage> storage;
                std::unique_ptr<NcaWriter> nca_writer;
                std::vector<std::uint8_t> ticket_buf;
                std::vector<std::uint8_t> cert_buf;
            };

            auto ensureStarted = [&](EntryState& entry) {
                if (entry.started) return true;
                if (!entry.is_nca) {
                    entry.started = true;
                    return true;
                }
                entry.storage = std::make_shared<nx::ncm::ContentStorage>(dest_storage);
                try { entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id); } catch (...) {}
                entry.nca_writer = std::make_unique<NcaWriter>(entry.nca_id, entry.storage);
                entry.started = true;
                return true;
            };

            StreamInstallHelper helper(dest_storage, inst::config::ignoreReqVers);

            std::unordered_map<std::string, EntryState> entries;
            entries.reserve(collections.size());
            auto cleanupEntries = [&entries]() {
                for (auto& [_, entry] : entries) {
                    if (!entry.is_nca || !entry.storage)
                        continue;
                    try {
                        if (entry.nca_writer)
                            entry.nca_writer->close();
                    } catch (...) {}
                    try { entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id); } catch (...) {}
                    try {
                        if (entry.storage->Has(entry.nca_id))
                            entry.storage->Delete(entry.nca_id);
                    } catch (...) {}
                }
            };

            std::vector<std::uint8_t> buf(0x800000);
            for (const auto& collection : collections) {
                if (inst::ui::instPage::isInstallCancelRequested()) {
                    cleanupEntries();
                    THROW_FORMAT("Installation canceled.");
                }
                EntryState entry;
                entry.name = collection.name;
                entry.size = collection.size;
                entry.is_nca = entry.name.find(".nca") != std::string::npos || entry.name.find(".ncz") != std::string::npos;
                entry.is_cnmt = entry.name.find(".cnmt.nca") != std::string::npos || entry.name.find(".cnmt.ncz") != std::string::npos;
                if (entry.is_nca && entry.name.size() >= 32) {
                    entry.nca_id = tin::util::GetNcaIdFromString(entry.name.substr(0, 32));
                }

                if (!ensureStarted(entry)) return false;

                u64 remaining = collection.size;
                u64 offset = collection.offset;
                while (remaining > 0) {
                    if (inst::ui::instPage::isInstallCancelRequested()) {
                        cleanupEntries();
                        THROW_FORMAT("Installation canceled.");
                    }
                    const auto chunk = static_cast<size_t>(std::min<u64>(remaining, buf.size()));
                    u64 bytes_read = 0;
                    if (R_FAILED(source.Read(buf.data(), static_cast<s64>(offset), static_cast<s64>(chunk), &bytes_read))) {
                        cleanupEntries();
                        return false;
                    }
                    if (bytes_read == 0) {
                        cleanupEntries();
                        return false;
                    }

                    if (entry.name.find(".tik") != std::string::npos) {
                        entry.ticket_buf.insert(entry.ticket_buf.end(), buf.data(), buf.data() + bytes_read);
                        entry.written += bytes_read;
                        if (entry.written >= entry.size) entry.complete = true;
                    } else if (entry.name.find(".cert") != std::string::npos) {
                        entry.cert_buf.insert(entry.cert_buf.end(), buf.data(), buf.data() + bytes_read);
                        entry.written += bytes_read;
                        if (entry.written >= entry.size) entry.complete = true;
                    } else if (entry.is_nca && entry.nca_writer) {
                        entry.nca_writer->write(buf.data(), bytes_read);
                        entry.written += bytes_read;
                        if (entry.written >= entry.size) {
                            entry.nca_writer->close();
                            try {
                                entry.storage->Register(*(NcmPlaceHolderId*)&entry.nca_id, entry.nca_id);
                                entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
                            } catch (...) {}
                            entry.complete = true;
                            if (entry.is_cnmt) {
                                try {
                                    std::string cnmt_path = entry.storage->GetPath(entry.nca_id);
                                    nx::ncm::ContentMeta meta = tin::util::GetContentMetaFromNCA(cnmt_path);
                                    NcmContentInfo cnmt_info{};
                                    cnmt_info.content_id = entry.nca_id;
                                    ncmU64ToContentInfoSize(entry.size & 0xFFFFFFFFFFFF, &cnmt_info);
                                    cnmt_info.content_type = NcmContentType_Meta;
                                    helper.AddContentMeta(meta, cnmt_info);
                                    helper.CommitLatest();
                                } catch (...) {}
                            }
                            // Release IPC handles immediately — no longer needed after
                            // Register/CommitLatest. Accumulating these across all NCA
                            // entries exhausts the kernel handle table on large XCI packages.
                            entry.nca_writer = nullptr;
                            entry.storage = nullptr;
                        }
                    }

                    offset += bytes_read;
                    remaining -= bytes_read;
                    processedBytes += bytes_read;

                    const u64 now = armGetSystemTick();
                    if (now - lastTick >= (freq / 2)) {
                        double speed = 0.0;
                        double speedBytesPerSec = 0.0;
                        if (processedBytes > lastProcessed) {
                            double deltaMb = (processedBytes - lastProcessed) / 1000000.0;
                            double deltaSec = (double)(now - lastTick) / (double)freq;
                            if (deltaSec > 0.0) {
                                speed = deltaMb / deltaSec;
                                speedBytesPerSec = (double)(processedBytes - lastProcessed) / deltaSec;
                            }
                        }
                        lastTick = now;
                        lastProcessed = processedBytes;

                        if (totalBytes > 0) {
                            int progress = (int)((double)processedBytes / (double)totalBytes * 100.0);
                            inst::ui::instPage::setInstBarPerc(progress);

                            std::string etaText = "--:--";
                            if (speedBytesPerSec > 0.0 && processedBytes < totalBytes) {
                                const auto etaSeconds = static_cast<std::uint64_t>((double)(totalBytes - processedBytes) / speedBytesPerSec);
                                etaText = FormatEta(etaSeconds);
                            }

                            inst::ui::instPage::setInstInfoText("inst.info_page.downloading"_lang + FormatOneDecimal(speed) + "MB/s");
                            inst::ui::instPage::setProgressDetailText(
                                "Downloaded " + FormatOneDecimal((double)processedBytes / 1000000.0) + " / " +
                                FormatOneDecimal((double)totalBytes / 1000000.0) + " MB (" +
                                std::to_string(progress) + "%) • ETA " + etaText
                            );
                        }
                    }
                }

                entries.emplace(entry.name, std::move(entry));
            }

            for (auto& [name, entry] : entries) {
                if (inst::ui::instPage::isInstallCancelRequested()) {
                    cleanupEntries();
                    THROW_FORMAT("Installation canceled.");
                }
                if (entry.name.find(".tik") != std::string::npos) {
                    const auto base = entry.name.substr(0, entry.name.size() - 4);
                    auto it = entries.find(base + ".cert");
                    if (it != entries.end() && !entry.ticket_buf.empty() && !it->second.cert_buf.empty()) {
                        ASSERT_OK(esImportTicket(entry.ticket_buf.data(), entry.ticket_buf.size(), it->second.cert_buf.data(), it->second.cert_buf.size()),
                            "Failed to import ticket");
                    }
                }
            }

            helper.CommitAll();
            inst::ui::instPage::setInstBarPerc(100);
            inst::ui::instPage::setProgressDetailText("Downloaded 100% • Verifying and installing...");
            return true;
        }
    }

    std::string FetchShopMotd(const std::string& shopUrl, const std::string& user, const std::string& pass)
    {
        std::string baseUrl = NormalizeShopUrl(shopUrl);
        if (baseUrl.empty())
            return "";

        FetchResult fetch = FetchShopResponse(baseUrl, user, pass);
        if (fetch.responseCode == 401 || fetch.responseCode == 403)
            return "";
        if (!fetch.error.empty())
            return "";
        if (fetch.body.rfind("TINFOIL", 0) == 0)
            return "";

        try {
            nlohmann::json shop = nlohmann::json::parse(fetch.body);
            if (shop.contains("success") && shop["success"].is_string())
                return shop["success"].get<std::string>();
        }
        catch (...) {
            return "";
        }

        return "";
    }

    void installTitleShop(const std::vector<ShopItem>& items, int storage, const std::string& sourceLabel)
    {
        inst::util::initInstallServices();
        inst::ui::instPage::loadInstallScreen();
        bool nspInstalled = true;
        NcmStorageId destStorageId = storage ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
        inst::diag::StartSession("shop", items.size());

        std::vector<std::string> names;
        names.reserve(items.size());
        for (const auto& item : items)
            names.push_back(inst::util::shortenString(item.name, 38, true));

        std::vector<int> previousClockValues;
        if (inst::config::overClock) {
            previousClockValues.push_back(inst::util::setClockSpeed(0, 1785000000)[0]);
            previousClockValues.push_back(inst::util::setClockSpeed(1, 76800000)[0]);
            previousClockValues.push_back(inst::util::setClockSpeed(2, 1600000000)[0]);
        }

        if (!inst::config::shopUser.empty() || !inst::config::shopPass.empty())
            tin::network::SetBasicAuth(inst::config::shopUser, inst::config::shopPass);
        else
            tin::network::ClearBasicAuth();

        std::string currentName;
        try {
            for (size_t i = 0; i < items.size(); i++) {
                LOG_DEBUG("%s %s\n", "Install request from", items[i].url.c_str());
                currentName = names[i];
                inst::diag::NoteTransferReceived(currentName);
                UpdateInstallIcon(items[i]);
                inst::ui::instPage::setTopInstInfoText("inst.info_page.top_info0"_lang + currentName + sourceLabel);
                std::unique_ptr<tin::install::Install> installTask;
                bool isXci = IsXciExtension(items[i].name) || IsXciExtension(items[i].url) || IsXciMagic(items[i].url);
                if (isXci) {
                    inst::ui::instPage::setInstInfoText("Transfer received. Install started...");
                    inst::diag::NoteInstallStarted(currentName);
                    if (!InstallXciHttpStream(items[i].url, destStorageId)) {
                        THROW_FORMAT("Failed to install XCI from shop.");
                    }
                    inst::diag::RecordSuccess(currentName);
                    continue;
                } else {
                    auto httpNSP = std::make_shared<tin::install::nsp::HTTPNSP>(items[i].url);
                    installTask = std::make_unique<tin::install::nsp::NSPInstall>(destStorageId, inst::config::ignoreReqVers, httpNSP);
                }

                LOG_DEBUG("%s\n", "Preparing installation");
                inst::ui::instPage::setInstInfoText("Transfer received. Install started...");
                inst::ui::instPage::setInstBarPerc(0);
                inst::diag::NoteInstallStarted(currentName);
                installTask->Prepare();
                installTask->Begin();
                inst::diag::RecordSuccess(currentName);
                inst::ui::instPage::setInstInfoText("Install succeeded: " + currentName);
            }
        }
        catch (std::exception& e) {
            LOG_DEBUG("Failed to install");
            LOG_DEBUG("%s", e.what());
            fprintf(stdout, "%s", e.what());
            std::string failedName = currentName.empty() ? names.front() : currentName;
            const std::string errorText = e.what();
            const auto failure = inst::diag::ClassifyFailure(errorText);
            inst::diag::RecordFailure(failedName, failure);
            if (failure.canceled) {
                inst::ui::instPage::setInstInfoText("Installation canceled.");
                inst::ui::instPage::setInstBarPerc(0);
                inst::ui::mainApp->CreateShowDialog("Canceled", inst::diag::BuildUserMessage(failure), {"common.ok"_lang}, true);
            } else {
                inst::ui::instPage::setInstInfoText("inst.info_page.failed"_lang + failedName);
                inst::ui::instPage::setInstBarPerc(0);
                std::string audioPath = "romfs:/audio/bark.wav";
                if (!inst::config::soundEnabled) audioPath = "";
                if (std::filesystem::exists(inst::config::appDir + "/bark.wav")) audioPath = inst::config::appDir + "/bark.wav";
                std::thread audioThread(inst::util::playAudio, audioPath);
                inst::ui::mainApp->CreateShowDialog("inst.info_page.failed"_lang + failedName + "!", inst::diag::BuildUserMessage(failure), {"common.ok"_lang}, true);
                audioThread.join();
            }
            nspInstalled = false;
        }

        tin::network::ClearBasicAuth();

        if (previousClockValues.size() > 0) {
            inst::util::setClockSpeed(0, previousClockValues[0]);
            inst::util::setClockSpeed(1, previousClockValues[1]);
            inst::util::setClockSpeed(2, previousClockValues[2]);
        }

        if (nspInstalled) {
            inst::ui::instPage::setInstInfoText("inst.info_page.complete"_lang);
            inst::ui::instPage::setInstBarPerc(100);
            std::string audioPath = "romfs:/audio/success.wav";
            if (!inst::config::soundEnabled) audioPath = "";
            if (std::filesystem::exists(inst::config::appDir + "/success.wav")) audioPath = inst::config::appDir + "/success.wav";
            std::thread audioThread(inst::util::playAudio, audioPath);
            if (items.size() > 1)
                inst::ui::mainApp->CreateShowDialog(std::to_string(items.size()) + "inst.info_page.desc0"_lang, Language::GetRandomMsg(), {"common.ok"_lang}, true);
            else
                inst::ui::mainApp->CreateShowDialog(names.front() + "inst.info_page.desc1"_lang, Language::GetRandomMsg(), {"common.ok"_lang}, true);
            audioThread.join();
        }

        LOG_DEBUG("Done");
        inst::queue::InstallQueue::Instance().MarkBatchComplete(nspInstalled);

        // Check if there are more items in the queue
        std::vector<ShopItem> nextBatch;
        int nextStorageId = 0;
        std::string nextSourceLabel;
        if (inst::queue::InstallQueue::Instance().PopNextBatch(nextBatch, nextStorageId, nextSourceLabel)) {
            // Continue with next batch without returning to shop
            inst::ui::instPage::loadMainMenu();
            inst::util::deinitInstallServices();
            installTitleShop(nextBatch, nextStorageId, nextSourceLabel);
            return;
        }

        inst::ui::instPage::loadMainMenu();
        inst::util::deinitInstallServices();
    }
}
