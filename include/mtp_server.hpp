#pragma once

#include <cstdint>

namespace inst::mtp {

bool StartInstallServer(int storage_choice);
void StopInstallServer();
bool IsInstallServerRunning();

}
