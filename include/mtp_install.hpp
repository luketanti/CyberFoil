#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace inst::mtp {

bool StartStreamInstall(const std::string& name, std::uint64_t size, int storage_choice);
bool WriteStreamInstall(const void* buf, size_t size, std::uint64_t offset);
void CloseStreamInstall();

bool IsStreamInstallActive();
bool ConsumeStreamInstallComplete();
void GetStreamInstallProgress(std::uint64_t* out_received, std::uint64_t* out_total);
std::string GetStreamInstallName();
bool GetStreamInstallTitleId(std::uint64_t* out_title_id);

}
