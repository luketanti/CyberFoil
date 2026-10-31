#pragma once

#include <switch.h>
#include <string>
#include <utility>

namespace inst::save_sync {
    inline std::string NormalizeBackupSaveType(const std::string& type) {
        return type == "account" || type == "device" ? type : "unknown";
    }

    inline bool CanRestoreSaveType(const std::string& backupType, const std::string& targetType) {
        return (backupType == "account" || backupType == "device") && backupType == targetType;
    }

    using SaveEntryKey = std::pair<std::uint64_t, std::string>;

    inline bool IsBackupSave(const FsSaveDataInfo& info, const AccountUid& user) {
        if (info.application_id == 0) return false;
        if (info.save_data_type == FsSaveDataType_Device) return true;
        return info.save_data_type == FsSaveDataType_Account && accountUidIsValid(&user) &&
            info.uid.uid[0] == user.uid[0] && info.uid.uid[1] == user.uid[1];
    }
}
