#pragma once

#include <cstdint>

struct AccountUid { std::uint64_t uid[2]; };
inline bool accountUidIsValid(const AccountUid* user) { return user->uid[0] || user->uid[1]; }
enum FsSaveDataType {
    FsSaveDataType_System = 0,
    FsSaveDataType_Account = 1,
    FsSaveDataType_Bcat = 2,
    FsSaveDataType_Device = 3,
    FsSaveDataType_Temporary = 4,
    FsSaveDataType_Cache = 5
};
// Fields consumed by the save discovery policy; no Switch services are mocked.
struct FsSaveDataInfo {
    std::uint64_t application_id = 0;
    std::uint8_t save_data_type = 0;
    AccountUid uid = {};
};
