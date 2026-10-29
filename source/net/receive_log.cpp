#include "receive_log.hpp"

#include "file_ops.hpp"
#include "naming.hpp"
#include "url.hpp"

#include <algorithm>
#include <cstddef>

namespace net {

namespace {

// What a link is called before it is saved: its file's name, or its host.
std::string linkName(const std::string &link)
{
    const std::string name = naming::chooseFilename("", link);
    return name == naming::DefaultFilename ? url::host(link) : name;
}

bool unfinished(ReceiveLog::State state)
{
    return state == ReceiveLog::State::Active ||
           state == ReceiveLog::State::Failed ||
           state == ReceiveLog::State::Stopped;
}

} // namespace

ReceiveLog::Row &ReceiveLog::add(Kind kind, int64_t now)
{
    Row row;
    row.id = nextId_++;
    row.kind = kind;
    row.changed = now;
    rows_.insert(rows_.begin(), row);
    if (rows_.size() > MaxRows) {
        rows_.pop_back();
    }
    ++generation_;
    return rows_.front();
}

ReceiveLog::Row &ReceiveLog::raise(size_t index)
{
    std::rotate(rows_.begin(), rows_.begin() + static_cast<std::ptrdiff_t>(index),
                rows_.begin() + static_cast<std::ptrdiff_t>(index) + 1);
    return rows_.front();
}

void ReceiveLog::touch(Row &row, int64_t now)
{
    row.changed = now;
    ++generation_;
}

void ReceiveLog::transfer(const Transfer &transfer, int64_t now)
{
    const remote::UploadBatch &batch = transfer.batch;
    const bool inFolder = transfer.upload && !batch.folder.empty();
    size_t found = rows_.size();
    for (size_t index = 0; index < rows_.size() && found == rows_.size(); ++index) {
        const Row &row = rows_[index];
        if (row.from != transfer.from) {
            continue;
        }
        if (inFolder ? row.kind == Kind::Folder && row.batch == batch.id &&
                           row.path == batch.folder
                     : row.kind == Kind::File && row.upload == transfer.upload &&
                           row.path == transfer.path && unfinished(row.state)) {
            found = index;
        }
    }

    Row *row = nullptr;
    if (found == rows_.size()) {
        row = &add(inFolder ? Kind::Folder : Kind::File, now);
        row->upload = transfer.upload;
        row->name = inFolder ? fileops::name(batch.folder) : transfer.name;
        row->path = inFolder ? batch.folder : transfer.path;
        row->from = transfer.from;
        row->device = transfer.device;
        row->files = inFolder ? batch.folderFiles : 0;
        row->batch = inFolder ? batch.id : "";
    } else if (rows_[found].state != State::Active) {
        row = &raise(found);
    } else {
        row = &rows_[found];
    }

    if (inFolder) {
        row->currentDone = transfer.finished ? 0 : transfer.done;
        row->currentTotal = transfer.finished ? 0 : transfer.total;
        if (transfer.finished && transfer.ok) {
            ++row->filesDone;
            row->done += transfer.total;
            row->total += transfer.total;
        } else if (transfer.finished) {
            ++row->filesFailed;
            row->message = transfer.message;
        }
        row->state = row->filesDone >= row->files ? State::Received : State::Active;
    } else if (!transfer.finished) {
        row->state = State::Active;
        row->done = transfer.done;
        row->total = transfer.total;
        row->message.clear();
    } else if (transfer.ok) {
        row->state = transfer.upload ? State::Received : State::Sent;
        row->done = row->total = transfer.total;
    } else {
        row->state = transfer.message.empty() ? State::Stopped : State::Failed;
        row->done = transfer.done;
        row->total = transfer.total;
        row->message = transfer.message;
    }
    touch(*row, now);
}

void ReceiveLog::addLinks(const Received &received, State state, int64_t now)
{
    // Added last to first, so they read in the order they came.
    for (size_t index = received.links.size(); index-- > 0;) {
        Row &row = add(Kind::Link, now);
        row.state = state;
        row.url = received.links[index];
        row.name = linkName(row.url);
        row.from = received.from;
        row.device = received.device;
    }
}

void ReceiveLog::linkEnded(const std::string &url, State state,
                           const std::string &name, const std::string &path,
                           uint64_t bytes, const std::string &message,
                           int64_t now)
{
    for (Row &row : rows_) {
        if (row.kind == Kind::Link && row.url == url &&
            (row.state == State::Active || row.state == State::Queued)) {
            row.state = state;
            if (!name.empty()) {
                row.name = name;
            }
            row.path = path;
            row.done = row.total = bytes;
            row.message = message;
            touch(row, now);
            return;
        }
    }
}

void ReceiveLog::settleLinks(State state, int64_t now)
{
    for (Row &row : rows_) {
        if (row.kind == Kind::Link && row.state == State::Active) {
            row.state = state;
            touch(row, now);
        }
    }
}

void ReceiveLog::installEnded(const std::string &path, const Received &received,
                              bool ok, const std::string &message, int64_t now)
{
    const State state = ok ? State::Installed : State::Failed;
    for (Row &row : rows_) {
        if (row.kind == Kind::File && row.upload && row.path == path) {
            row.state = state;
            row.message = ok ? "" : message;
            touch(row, now);
            return;
        }
    }
    // A CIA that was on the card before it was sent to install.
    Row &row = add(Kind::File, now);
    row.state = state;
    row.name = fileops::name(path);
    row.path = path;
    row.from = received.from;
    row.device = received.device;
    row.message = ok ? "" : message;
}

void ReceiveLog::stop(Row &row, int64_t now)
{
    if (row.kind == Kind::Folder) {
        row.state = row.filesDone >= row.files ? State::Received
                    : row.filesFailed > 0 && !row.message.empty()
                        ? State::Failed
                        : State::Stopped;
        row.currentDone = row.currentTotal = 0;
    } else {
        row.state = State::Stopped;
    }
    touch(row, now);
}

void ReceiveLog::settle(int64_t now)
{
    for (Row &row : rows_) {
        if (row.kind == Kind::Folder && row.state == State::Active &&
            now - row.changed >= QuietMs) {
            stop(row, now);
        }
    }
}

void ReceiveLog::stopAll(int64_t now)
{
    for (Row &row : rows_) {
        if (row.state == State::Active) {
            stop(row, now);
        }
    }
}

float progress(const ReceiveLog::Row &row)
{
    const auto part = [](uint64_t done, uint64_t total) {
        return total > 0 ? static_cast<float>(static_cast<double>(std::min(done, total)) /
                                              static_cast<double>(total))
                         : 0.0f;
    };
    if (row.kind != ReceiveLog::Kind::Folder) {
        return part(row.done, row.total);
    }
    if (row.files == 0) {
        return 0;
    }
    const float files = static_cast<float>(row.filesDone + row.filesFailed) +
                        part(row.currentDone, row.currentTotal);
    return std::min(1.0f, files / static_cast<float>(row.files));
}

} // namespace net
