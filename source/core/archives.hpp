#pragma once

#include "archive_writer.hpp"
#include "zip.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Archives 3dsDL extracts. ZIP goes through 3dsDL's own reader; 7z, RAR,
// tar, compressed tars, and ZIPs packed with bzip2, LZMA, XZ or Zstandard go
// through libarchive. Both write through archives::Writer.
namespace archives {

struct Entry {
    std::string name; // as stored, made valid UTF-8
    std::string path; // safe relative path, "" when it would leave the folder
    uint64_t size = 0;
    bool directory = false;
    bool supported = true; // false: encrypted, a link, or unknown compression
};

struct Listing {
    std::string format; // "ZIP", "7z", "RAR", "tar.gz"
    uint64_t archiveSize = 0;
    // False after a quick read of an archive that cannot be listed without
    // decompressing all of it (compressed tars, solid RARs). entries is
    // empty then.
    bool complete = false;
    std::vector<Entry> entries;
    unsigned fileCount = 0;
    uint64_t totalSize = 0; // uncompressed bytes of all files
    // The one thing at the top of the archive, or "" when there are more.
    std::string top;
    bool topIsFolder = false;

    // Set when 3dsDL's own ZIP reader extracts it.
    bool ownZip = false;
    zip::Archive zip;
};

enum class Scan {
    Quick, // stops early when listing would decompress the whole archive
    Full,
};

// Reads what the archive holds. observer, when given, can stop a full scan
// and sees Progress::archiveBytes. Returns false with error empty when it
// was stopped.
bool read(const std::string &path, Scan scan, Listing &listing,
          std::string &error, Observer *observer = nullptr);

// The listing as Options::place would extract it, to show and to check
// for clashes: each entry where place puts it, the ones it leaves out gone,
// and the counts and top to match. It is not one to extract from.
Listing placed(const Listing &listing,
               const std::function<std::string(const std::string &path,
                                               bool directory)> &place);

// Relative paths of the listing's files already in folder. Reads each
// folder once instead of looking up every file. observer sees
// Progress::files and can stop it, which returns false.
bool existingFiles(const Listing &listing, const std::string &folder,
                   std::vector<std::string> &existing,
                   Observer *observer = nullptr);

// What the files take on the SD card, each rounded up to whole clusters,
// with a cluster for every folder.
uint64_t spaceNeeded(const Listing &listing, uint64_t clusterSize);

Result extract(const std::string &path, const Listing &listing,
               const std::string &folder, const Options &options,
               Observer &observer);

// Whether a RAR's header marks it solid, from its first bytes. Solid RARs
// decompress everything before an entry to reach it.
bool solidRar(const unsigned char *data, size_t size);

} // namespace archives
