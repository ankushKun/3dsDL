#include "../check.hpp"

#include "saves.hpp"

#include <string>
#include <vector>

TEST("saves: Checkpoint's folder names")
{
    CHECK_EQ(saves::titleFolder(0x0004000000055D00ULL, "Pokemon X"),
             std::string("0x0055D Pokemon X"));
    CHECK_EQ(saves::titleFolder(0x0004000000164800ULL, "Pok\xC3\xA9mon: Moon!"),
             std::string("0x01648 Pok\xC3\xA9mon  Moon"));
    // No name: Checkpoint keeps the trailing space.
    CHECK_EQ(saves::titleFolder(0x0004000000123400ULL, ""),
             std::string("0x01234 "));
    CHECK_EQ(saves::titleFolder(0x0004000000123400ULL, "..."),
             std::string("0x01234 "));
    CHECK_EQ(saves::sanitize(" a/b\\c:d*e?f\"g<h>i|j,k.l! "),
             std::string(" a b c d e f g h i j k l"));
}

TEST("saves: extra data IDs, with the games that share")
{
    CHECK_EQ(saves::extDataId(0x0004000000086300ULL), static_cast<uint32_t>(0x863));
    CHECK_EQ(saves::extDataId(0x0004000000055E00ULL), static_cast<uint32_t>(0x055D));
    CHECK_EQ(saves::extDataId(0x0004000000175E00ULL), static_cast<uint32_t>(0x1648));
    CHECK_EQ(saves::extDataId(0x000400000017A800ULL), static_cast<uint32_t>(0x1795));
    CHECK_EQ(saves::extDataId(0x00040000001B5100ULL), static_cast<uint32_t>(0x1B50));
}

TEST("saves: exclusions, backup names and host files")
{
    CHECK(saves::isExcluded(0x0004000E00055D00ULL));
    CHECK(saves::isExcluded(0x0004800F484E4841ULL));
    CHECK(saves::isExcluded(0x0004000000008602ULL));
    CHECK(!saves::isExcluded(0x0004000000055D00ULL));

    std::tm when {};
    when.tm_year = 126;
    when.tm_mon = 9;
    when.tm_mday = 19;
    when.tm_hour = 14;
    when.tm_min = 30;
    when.tm_sec = 5;
    CHECK_EQ(saves::backupName(when), std::string("20261019-143005"));

    CHECK_EQ(saves::customBackupName("before the boss: try 2!"),
             std::string("before the boss  try"));
    // A multi-byte character cut in half is dropped, not left broken.
    CHECK_EQ(saves::customBackupName("1234567890123456789\xC3\xA9"),
             std::string("1234567890123456789"));

    for (const char *junk : {"._save.bin", ".DS_Store", "Thumbs.db",
                             "desktop.ini", "$RECYCLE.BIN", ".Trashes"}) {
        CHECK(saves::isHostMetadata(junk));
    }
    CHECK(!saves::isHostMetadata("main"));
    CHECK(!saves::isHostMetadata(".secret"));

    std::vector<std::string> names = {"20250101-000000", "20261019-143005",
                                      "20260301-120000"};
    saves::sortNewestFirst(names);
    CHECK_EQ(names[0], std::string("20261019-143005"));
    CHECK_EQ(names[2], std::string("20250101-000000"));
}

TEST("saves: a Checkpoint folder names the title it belongs to")
{
    const uint64_t fm = 0x0004000003D5F100ULL;
    CHECK_EQ(saves::nameFromFolder(fm, "0x3D5F1 3dsFM"), std::string("3dsFM"));
    CHECK_EQ(saves::nameFromFolder(fm, "0x3d5f1 3dsFM "), std::string("3dsFM"));
    CHECK_EQ(saves::nameFromFolder(fm, saves::titleFolder(fm, "3dsFM")),
             std::string("3dsFM"));
    CHECK_EQ(saves::nameFromFolder(fm, "0x3D5F2 Other"), std::string());
    CHECK_EQ(saves::nameFromFolder(fm, "0x3D5F1 "), std::string());
    CHECK_EQ(saves::nameFromFolder(fm, "0x3D5F1"), std::string());
}
