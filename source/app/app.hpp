#pragma once

#include "archives.hpp"
#include "cia.hpp"
#include "download_engine.hpp"
#include "file_index.hpp"
#include "file_types.hpp"
#include "library.hpp"
#include "savedata.hpp"
#include "platform.hpp"
#include "receive_log.hpp"
#include "records.hpp"
#include "update.hpp"

#include <3ds.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// The screens of 3dsDL and the flow between them.
class App {
public:
    App();
    ~App();
    void run();

private:
    enum class Action {
        None,
        Scan,
        EnterUrl,
        History,
        Files,
        Titles,
        Queue,
        Receive,
        Folder,
        Settings,
        Exit
    };

    Action home();

    enum class Start { Cancelled, Done, Queued };
    // Asks to confirm, then downloads, or adds the link to the queue.
    Start startDownload(const std::string &address, std::string folder);
    enum class Confirm { Cancel, Download, Queue };
    Confirm confirmDownload(const std::string &address, std::string &folder);
    // Installs a CIA from the internet without saving it, for a full SD card
    // and for FBI's tools. quiet leaves out the screens that say how it
    // went, for a caller that shows that itself; message then says why it
    // failed. Returns Installed, Failed or Cancelled.
    HistoryStatus installDirectly(const std::string &address,
                                  bool quiet = false,
                                  std::string *message = nullptr);
    void finishDownload(const std::string &address, const std::string &folder,
                        const net::Result &result);
    // Asks where to extract an archive, then extracts it. known is a
    // listing already read, such as the archive viewer's.
    void offerExtraction(const std::string &path, const std::string &folder,
                         const std::string &summary,
                         const std::string &title =
                             "Download complete. Extract the archive?",
                         const archives::Listing *known = nullptr);

    // Asks what to do with a CIA that just finished downloading.
    void offerInstall(const std::string &path, const std::string &address,
                      const std::string &summary);
    // Installs the CIA at path after the checks and questions that come
    // first. address is the download it came from, or "" for a file picked
    // on the SD card. Returns true when it was installed.
    // quiet skips the success screen, for installing several in a row.
    // True once the CIA's title is installed, including when the same
    // version already was and the person chose to keep it; alreadyInstalled
    // then says so.
    bool installCia(const std::string &path, const cia::Info &info,
                    bool deleteAfter, const std::string &address,
                    bool quiet = false, bool *alreadyInstalled = nullptr);
    void markInstalled(const std::string &address, const std::string &path,
                       const std::string &message);

    // Files, in files.cpp. Paths of entries never end with '/'; folders
    // being browsed always do.
    // Files, then saves the index if it changed. select, when given, is the
    // entry to open Files on, highlighted in its folder.
    void filesScreen(const std::string &select = "");
    void browseFiles(const std::string &select = "");
    // folder, when given, is the whole folder path is in, already read and
    // sorted, for the picture viewer's previous and next.
    void openFile(const std::string &path,
                  const std::vector<fs::Entry> *folder = nullptr);
    // Picks how Files orders folders; changes apply as they are picked.
    // folder is shown on the top screen when given. True when it changed.
    bool sortScreen(const std::string &folder);
    void ciaOptions(const std::string &path);
    // Options for the marked entries, or the highlighted one. Returns true
    // when the folder's contents may have changed.
    // entries is the folder as read so far, complete once all of it is;
    // options that need every entry wait for that.
    bool entryOptions(const std::string &folder,
                      const std::vector<std::string> &paths,
                      std::vector<std::string> &marked,
                      const std::vector<fs::Entry> &entries, bool complete);
    bool folderOptions(const std::string &folder,
                       const std::vector<fs::Entry> &entries, bool complete,
                       std::vector<std::string> &marked);
    void textEditor(const std::string &path);
    void hexViewer(const std::string &path);
    void pictureViewer(const std::vector<std::string> &pictures,
                       size_t index);
    // folderCover is shown for songs without a cover of their own.
    void songViewer(const std::vector<std::string> &songs, size_t index,
                    const std::string &folderCover);
    void archiveViewer(const std::string &path);
    void titleDetails(const std::string &path, filetype::Kind kind);
    void showProperties(const std::vector<std::string> &paths);
    void showChecksum(const std::string &path);
    void installTicketFile(const std::string &path);
    bool renameEntry(const std::string &path);
    bool deleteEntries(const std::vector<std::string> &paths);
    bool pasteInto(const std::string &folder);
    bool createFolder(const std::string &folder);
    bool createFile(const std::string &folder);
    void installCias(const std::vector<std::string> &paths, bool deleteAfter);

    bool chooseFolder(std::string &folder);
    bool browseFolders(std::string &folder);
    bool typeFolder(std::string &folder);
    void changeDefaultFolder();

    void historyScreen();
    bool confirmClearHistory();
    void settingsScreen();
    void aboutScreen();
    // Where to report a problem, as QR codes to scan, in report.cpp.
    void reportScreen();

    // Installed software and its data, in titles_screens.cpp.
    void titlesScreen();
    // What is installed is read in the background into library_ (see
    // library::Loader); startLibrary reads it again from scratch, and
    // pollLibrary, run every frame, takes what has arrived.
    void startLibrary();
    void pollLibrary();
    // Reads again and waits for all of it with a progress bar, for screens
    // that need the whole picture; false, after saying why, when it could
    // not be read.
    bool reloadLibrary();
    // Something at path on the SD card was written, moved or removed by
    // 3dsDL: the index forgets it and what holds it.
    void sdChanged(const std::string &path);
    // Before using the index: forgets every folder total when the card may
    // have changed without 3dsDL saying where.
    void checkSdIndex();
    // The index is saved between runs in sdmc:/3ds/3dsDL/index.bin: read on
    // a worker at start, taken in by the frame hook, saved when Files closes
    // and when 3dsDL does.
    void startIndexLoad();
    void pollIndex();
    void saveIndex();
    void titleList(bool software);
    // Order (saved) and what the list shows (until 3dsDL closes).
    void titleSortScreen(bool software);
    void titleOptions(titles::Title title);
    void extraDataScreen();
    void ticketsScreen();
    void deleteUnusedTickets();
    void cleanUpScreen();
    void importSeeds();

    // Save data and extra data, in saves_screens.cpp.
    void saveTitlesScreen();
    void saveDataScreen(const savedata::Target &target,
                        const std::vector<uint16_t> &icon);
    void dataBrowser(const savedata::Target &target);

    // The download queue and receiving links over the network, in
    // queue.cpp.
    struct QueuedLink {
        enum class State { Waiting, Failed, Stopped };
        uint64_t id = 0;
        std::string url;
        std::string folder;
        State state = State::Waiting;
        std::string message; // why it failed
    };
    void addToQueue(const std::vector<std::string> &links,
                    const std::string &folder, bool announce);
    void queueScreen();
    // How one link of a queue run went: Saved, Installed, Failed or
    // Cancelled.
    struct QueueResult {
        std::string url;
        HistoryStatus status = HistoryStatus::Failed;
        std::string name;    // the file's name, once known
        std::string path;    // where it was saved, when it was
        uint64_t bytes = 0;
        std::string message; // why it failed
    };
    // Downloads the queued links in order, or only those in ids when given,
    // without stopping to ask. Finished ones leave the queue. With results,
    // how each went goes there instead of into a summary at the end.
    void runQueue(const std::vector<uint64_t> *ids = nullptr,
                  std::vector<QueueResult> *results = nullptr);
    void receiveScreen();
    // A row of Receive's list, opened: its facts, and what can be done with
    // it. address is the console's, for the top screen.
    void receivedOptions(net::ReceiveLog::Row row, const std::string &address);

    // Software updates, in software_update.cpp.
    void softwareUpdateScreen();
    void showReleaseNotes(const update::Release &release);
    void installUpdate(const update::Release &release);
    // Runs every frame: notices when the background check finishes and
    // shows the notification once.
    void pollUpdate();

    // Compares the console clock with internet time and offers to set it.
    // Returns true when the clock was set.
    bool clockScreen();
    // utcNowSeconds < 0 when the real time is unknown.
    bool chooseTimeZone(int64_t utcNowSeconds);

    void record(HistoryEntry entry);
    void persistSettings();

    Settings settings_;
    std::vector<HistoryEntry> history_;
    int homeSelection_ = 2; // Scan QR Code
    std::string filesFolder_;
    // Copied or cut entries waiting to be pasted.
    std::vector<std::string> clipboard_;
    bool clipboardCut_ = false;
    library::Snapshot library_;
    library::Loader libraryLoader_;
    // Goes up each time library_ changes, so screens know to look again.
    uint32_t libraryGeneration_ = 0;
    // What Files has read of the SD card this session.
    fileindex::Index fileIndex_;
    int64_t sdFreeSeen_ = -1; // free space when the index was last checked
    std::atomic<bool> sdMayHaveChanged_ {false};
    aptHookCookie aptCookie_ {};
    uint64_t indexCard_ = 0; // platform::sdCardId when it was loaded
    Thread indexLoader_ = nullptr;
    std::unique_ptr<fileindex::Index> loadingIndex_;
    std::atomic<bool> indexLoaded_ {false};
    titles::Show softwareShow_ = titles::Show::All;
    titles::Show addOnShow_ = titles::Show::All;
    std::vector<QueuedLink> queue_;
    uint64_t nextQueueId_ = 1;
    // What Receive has taken in and sent out since 3dsDL started.
    net::ReceiveLog receiveLog_;
    bool updateAvailable_ = false;
    bool updateNoticeShown_ = false;
    uint32_t updateGeneration_ = 0;
    // Set when 3dsDL should close, e.g. to restart into an update.
    bool quit_ = false;
};
