#pragma once

#include <string>
#include <vector>

// Files' favourites: SD card paths in the order they were added, folders
// ending in '/' as Files browses them and files without. They are matched
// ignoring case, as FAT does.
namespace favorites {

// The form paths are matched in.
std::string key(const std::string &path);
bool contains(const std::vector<std::string> &list, const std::string &path);
// Adds path, or takes it out when it is there. True when it was added.
bool toggle(std::vector<std::string> &list, const std::string &path);
// After the entry at from was renamed or moved to to (entry paths, without
// a trailing '/'), its favourite and those inside it follow. True when any
// changed.
bool moved(std::vector<std::string> &list, const std::string &from,
           const std::string &to);
// After the entry at path was deleted, its favourite and those inside it
// go. True when any did.
bool removed(std::vector<std::string> &list, const std::string &path);

} // namespace favorites
