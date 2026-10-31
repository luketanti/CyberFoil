#include "util/save_data_policy.hpp"

#include <iostream>
#include <map>
#include <stdexcept>

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        std::map<inst::save_sync::SaveEntryKey, int> histories;
        histories[{1, "account"}] = 1;
        histories[{1, "device"}] = 2;
        histories[{1, "unknown"}] = 3;
        Check(histories.size() == 3, "same title keeps separate account, device, and unknown histories");
        Check(inst::save_sync::CanRestoreSaveType("account", "account"), "account backup restores to account container");
        Check(inst::save_sync::CanRestoreSaveType("device", "device"), "device backup restores to device container");
        Check(!inst::save_sync::CanRestoreSaveType("account", "device"), "account backup cannot overwrite device save");
        Check(!inst::save_sync::CanRestoreSaveType("device", "account"), "device backup cannot overwrite account save");
        Check(!inst::save_sync::CanRestoreSaveType("unknown", "account"), "untyped backup is not guessed to be account data");
        Check(!inst::save_sync::CanRestoreSaveType("unknown", "device"), "untyped backup is not guessed to be device data");
        Check(!inst::save_sync::CanRestoreSaveType("unknown", "unknown"), "unknown backups cannot be restored automatically");
        Check(inst::save_sync::NormalizeBackupSaveType("system") == "unknown", "unsupported remote save type remains unclassified");
        const AccountUid active{{1, 2}}, other{{1, 3}}, none{};
        FsSaveDataInfo info;
        info.application_id = 0x01006F8002326000ULL;
        info.save_data_type = FsSaveDataType_Device;
        Check(inst::save_sync::IsBackupSave(info, active), "device save with no owner is included for the active user");
        Check(inst::save_sync::IsBackupSave(info, other), "device save is shared with other users");
        Check(inst::save_sync::IsBackupSave(info, none), "device save is included without a user account");
        info.save_data_type = FsSaveDataType_Account;
        info.uid = active;
        Check(inst::save_sync::IsBackupSave(info, active), "selected user's account save is included");
        Check(!inst::save_sync::IsBackupSave(info, other), "other user's account save is excluded");
        Check(!inst::save_sync::IsBackupSave(info, none), "missing user cannot select account saves");
        info.uid = {};
        Check(!inst::save_sync::IsBackupSave(info, active), "ownerless account save is excluded");
        for (auto type : {FsSaveDataType_System, FsSaveDataType_Bcat, FsSaveDataType_Temporary, FsSaveDataType_Cache}) {
            info.save_data_type = type;
            Check(!inst::save_sync::IsBackupSave(info, active), "non-game save types are excluded");
        }
        info.save_data_type = FsSaveDataType_Device;
        info.application_id = 0;
        Check(!inst::save_sync::IsBackupSave(info, active), "save without a title cannot be uploaded");
        std::cout << "Save data policy tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
