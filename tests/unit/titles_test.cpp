#include "../check.hpp"

#include "titles.hpp"

#include <set>
#include <string>
#include <vector>

namespace {

constexpr uint64_t Game = 0x0004000000055D00ULL;
constexpr uint64_t GameUpdate = 0x0004000E00055D00ULL;
constexpr uint64_t GameDlc = 0x0004008C00055D00ULL;
constexpr uint64_t Demo = 0x0004000200123400ULL;
constexpr uint64_t DsiWare = 0x00048004484E4741ULL;
constexpr uint64_t HomeMenu = 0x0004003000008F02ULL;
constexpr uint64_t SystemDsiWare = 0x0004800542383841ULL;
constexpr uint64_t DownloadPlayChild = 0x0004000100055D00ULL;

titles::Title title(uint64_t id, const std::string &name)
{
    titles::Title value;
    value.id = id;
    value.name = name;
    return value;
}

void putLe(std::string &data, size_t offset, uint64_t value, int bytes)
{
    for (int index = 0; index < bytes; ++index) {
        data[offset + index] = static_cast<char>(value >> (8 * index));
    }
}

} // namespace

TEST("titles: only user software is shown")
{
    for (const uint64_t id : {Game, GameUpdate, GameDlc, Demo, DsiWare}) {
        CHECK(titles::isShown(id));
    }
    // System DSi titles on platform 3 are never shown either.
    constexpr uint64_t TwlSystem = 0x0003000548414E41ULL;
    for (const uint64_t id : {HomeMenu, SystemDsiWare, DownloadPlayChild,
                              TwlSystem}) {
        CHECK(!titles::isShown(id));
    }
    CHECK(titles::isSoftware(Game));
    CHECK(titles::isSoftware(Demo));
    CHECK(titles::isSoftware(DsiWare));
    CHECK(!titles::isSoftware(GameUpdate));
    CHECK(!titles::isSoftware(GameDlc));
    CHECK(!titles::isSoftware(HomeMenu));
}

TEST("titles: updates and DLC are named after their game")
{
    const std::vector<titles::Title> all = {
        title(Game, "Pilotwings"), title(GameUpdate, ""), title(GameDlc, ""),
        title(Demo, "")};
    const titles::Index index = titles::indexOf(all);
    CHECK_EQ(titles::displayName(all[1], index), std::string("Pilotwings"));
    CHECK_EQ(titles::displayName(all[2], index), std::string("Pilotwings"));
    CHECK_EQ(titles::displayName(all[3], index),
             std::string("0004000200123400"));
    // A ticket for an update whose game is gone.
    CHECK_EQ(titles::displayName(0x0004000E00099900ULL, index),
             std::string("0004000E00099900"));
}

TEST("titles: sorts by name without case, then by ID")
{
    const std::vector<titles::Title> all = {
        title(Demo, "beta"), title(Game, "Alpha"), title(DsiWare, "alpha"),
        title(GameUpdate, "")};
    const titles::Index index = titles::indexOf(all);
    std::vector<const titles::Title *> list;
    for (const titles::Title &value : all) {
        list.push_back(&value);
    }
    titles::sortByName(list, index);
    CHECK_EQ(list[0]->id, Game);
    CHECK_EQ(list[1]->id, GameUpdate); // named "Alpha" after its game
    CHECK_EQ(list[2]->id, DsiWare);
    CHECK_EQ(list[3]->id, Demo);
}

TEST("titles: sorts by size, largest first, and reverses")
{
    std::vector<titles::Title> all = {title(Demo, "beta"), title(Game, "Alpha"),
                                      title(DsiWare, "gamma")};
    all[0].size = 300;
    all[1].size = 100;
    all[2].size = 300;
    const titles::Index index = titles::indexOf(all);
    std::vector<const titles::Title *> list = {&all[1], &all[2], &all[0]};
    titles::sortTitles(list, index, fs::SortOrder::Size, false);
    CHECK_EQ(list[0]->id, Demo); // ties go by name
    CHECK_EQ(list[1]->id, DsiWare);
    CHECK_EQ(list[2]->id, Game);
    titles::sortTitles(list, index, fs::SortOrder::Size, true);
    CHECK_EQ(list[0]->id, Game);
    titles::sortTitles(list, index, fs::SortOrder::Name, true);
    CHECK_EQ(list[0]->id, DsiWare);
    CHECK_EQ(list[2]->id, Game);
}

TEST("titles: shows by place and by kind")
{
    titles::Title game = title(Game, "Alpha");
    titles::Title nand = title(DsiWare, "beta");
    nand.place = titles::Place::SystemMemory;
    CHECK(titles::shows(titles::Show::All, nand));
    CHECK(titles::shows(titles::Show::SdCard, game));
    CHECK(!titles::shows(titles::Show::SdCard, nand));
    CHECK(titles::shows(titles::Show::SystemMemory, nand));
    CHECK(titles::shows(titles::Show::Updates, title(GameUpdate, "")));
    CHECK(!titles::shows(titles::Show::Updates, title(GameDlc, "")));
    CHECK(titles::shows(titles::Show::Dlc, title(GameDlc, "")));
}

TEST("titles: extra data belongs to the software with its unique ID")
{
    const std::vector<titles::Title> all = {title(GameUpdate, ""),
                                            title(Game, "Pilotwings")};
    CHECK_EQ(titles::uniqueId(Game), static_cast<uint32_t>(0x55D));
    const titles::Title *owner = titles::ownerOfExtData(0x55D, all);
    CHECK(owner != nullptr && owner->id == Game);
    CHECK(titles::ownerOfExtData(0x777, all) == nullptr);
    // Unlisted, but its folder is on the SD card: still in use.
    CHECK(titles::extDataInUse(0x55D, all, {}));
    CHECK(titles::extDataInUse(0x55D, {}, {Game}));
    CHECK(!titles::extDataInUse(0x55D, {}, {GameUpdate}));
    CHECK(!titles::extDataInUse(0x777, all, {Game}));
}

TEST("titles: unused tickets leave out installed and system titles")
{
    const std::vector<titles::Title> installed = {title(Game, "Pilotwings")};
    const std::vector<uint64_t> unused = titles::unusedTickets(
        {Game, GameUpdate, HomeMenu, Demo}, installed);
    CHECK_EQ(unused.size(), static_cast<size_t>(2));
    CHECK_EQ(unused[0], GameUpdate);
    CHECK_EQ(unused[1], Demo);
}

TEST("titles: a title list that failed to read never makes tickets unused")
{
    // Every game's folder is on the SD card, but the title list came back
    // empty, as when reading it fails: nothing may count as unused.
    const std::set<uint64_t> onSdCard = {Game, GameUpdate, Demo};
    CHECK(titles::unusedTickets({Game, GameUpdate, Demo}, {}, onSdCard).empty());
    // Only what is neither listed nor on the card is unused.
    const std::vector<uint64_t> unused =
        titles::unusedTickets({Game, Demo}, {}, {Game});
    CHECK_EQ(unused.size(), static_cast<size_t>(1));
    CHECK_EQ(unused[0], Demo);
    // DSiWare lives in system memory, where its folder cannot be checked,
    // so its tickets are never offered for bulk deletion.
    CHECK(titles::unusedTickets({DsiWare}, {}).empty());
}

TEST("titles: SD card title folders name their titles")
{
    uint64_t id = 0;
    CHECK(titles::titleFromSdFolders("00040000", "0012d400", id));
    CHECK_EQ(id, static_cast<uint64_t>(0x000400000012D400ULL));
    CHECK(titles::titleFromSdFolders("0004000E", "0012D400", id));
    CHECK_EQ(id, static_cast<uint64_t>(0x0004000E0012D400ULL));
    CHECK(!titles::titleFromSdFolders("0004000", "0012d400", id));
    CHECK(!titles::titleFromSdFolders("00040000", "0012d40g", id));
    CHECK(!titles::titleFromSdFolders("data", "0012d400", id));
}

TEST("titles: reads seeddb.bin and keeps seeds for installed software")
{
    std::string data(16 + 3 * 32, '\0');
    putLe(data, 0, 3, 4);
    const uint64_t ids[] = {Demo, Game, Game};
    for (int entry = 0; entry < 3; ++entry) {
        putLe(data, 16 + entry * 32, ids[entry], 8);
        for (int byte = 0; byte < 16; ++byte) {
            data[16 + entry * 32 + 8 + byte] = static_cast<char>(entry * 16 + byte);
        }
    }
    std::vector<titles::SeedEntry> entries;
    std::string error;
    CHECK(titles::parseSeedDb(data, entries, error));
    CHECK_EQ(entries.size(), static_cast<size_t>(3));
    CHECK_EQ(entries[1].titleId, Game);
    CHECK_EQ(static_cast<int>(entries[1].seed[0]), 16);
    CHECK_EQ(static_cast<int>(entries[1].seed[15]), 31);

    const std::vector<titles::SeedEntry> wanted =
        titles::seedsFor(entries, {title(Game, "Pilotwings"), title(GameDlc, "")});
    CHECK_EQ(wanted.size(), static_cast<size_t>(1));
    CHECK_EQ(wanted[0].titleId, Game);
    CHECK_EQ(static_cast<int>(wanted[0].seed[0]), 16);

    CHECK(!titles::parseSeedDb(data.substr(0, data.size() - 1), entries, error));
    CHECK(error.find("3 seeds") != std::string::npos);
    CHECK(!titles::parseSeedDb("short", entries, error));
    std::string empty(16, '\0');
    CHECK(titles::parseSeedDb(empty, entries, error));
    CHECK(entries.empty());
}

TEST("titles: names of software once installed are remembered")
{
    titles::Names names;
    std::vector<titles::Title> installed(3);
    installed[0].id = 0x0004000003D5F100ULL;
    installed[0].name = "3dsFM";
    installed[1].id = 0x0004000E03D5F100ULL; // its update: no name of its own
    installed[2].id = 0x000400000F5D2300ULL;
    installed[2].name = "3dsDL";
    CHECK(titles::rememberNames(names, installed));
    CHECK(!titles::rememberNames(names, installed));
    CHECK_EQ(names.size(), static_cast<size_t>(2));
    // The update and a DLC are named after their game.
    CHECK_EQ(titles::rememberedName(0x0004000E03D5F100ULL, names), std::string("3dsFM"));
    CHECK_EQ(titles::rememberedName(0x0004008C03D5F100ULL, names), std::string("3dsFM"));
    CHECK_EQ(titles::rememberedName(0x0004000000055D00ULL, names), std::string());

    const titles::Names back = titles::parseNames(titles::serializeNames(names));
    CHECK(back == names);
    CHECK_EQ(titles::serializeNames(names).substr(0, 23),
             std::string("0004000003D5F100 3dsFM\n"));
    // Damaged lines are skipped, the rest kept.
    const titles::Names read = titles::parseNames(
        "garbage\n0004000003D5F10 short\n000400000F5D2300   3dsDL  \n"
        "zz04000003D5F100 bad\n0000000000000000 zero\n0004000003D5F100 \n");
    CHECK_EQ(read.size(), static_cast<size_t>(1));
    CHECK_EQ(read.at(0x000400000F5D2300ULL), std::string("3dsDL"));
    // A new name replaces an old one.
    installed[0].name = "3dsFM 2";
    CHECK(titles::rememberNames(names, installed));
    CHECK_EQ(titles::rememberedName(0x0004000003D5F100ULL, names), std::string("3dsFM 2"));
}
