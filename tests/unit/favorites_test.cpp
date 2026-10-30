#include "../check.hpp"

#include "favorites.hpp"
#include "records.hpp"

#include <string>
#include <vector>

using List = std::vector<std::string>;

TEST("favorites: toggling adds at the end and takes out")
{
    List list;
    CHECK(favorites::toggle(list, "sdmc:/3ds/Checkpoint/saves/"));
    CHECK(favorites::toggle(list, "sdmc:/3ds/FBI.3dsx"));
    CHECK(favorites::contains(list, "sdmc:/3ds/FBI.3dsx"));
    CHECK(!favorites::contains(list, "sdmc:/3ds/Checkpoint/saves"));
    CHECK_EQ(list.size(), size_t(2));
    CHECK_EQ(list.back(), std::string("sdmc:/3ds/FBI.3dsx"));
    CHECK(!favorites::toggle(list, "sdmc:/3ds/Checkpoint/saves/"));
    CHECK((list == List{"sdmc:/3ds/FBI.3dsx"}));
}

TEST("favorites: case does not matter, as on FAT")
{
    List list = {"sdmc:/cias/Sub/"};
    CHECK(favorites::contains(list, "sdmc:/CIAs/sub/"));
    CHECK(!favorites::toggle(list, "sdmc:/CIAS/SUB/"));
    CHECK(list.empty());
    list = {"sdmc:/cias/Sub/", "sdmc:/cias/sub/inner.txt"};
    CHECK(favorites::moved(list, "sdmc:/CIAS/sub", "sdmc:/cias/Other"));
    CHECK((list == List{"sdmc:/cias/Other/", "sdmc:/cias/Other/inner.txt"}));
    CHECK(favorites::removed(list, "SDMC:/Cias/other"));
    CHECK(list.empty());
}

TEST("favorites: a rename or move carries the entry and what is inside it")
{
    List list = {"sdmc:/games/", "sdmc:/games/gba/", "sdmc:/games.txt",
                 "sdmc:/gamesx/", "sdmc:/cias/a.cia"};
    CHECK(favorites::moved(list, "sdmc:/games", "sdmc:/roms"));
    CHECK((list == List{"sdmc:/roms/", "sdmc:/roms/gba/", "sdmc:/games.txt",
                        "sdmc:/gamesx/", "sdmc:/cias/a.cia"}));
    CHECK(favorites::moved(list, "sdmc:/cias/a.cia", "sdmc:/b.cia"));
    CHECK_EQ(list.back(), std::string("sdmc:/b.cia"));
    CHECK(!favorites::moved(list, "sdmc:/nothing", "sdmc:/else"));
}

TEST("favorites: one moved onto another favourite becomes that one")
{
    // Cut a/x and paste it over b/x with both starred.
    List list = {"sdmc:/a/x/", "sdmc:/b/x/", "sdmc:/c/"};
    CHECK(favorites::moved(list, "sdmc:/a/x", "sdmc:/b/x"));
    CHECK((list == List{"sdmc:/b/x/", "sdmc:/c/"}));
}

TEST("favorites: deleting an entry drops it and what was inside it")
{
    List list = {"sdmc:/games/", "sdmc:/games/gba/", "sdmc:/games.txt",
                 "sdmc:/gamesx/"};
    CHECK(favorites::removed(list, "sdmc:/games"));
    CHECK((list == List{"sdmc:/games.txt", "sdmc:/gamesx/"}));
    CHECK(favorites::removed(list, "sdmc:/games.txt"));
    CHECK(!favorites::removed(list, "sdmc:/games"));
    CHECK((list == List{"sdmc:/gamesx/"}));
}

TEST("favorites: kept in settings in order, once each, on the SD card only")
{
    Settings settings;
    settings.favorites = {"sdmc:/3ds/Checkpoint/saves/", "sdmc:/a=b.3dsx",
                          "sdmc:/Über/"};
    CHECK(parseSettings(serializeSettings(settings)).favorites ==
          settings.favorites);
    const Settings parsed = parseSettings(
        "favorite=sdmc:/b/\nfavorite=romfs:/x\nfavorite=\n"
        "Favorite = sdmc:/a/\nfavorite=sdmc:/b/\nfavorite=SDMC:/A/\n");
    CHECK((parsed.favorites == List{"sdmc:/b/", "sdmc:/a/"}));
    CHECK(parseSettings("").favorites.empty());
}
