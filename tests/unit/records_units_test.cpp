#include "../check.hpp"

#include "records.hpp"
#include "units.hpp"

TEST("units: byte, rate and duration formatting")
{
    CHECK_EQ(units::bytes(0), std::string("0 B"));
    CHECK_EQ(units::bytes(1023), std::string("1023 B"));
    CHECK_EQ(units::bytes(1536), std::string("1.50 KiB"));
    CHECK_EQ(units::bytes(12 * 1024 * 1024 + 300 * 1024),
             std::string("12.3 MiB"));
    CHECK_EQ(units::bytes(4294967295ULL), std::string("4.00 GiB"));

    // At most four characters before the unit, whatever the size.
    const uint64_t GiB = 1024ULL * 1024 * 1024;
    CHECK_EQ(units::grouped(0), std::string("0"));
    CHECK_EQ(units::grouped(999), std::string("999"));
    CHECK_EQ(units::grouped(1000), std::string("1,000"));
    CHECK_EQ(units::grouped(3210180450ULL), std::string("3,210,180,450"));
    CHECK_EQ(units::shortBytes(500), std::string("500 B"));
    CHECK_EQ(units::shortBytes(1000), std::string("0.98 KiB"));
    CHECK_EQ(units::shortBytes(8 * GiB), std::string("8.00 GiB"));
    CHECK_EQ(units::shortBytes(GiB * 9996 / 1000), std::string("10.0 GiB"));
    CHECK_EQ(units::shortBytes(GiB * 9954 / 100), std::string("99.5 GiB"));
    CHECK_EQ(units::shortBytes(GiB * 23847 / 100), std::string("238 GiB"));
    CHECK_EQ(units::shortBytes(1023ULL * 1024 * 1024 + 1000 * 1024),
             std::string("1.00 GiB"));
    CHECK_EQ(units::shortBytes(GiB * 9996 / 10), std::string("0.98 TiB"));
    CHECK_EQ(units::shortBytes(2048 * GiB), std::string("2.00 TiB"));
    CHECK_EQ(units::rate(2.5 * 1024 * 1024), std::string("2.50 MiB/s"));
    CHECK_EQ(units::rate(-5), std::string("0 B/s"));
    CHECK_EQ(units::duration(42), std::string("42s"));
    CHECK_EQ(units::duration(185), std::string("3m 05s"));
    CHECK_EQ(units::duration(3720), std::string("1h 02m"));
    CHECK_EQ(units::dateTime(1791813780), std::string("2026-10-12 14:03"));
}

TEST("units: rate meter smooths samples and estimates time left")
{
    units::RateMeter meter;
    CHECK(!meter.hasRate());
    meter.sample(0, 0);
    meter.sample(200, 1000); // too soon, ignored
    CHECK(!meter.hasRate());
    meter.sample(1000, 100000);
    CHECK(meter.hasRate());
    CHECK(meter.bytesPerSecond() > 99999.0 && meter.bytesPerSecond() < 100001.0);
    CHECK_EQ(meter.secondsLeft(500000), static_cast<int64_t>(5));

    meter.sample(2000, 100000); // stalled for a second
    CHECK(meter.bytesPerSecond() < 100000.0);
    CHECK(meter.bytesPerSecond() > 0.0);

    meter.sample(1500, 0); // time went backwards: restart
    CHECK(meter.hasRate());
    meter.reset();
    CHECK(!meter.hasRate());
    CHECK_EQ(meter.secondsLeft(10), static_cast<int64_t>(-1));
}

TEST("records: settings round trip and defaults")
{
    Settings settings;
    settings.destination = "sdmc:/cias/";
    settings.resumeDownloads = false;
    settings.offerZipExtraction = true;
    settings.checkForUpdates = false;
    settings.sounds = false;
    const Settings parsed = parseSettings(serializeSettings(settings));
    CHECK_EQ(parsed.checkForUpdates, false);
    CHECK_EQ(parsed.sounds, false);
    CHECK_EQ(parsed.destination, std::string("sdmc:/cias/"));
    CHECK_EQ(parsed.resumeDownloads, false);
    CHECK_EQ(parsed.offerZipExtraction, true);
    CHECK(parsed.afterCiaDownload == CiaAction::Ask);

    for (const CiaAction action : {CiaAction::Install,
                                   CiaAction::InstallAndDelete,
                                   CiaAction::Keep}) {
        settings.afterCiaDownload = action;
        CHECK(parseSettings(serializeSettings(settings)).afterCiaDownload ==
              action);
    }
    CHECK(parseSettings("after_cia_download=sometimes\n").afterCiaDownload ==
          CiaAction::Ask);

    for (int count = 1; count <= MaxDownloadConnections; ++count) {
        settings.maxConnections = count;
        CHECK_EQ(parseSettings(serializeSettings(settings)).maxConnections,
                 count);
    }
    // Anything out of range or not a number keeps the default.
    for (const char *bad : {"max_connections=0\n", "max_connections=5\n",
                            "max_connections=two\n", "max_connections=\n",
                            "max_connections=3x\n"}) {
        CHECK_EQ(parseSettings(bad).maxConnections, MaxDownloadConnections);
    }

    const Settings defaults = parseSettings("# nothing\nunknown=1\nbroken\n");
    CHECK_EQ(defaults.destination, std::string("sdmc:/"));
    CHECK_EQ(defaults.resumeDownloads, true);
    CHECK_EQ(defaults.offerZipExtraction, false);
    CHECK_EQ(defaults.checkForUpdates, true);
    CHECK_EQ(defaults.sounds, true);
    CHECK_EQ(defaults.maxConnections, MaxDownloadConnections);

    CHECK_EQ(defaults.timeZoneSet, false);

    Settings zoned;
    zoned.timeZoneSet = true;
    zoned.timeZoneMinutes = 330;
    const Settings zonedParsed = parseSettings(serializeSettings(zoned));
    CHECK(zonedParsed.timeZoneSet);
    CHECK_EQ(zonedParsed.timeZoneMinutes, 330);
    CHECK(!parseSettings("time_zone_minutes=7\n").timeZoneSet);
    CHECK(!parseSettings("time_zone_minutes=9999\n").timeZoneSet);
    CHECK(!parseSettings("time_zone_minutes=60x\n").timeZoneSet);
    CHECK_EQ(parseSettings("time_zone_minutes=-210\n").timeZoneMinutes, -210);

    const Settings spelled = parseSettings("Resume_Downloads = off\r\n");
    CHECK_EQ(spelled.resumeDownloads, false);
    CHECK_EQ(parseLegacyDestination("sdmc:/old/\nignored"),
             std::string("sdmc:/old/"));
}

TEST("records: history round trip, ordering and limit")
{
    std::vector<HistoryEntry> entries;
    for (int index = 0; index < 5; ++index) {
        HistoryEntry entry;
        entry.time = 1000 + index;
        entry.status = index == 2       ? HistoryStatus::Installed
                     : index % 2 == 0 ? HistoryStatus::Saved
                                      : HistoryStatus::Failed;
        entry.bytes = static_cast<uint64_t>(index) * 10;
        entry.resumable = index == 1;
        entry.url = "https://x/" + std::to_string(index);
        entry.folder = "sdmc:/";
        entry.path = "sdmc:/f" + std::to_string(index);
        entry.message = "line one\twith tab\nand newline";
        entry.name = index == 4 ? "game\tname.cia" : "";
        addHistoryEntry(entries, entry, 3);
    }
    CHECK_EQ(entries.size(), static_cast<size_t>(3));
    CHECK_EQ(entries.front().url, std::string("https://x/4"));

    const std::vector<HistoryEntry> parsed =
        parseHistory(serializeHistory(entries));
    CHECK_EQ(parsed.size(), static_cast<size_t>(3));
    CHECK_EQ(parsed[0].url, std::string("https://x/4"));
    CHECK(parsed[0].status == HistoryStatus::Saved);
    CHECK(parsed[1].status == HistoryStatus::Failed);
    CHECK(parsed[2].status == HistoryStatus::Installed);
    CHECK_EQ(parsed[2].time, static_cast<int64_t>(1002));
    CHECK_EQ(parsed[0].message, std::string("line one with tab and newline"));
    CHECK_EQ(parsed[0].name, std::string("game name.cia"));
    CHECK_EQ(parsed[1].name, std::string(""));

    // Files from before 0.7 have no name field.
    const std::vector<HistoryEntry> older = parseHistory(
        "3dsDL-history 1\n5\tsaved\t10\t0\thttps://x/a\tsdmc:/\tsdmc:/a\tSaved\n");
    CHECK_EQ(older.size(), static_cast<size_t>(1));
    CHECK_EQ(older[0].name, std::string(""));

    CHECK(parseHistory("not a history file\n").empty());
    CHECK(parseHistory("3dsDL-history 1\ngarbage\n1\tsaved\n").empty());
}

TEST("records: resume info and validators")
{
    ResumeInfo info;
    info.url = "https://x/f.zip";
    info.etag = "\"abc\"";
    info.lastModified = "Wed, 21 Oct 2015 07:28:00 GMT";
    info.filename = "f.zip";
    info.total = 1234;

    ResumeInfo parsed;
    CHECK(parseResumeInfo(serializeResumeInfo(info), parsed));
    CHECK_EQ(parsed.url, info.url);
    CHECK_EQ(parsed.total, static_cast<uint64_t>(1234));
    CHECK_EQ(ifRangeValidator(parsed), std::string("\"abc\""));

    parsed.etag = "W/\"weak\"";
    CHECK_EQ(ifRangeValidator(parsed), info.lastModified);
    parsed.lastModified.clear();
    CHECK_EQ(ifRangeValidator(parsed), std::string(""));

    CHECK(!parseResumeInfo("garbage", parsed));
}

TEST("records: split resume info keeps its done ranges")
{
    ResumeInfo info;
    info.url = "https://x/f.zip";
    info.etag = "\"abc\"";
    info.total = 3000;
    info.split = true;
    info.done = {{0, 1000}, {1500, 2000}};

    const std::string text = serializeResumeInfo(info);
    // Older releases only read version 1, which would take the whole
    // full-size file as downloaded.
    CHECK_EQ(text.substr(0, text.find('\n')),
             std::string("3dsDL-resume 2"));
    ResumeInfo parsed;
    CHECK(parseResumeInfo(text, parsed));
    CHECK(parsed.split);
    CHECK_EQ(parsed.done.size(), static_cast<size_t>(2));
    CHECK_EQ(parsed.done[1].first, static_cast<uint64_t>(1500));
    CHECK_EQ(parsed.done[1].second, static_cast<uint64_t>(2000));

    info.done.clear();
    CHECK(parseResumeInfo(serializeResumeInfo(info), parsed));
    CHECK(parsed.split);
    CHECK(parsed.done.empty());

    CHECK(!parseResumeInfo("3dsDL-resume 2\nurl=https://x\ndone=5-5\n",
                           parsed));
    CHECK(!parseResumeInfo("3dsDL-resume 2\nurl=https://x\ndone=1-x\n",
                           parsed));
}

TEST("records: partial paths and Content-Range")
{
    const PartialPaths first = partialPaths("sdmc:/d/", "https://x/a");
    const PartialPaths again = partialPaths("sdmc:/d/", "https://x/a");
    const PartialPaths other = partialPaths("sdmc:/d/", "https://x/b");
    CHECK_EQ(first.data, again.data);
    CHECK(first.data != other.data);
    CHECK(first.data.rfind("sdmc:/d/.3dsdl-", 0) == 0);
    CHECK(first.info.size() > 7 &&
          first.info.compare(first.info.size() - 7, 7, ".resume") == 0);

    CHECK_EQ(contentRangeTotal("bytes 100-199/1000"), static_cast<int64_t>(1000));
    CHECK_EQ(contentRangeTotal("bytes 100-199/*"), static_cast<int64_t>(-1));
    CHECK_EQ(contentRangeTotal(""), static_cast<int64_t>(-1));
}
