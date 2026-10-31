#include "util/file_selection.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        // A parent row and two folders precede the files in an SD/HDD browser.
        const std::vector<std::string> files{"first.nsp", "second.nsp"};
        std::vector<std::string> selected;
        const auto toggleRow = [&](int row) {
            const auto index = inst::util::FileRowIndex(row, 3, files.size());
            if (index) inst::util::ToggleFileSelection(selected, files[*index]);
        };
        for (int row : {-1, 0, 1, 2, 5}) toggleRow(row);
        Check(selected.empty(), "directory and invalid rows cannot select files");
        toggleRow(3);
        toggleRow(4);
        Check(selected == files, "both files can be selected");
        toggleRow(3);
        Check(selected == std::vector<std::string>{"second.nsp"}, "deselect only the requested file");
        toggleRow(3);
        Check(selected.size() == 2, "reselect without duplicates");
        toggleRow(4);
        toggleRow(3);
        Check(selected.empty(), "all files can be deselected");

        Check(inst::util::FileRowIndex(0, 0, files.size()) == 0, "USB first file has no directory offset");
        Check(inst::util::FileRowIndex(1, 0, files.size()) == 1, "USB last file");
        Check(!inst::util::FileRowIndex(0, 0, 0), "empty USB list has no selectable row");
        Check(!inst::util::FileRowIndex(2, 0, files.size()), "past the last USB file");
        std::cout << "File selection tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
