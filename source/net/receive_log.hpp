#pragma once

#include "remote_server.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace net {

// What came in through Receive and went out through its page, newest first:
// a row for each file, each folder the page dropped whole and each link,
// keeping how it ended. The 3DS app keeps one for as long as it runs, so
// leaving Receive and coming back finds the same list.
class ReceiveLog {
public:
    enum class Kind { File, Folder, Link };
    enum class State {
        Active,   // on its way, or waiting its turn
        Received, // on the SD card
        Sent,     // the page saved it from the SD card
        Queued,   // a link waiting in the Queue
        Installed,
        Failed,   // message says why
        Stopped,  // given up part way, by the sender or the console
    };

    struct Row {
        uint64_t id = 0;
        Kind kind = Kind::File;
        State state = State::Active;
        bool upload = true; // false for a file the page saved from the card
        std::string name;   // the file or folder, or the link's file
        std::string path;   // on the SD card, "" when nothing was saved
        std::string url;    // links only
        std::string from;   // the sender's address
        std::string device; // "iPhone", "" when unknown
        std::string message;
        uint64_t done = 0;  // bytes
        uint64_t total = 0; // bytes, 0 when not known
        // Folders: how many files the page is sending into it, and how many
        // of those have arrived or failed so far.
        uint32_t files = 0;
        uint32_t filesDone = 0;
        uint32_t filesFailed = 0;
        int64_t changed = 0; // the now of the call that last changed it

        // Kept for the log's own use: the page's batch a folder belongs to,
        // and the bytes of the folder's file on its way.
        std::string batch;
        uint64_t currentDone = 0;
        uint64_t currentTotal = 0;
    };

    // The rows kept; the oldest go first.
    static constexpr size_t MaxRows = 400;
    // A folder the page has said nothing about for this long has stopped
    // short: the page was closed, or its uploads cancelled.
    static constexpr int64_t QuietMs = 10000;

    // now is in milliseconds, from a clock that only goes forward.

    // A report from FileSharing::progress. A retry picks up the row of the
    // try that stopped short, at the top again.
    void transfer(const Transfer &transfer, int64_t now);
    // Links a sender handed over, Active while they download now or Queued.
    void addLinks(const Received &received, State state, int64_t now);
    // How the newest unfinished row for url ended, with the name and path
    // it was saved as when it was.
    void linkEnded(const std::string &url, State state, const std::string &name,
                   const std::string &path, uint64_t bytes,
                   const std::string &message, int64_t now);
    // Links still Active become state, for a run that ended early.
    void settleLinks(State state, int64_t now);
    // How installing the CIA at path, which the sender asked for, went.
    void installEnded(const std::string &path, const Received &received,
                      bool ok, const std::string &message, int64_t now);
    // Folders quiet for QuietMs stop where they are.
    void settle(int64_t now);
    // Everything still on its way stops, as Receive closes.
    void stopAll(int64_t now);

    const std::vector<Row> &rows() const { return rows_; }
    // Goes up with every change, so a screen knows to look again.
    uint32_t generation() const { return generation_; }

private:
    Row &add(Kind kind, int64_t now);
    // Moves row to the top, where new activity goes; returns its new place.
    Row &raise(size_t index);
    void touch(Row &row, int64_t now);
    void stop(Row &row, int64_t now);

    std::vector<Row> rows_;
    uint64_t nextId_ = 1;
    uint32_t generation_ = 0;
};

// How far a row on its way has got, from 0 to 1.
float progress(const ReceiveLog::Row &row);

} // namespace net
