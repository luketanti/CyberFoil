#include "../include/mtp_install.hpp"

#include "mtp_install.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "switch.h"
#include <cstring>

#include "install/install_nsp.hpp"
#include "install/install_xci.hpp"
#include "install/install.hpp"
#include "install/nca.hpp"
#include "install/pfs0.hpp"
#include "install/hfs0.hpp"
#include "data/byte_buffer.hpp"
#include "nx/nca_writer.h"
#include "nx/ncm.hpp"
#include "util/config.hpp"
#include "util/error.hpp"
#include "util/file_util.hpp"
#include "util/title_util.hpp"
#include "util/lang.hpp"
#include "util/util.hpp"
#include "ui/MainApplication.hpp"
#include "ui/instPage.hpp"

namespace inst::ui {
    extern MainApplication* mainApp;
}

namespace inst::mtp {
namespace {

std::atomic<bool> g_stream_active{false};
std::atomic<bool> g_stream_complete{false};
std::atomic<std::uint64_t> g_stream_total{0};
std::atomic<std::uint64_t> g_stream_received{0};
std::atomic<std::uint64_t> g_stream_title_id{0};
std::mutex g_stream_mutex;
std::string g_stream_name;

class StreamInstaller {
public:
    StreamInstaller() = default;
    virtual ~StreamInstaller() = default;
    virtual bool Feed(const void* buf, size_t size, std::uint64_t offset) = 0;
    virtual bool Finalize() = 0;
};

class MtpNspStream final : public StreamInstaller {
public:
    explicit MtpNspStream(std::uint64_t total_size, NcmStorageId dest_storage);

    bool Feed(const void* buf, size_t size, std::uint64_t offset) override;
    bool Finalize() override;

private:
    struct EntryState {
        std::string name;
        NcmContentId nca_id{};
        std::uint64_t data_offset = 0;
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

    bool ParseHeaderIfReady();
    bool EnsureEntryStarted(EntryState& entry);
    bool WriteEntryData(EntryState& entry, const std::uint8_t* data, size_t size, std::uint64_t rel_offset);
    void MaybeUpdateProgress(std::uint64_t received);
    bool CommitCnmt(EntryState& entry);

    NcmStorageId m_dest_storage = NcmStorageId_SdCard;
    std::uint64_t m_total_size = 0;
    std::uint64_t m_received = 0;
    std::vector<std::uint8_t> m_header_bytes;
    std::vector<EntryState> m_entries;
    bool m_header_parsed = false;
    std::unique_ptr<class StreamInstallHelper> m_helper;
};

class MtpXciStream final : public StreamInstaller {
public:
    explicit MtpXciStream(std::uint64_t total_size, NcmStorageId dest_storage);

    bool Feed(const void* buf, size_t size, std::uint64_t offset) override;
    bool Finalize() override;

private:
    struct EntryState {
        std::string name;
        NcmContentId nca_id{};
        std::uint64_t data_offset = 0;
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

    bool ParseHeaderIfReady();
    bool EnsureEntryStarted(EntryState& entry);
    bool WriteEntryData(EntryState& entry, const std::uint8_t* data, size_t size, std::uint64_t rel_offset);
    void MaybeUpdateProgress(std::uint64_t received);
    bool CommitCnmt(EntryState& entry);
    bool ProcessChunk(const std::uint8_t* data, size_t size, std::uint64_t offset);

    NcmStorageId m_dest_storage = NcmStorageId_SdCard;
    std::uint64_t m_total_size = 0;
    std::uint64_t m_received = 0;
    std::uint64_t m_next_offset = 0;
    std::uint64_t m_header_offset = 0;
    std::vector<std::uint8_t> m_header_bytes;
    std::vector<std::uint8_t> m_header_bytes_alt;
    std::vector<std::uint8_t> m_secure_header_bytes;
    std::vector<EntryState> m_entries;
    bool m_header_parsed = false;
    std::uint64_t m_secure_header_offset = 0;
    std::unique_ptr<class StreamInstallHelper> m_helper;
    std::map<std::uint64_t, std::vector<std::uint8_t>> m_pending_chunks;
};

std::unique_ptr<StreamInstaller> g_stream;

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

bool IsXciName(const std::string& name) {
    auto pos = name.find_last_of('.');
    if (pos == std::string::npos) return false;
    auto ext = name.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".xci" || ext == ".xcz";
}

bool IsNspName(const std::string& name) {
    auto pos = name.find_last_of('.');
    if (pos == std::string::npos) return false;
    auto ext = name.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".nsp" || ext == ".nsz";
}

void ShowInstallScreen(const std::string& name) {
    inst::ui::instPage::loadInstallScreen();
    inst::ui::instPage::setTopInstInfoText("inst.info_page.top_info0"_lang + name + " (MTP)");
    inst::ui::instPage::setInstInfoText("inst.info_page.preparing"_lang);
    inst::ui::instPage::setInstBarPerc(0);
}

MtpNspStream::MtpNspStream(std::uint64_t total_size, NcmStorageId dest_storage)
    : m_dest_storage(dest_storage), m_total_size(total_size)
{
    m_helper = std::make_unique<StreamInstallHelper>(dest_storage, inst::config::ignoreReqVers);
}

MtpXciStream::MtpXciStream(std::uint64_t total_size, NcmStorageId dest_storage)
    : m_dest_storage(dest_storage), m_total_size(total_size)
{
    m_helper = std::make_unique<StreamInstallHelper>(dest_storage, inst::config::ignoreReqVers);
}


bool MtpNspStream::ParseHeaderIfReady()
{
    if (m_header_parsed) return true;
    if (m_header_bytes.size() < sizeof(tin::install::PFS0BaseHeader)) return false;

    const auto* base = reinterpret_cast<const tin::install::PFS0BaseHeader*>(m_header_bytes.data());
    if (base->magic != 0x30534650) {
        THROW_FORMAT("Invalid PFS0 magic");
    }

    const size_t header_size = sizeof(tin::install::PFS0BaseHeader) +
        base->numFiles * sizeof(tin::install::PFS0FileEntry) + base->stringTableSize;

    if (m_header_bytes.size() < header_size) return false;

    m_header_bytes.resize(header_size);
    m_header_parsed = true;

    m_entries.clear();
    for (u32 i = 0; i < base->numFiles; i++) {
        const auto* entry = reinterpret_cast<const tin::install::PFS0FileEntry*>(
            m_header_bytes.data() + sizeof(tin::install::PFS0BaseHeader) + i * sizeof(tin::install::PFS0FileEntry));
        const char* name = reinterpret_cast<const char*>(
            m_header_bytes.data() + sizeof(tin::install::PFS0BaseHeader) +
            base->numFiles * sizeof(tin::install::PFS0FileEntry) + entry->stringTableOffset);

        EntryState st;
        st.name = name;
        st.data_offset = header_size + entry->dataOffset;
        st.size = entry->fileSize;
        st.is_nca = st.name.find(".nca") != std::string::npos || st.name.find(".ncz") != std::string::npos;
        st.is_cnmt = st.name.find(".cnmt.nca") != std::string::npos || st.name.find(".cnmt.ncz") != std::string::npos;
        if (st.is_nca && st.name.size() >= 32) {
            st.nca_id = tin::util::GetNcaIdFromString(st.name.substr(0, 32));
        }
        m_entries.emplace_back(std::move(st));
    }

    return true;
}

bool MtpNspStream::EnsureEntryStarted(EntryState& entry)
{
    if (entry.started) return true;
    if (!entry.is_nca) {
        entry.started = true;
        return true;
    }

    entry.storage = std::make_shared<nx::ncm::ContentStorage>(m_dest_storage);
    try {
        entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
    } catch (...) {}
    entry.nca_writer = std::make_unique<NcaWriter>(entry.nca_id, entry.storage);
    entry.started = true;
    return true;
}

bool MtpNspStream::CommitCnmt(EntryState& entry)
{
    if (!entry.is_cnmt || !entry.storage) return false;

    try {
        std::string cnmt_path = entry.storage->GetPath(entry.nca_id);
        nx::ncm::ContentMeta meta = tin::util::GetContentMetaFromNCA(cnmt_path);
        {
            const auto key = meta.GetContentMetaKey();
            const auto base_id = tin::util::GetBaseTitleId(key.id, static_cast<NcmContentMetaType>(key.type));
            g_stream_title_id.store(base_id, std::memory_order_relaxed);
        }
        NcmContentInfo cnmt_info{};
        cnmt_info.content_id = entry.nca_id;
        ncmU64ToContentInfoSize(entry.size & 0xFFFFFFFFFFFF, &cnmt_info);
        cnmt_info.content_type = NcmContentType_Meta;
        m_helper->AddContentMeta(meta, cnmt_info);
        m_helper->CommitLatest();
        return true;
    } catch (...) {
        return false;
    }
}

bool MtpNspStream::WriteEntryData(EntryState& entry, const std::uint8_t* data, size_t size, std::uint64_t rel_offset)
{
    if (entry.name.find(".tik") != std::string::npos) {
        entry.ticket_buf.insert(entry.ticket_buf.end(), data, data + size);
        entry.written += size;
        if (entry.written >= entry.size) {
            entry.complete = true;
        }
        return true;
    }
    if (entry.name.find(".cert") != std::string::npos) {
        entry.cert_buf.insert(entry.cert_buf.end(), data, data + size);
        entry.written += size;
        if (entry.written >= entry.size) {
            entry.complete = true;
        }
        return true;
    }

    if (!entry.is_nca || !entry.nca_writer) return false;
    if (rel_offset != entry.written) return false;
    entry.nca_writer->write(data, size);
    entry.written += size;
    if (entry.written >= entry.size) {
        entry.nca_writer->close();
        try {
            entry.storage->Register(*(NcmPlaceHolderId*)&entry.nca_id, entry.nca_id);
            entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
        } catch (...) {}
        entry.complete = true;
        if (entry.is_cnmt) {
            CommitCnmt(entry);
        }
    }
    return true;
}

void MtpNspStream::MaybeUpdateProgress(std::uint64_t received)
{
    if (!m_total_size) return;
    double percent = (double)received / (double)m_total_size * 100.0;
    inst::ui::instPage::setInstBarPerc(percent);
}

bool MtpNspStream::Feed(const void* buf, size_t size, std::uint64_t offset)
{
    try {
    const auto* data = static_cast<const std::uint8_t*>(buf);
    if (offset == m_received) {
        m_received += size;
    }

    if (m_total_size) {
        const auto current = g_stream_received.load(std::memory_order_relaxed);
        if (m_received > current) {
            g_stream_received.store(m_received, std::memory_order_relaxed);
        }
    }
    if (!m_header_parsed && offset <= 0x20000) {
        const auto start = offset;
        const auto end = std::min<std::uint64_t>(offset + size, 0x20000);
        const auto rel = start;
        const auto len = static_cast<size_t>(end - start);
        if (m_header_bytes.size() < rel + len) {
            m_header_bytes.resize(rel + len);
        }
        std::memcpy(m_header_bytes.data() + rel, data, len);
    }

    if (!ParseHeaderIfReady()) {
        return true;
    }

    for (auto& entry : m_entries) {
        const auto entry_start = entry.data_offset;
        const auto entry_end = entry.data_offset + entry.size;
        const auto chunk_start = offset;
        const auto chunk_end = offset + size;

        if (chunk_end <= entry_start || chunk_start >= entry_end) continue;

        const auto write_start = std::max<std::uint64_t>(chunk_start, entry_start);
        const auto write_end = std::min<std::uint64_t>(chunk_end, entry_end);
        const auto rel = write_start - chunk_start;
        const auto write_size = static_cast<size_t>(write_end - write_start);

        if (!EnsureEntryStarted(entry)) return false;
        const auto entry_rel = write_start - entry_start;
        if (!WriteEntryData(entry, data + rel, write_size, entry_rel)) return false;
    }

    return true;
    } catch (...) {
        return false;
    }
}

bool MtpNspStream::Finalize()
{
    if (!m_helper) return true;

    std::vector<std::vector<std::uint8_t>> tickets;
    std::vector<std::vector<std::uint8_t>> certs;
    for (const auto& entry : m_entries) {
        if (entry.name.find(".tik") != std::string::npos) {
            tickets.push_back(entry.ticket_buf);
        }
        if (entry.name.find(".cert") != std::string::npos) {
            certs.push_back(entry.cert_buf);
        }
    }

    const size_t count = std::min(tickets.size(), certs.size());
    for (size_t i = 0; i < count; i++) {
        if (!tickets[i].empty() && !certs[i].empty()) {
            ASSERT_OK(esImportTicket(tickets[i].data(), tickets[i].size(), certs[i].data(), certs[i].size()),
                "Failed to import ticket");
        }
    }

    m_helper->CommitAll();
    return true;
}

bool MtpXciStream::ParseHeaderIfReady()
{
    if (m_header_parsed) return true;
    if (!m_header_offset) {
        if (m_header_bytes.size() >= sizeof(tin::install::HFS0BaseHeader)) {
            const auto* base = reinterpret_cast<const tin::install::HFS0BaseHeader*>(m_header_bytes.data());
            if (base->magic == MAGIC_HFS0) {
                m_header_offset = 0xf000;
            }
        }
        if (!m_header_offset && m_header_bytes_alt.size() >= sizeof(tin::install::HFS0BaseHeader)) {
            const auto* base = reinterpret_cast<const tin::install::HFS0BaseHeader*>(m_header_bytes_alt.data());
            if (base->magic == MAGIC_HFS0) {
                m_header_offset = 0x10000;
                m_header_bytes.swap(m_header_bytes_alt);
            }
        }
        if (!m_header_offset) {
            if (m_header_bytes.size() >= sizeof(tin::install::HFS0BaseHeader) &&
                m_header_bytes_alt.size() >= sizeof(tin::install::HFS0BaseHeader)) {
                THROW_FORMAT("Invalid HFS0 magic");
            }
            return false;
        }
    }

    if (m_header_bytes.size() < sizeof(tin::install::HFS0BaseHeader)) return false;

    const auto* base = reinterpret_cast<const tin::install::HFS0BaseHeader*>(m_header_bytes.data());
    if (base->magic != MAGIC_HFS0) {
        THROW_FORMAT("Invalid HFS0 magic");
    }

    const size_t header_size = sizeof(tin::install::HFS0BaseHeader) +
        base->numFiles * sizeof(tin::install::HFS0FileEntry) + base->stringTableSize;

    if (m_header_bytes.size() < header_size) return false;
    m_header_bytes.resize(header_size);

    for (u32 i = 0; i < base->numFiles; i++) {
        const auto* entry = tin::install::hfs0GetFileEntry(base, i);
        const char* name = tin::install::hfs0GetFileName(base, entry);
        if (std::strcmp(name, "secure") == 0) {
            m_secure_header_offset = m_header_offset + header_size + entry->dataOffset;
            break;
        }
    }

    if (!m_secure_header_offset) return false;
    if (m_secure_header_bytes.size() < sizeof(tin::install::HFS0BaseHeader)) return false;

    const auto* secure_base = reinterpret_cast<const tin::install::HFS0BaseHeader*>(m_secure_header_bytes.data());
    if (secure_base->magic != MAGIC_HFS0) {
        THROW_FORMAT("Invalid secure HFS0 magic");
    }

    const size_t secure_header_size = sizeof(tin::install::HFS0BaseHeader) +
        secure_base->numFiles * sizeof(tin::install::HFS0FileEntry) + secure_base->stringTableSize;

    if (m_secure_header_bytes.size() < secure_header_size) return false;
    m_secure_header_bytes.resize(secure_header_size);

    m_entries.clear();
    for (u32 i = 0; i < secure_base->numFiles; i++) {
        const auto* entry = tin::install::hfs0GetFileEntry(secure_base, i);
        const char* name = tin::install::hfs0GetFileName(secure_base, entry);
        EntryState st;
        st.name = name;
        st.data_offset = m_secure_header_offset + secure_header_size + entry->dataOffset;
        st.size = entry->fileSize;
        st.is_nca = st.name.find(".nca") != std::string::npos || st.name.find(".ncz") != std::string::npos;
        st.is_cnmt = st.name.find(".cnmt.nca") != std::string::npos || st.name.find(".cnmt.ncz") != std::string::npos;
        if (st.is_nca && st.name.size() >= 32) {
            st.nca_id = tin::util::GetNcaIdFromString(st.name.substr(0, 32));
        }
        m_entries.emplace_back(std::move(st));
    }

    m_header_parsed = true;
    return true;
}

bool MtpXciStream::EnsureEntryStarted(EntryState& entry)
{
    if (entry.started) return true;
    if (!entry.is_nca) {
        entry.started = true;
        return true;
    }

    entry.storage = std::make_shared<nx::ncm::ContentStorage>(m_dest_storage);
    try {
        entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
    } catch (...) {}
    entry.nca_writer = std::make_unique<NcaWriter>(entry.nca_id, entry.storage);
    entry.started = true;
    return true;
}

bool MtpXciStream::CommitCnmt(EntryState& entry)
{
    if (!entry.is_cnmt || !entry.storage) return false;

    try {
        std::string cnmt_path = entry.storage->GetPath(entry.nca_id);
        nx::ncm::ContentMeta meta = tin::util::GetContentMetaFromNCA(cnmt_path);
        {
            const auto key = meta.GetContentMetaKey();
            const auto base_id = tin::util::GetBaseTitleId(key.id, static_cast<NcmContentMetaType>(key.type));
            g_stream_title_id.store(base_id, std::memory_order_relaxed);
        }
        NcmContentInfo cnmt_info{};
        cnmt_info.content_id = entry.nca_id;
        ncmU64ToContentInfoSize(entry.size & 0xFFFFFFFFFFFF, &cnmt_info);
        cnmt_info.content_type = NcmContentType_Meta;
        m_helper->AddContentMeta(meta, cnmt_info);
        m_helper->CommitLatest();
        return true;
    } catch (...) {
        return false;
    }
}

bool MtpXciStream::WriteEntryData(EntryState& entry, const std::uint8_t* data, size_t size, std::uint64_t rel_offset)
{
    if (entry.name.find(".tik") != std::string::npos) {
        entry.ticket_buf.insert(entry.ticket_buf.end(), data, data + size);
        entry.written += size;
        if (entry.written >= entry.size) {
            entry.complete = true;
        }
        return true;
    }
    if (entry.name.find(".cert") != std::string::npos) {
        entry.cert_buf.insert(entry.cert_buf.end(), data, data + size);
        entry.written += size;
        if (entry.written >= entry.size) {
            entry.complete = true;
        }
        return true;
    }

    if (!entry.is_nca || !entry.nca_writer) return false;
    if (rel_offset != entry.written) return false;
    entry.nca_writer->write(data, size);
    entry.written += size;
    if (entry.written >= entry.size) {
        entry.nca_writer->close();
        try {
            entry.storage->Register(*(NcmPlaceHolderId*)&entry.nca_id, entry.nca_id);
            entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
        } catch (...) {}
        entry.complete = true;
        if (entry.is_cnmt) {
            CommitCnmt(entry);
        }
    }
    return true;
}

void MtpXciStream::MaybeUpdateProgress(std::uint64_t received)
{
    if (!m_total_size) return;
    double percent = (double)received / (double)m_total_size * 100.0;
    inst::ui::instPage::setInstBarPerc(percent);
}

bool MtpXciStream::ProcessChunk(const std::uint8_t* data, size_t size, std::uint64_t offset)
{
    const u64 hfs0_offset = 0xf000;
    const u64 hfs0_offset_alt = 0x10000;
    const u64 header_max = 0x20000;
    if (offset + size > hfs0_offset && offset < hfs0_offset + header_max) {
        const auto start = std::max<std::uint64_t>(offset, hfs0_offset);
        const auto end = std::min<std::uint64_t>(offset + size, hfs0_offset + header_max);
        const auto rel = start - hfs0_offset;
        const auto len = static_cast<size_t>(end - start);
        if (m_header_bytes.size() < rel + len) {
            m_header_bytes.resize(rel + len);
        }
        std::memcpy(m_header_bytes.data() + rel, data + (start - offset), len);
    }
    if (offset + size > hfs0_offset_alt && offset < hfs0_offset_alt + header_max) {
        const auto start = std::max<std::uint64_t>(offset, hfs0_offset_alt);
        const auto end = std::min<std::uint64_t>(offset + size, hfs0_offset_alt + header_max);
        const auto rel = start - hfs0_offset_alt;
        const auto len = static_cast<size_t>(end - start);
        if (m_header_bytes_alt.size() < rel + len) {
            m_header_bytes_alt.resize(rel + len);
        }
        std::memcpy(m_header_bytes_alt.data() + rel, data + (start - offset), len);
    }

    if (m_secure_header_offset && offset + size > m_secure_header_offset && offset < m_secure_header_offset + header_max) {
        const auto start = std::max<std::uint64_t>(offset, m_secure_header_offset);
        const auto end = std::min<std::uint64_t>(offset + size, m_secure_header_offset + header_max);
        const auto rel = start - m_secure_header_offset;
        const auto len = static_cast<size_t>(end - start);
        if (m_secure_header_bytes.size() < rel + len) {
            m_secure_header_bytes.resize(rel + len);
        }
        std::memcpy(m_secure_header_bytes.data() + rel, data + (start - offset), len);
    }

    if (!ParseHeaderIfReady()) {
        return true;
    }

    for (auto& entry : m_entries) {
        const auto entry_start = entry.data_offset;
        const auto entry_end = entry.data_offset + entry.size;
        const auto chunk_start = offset;
        const auto chunk_end = offset + size;

        if (chunk_end <= entry_start || chunk_start >= entry_end) continue;

        const auto write_start = std::max<std::uint64_t>(chunk_start, entry_start);
        const auto write_end = std::min<std::uint64_t>(chunk_end, entry_end);
        const auto rel = write_start - chunk_start;
        const auto write_size = static_cast<size_t>(write_end - write_start);

        if (!EnsureEntryStarted(entry)) return false;
        const auto entry_rel = write_start - entry_start;
        if (!WriteEntryData(entry, data + rel, write_size, entry_rel)) return false;
    }

    return true;
}

bool MtpXciStream::Feed(const void* buf, size_t size, std::uint64_t offset)
{
    try {
        if (!size) return true;

        if (offset < m_next_offset) {
            const auto skip = static_cast<size_t>(m_next_offset - offset);
            if (skip >= size) return true;
            offset += skip;
            buf = static_cast<const std::uint8_t*>(buf) + skip;
            size -= skip;
        }

        std::vector<std::uint8_t> chunk(size);
        std::memcpy(chunk.data(), buf, size);
        m_pending_chunks.emplace(offset, std::move(chunk));

        while (true) {
            auto it = m_pending_chunks.find(m_next_offset);
            if (it == m_pending_chunks.end()) break;
            const auto& data = it->second;
            if (!ProcessChunk(data.data(), data.size(), m_next_offset)) return false;
            m_next_offset += data.size();
            m_received = m_next_offset;
            if (m_total_size) {
                const auto current = g_stream_received.load(std::memory_order_relaxed);
                if (m_received > current) {
                    g_stream_received.store(m_received, std::memory_order_relaxed);
                }
            }
            m_pending_chunks.erase(it);
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool MtpXciStream::Finalize()
{
    if (!m_helper) return true;

    std::vector<std::vector<std::uint8_t>> tickets;
    std::vector<std::vector<std::uint8_t>> certs;
    for (const auto& entry : m_entries) {
        if (entry.name.find(".tik") != std::string::npos) {
            tickets.push_back(entry.ticket_buf);
        }
        if (entry.name.find(".cert") != std::string::npos) {
            certs.push_back(entry.cert_buf);
        }
    }

    const size_t count = std::min(tickets.size(), certs.size());
    for (size_t i = 0; i < count; i++) {
        if (!tickets[i].empty() && !certs[i].empty()) {
            ASSERT_OK(esImportTicket(tickets[i].data(), tickets[i].size(), certs[i].data(), certs[i].size()),
                "Failed to import ticket");
        }
    }

    m_helper->CommitAll();
    return true;
}

class MtpStreamBuffer {
public:
    explicit MtpStreamBuffer(size_t max_size) : m_max_size(max_size) {}

    bool Push(const void* buf, size_t size) {
        const auto* data = static_cast<const std::uint8_t*>(buf);
        while (size > 0) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_can_write.wait(lock, [&]() { return !m_active || m_buffer.size() < m_max_size; });
            if (!m_active) return false;

            const size_t writable = m_max_size - m_buffer.size();
            const size_t chunk = std::min<size_t>(size, writable);
            const size_t offset = m_buffer.size();
            m_buffer.resize(offset + chunk);
            std::memcpy(m_buffer.data() + offset, data, chunk);
            data += chunk;
            size -= chunk;
            lock.unlock();
            m_can_read.notify_one();
        }
        return true;
    }

    bool ReadChunk(void* buf, size_t size, u64* out_read) {
        auto* out = static_cast<std::uint8_t*>(buf);
        *out_read = 0;
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_active && m_buffer.empty()) {
            m_can_read.wait(lock);
        }
        if (!m_active && m_buffer.empty()) {
            return false;
        }

        const size_t chunk = std::min<size_t>(size, m_buffer.size());
        std::memcpy(out, m_buffer.data(), chunk);
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + chunk);
        *out_read = chunk;
        lock.unlock();
        m_can_write.notify_one();
        return true;
    }

    void Disable() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = false;
        m_can_read.notify_all();
        m_can_write.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_can_read;
    std::condition_variable m_can_write;
    std::vector<std::uint8_t> m_buffer;
    size_t m_max_size = 0;
    bool m_active = true;
};

class MtpStreamSource {
public:
    explicit MtpStreamSource(MtpStreamBuffer& buffer) : m_buffer(buffer) {}

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) {
        if (off < m_offset) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        auto* out = static_cast<std::uint8_t*>(buf);
        *bytes_read = 0;
        std::vector<std::uint8_t> temp(0x10000);

        while (size > 0) {
            if (off > m_offset) {
                const auto skip = static_cast<s64>(off - m_offset);
                const auto chunk = static_cast<size_t>(std::min<s64>(skip, static_cast<s64>(temp.size())));
                u64 read = 0;
                if (!m_buffer.ReadChunk(temp.data(), chunk, &read)) {
                    return KERNELRESULT(NotImplemented);
                }
                m_offset += static_cast<s64>(read);
                continue;
            }

            const auto chunk = static_cast<size_t>(std::min<s64>(size, static_cast<s64>(temp.size())));
            u64 read = 0;
            if (!m_buffer.ReadChunk(out, chunk, &read)) {
                return KERNELRESULT(NotImplemented);
            }
            *bytes_read += read;
            out += read;
            m_offset += static_cast<s64>(read);
            off += static_cast<s64>(read);
            size -= static_cast<s64>(read);
        }

        return 0;
    }

private:
    MtpStreamBuffer& m_buffer;
    s64 m_offset = 0;
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

static bool ReadHfs0Partition(MtpStreamSource& source, s64 off, StreamHfs0& out) {
    u64 bytes_read = 0;
    if (R_FAILED(source.Read(&out.header, off, sizeof(out.header), &bytes_read))) return false;
    if (out.header.magic != 0x30534648) return false;
    off += bytes_read;

    out.file_table.resize(out.header.total_files);
    const auto file_table_size = static_cast<s64>(out.file_table.size() * sizeof(StreamHfs0FileTableEntry));
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

static bool GetXciCollections(MtpStreamSource& source, std::vector<StreamCollectionEntry>& out) {
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

class MtpXciStreamPull final : public StreamInstaller {
public:
    explicit MtpXciStreamPull(std::uint64_t total_size, NcmStorageId dest_storage)
        : m_dest_storage(dest_storage), m_total_size(total_size), m_buffer(1024 * 1024) {
        m_helper = std::make_unique<StreamInstallHelper>(dest_storage, inst::config::ignoreReqVers);
        m_thread = std::thread([this]() {
            MtpStreamSource source(m_buffer);
            m_ok.store(InstallFromSource(source), std::memory_order_relaxed);
            m_done.store(true, std::memory_order_relaxed);
            m_buffer.Disable();
        });
    }

    ~MtpXciStreamPull() override {
        m_buffer.Disable();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    bool Feed(const void* buf, size_t size, std::uint64_t /*offset*/) override {
        if (size == 0) return true;
        m_received += size;
        if (m_total_size) {
            const auto current = g_stream_received.load(std::memory_order_relaxed);
            if (m_received > current) {
                g_stream_received.store(m_received, std::memory_order_relaxed);
            }
        }
        return m_buffer.Push(buf, size);
    }

    bool Finalize() override {
        m_buffer.Disable();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        return m_ok.load(std::memory_order_relaxed);
    }

private:
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

    bool EnsureEntryStarted(EntryState& entry) {
        if (entry.started) return true;
        if (!entry.is_nca) {
            entry.started = true;
            return true;
        }

        entry.storage = std::make_shared<nx::ncm::ContentStorage>(m_dest_storage);
        try {
            entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
        } catch (...) {}
        entry.nca_writer = std::make_unique<NcaWriter>(entry.nca_id, entry.storage);
        entry.started = true;
        return true;
    }

    bool CommitCnmt(EntryState& entry) {
        if (!entry.is_cnmt || !entry.storage) return false;

        try {
            std::string cnmt_path = entry.storage->GetPath(entry.nca_id);
            nx::ncm::ContentMeta meta = tin::util::GetContentMetaFromNCA(cnmt_path);
            {
                const auto key = meta.GetContentMetaKey();
                const auto base_id = tin::util::GetBaseTitleId(key.id, static_cast<NcmContentMetaType>(key.type));
                g_stream_title_id.store(base_id, std::memory_order_relaxed);
            }
            NcmContentInfo cnmt_info{};
            cnmt_info.content_id = entry.nca_id;
            ncmU64ToContentInfoSize(entry.size & 0xFFFFFFFFFFFF, &cnmt_info);
            cnmt_info.content_type = NcmContentType_Meta;
            m_helper->AddContentMeta(meta, cnmt_info);
            m_helper->CommitLatest();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool WriteEntryData(EntryState& entry, const std::uint8_t* data, size_t size) {
        if (entry.name.find(".tik") != std::string::npos) {
            entry.ticket_buf.insert(entry.ticket_buf.end(), data, data + size);
            entry.written += size;
            if (entry.written >= entry.size) {
                entry.complete = true;
            }
            return true;
        }
        if (entry.name.find(".cert") != std::string::npos) {
            entry.cert_buf.insert(entry.cert_buf.end(), data, data + size);
            entry.written += size;
            if (entry.written >= entry.size) {
                entry.complete = true;
            }
            return true;
        }

        if (!entry.is_nca || !entry.nca_writer) return false;
        entry.nca_writer->write(data, size);
        entry.written += size;
        if (entry.written >= entry.size) {
            entry.nca_writer->close();
            try {
                entry.storage->Register(*(NcmPlaceHolderId*)&entry.nca_id, entry.nca_id);
                entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
            } catch (...) {}
            entry.complete = true;
            if (entry.is_cnmt) {
                CommitCnmt(entry);
            }
        }
        return true;
    }

    bool InstallFromSource(MtpStreamSource& source) {
        std::vector<StreamCollectionEntry> collections;
        if (!GetXciCollections(source, collections)) return false;

        std::sort(collections.begin(), collections.end(), [](const auto& a, const auto& b) {
            return a.offset < b.offset;
        });

        std::unordered_map<std::string, EntryState> entries;
        entries.reserve(collections.size());

        for (const auto& collection : collections) {
            EntryState entry;
            entry.name = collection.name;
            entry.size = collection.size;
            entry.is_nca = entry.name.find(".nca") != std::string::npos || entry.name.find(".ncz") != std::string::npos;
            entry.is_cnmt = entry.name.find(".cnmt.nca") != std::string::npos || entry.name.find(".cnmt.ncz") != std::string::npos;
            if (entry.is_nca && entry.name.size() >= 32) {
                entry.nca_id = tin::util::GetNcaIdFromString(entry.name.substr(0, 32));
            }

            if (!EnsureEntryStarted(entry)) return false;

            u64 remaining = collection.size;
            u64 offset = collection.offset;
            std::vector<std::uint8_t> buf(0x400000);
            while (remaining > 0) {
                const auto chunk = static_cast<size_t>(std::min<u64>(remaining, buf.size()));
                u64 bytes_read = 0;
                if (R_FAILED(source.Read(buf.data(), static_cast<s64>(offset), static_cast<s64>(chunk), &bytes_read))) {
                    return false;
                }
                if (bytes_read == 0) return false;
                if (!WriteEntryData(entry, buf.data(), static_cast<size_t>(bytes_read))) return false;
                offset += bytes_read;
                remaining -= bytes_read;
            }

            entries.emplace(entry.name, std::move(entry));
        }

        for (auto& [name, entry] : entries) {
            if (entry.name.find(".tik") != std::string::npos) {
                const auto base = entry.name.substr(0, entry.name.size() - 4);
                auto it = entries.find(base + ".cert");
                if (it != entries.end() && !entry.ticket_buf.empty() && !it->second.cert_buf.empty()) {
                    ASSERT_OK(esImportTicket(entry.ticket_buf.data(), entry.ticket_buf.size(), it->second.cert_buf.data(), it->second.cert_buf.size()),
                        "Failed to import ticket");
                }
            }
        }

        m_helper->CommitAll();
        return true;
    }

    NcmStorageId m_dest_storage = NcmStorageId_SdCard;
    std::uint64_t m_total_size = 0;
    std::uint64_t m_received = 0;
    MtpStreamBuffer m_buffer;
    std::thread m_thread;
    std::atomic<bool> m_done{false};
    std::atomic<bool> m_ok{true};
    std::unique_ptr<StreamInstallHelper> m_helper;
};

} // namespace

bool StartStreamInstall(const std::string& name, std::uint64_t size, int storage_choice)
{
    g_stream.reset();

    g_stream_total.store(size, std::memory_order_relaxed);
    g_stream_received.store(0, std::memory_order_relaxed);
    g_stream_active.store(true, std::memory_order_relaxed);
    g_stream_complete.store(false, std::memory_order_relaxed);
    g_stream_title_id.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        g_stream_name = name;
    }

    NcmStorageId storage = (storage_choice == 1) ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
    if (IsNspName(name)) {
        g_stream = std::make_unique<MtpNspStream>(size, storage);
    } else if (IsXciName(name)) {
        g_stream = std::make_unique<MtpXciStreamPull>(size, storage);
    } else {
        return false;
    }

    inst::util::initInstallServices();
    return true;
}

bool WriteStreamInstall(const void* buf, size_t size, std::uint64_t offset)
{
    if (!g_stream) return false;
    return g_stream->Feed(buf, size, offset);
}

void CloseStreamInstall()
{
    if (!g_stream) return;
    g_stream->Finalize();
    g_stream.reset();
    g_stream_active.store(false, std::memory_order_relaxed);
    g_stream_complete.store(true, std::memory_order_relaxed);
    inst::util::deinitInstallServices();
}

bool IsStreamInstallActive()
{
    return g_stream_active.load(std::memory_order_relaxed);
}

bool ConsumeStreamInstallComplete()
{
    if (!g_stream_complete.load(std::memory_order_relaxed)) {
        return false;
    }
    g_stream_complete.store(false, std::memory_order_relaxed);
    return true;
}

void GetStreamInstallProgress(std::uint64_t* out_received, std::uint64_t* out_total)
{
    if (out_received) {
        *out_received = g_stream_received.load(std::memory_order_relaxed);
    }
    if (out_total) {
        *out_total = g_stream_total.load(std::memory_order_relaxed);
    }
}

std::string GetStreamInstallName()
{
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    return g_stream_name;
}

bool GetStreamInstallTitleId(std::uint64_t* out_title_id)
{
    const auto value = g_stream_title_id.load(std::memory_order_relaxed);
    if (value == 0) {
        return false;
    }
    if (out_title_id) {
        *out_title_id = value;
    }
    return true;
}

} // namespace inst::mtp
