#define NXLINK_LOG 0

#include <switch.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#if NXLINK_LOG
#include <unistd.h>
#endif
#include "haze.h"

namespace {

Mutex g_mutex;
std::vector<haze::CallbackData> g_callback_data;

struct FsNative : haze::FileSystemProxyImpl {
    FsNative() = default;
    FsNative(FsFileSystem* fs, bool own) {
        m_fs = *fs;
        m_own = own;
    }

    ~FsNative() {
        fsFsCommit(&m_fs);
        if (m_own) {
            fsFsClose(&m_fs);
        }
    }

    auto FixPath(const char* path, char* out = nullptr) const -> const char* {
        static char buf[FS_MAX_PATH];
        const auto len = std::strlen(GetName());

        if (!out) {
            out = buf;
        }

        if (len && !strncasecmp(path + 1, GetName(), len)) {
            std::snprintf(out, sizeof(buf), "/%s", path + 1 + len);
        } else {
            std::strcpy(out, path);
        }

        return out;
    }

    Result GetTotalSpace(const char *path, s64 *out) override {
        return fsFsGetTotalSpace(&m_fs, FixPath(path), out);
    }
    Result GetFreeSpace(const char *path, s64 *out) override {
        return fsFsGetFreeSpace(&m_fs, FixPath(path), out);
    }
    Result GetEntryType(const char *path, FsDirEntryType *out_entry_type) override {
        return fsFsGetEntryType(&m_fs, FixPath(path), out_entry_type);
    }
    Result CreateFile(const char* path, s64 size, u32 option) override {
        return fsFsCreateFile(&m_fs, FixPath(path), size, option);
    }
    Result DeleteFile(const char* path) override {
        return fsFsDeleteFile(&m_fs, FixPath(path));
    }
    Result RenameFile(const char *old_path, const char *new_path) override {
        char temp[FS_MAX_PATH];
        return fsFsRenameFile(&m_fs, FixPath(old_path, temp), FixPath(new_path));
    }
    Result OpenFile(const char *path, u32 mode, FsFile *out_file) override {
        return fsFsOpenFile(&m_fs, FixPath(path), mode, out_file);
    }
    Result GetFileSize(FsFile *file, s64 *out_size) override {
        return fsFileGetSize(file, out_size);
    }
    Result SetFileSize(FsFile *file, s64 size) override {
        return fsFileSetSize(file, size);
    }
    Result ReadFile(FsFile *file, s64 off, void *buf, u64 read_size, u32 option, u64 *out_bytes_read) override {
        return fsFileRead(file, off, buf, read_size, option, out_bytes_read);
    }
    Result WriteFile(FsFile *file, s64 off, const void *buf, u64 write_size, u32 option) override {
        return fsFileWrite(file, off, buf, write_size, option);
    }
    void CloseFile(FsFile *file) override {
        fsFileClose(file);
    }

    Result CreateDirectory(const char* path) override {
        return fsFsCreateDirectory(&m_fs, FixPath(path));
    }
    Result DeleteDirectoryRecursively(const char* path) override {
        return fsFsDeleteDirectoryRecursively(&m_fs, FixPath(path));
    }
    Result RenameDirectory(const char *old_path, const char *new_path) override {
        char temp[FS_MAX_PATH];
        return fsFsRenameDirectory(&m_fs, FixPath(old_path, temp), FixPath(new_path));
    }
    Result OpenDirectory(const char *path, u32 mode, FsDir *out_dir) override {
        return fsFsOpenDirectory(&m_fs, FixPath(path), mode, out_dir);
    }
    Result ReadDirectory(FsDir *d, s64 *out_total_entries, size_t max_entries, FsDirectoryEntry *buf) override {
        return fsDirRead(d, out_total_entries, max_entries, buf);
    }
    Result GetDirectoryEntryCount(FsDir *d, s64 *out_count) override {
        return fsDirGetEntryCount(d, out_count);
    }
    void CloseDirectory(FsDir *d) override {
        fsDirClose(d);
    }

    FsFileSystem m_fs{};
    bool m_own{true};
};

struct FsSdmc final : FsNative {
    FsSdmc() : FsNative(fsdevGetDeviceFileSystem("sdmc"), false) {
    }

    const char* GetName() const {
        return "";
    }
    const char* GetDisplayName() const {
        return "micro SD Card";
    }
};

struct FsAlbum final : FsNative {
    FsAlbum(FsImageDirectoryId id) {
        fsOpenImageDirectoryFileSystem(&m_fs, id);
    }

    const char* GetName() const {
        return "album:/";
    }
    const char* GetDisplayName() const {
        return "Album";
    }
};

void callbackHandler(const haze::CallbackData* data) {
    mutexLock(&g_mutex);
        g_callback_data.emplace_back(*data);
    mutexUnlock(&g_mutex);
}

void processEvents() {
    std::vector<haze::CallbackData> data;

    mutexLock(&g_mutex);
        std::swap(data, g_callback_data);
    mutexUnlock(&g_mutex);

    // log events
    for (const auto& e : data) {
        switch (e.type) {
            case haze::CallbackType_OpenSession: std::printf("Opening Session\n"); break;
            case haze::CallbackType_CloseSession: std::printf("Closing Session\n"); break;

            case haze::CallbackType_CreateFile: std::printf("Creating File: %s\n", e.file.filename); break;
            case haze::CallbackType_DeleteFile: std::printf("Deleting File: %s\n", e.file.filename); break;

            case haze::CallbackType_RenameFile: std::printf("Rename File: %s -> %s\n", e.rename.filename, e.rename.newname); break;
            case haze::CallbackType_RenameFolder: std::printf("Rename Folder: %s -> %s\n", e.rename.filename, e.rename.newname); break;

            case haze::CallbackType_CreateFolder: std::printf("Creating Folder: %s\n", e.file.filename); break;
            case haze::CallbackType_DeleteFolder: std::printf("Deleting Folder: %s\n", e.file.filename); break;

            case haze::CallbackType_ReadBegin: std::printf("Reading File Begin: %s \r", e.file.filename); break;
            case haze::CallbackType_ReadProgress: std::printf("Reading File: offset: %lld size: %lld\r", e.progress.offset, e.progress.size); break;
            case haze::CallbackType_ReadEnd: std::printf("Reading File Finished: %s\n", e.file.filename); break;

            case haze::CallbackType_WriteBegin: std::printf("Writing File Begin: %s \r", e.file.filename); break;
            case haze::CallbackType_WriteProgress: std::printf("Writing File: offset: %lld size: %lld\r", e.progress.offset, e.progress.size); break;
            case haze::CallbackType_WriteEnd: std::printf("Writing File Finished: %s\n", e.file.filename); break;
        }
    }

    consoleUpdate(NULL);
}

} // namespace

int main(int argc, char** argv) {
    #if NXLINK_LOG
    socketInitializeDefault();
    int fd = nxlinkStdio();
    #endif

    haze::FsEntries fs_entries;
    fs_entries.emplace_back(std::make_shared<FsSdmc>());
    fs_entries.emplace_back(std::make_shared<FsAlbum>(FsImageDirectoryId_Sd));

    mutexInit(&g_mutex);
    haze::Initialize(callbackHandler, 0x2C, 2, fs_entries); // init libhaze (creates thread)
    consoleInit(NULL); // console to display to the screen

    // init controller
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    std::printf("libhaze example!\n\nPress (+) to exit\n");
    consoleUpdate(NULL);

    // loop until + button is pressed
    while (appletMainLoop()) {
        padUpdate(&pad);

        const u64 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_Plus)
            break; // break in order to return to hbmenu

        processEvents();
        svcSleepThread(1e+9/60);
    }

    #if NXLINK_LOG
    close(fd);
    socketExit();
    #endif
    consoleExit(NULL); // exit console display
    haze::Exit(); // signals libhaze to exit, closes thread
}

extern "C" {

// called before main
void userAppInit(void) {
    appletLockExit(); // block exit until everything is cleaned up
}

// called after main has exit
void userAppExit(void) {
    appletUnlockExit(); // unblocks exit to cleanly exit
}

} // extern "C"
