#pragma once

#include "records.hpp"
#include "titles.hpp"

#include <string>
#include <vector>

// Settings and history on the SD card, in sdmc:/3ds/3dsDL/ next to the
// Homebrew Launcher build.
namespace storage {

constexpr const char *DataFolder = "sdmc:/3ds/3dsDL/";

// Loads the settings, moving the pre-0.3 sdmc:/3dsDL.cfg over if needed.
Settings loadSettings();
bool saveSettings(const Settings &settings);

std::vector<HistoryEntry> loadHistory();
bool saveHistory(const std::vector<HistoryEntry> &entries);

// Names of software seen installed (titles::Names), in names.txt.
titles::Names loadNames();
bool saveNames(const titles::Names &names);

} // namespace storage
