#include "titles.hpp"

#include "text.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

namespace titles {

namespace {

uint16_t platform(uint64_t titleId)
{
    return static_cast<uint16_t>(titleId >> 48);
}

uint16_t category(uint64_t titleId)
{
    return static_cast<uint16_t>(titleId >> 32);
}

uint64_t readLe(const std::string &data, size_t offset, int bytes)
{
    uint64_t value = 0;
    for (int index = bytes - 1; index >= 0; --index) {
        value = value << 8 | static_cast<uint8_t>(data[offset + index]);
    }
    return value;
}

} // namespace

bool isShown(uint64_t titleId)
{
    // A list, not a rule: system applets, system DSiWare (0x8005, 0x800F)
    // and download play children never appear, so they cannot be deleted.
    if (platform(titleId) != 0x0004) {
        return false;
    }
    switch (category(titleId)) {
    case 0x0000: // games and apps
    case 0x0002: // demos
    case 0x000e: // updates
    case 0x008c: // DLC
    case 0x8004: // DSiWare
        return true;
    default:
        return false;
    }
}

bool isSoftware(uint64_t titleId)
{
    const uint16_t value = category(titleId);
    return isShown(titleId) && value != 0x000e && value != 0x008c;
}

Index indexOf(const std::vector<Title> &all)
{
    Index index;
    for (const Title &title : all) {
        index[title.id] = &title;
    }
    return index;
}

std::string displayName(uint64_t titleId, const Index &index)
{
    const auto own = index.find(titleId);
    if (own != index.end() && !own->second->name.empty()) {
        return own->second->name;
    }
    const uint64_t base = cia::baseTitle(titleId);
    if (base != titleId) {
        const auto game = index.find(base);
        if (game != index.end() && !game->second->name.empty()) {
            return game->second->name;
        }
    }
    return cia::titleIdText(titleId);
}

std::string displayName(const Title &title, const Index &index)
{
    return displayName(title.id, index);
}

void sortByName(std::vector<const Title *> &list, const Index &index)
{
    std::vector<std::pair<std::string, const Title *>> keyed;
    keyed.reserve(list.size());
    for (const Title *title : list) {
        keyed.emplace_back(text::lower(displayName(*title, index)), title);
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto &a, const auto &b) {
                         if (a.first != b.first) {
                             return a.first < b.first;
                         }
                         return a.second->id < b.second->id;
                     });
    for (size_t index = 0; index < list.size(); ++index) {
        list[index] = keyed[index].second;
    }
}

void sortTitles(std::vector<const Title *> &list, const Index &index,
                fs::SortOrder order, bool reverse)
{
    sortByName(list, index);
    if (order == fs::SortOrder::Size) {
        std::stable_sort(list.begin(), list.end(),
                         [](const Title *a, const Title *b) {
                             return a->size > b->size;
                         });
    }
    if (reverse) {
        std::reverse(list.begin(), list.end());
    }
}

bool shows(Show show, const Title &title)
{
    switch (show) {
    case Show::SdCard:
        return title.place == Place::SdCard;
    case Show::SystemMemory:
        return title.place == Place::SystemMemory;
    case Show::Updates:
        return category(title.id) == 0x000e;
    case Show::Dlc:
        return category(title.id) == 0x008c;
    case Show::All:
        break;
    }
    return true;
}

uint32_t uniqueId(uint64_t titleId)
{
    return static_cast<uint32_t>(titleId >> 8) & 0xFFFFF;
}

const Title *ownerOfExtData(uint64_t extDataId, const std::vector<Title> &all)
{
    for (const Title &title : all) {
        if (isSoftware(title.id) && uniqueId(title.id) == extDataId) {
            return &title;
        }
    }
    return nullptr;
}

bool extDataInUse(uint64_t extDataId, const std::vector<Title> &all,
                  const std::set<uint64_t> &onSdCard)
{
    if (ownerOfExtData(extDataId, all) != nullptr) {
        return true;
    }
    for (const uint64_t id : onSdCard) {
        if (isSoftware(id) && uniqueId(id) == extDataId) {
            return true;
        }
    }
    return false;
}

std::vector<uint64_t> unusedTickets(const std::vector<uint64_t> &tickets,
                                    const std::vector<Title> &installed,
                                    const std::set<uint64_t> &onSdCard)
{
    std::set<uint64_t> present(onSdCard);
    for (const Title &title : installed) {
        present.insert(title.id);
    }
    std::vector<uint64_t> unused;
    for (const uint64_t ticket : tickets) {
        if (isShown(ticket) && category(ticket) != 0x8004 &&
            present.count(ticket) == 0) {
            unused.push_back(ticket);
        }
    }
    return unused;
}

bool titleFromSdFolders(const std::string &high, const std::string &low,
                        uint64_t &titleId)
{
    if (high.size() != 8 || low.size() != 8) {
        return false;
    }
    uint64_t value = 0;
    for (const char c : high + low) {
        const int digit = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
        if (digit < 0) {
            return false;
        }
        value = value << 4 | static_cast<uint64_t>(digit);
    }
    titleId = value;
    return true;
}

bool parseSeedDb(const std::string &data, std::vector<SeedEntry> &out,
                 std::string &error)
{
    constexpr size_t Header = 16;
    constexpr size_t EntrySize = 32;
    out.clear();
    if (data.size() < Header) {
        error = "The file is too short to be a seeddb.bin.";
        return false;
    }
    const uint64_t count = readLe(data, 0, 4);
    if (Header + count * EntrySize != data.size()) {
        error = "The file says it holds " + std::to_string(count) +
                " seeds, but its size does not match. It is probably not a "
                "seeddb.bin.";
        return false;
    }
    out.reserve(static_cast<size_t>(count));
    for (uint64_t entry = 0; entry < count; ++entry) {
        const size_t offset = Header + static_cast<size_t>(entry) * EntrySize;
        SeedEntry seed;
        seed.titleId = readLe(data, offset, 8);
        std::memcpy(seed.seed.data(), data.data() + offset + 8, seed.seed.size());
        out.push_back(seed);
    }
    return true;
}

std::vector<SeedEntry> seedsFor(const std::vector<SeedEntry> &entries,
                                const std::vector<Title> &installed)
{
    std::set<uint64_t> wanted;
    for (const Title &title : installed) {
        if (isSoftware(title.id)) {
            wanted.insert(title.id);
        }
    }
    std::vector<SeedEntry> matched;
    for (const SeedEntry &entry : entries) {
        if (wanted.count(entry.titleId) != 0) {
            matched.push_back(entry);
            wanted.erase(entry.titleId); // the first one wins
        }
    }
    return matched;
}

Names parseNames(const std::string &text)
{
    Names names;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t space = line.find(' ');
        if (space != 16) {
            continue;
        }
        char *stop = nullptr;
        const std::string hex = line.substr(0, 16);
        const unsigned long long id = std::strtoull(hex.c_str(), &stop, 16);
        const std::string name = text::trim(line.substr(space + 1));
        if (stop == hex.c_str() + 16 && id != 0 && !name.empty()) {
            names[id] = name;
        }
    }
    return names;
}

std::string serializeNames(const Names &names)
{
    std::string text;
    for (const auto &entry : names) {
        text += cia::titleIdText(entry.first) + " " + text::singleLine(entry.second) + "\n";
    }
    return text;
}

bool rememberNames(Names &names, const std::vector<Title> &titles)
{
    bool changed = false;
    for (const Title &title : titles) {
        if (title.name.empty()) {
            continue;
        }
        std::string &kept = names[title.id];
        if (kept != title.name) {
            kept = title.name;
            changed = true;
        }
    }
    return changed;
}

std::string rememberedName(uint64_t titleId, const Names &names)
{
    const auto own = names.find(titleId);
    if (own != names.end()) {
        return own->second;
    }
    const auto game = names.find(cia::baseTitle(titleId));
    return game != names.end() ? game->second : "";
}

} // namespace titles
