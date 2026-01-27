#pragma once

#include <switch.h>
#include <vector>
#include <memory>

namespace haze {

typedef enum {
    CallbackType_OpenSession, // data = none
    CallbackType_CloseSession, // data = none
    CallbackType_CreateFile, // data = file
    CallbackType_DeleteFile, // data = file
    CallbackType_RenameFile, // data = rename
    CallbackType_RenameFolder, // data = rename
    CallbackType_CreateFolder, // data = file
    CallbackType_DeleteFolder, // data = file
    CallbackType_ReadBegin, // data = file
    CallbackType_ReadProgress, // data = progress
    CallbackType_ReadEnd, // data = file
    CallbackType_WriteBegin, // data = file
    CallbackType_WriteProgress, // data = progress
    CallbackType_WriteEnd, // data = file
} CallbackType;

typedef struct {
    char filename[FS_MAX_PATH];
} CallbackDataFile;

typedef struct {
    char filename[FS_MAX_PATH];
    char newname[FS_MAX_PATH];
} CallbackDataRename;

typedef struct {
    long long offset;
    long long size;
} CallbackDataProgress;


typedef struct {
    CallbackType type;
    union {
        CallbackDataFile file;
        CallbackDataRename rename;
        CallbackDataProgress progress;
    };
} CallbackData;

typedef void(*Callback)(const CallbackData* data);

struct FileSystemProxyImpl {
    virtual const char* GetName() const = 0;
    virtual const char* GetDisplayName() const = 0;

    virtual Result GetTotalSpace(const char *path, s64 *out) = 0;
    virtual Result GetFreeSpace(const char *path, s64 *out) = 0;
    virtual Result GetEntryType(const char *path, FsDirEntryType *out_entry_type) = 0;
    virtual Result CreateFile(const char* path, s64 size, u32 option) = 0;
    virtual Result DeleteFile(const char* path) = 0;
    virtual Result RenameFile(const char *old_path, const char *new_path) = 0;
    virtual Result OpenFile(const char *path, u32 mode, FsFile *out_file) = 0;
    virtual Result GetFileSize(FsFile *file, s64 *out_size) = 0;
    virtual Result SetFileSize(FsFile *file, s64 size) = 0;
    virtual Result ReadFile(FsFile *file, s64 off, void *buf, u64 read_size, u32 option, u64 *out_bytes_read) = 0;
    virtual Result WriteFile(FsFile *file, s64 off, const void *buf, u64 write_size, u32 option) = 0;
    virtual void CloseFile(FsFile *file) = 0;

    virtual Result CreateDirectory(const char* path) = 0;
    virtual Result DeleteDirectoryRecursively(const char* path) = 0;
    virtual Result RenameDirectory(const char *old_path, const char *new_path) = 0;
    virtual Result OpenDirectory(const char *path, u32 mode, FsDir *out_dir) = 0;
    virtual Result ReadDirectory(FsDir *d, s64 *out_total_entries, size_t max_entries, FsDirectoryEntry *buf) = 0;
    virtual Result GetDirectoryEntryCount(FsDir *d, s64 *out_count) = 0;
    virtual void CloseDirectory(FsDir *d) = 0;

    virtual bool MultiThreadTransfer(s64 size, bool read) { return true; }
};

using FsEntries = std::vector<std::shared_ptr<FileSystemProxyImpl>>;

/* Callback is optional */
bool Initialize(Callback callback, int prio, int cpuid, const FsEntries& entries, u16 vid = 0x057e, u16 pid = 0x201d);
void Exit();

} // namespace haze
