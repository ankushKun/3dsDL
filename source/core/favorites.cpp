#include "favorites.hpp"

#include "text.hpp"

#include <algorithm>

namespace favorites {
namespace {

// favorite is entry itself, as a file or as a folder, or inside it.
bool covers(const std::string &favorite, const std::string &entry)
{
    return favorite.size() >= entry.size() &&
           key(favorite.substr(0, entry.size())) == key(entry) &&
           (favorite.size() == entry.size() || favorite[entry.size()] == '/');
}

} // namespace

std::string key(const std::string &path)
{
    return text::lower(path);
}

bool contains(const std::vector<std::string> &list, const std::string &path)
{
    return std::any_of(list.begin(), list.end(), [&](const std::string &favorite) {
        return key(favorite) == key(path);
    });
}

bool toggle(std::vector<std::string> &list, const std::string &path)
{
    const auto at = std::find_if(list.begin(), list.end(),
                                 [&](const std::string &favorite) {
                                     return key(favorite) == key(path);
                                 });
    if (at != list.end()) {
        list.erase(at);
        return false;
    }
    list.push_back(path);
    return true;
}

bool moved(std::vector<std::string> &list, const std::string &from,
           const std::string &to)
{
    bool changed = false;
    std::vector<std::string> kept;
    for (const std::string &favorite : list) {
        const bool follows = covers(favorite, from);
        const std::string now =
            follows ? to + favorite.substr(from.size()) : favorite;
        changed |= follows;
        // One that lands on another favourite becomes that one.
        if (!contains(kept, now)) {
            kept.push_back(now);
        }
    }
    list.swap(kept);
    return changed;
}

bool removed(std::vector<std::string> &list, const std::string &path)
{
    const size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const std::string &favorite) {
                                  return covers(favorite, path);
                              }),
               list.end());
    return list.size() != before;
}

} // namespace favorites
