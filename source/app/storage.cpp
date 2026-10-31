#include "storage.hpp"

#include "fs_util.hpp"
#include "naming.hpp"

namespace storage {
namespace {

constexpr const char *LegacyConfig = "sdmc:/3dsDL.cfg";

std::string settingsPath()
{
    return std::string(DataFolder) + "settings.ini";
}

std::string historyPath()
{
    return std::string(DataFolder) + "history.txt";
}

std::string namesPath()
{
    return std::string(DataFolder) + "names.txt";
}

} // namespace

Settings loadSettings()
{
    Settings settings;
    std::string contents;
    if (fs::readFile(settingsPath(), contents)) {
        settings = parseSettings(contents);
    } else if (fs::readFile(LegacyConfig, contents)) {
        const std::string legacy = parseLegacyDestination(contents);
        if (!legacy.empty()) {
            settings.destination = legacy;
        }
        if (saveSettings(settings)) {
            fs::removeFile(LegacyConfig);
        }
    }

    settings.destination = naming::normaliseFolder(settings.destination);
    if (!naming::isSdPath(settings.destination) ||
        !fs::directoryExists(settings.destination)) {
        settings.destination = "sdmc:/";
    }
    return settings;
}

bool saveSettings(const Settings &settings)
{
    return fs::makeDirectories(DataFolder) &&
           fs::writeFile(settingsPath(), serializeSettings(settings));
}

std::vector<HistoryEntry> loadHistory()
{
    std::string contents;
    if (!fs::readFile(historyPath(), contents)) {
        return {};
    }
    return parseHistory(contents);
}

bool saveHistory(const std::vector<HistoryEntry> &entries)
{
    return fs::makeDirectories(DataFolder) &&
           fs::writeFile(historyPath(), serializeHistory(entries));
}

titles::Names loadNames()
{
    std::string contents;
    return fs::readFile(namesPath(), contents) ? titles::parseNames(contents)
                                               : titles::Names();
}

bool saveNames(const titles::Names &names)
{
    return fs::makeDirectories(DataFolder) &&
           fs::writeFile(namesPath(), titles::serializeNames(names));
}

} // namespace storage
