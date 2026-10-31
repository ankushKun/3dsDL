#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// Save data and extra data backups, laid out exactly as Checkpoint lays
// them out, so a backup made by either app restores in the other:
//
//   sdmc:/3ds/Checkpoint/saves/0x0055D Pilotwings Resort/20261019-143005/
//   sdmc:/3ds/Checkpoint/extdata/0x0055D Pilotwings Resort/20261019-143005/
//
// The rules here follow Checkpoint's source (3ds/source/paths.cpp,
// titlequirks.cpp, util.cpp and io.cpp). Only names and rules live here;
// app/savedata does the copying.
namespace saves {

constexpr const char *SavesRoot = "sdmc:/3ds/Checkpoint/saves/";
constexpr const char *ExtDataRoot = "sdmc:/3ds/Checkpoint/extdata/";
// Checkpoint limits a typed backup name to this many bytes.
constexpr size_t MaxCustomName = 20;

// Checkpoint's sanitiser: each of .,!\/:?*"<>| becomes a space, then
// trailing spaces go.
std::string sanitize(const std::string &name);

// The title's folder name: "0x%05X " (unique ID) and its sanitised English
// short name. Checkpoint keeps the trailing space when the name is empty,
// and so does this.
std::string titleFolder(uint64_t titleId, const std::string &englishShortName);

// The name a Checkpoint backup folder ("0x3D5F1 3dsFM") gives the title it
// belongs to, when it is titleId's (by unique ID); "" otherwise.
std::string nameFromFolder(uint64_t titleId, const std::string &folder);

// The extra data a title keeps: its unique ID, or another one for the few
// games that share (Checkpoint's table).
uint32_t extDataId(uint64_t titleId);

// Titles Checkpoint never lists for saves: manuals, a system applet,
// updates and DSi data archives.
bool isExcluded(uint64_t titleId);

// The default name of a new backup, "YYYYMMDD-HHMMSS" in local time.
std::string backupName(const std::tm &local);
// A typed backup name: sanitised and cut to MaxCustomName bytes.
std::string customBackupName(const std::string &typed);

// Files a computer leaves in folders (._ files, .DS_Store, Thumbs.db and
// the like), which must never be restored into a save.
bool isHostMetadata(const std::string &name);

// Backup folder names, newest first. Checkpoint's names sort by time.
void sortNewestFirst(std::vector<std::string> &names);

} // namespace saves
