#pragma once

#include "cia.hpp"
#include "fs_util.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// What 3dsDL knows about the software installed on the console, for the
// Titles screens: installed titles, tickets, extra data and seeds. Only
// data and rules live here; app/library reads them from the console.
namespace titles {

enum class Place { SdCard, SystemMemory, GameCard };

struct Title {
    uint64_t id = 0;
    Place place = Place::SdCard;
    uint16_t version = 0;
    uint64_t size = 0;
    std::string productCode; // "CTR-P-AXXE", or "" when unknown
    // From the title's own icon data; empty for updates, DLC and DSiWare.
    std::string name;
    // The English short name as stored, which Checkpoint names backup
    // folders after (see core/saves).
    std::string englishShortName;
    std::string publisher;
    std::vector<uint16_t> icon; // 48x48 RGB565, or empty
};

// The installed software that 3dsDL shows: everything but system titles.
bool isShown(uint64_t titleId);
// Software (apps, demos, DSiWare) as opposed to updates and DLC.
bool isSoftware(uint64_t titleId);

// Titles by ID, for naming updates, DLC, tickets and extra data after the
// game they belong to.
using Index = std::map<uint64_t, const Title *>;
Index indexOf(const std::vector<Title> &all);

// A name for the list: the title's own, or for an update or DLC its game's
// name, or its Title ID when nothing better is known.
std::string displayName(const Title &title, const Index &index);
std::string displayName(uint64_t titleId, const Index &index);

// Names of software seen installed, kept on the SD card, so a ticket or
// extra data left behind once the software is gone can still be named.
// The file holds a line "0004000003D5F100 3dsFM" for each.
using Names = std::map<uint64_t, std::string>;
Names parseNames(const std::string &text);
std::string serializeNames(const Names &names);
// Adds the names titles have; true when any was new or changed.
bool rememberNames(Names &names, const std::vector<Title> &titles);
// The remembered name for titleId, or for an update or DLC its game's; ""
// when there is none.
std::string rememberedName(uint64_t titleId, const Names &names);

// Sorted by display name, ignoring case, then by Title ID.
void sortByName(std::vector<const Title *> &list, const Index &index);
// The title lists' orders: Name (A to Z) or Size (largest first), the way
// files sort; reverse flips it. Ties go by name.
void sortTitles(std::vector<const Title *> &list, const Index &index,
                fs::SortOrder order, bool reverse);

// What a title list shows: Software by where it is installed, Updates and
// DLC by kind.
enum class Show { All, SdCard, SystemMemory, Updates, Dlc };
bool shows(Show show, const Title &title);

// Most software keeps its extra data under its unique ID, bits 8 to 27 of
// the Title ID.
uint32_t uniqueId(uint64_t titleId);
// The installed software whose unique ID is extDataId, or nullptr.
const Title *ownerOfExtData(uint64_t extDataId, const std::vector<Title> &all);
// Whether software with extDataId as its unique ID is installed, by the
// title list or by its folder on the SD card (see unusedTickets).
bool extDataInUse(uint64_t extDataId, const std::vector<Title> &all,
                  const std::set<uint64_t> &onSdCard);

// Tickets whose title is not installed, leaving out system tickets and
// DSiWare, whose titles live in system memory where nothing else can
// confirm they are gone. onSdCard lists titles that have a folder on the
// SD card (see titleFromSdFolders); they count as installed whatever the
// title list says, since a title list that failed to read in full would
// otherwise make the tickets of installed software look unused.
std::vector<uint64_t> unusedTickets(const std::vector<uint64_t> &tickets,
                                    const std::vector<Title> &installed,
                                    const std::set<uint64_t> &onSdCard = {});

// The title of an SD card folder Nintendo 3DS/<id0>/<id1>/title/<high>/<low>,
// from its two 8-digit hex names, or false when they are not.
bool titleFromSdFolders(const std::string &high, const std::string &low,
                        uint64_t &titleId);

// --- Seeds ---------------------------------------------------------------------

using Seed = std::array<uint8_t, 16>;

struct SeedEntry {
    uint64_t titleId = 0;
    Seed seed {};
};

// Reads a seeddb.bin: a little-endian count, 12 bytes of padding, then
// 32-byte entries of Title ID, seed and 8 reserved bytes. Fails with a reason
// when the size does not match the count.
bool parseSeedDb(const std::string &data, std::vector<SeedEntry> &out,
                 std::string &error);

// The entries for titles in installed, the only ones worth adding.
std::vector<SeedEntry> seedsFor(const std::vector<SeedEntry> &entries,
                                const std::vector<Title> &installed);

} // namespace titles
