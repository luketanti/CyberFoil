#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace inst::util {
    // File browsers list parent/directory rows before their selectable files.
    inline std::optional<std::size_t> FileRowIndex(int row, std::size_t directoryRows, std::size_t fileCount) {
        if (row < 0 || static_cast<std::size_t>(row) < directoryRows) return std::nullopt;
        const auto index = static_cast<std::size_t>(row) - directoryRows;
        return index < fileCount ? std::optional<std::size_t>(index) : std::nullopt;
    }

    template<typename T>
    void ToggleFileSelection(std::vector<T>& selected, const T& file) {
        const auto found = std::find(selected.begin(), selected.end(), file);
        if (found == selected.end()) selected.push_back(file);
        else selected.erase(found);
    }
}
