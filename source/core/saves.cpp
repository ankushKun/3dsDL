#include "saves.hpp"

#include "text.hpp"

#include <algorithm>
#include <cstdio>

namespace saves {

std::string sanitize(const std::string &name)
{
    static const std::string Illegal = ".,!\\/:?*\"<>|";
    std::string out = name;
    for (char &c : out) {
        if (Illegal.find(c) != std::string::npos) {
            c = ' ';
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string titleFolder(uint64_t titleId, const std::string &englishShortName)
{
    char unique[16];
    std::snprintf(unique, sizeof(unique), "0x%05X ",
                  static_cast<unsigned>(static_cast<uint32_t>(titleId) >> 8));
    return unique + sanitize(englishShortName);
}

std::string nameFromFolder(uint64_t titleId, const std::string &folder)
{
    const std::string prefix = titleFolder(titleId, "");
    if (folder.size() <= prefix.size() ||
        text::lower(folder.substr(0, prefix.size())) != text::lower(prefix)) {
        return "";
    }
    return text::trim(folder.substr(prefix.size()));
}

uint32_t extDataId(uint64_t titleId)
{
    const uint32_t low = static_cast<uint32_t>(titleId);
    switch (low) {
    case 0x00055E00:
        return 0x055D; // Pokemon Y shares Pokemon X's
    case 0x0011C400:
        return 0x11C5; // Pokemon Omega Ruby and Alpha Sapphire
    case 0x00175E00:
        return 0x1648; // Pokemon Moon shares Pokemon Sun's
    case 0x00179600:
    case 0x00179800:
        return 0x1794; // Fire Emblem Fates, special edition and Conquest
    case 0x00179700:
    case 0x0017A800:
        return 0x1795; // Fire Emblem Fates, Birthright and Revelation
    case 0x0012DD00:
    case 0x0012DE00:
        return 0x12DC; // Fire Emblem Awakening
    case 0x001B5100:
        return 0x1B50; // Pokemon Ultra Moon shares Ultra Sun's
    default:
        return low >> 8;
    }
}

bool isExcluded(uint64_t titleId)
{
    const uint32_t low = static_cast<uint32_t>(titleId);
    const uint32_t high = static_cast<uint32_t>(titleId >> 32);
    if (high == 0x0004000E || high == 0x0004800F) {
        return true; // updates and DSi data archives
    }
    switch (low) {
    case 0x00008602: // electronic manuals of the system applets
    case 0x00009202:
    case 0x00009B02:
    case 0x0000A402:
    case 0x0000AC02:
    case 0x0000B402:
    case 0x00021A00:
        return true;
    default:
        return false;
    }
}

std::string backupName(const std::tm &local)
{
    char name[32];
    std::snprintf(name, sizeof(name), "%04i%02i%02i-%02i%02i%02i",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return name;
}

std::string customBackupName(const std::string &typed)
{
    std::string cut = typed.substr(0, std::min(typed.size(), MaxCustomName));
    // Do not leave half of a UTF-8 character at the end.
    while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) {
        cut.pop_back();
    }
    if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0x80) != 0) {
        cut.pop_back();
    }
    return sanitize(cut);
}

bool isHostMetadata(const std::string &name)
{
    const std::string lowered = text::lower(name);
    if (lowered.rfind("._", 0) == 0) {
        return true;
    }
    static const char *const Names[] = {
        ".ds_store",       ".spotlight-v100",  ".fseventsd", ".temporaryitems",
        ".trashes",        ".apdisk",          "thumbs.db",  "desktop.ini",
        "system volume information", "$recycle.bin",
    };
    for (const char *known : Names) {
        if (lowered == known) {
            return true;
        }
    }
    return false;
}

void sortNewestFirst(std::vector<std::string> &names)
{
    std::sort(names.begin(), names.end(), [](const std::string &a,
                                             const std::string &b) {
        return a > b;
    });
}

} // namespace saves
