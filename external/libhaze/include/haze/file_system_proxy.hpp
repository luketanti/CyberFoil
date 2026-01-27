/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <haze/common.hpp>
#include <haze/event_reactor.hpp>

namespace haze {

    class FileSystemProxy final {
        private:
            EventReactor *m_reactor;
            std::shared_ptr<FileSystemProxyImpl> m_filesystem;
        public:
            constexpr explicit FileSystemProxy() : m_reactor(), m_filesystem() { /* ... */ }

            void Initialize(EventReactor *reactor, std::shared_ptr<FileSystemProxyImpl> fs) {
                HAZE_ASSERT(fs != nullptr);

                m_reactor = reactor;
                m_filesystem = std::move(fs);
            }

            void Finalize() {
                m_reactor = nullptr;
                m_filesystem = nullptr;
            }
        private:
            // template <typename F, typename... Args>
            Result ForwardResult(Result rc) {
                /* If the event loop was stopped, return that here. */
                R_TRY(m_reactor->GetResult());

                /* Otherwise, return the call result. */
                R_RETURN(rc);
            }

            auto FixPath(const char* path) const {
                // removes the leading '/' from the parent path concat.
                if (util::Strlen(path) > 1) {
                    return path + 1;
                }
                return path;
            }
        public:
            Result GetTotalSpace(const char *path, s64 *out) {
                R_RETURN(this->ForwardResult(m_filesystem->GetTotalSpace(FixPath(path), out)));
            }

            Result GetFreeSpace(const char *path, s64 *out) {
                R_RETURN(this->ForwardResult(m_filesystem->GetFreeSpace(FixPath(path), out)));
            }

            Result GetEntryType(const char *path, FsDirEntryType *out_entry_type) {
                R_RETURN(this->ForwardResult(m_filesystem->GetEntryType(FixPath(path), out_entry_type)));
            }

            Result CreateFile(const char* path, s64 size, u32 option) {
                R_RETURN(this->ForwardResult(m_filesystem->CreateFile(FixPath(path), size, option)));
            }

            Result DeleteFile(const char* path) {
                R_RETURN(this->ForwardResult(m_filesystem->DeleteFile(FixPath(path))));
            }

            Result RenameFile(const char *old_path, const char *new_path) {
                R_RETURN(this->ForwardResult(m_filesystem->RenameFile(FixPath(old_path), FixPath(new_path))));
            }

            Result OpenFile(const char *path, u32 mode, FsFile *out_file) {
                R_RETURN(this->ForwardResult(m_filesystem->OpenFile(FixPath(path), mode, out_file)));
            }

            Result GetFileSize(FsFile *file, s64 *out_size) {
                R_RETURN(this->ForwardResult(m_filesystem->GetFileSize(file, out_size)));
            }

            Result SetFileSize(FsFile *file, s64 size) {
                R_RETURN(this->ForwardResult(m_filesystem->SetFileSize(file, size)));
            }

            Result ReadFile(FsFile *file, s64 off, void *buf, u64 read_size, u32 option, u64 *out_bytes_read) {
                R_RETURN(this->ForwardResult(m_filesystem->ReadFile(file, off, buf, read_size, option, out_bytes_read)));
            }

            Result WriteFile(FsFile *file, s64 off, const void *buf, u64 write_size, u32 option) {
                R_RETURN(this->ForwardResult(m_filesystem->WriteFile(file, off, buf, write_size, option)));
            }

            void CloseFile(FsFile *file) {
                m_filesystem->CloseFile(file);
            }

            Result CreateDirectory(const char* path) {
                R_RETURN(this->ForwardResult(m_filesystem->CreateDirectory(FixPath(path))));
            }

            Result DeleteDirectoryRecursively(const char* path) {
                R_RETURN(this->ForwardResult(m_filesystem->DeleteDirectoryRecursively(FixPath(path))));
            }

            Result RenameDirectory(const char *old_path, const char *new_path) {
                R_RETURN(this->ForwardResult(m_filesystem->RenameDirectory(FixPath(old_path), FixPath(new_path))));
            }

            Result OpenDirectory(const char *path, u32 mode, FsDir *out_dir) {
                R_RETURN(this->ForwardResult(m_filesystem->OpenDirectory(FixPath(path), mode, out_dir)));
            }

            Result ReadDirectory(FsDir *d, s64 *out_total_entries, size_t max_entries, FsDirectoryEntry *buf) {
                R_RETURN(this->ForwardResult(m_filesystem->ReadDirectory(d, out_total_entries, max_entries, buf)));
            }

            Result GetDirectoryEntryCount(FsDir *d, s64 *out_count) {
                R_RETURN(this->ForwardResult(m_filesystem->GetDirectoryEntryCount(d, out_count)));
            }

            void CloseDirectory(FsDir *d) {
                m_filesystem->CloseDirectory(d);
            }

            bool MultiThreadTransfer(s64 size, bool read) {
                return m_filesystem->MultiThreadTransfer(size, read);
            }
    };

}
