#include "../include/mtp_server.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>

#include "switch.h"
#include <haze.hpp>

#include "../include/mtp_install.hpp"
#include "util/error.hpp"

namespace inst::mtp {
namespace {

struct InstallSharedData {
    std::mutex mutex;
    bool enabled = false;
    bool in_progress = false;
    std::string current_file;
};

InstallSharedData g_shared;
bool g_running = false;
std::mutex g_state_mutex;
int g_storage_choice = 0;
bool g_ncm_ready = false;

constexpr u16 kMtpVid = 0x057e; // Nintendo
constexpr u16 kMtpPid = 0x201d; // Switch

struct FsProxyVfs : haze::FileSystemProxyImpl {
    struct File {
        u64 index{};
        u32 mode{};
    };

    struct Dir {
        u64 pos{};
    };

    FsProxyVfs(const char* name, const char* display_name)
    : m_name(name), m_display_name(display_name) {
    }

    const char* GetName() const override {
        return m_name.c_str();
    }

    const char* GetDisplayName() const override {
        return m_display_name.c_str();
    }

    Result GetTotalSpace(const char* /*path*/, s64* out) override {
        return QuerySpace(out, nullptr);
    }

    Result GetFreeSpace(const char* /*path*/, s64* out) override {
        return QuerySpace(nullptr, out);
    }

    Result GetEntryType(const char* path, FsDirEntryType* out_entry_type) override {
        if (std::strcmp(path, "/") == 0) {
            *out_entry_type = FsDirEntryType_Dir;
            return 0;
        }

        const auto file_name = GetFileName(path);
        if (!file_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [file_name](const auto& e){ return !strcasecmp(file_name, e.name); });
        if (it == m_entries.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        *out_entry_type = FsDirEntryType_File;
        return 0;
    }

    Result CreateFile(const char* path, s64 size, u32 option) override {
        const auto file_name = GetFileName(path);
        if (!file_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [file_name](const auto& e){ return !strcasecmp(file_name, e.name); });
        if (it != m_entries.end()) return KERNELRESULT(AlreadyExists);

        FsDirectoryEntry entry{};
        std::strcpy(entry.name, file_name);
        entry.type = FsDirEntryType_File;
        entry.file_size = size;
        m_entries.emplace_back(entry);
        return 0;
    }

    Result DeleteFile(const char* path) override {
        const auto file_name = GetFileName(path);
        if (!file_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [file_name](const auto& e){ return !strcasecmp(file_name, e.name); });
        if (it == m_entries.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        m_entries.erase(it);
        return 0;
    }

    Result RenameFile(const char* old_path, const char* new_path) override {
        const auto file_name = GetFileName(old_path);
        if (!file_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [file_name](const auto& e){ return !strcasecmp(file_name, e.name); });
        if (it == m_entries.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto new_name = GetFileName(new_path);
        if (!new_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto new_it = std::find_if(m_entries.begin(), m_entries.end(),
            [new_name](const auto& e){ return !strcasecmp(new_name, e.name); });
        if (new_it != m_entries.end()) return KERNELRESULT(AlreadyExists);

        std::strcpy(it->name, new_name);
        return 0;
    }

    Result OpenFile(const char* path, u32 mode, FsFile* out_file) override {
        const auto file_name = GetFileName(path);
        if (!file_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [file_name](const auto& e){ return !strcasecmp(file_name, e.name); });
        if (it == m_entries.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        std::memset(out_file, 0, sizeof(*out_file));
        auto file = std::make_unique<File>();
        file->index = static_cast<u64>(std::distance(m_entries.begin(), it));
        file->mode = mode;
        m_open_files.emplace(out_file, std::move(file));
        return 0;
    }

    Result GetFileSize(FsFile* file, s64* out_size) override {
        const auto it = m_open_files.find(file);
        if (it == m_open_files.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        *out_size = m_entries[it->second->index].file_size;
        return 0;
    }

    Result SetFileSize(FsFile* file, s64 size) override {
        const auto it = m_open_files.find(file);
        if (it == m_open_files.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        m_entries[it->second->index].file_size = size;
        return 0;
    }

    Result ReadFile(FsFile* /*file*/, s64 /*off*/, void* /*buf*/, u64 /*read_size*/, u32 /*option*/, u64* /*out_bytes_read*/) override {
        return KERNELRESULT(NotImplemented);
    }

    Result WriteFile(FsFile* file, s64 off, const void* /*buf*/, u64 write_size, u32 /*option*/) override {
        const auto it = m_open_files.find(file);
        if (it == m_open_files.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        const auto new_size = static_cast<s64>(off + static_cast<s64>(write_size));
        auto& entry = m_entries[it->second->index];
        if (new_size > entry.file_size) {
            entry.file_size = new_size;
        }
        return 0;
    }

    void CloseFile(FsFile* file) override {
        m_open_files.erase(file);
    }

    Result CreateDirectory(const char* /*path*/) override { return KERNELRESULT(NotImplemented); }
    Result DeleteDirectoryRecursively(const char* /*path*/) override { return KERNELRESULT(NotImplemented); }
    Result RenameDirectory(const char* /*old_path*/, const char* /*new_path*/) override { return KERNELRESULT(NotImplemented); }

    Result OpenDirectory(const char* /*path*/, u32 /*mode*/, FsDir* out_dir) override {
        std::memset(out_dir, 0, sizeof(*out_dir));
        auto dir = std::make_unique<Dir>();
        dir->pos = 0;
        m_open_dirs.emplace(out_dir, std::move(dir));
        return 0;
    }

    Result ReadDirectory(FsDir* d, s64* out_total_entries, size_t max_entries, FsDirectoryEntry* buf) override {
        const auto it = m_open_dirs.find(d);
        if (it == m_open_dirs.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        const auto pos = static_cast<size_t>(it->second->pos);
        if (pos >= m_entries.size()) {
            *out_total_entries = 0;
            return 0;
        }
        max_entries = std::min<size_t>(m_entries.size() - pos, max_entries);
        for (size_t i = 0; i < max_entries; i++) {
            buf[i] = m_entries[pos + i];
        }
        it->second->pos += max_entries;
        *out_total_entries = static_cast<s64>(max_entries);
        return 0;
    }

    Result GetDirectoryEntryCount(FsDir* /*d*/, s64* out_count) override {
        *out_count = m_entries.size();
        return 0;
    }

    void CloseDirectory(FsDir* d) override {
        m_open_dirs.erase(d);
    }

protected:
    Result QuerySpace(s64* out_total, s64* out_free) const {
        const auto storage_id = (g_storage_choice == 1) ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
        NcmContentStorage storage{};
        Result rc = ncmOpenContentStorage(&storage, storage_id);
        if (R_FAILED(rc)) return rc;
        if (out_total) {
            rc = ncmContentStorageGetTotalSpaceSize(&storage, out_total);
        }
        if (R_SUCCEEDED(rc) && out_free) {
            rc = ncmContentStorageGetFreeSpaceSize(&storage, out_free);
        }
        ncmContentStorageClose(&storage);
        return rc;
    }
    const char* GetFileName(const char* path) const {
        const auto* file_name = std::strrchr(path, '/');
        if (!file_name || file_name[1] == '\0') return nullptr;
        return file_name + 1;
    }

    std::string m_name;
    std::string m_display_name;
    std::vector<FsDirectoryEntry> m_entries;
    std::unordered_map<FsFile*, std::unique_ptr<File>> m_open_files;
    std::unordered_map<FsDir*, std::unique_ptr<Dir>> m_open_dirs;
};

struct FsInstallProxy final : FsProxyVfs {
    using FsProxyVfs::FsProxyVfs;

    bool IsValidFileType(const char* name) const {
        const auto* ext = std::strrchr(name, '.');
        if (!ext) return false;
        return !strcasecmp(ext, ".nsp") || !strcasecmp(ext, ".nsz") ||
               !strcasecmp(ext, ".xci") || !strcasecmp(ext, ".xcz");
    }

    Result CreateFile(const char* path, s64 size, u32 option) override {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        if (!g_shared.enabled) return KERNELRESULT(NotImplemented);
        if (!IsValidFileType(path)) return KERNELRESULT(NotImplemented);
        return FsProxyVfs::CreateFile(path, size, option);
    }

    Result OpenFile(const char* path, u32 mode, FsFile* out_file) override {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        if (!g_shared.enabled) return KERNELRESULT(NotImplemented);
        if (!IsValidFileType(path)) return KERNELRESULT(NotImplemented);

        Result rc = FsProxyVfs::OpenFile(path, mode, out_file);
        if (R_FAILED(rc)) return rc;

        if (mode & FsOpenMode_Write) {
            const auto it = m_open_files.find(out_file);
            if (it == m_open_files.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
            const auto& e = m_entries[it->second->index];

            if (!g_shared.current_file.empty()) return KERNELRESULT(NotImplemented);
            g_shared.current_file = e.name;
            g_shared.in_progress = true;
            if (!StartStreamInstall(e.name, e.file_size, g_storage_choice)) {
                g_shared.current_file.clear();
                g_shared.in_progress = false;
                return KERNELRESULT(NotImplemented);
            }
        }

        return 0;
    }

    Result WriteFile(FsFile* file, s64 off, const void* buf, u64 write_size, u32 option) override {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        if (!g_shared.enabled) return KERNELRESULT(NotImplemented);

        if (!WriteStreamInstall(buf, write_size, off)) {
            return KERNELRESULT(NotImplemented);
        }

        return FsProxyVfs::WriteFile(file, off, buf, write_size, option);
    }

    void CloseFile(FsFile* file) override {
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            const auto it = m_open_files.find(file);
            if (it != m_open_files.end() && (it->second->mode & FsOpenMode_Write)) {
                CloseStreamInstall();
                g_shared.current_file.clear();
                g_shared.in_progress = false;
            }
        }

        FsProxyVfs::CloseFile(file);
    }
};

haze::FsEntries g_entries;

} // namespace

bool StartInstallServer(int storage_choice)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_running) return true;

    g_storage_choice = storage_choice;
    if (!g_ncm_ready) {
        if (R_SUCCEEDED(ncmInitialize())) {
            g_ncm_ready = true;
        }
    }
    g_entries.clear();
    g_entries.emplace_back(std::make_shared<FsInstallProxy>("install", "Install (NSP, XCI, NSZ, XCZ)"));

    if (!haze::Initialize(nullptr, 0x2C, 2, g_entries, kMtpVid, kMtpPid)) {
        return false;
    }

    if (R_SUCCEEDED(usbDsDisable())) {
        svcSleepThread(50'000'000);
        usbDsEnable();
    }

    g_shared.enabled = true;
    g_running = true;
    return true;
}

void StopInstallServer()
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_running) return;
    haze::Exit();
    g_entries.clear();
    g_shared.enabled = false;
    if (g_ncm_ready) {
        ncmExit();
        g_ncm_ready = false;
    }
    g_running = false;
}

bool IsInstallServerRunning()
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return g_running;
}

} // namespace inst::mtp
