#include "../check.hpp"

#include "receive_log.hpp"

#include <string>

namespace {

using Log = net::ReceiveLog;

net::Transfer upload(const std::string &path, uint64_t done, uint64_t total)
{
    net::Transfer transfer;
    transfer.name = path.substr(path.rfind('/') + 1);
    transfer.path = path;
    transfer.from = "192.168.1.24";
    transfer.device = "Mac";
    transfer.done = done;
    transfer.total = total;
    return transfer;
}

net::Transfer ended(net::Transfer transfer, bool ok, const std::string &message = "")
{
    transfer.finished = true;
    transfer.ok = ok;
    transfer.message = message;
    return transfer;
}

net::Transfer inFolder(net::Transfer transfer, const std::string &batch,
                       uint32_t files)
{
    transfer.batch.id = batch;
    transfer.batch.folder = "sdmc:/3ds/Checkpoint";
    transfer.batch.folderFiles = files;
    return transfer;
}

} // namespace

TEST("receive log: a file on its way, then how it ended")
{
    Log log;
    CHECK(log.rows().empty());
    const uint32_t before = log.generation();
    log.transfer(upload("sdmc:/cias/a.cia", 100, 400), 1000);
    CHECK(log.generation() != before);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(1));
    const Log::Row &row = log.rows()[0];
    CHECK(row.kind == Log::Kind::File && row.state == Log::State::Active);
    CHECK_EQ(row.name, std::string("a.cia"));
    CHECK_EQ(row.device, std::string("Mac"));
    CHECK(net::progress(row) == 0.25f);

    log.transfer(ended(upload("sdmc:/cias/a.cia", 400, 400), true), 2000);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(1));
    CHECK(log.rows()[0].state == Log::State::Received);
    CHECK_EQ(log.rows()[0].total, static_cast<uint64_t>(400));

    // The page saving a file from the card is a row of its own.
    net::Transfer saved = upload("sdmc:/cias/a.cia", 0, 400);
    saved.upload = false;
    log.transfer(saved, 3000);
    log.transfer(ended(saved, true), 3100);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(2));
    CHECK(log.rows()[0].state == Log::State::Sent);
    CHECK(log.rows()[1].state == Log::State::Received);
}

TEST("receive log: a retry takes over the try that stopped short")
{
    Log log;
    log.transfer(ended(upload("sdmc:/a.bin", 50, 400), false), 1000);
    CHECK(log.rows()[0].state == Log::State::Stopped);
    log.transfer(ended(upload("sdmc:/b.bin", 9, 9), false, "No room for b.bin."), 2000);
    CHECK(log.rows()[0].state == Log::State::Failed);
    CHECK_EQ(log.rows()[0].message, std::string("No room for b.bin."));

    // a.bin again goes back to the top, in the same row.
    const uint64_t id = log.rows()[1].id;
    log.transfer(upload("sdmc:/a.bin", 10, 400), 3000);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(2));
    CHECK_EQ(log.rows()[0].id, id);
    CHECK(log.rows()[0].state == Log::State::Active);
    CHECK(log.rows()[0].message.empty());
    log.transfer(ended(upload("sdmc:/a.bin", 400, 400), true), 4000);
    CHECK(log.rows()[0].state == Log::State::Received);

    // Sending a file that already arrived is a new row.
    log.transfer(upload("sdmc:/a.bin", 0, 400), 5000);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(3));
}

TEST("receive log: a folder dropped whole is one row")
{
    Log log;
    log.transfer(inFolder(upload("sdmc:/3ds/Checkpoint/a", 5, 10), "b1", 3), 1000);
    log.transfer(ended(inFolder(upload("sdmc:/3ds/Checkpoint/a", 10, 10), "b1", 3), true), 1100);
    log.transfer(inFolder(upload("sdmc:/3ds/Checkpoint/saves/b", 10, 40), "b1", 3), 1200);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(1));
    const Log::Row &row = log.rows()[0];
    CHECK(row.kind == Log::Kind::Folder && row.state == Log::State::Active);
    CHECK_EQ(row.name, std::string("Checkpoint"));
    CHECK_EQ(row.filesDone, static_cast<uint32_t>(1));
    CHECK(net::progress(row) == 1.25f / 3);

    // Quiet for long enough, it stops where it is.
    log.settle(1200 + Log::QuietMs - 1);
    CHECK(log.rows()[0].state == Log::State::Active);
    log.settle(1200 + Log::QuietMs);
    CHECK(log.rows()[0].state == Log::State::Stopped);

    // A failed file sent again still completes the folder.
    log.transfer(ended(inFolder(upload("sdmc:/3ds/Checkpoint/saves/b", 0, 40), "b1", 3), false,
                       "Could not write b."),
                 20000);
    CHECK(log.rows()[0].state == Log::State::Active);
    log.transfer(ended(inFolder(upload("sdmc:/3ds/Checkpoint/saves/b", 40, 40), "b1", 3), true), 21000);
    log.transfer(ended(inFolder(upload("sdmc:/3ds/Checkpoint/c", 50, 50), "b1", 3), true), 22000);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(1));
    CHECK(log.rows()[0].state == Log::State::Received);
    CHECK_EQ(log.rows()[0].done, static_cast<uint64_t>(100));

    // The same folder dropped again is a new row.
    log.transfer(inFolder(upload("sdmc:/3ds/Checkpoint/a", 1, 10), "b2", 3), 30000);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(2));
    log.stopAll(30001);
    CHECK(log.rows()[0].state == Log::State::Stopped);
}

TEST("receive log: links and installs keep how they went")
{
    Log log;
    net::Received received;
    received.links = {"https://example.com/a.cia", "https://example.com/"};
    received.from = "192.168.1.31";
    received.device = "iPhone";
    log.addLinks(received, Log::State::Active, 1000);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(2));
    CHECK_EQ(log.rows()[0].name, std::string("a.cia"));
    CHECK_EQ(log.rows()[1].name, std::string("example.com"));
    CHECK(log.rows()[0].kind == Log::Kind::Link);

    log.linkEnded("https://example.com/a.cia", Log::State::Installed, "a.cia",
                  "sdmc:/cias/a.cia", 400, "", 2000);
    CHECK(log.rows()[0].state == Log::State::Installed);
    CHECK_EQ(log.rows()[0].path, std::string("sdmc:/cias/a.cia"));
    log.settleLinks(Log::State::Queued, 3000);
    CHECK(log.rows()[1].state == Log::State::Queued);
    log.linkEnded("https://example.com/", Log::State::Received, "b.zip",
                  "sdmc:/cias/b.zip", 9, "", 4000);
    CHECK_EQ(log.rows()[1].name, std::string("b.zip"));

    // An install picks up the upload it came from, or adds a row.
    net::Received page;
    page.from = "192.168.1.24";
    log.transfer(ended(upload("sdmc:/cias/c.cia", 9, 9), true), 5000);
    log.installEnded("sdmc:/cias/c.cia", page, true, "", 6000);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(3));
    CHECK(log.rows()[0].state == Log::State::Installed);
    log.installEnded("sdmc:/old.cia", page, false, "Not enough space.", 7000);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(4));
    CHECK(log.rows()[0].state == Log::State::Failed);
    CHECK_EQ(log.rows()[0].name, std::string("old.cia"));
}

TEST("receive log: only the newest rows are kept")
{
    Log log;
    for (size_t index = 0; index < Log::MaxRows + 5; ++index) {
        log.transfer(ended(upload("sdmc:/f" + std::to_string(index), 1, 1), true),
                     static_cast<int64_t>(index));
    }
    CHECK_EQ(log.rows().size(), Log::MaxRows);
    CHECK_EQ(log.rows()[0].name, "f" + std::to_string(Log::MaxRows + 4));
}

TEST("receive log: clearing drops what is done and keeps what is on its way")
{
    Log log;
    CHECK(!log.anyFinished());
    CHECK(!log.clearFinished());
    log.transfer(ended(upload("sdmc:/cias/a.cia", 10, 10), true), 1);
    log.transfer(ended(upload("sdmc:/cias/b.cia", 4, 10), false, "Lost"), 2);
    log.transfer(upload("sdmc:/cias/c.cia", 3, 10), 3);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(3));
    CHECK(log.anyFinished());
    const uint32_t before = log.generation();
    CHECK(log.clearFinished());
    CHECK(log.generation() != before);
    CHECK_EQ(log.rows().size(), static_cast<size_t>(1));
    CHECK_EQ(log.rows()[0].name, std::string("c.cia"));
    CHECK(log.rows()[0].state == Log::State::Active);
    CHECK(!log.anyFinished());
    CHECK(!log.clearFinished());
    // What arrives after carries on as before.
    log.transfer(ended(upload("sdmc:/cias/c.cia", 10, 10), true), 4);
    CHECK(log.rows()[0].state == Log::State::Received);
}
