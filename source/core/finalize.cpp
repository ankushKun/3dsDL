#include "finalize.hpp"

#include "fs_util.hpp"
#include "naming.hpp"

#include <vector>

namespace finalize {
namespace {

bool moveIntoPlace(const std::string &partPath, const std::string &target,
                   bool overwrite)
{
    if (!fs::fileExists(target)) {
        return fs::renameFile(partPath, target);
    }
    if (!overwrite) {
        return false;
    }

    const std::string folder = target.substr(0, target.find_last_of('/') + 1);
    const std::string name = target.substr(folder.size());
    const std::string backup =
        naming::unusedPath(folder, name + ".3dsdl-backup", fs::fileExists);
    if (backup.empty() || !fs::renameFile(target, backup)) {
        return false;
    }
    if (!fs::renameFile(partPath, target)) {
        fs::renameFile(backup, target);
        return false;
    }
    fs::removeFile(backup);
    return true;
}

} // namespace

Result commit(const std::string &partPath, const std::string &folder,
              const std::string &filename, bool overwrite, int64_t timestamp)
{
    Result result;
    result.path = partPath;

    const std::string preferred = folder + filename;
    const std::string target = overwrite
        ? preferred
        : naming::unusedPath(folder, filename, fs::fileExists);
    if (!target.empty() && moveIntoPlace(partPath, target, overwrite)) {
        result.ok = true;
        result.path = target;
        return result;
    }

    std::vector<std::string> fallbacks;
    const std::string ascii = naming::asciiFilename(filename);
    if (ascii != filename) {
        fallbacks.push_back(ascii);
    }
    fallbacks.push_back("download-" + std::to_string(timestamp) +
                        naming::extension(ascii));

    for (const std::string &name : fallbacks) {
        const std::string candidate =
            naming::unusedPath(folder, name, fs::fileExists);
        if (!candidate.empty() && fs::renameFile(partPath, candidate)) {
            result.ok = true;
            result.usedFallbackName = true;
            result.path = candidate;
            result.message = "The SD card would not accept the name \"" +
                             filename + "\", so the file was saved as " +
                             candidate + ".";
            return result;
        }
    }

    result.message = "The file could not be renamed, so the downloaded data "
                     "was left in " + partPath + ".";
    return result;
}

std::string keptBeside(const std::string &path, const std::string &filename)
{
    const std::string older =
        path.substr(0, path.find_last_of('/') + 1) + filename;
    return !filename.empty() && older != path && fs::fileExists(older) ? older
                                                                       : "";
}

bool replace(const std::string &path, const std::string &target)
{
    return fs::fileExists(path) && moveIntoPlace(path, target, true);
}

} // namespace finalize
