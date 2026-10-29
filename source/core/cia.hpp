#pragma once

#include "smdh.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Reads what 3dsDL shows about a CIA installer before installing it: the
// title ID and version from its TMD, the name and icon from its meta section
// and whether the game needs a seed. Installing is the AM service's job (see
// app/installer); this file has no libctru dependency so the tests can run it.
namespace cia {

constexpr size_t HeaderSize = 0x2020;

// Where each section of the file starts. Sections follow each other in this
// order, each aligned to 64 bytes.
struct Layout {
    uint32_t certSize = 0;
    uint32_t ticketSize = 0;
    uint32_t tmdSize = 0;
    uint32_t metaSize = 0;
    uint64_t contentSize = 0;
    uint64_t certOffset = 0;
    uint64_t ticketOffset = 0;
    uint64_t tmdOffset = 0;
    uint64_t contentOffset = 0;
    uint64_t metaOffset = 0;
    uint64_t end = 0;
};

// True when the first bytes of a file look like a CIA header. size must be
// at least 0x10 to say yes.
bool looksLikeCia(const uint8_t *data, size_t size);
bool parseLayout(const uint8_t *header, size_t size, Layout &layout);

enum class Seed { NotNeeded, Needed, Unknown };

struct Info {
    uint64_t titleId = 0;
    uint16_t version = 0;
    uint64_t fileSize = 0; // the CIA's, inside a .zcia too
    bool hasSmdh = false;
    smdh::Info smdh;
    // Games from 2015 on can be encrypted with a seed the console has to
    // fetch. Unknown when the content is encrypted and its header unreadable.
    Seed seed = Seed::Unknown;
    // The contents' total, from the CIA's header.
    uint64_t contentSize = 0;
    // Read from a CIA compressed as Azahar's .zcia.
    bool compressed = false;
    // A quick read left out the name and icon at the CIA's end.
    bool smdhLeft = false;
};

// Reads size bytes at offset into buffer. False when that is not possible.
using Reader = std::function<bool(uint64_t offset, void *buffer, size_t size)>;

// fileSize may be 0 when unknown. Sets error and returns false when the data
// is not a CIA. Without withSmdh, the name and icon at its end are left out.
bool readInfo(const Reader &read, uint64_t fileSize, Info &info,
              std::string &error, bool withSmdh = true);
// A .cia, or a CIA compressed as Azahar's .zcia. Azahar's own .zcia files
// carry the name and icon up front; any other has them only at its end,
// which can take seconds to decompress, so a quick read leaves them out.
// Setting *stop ends the read early.
bool readInfo(const std::string &path, Info &info, std::string &error,
              bool quick = false, const std::atomic<bool> *stop = nullptr);
// Checks only the header, for listing files quickly.
bool isCiaFile(const std::string &path);

// Reads a CIA as it arrives, for installing it without saving the file.
// The title ID and version are known once the header and title metadata
// have come in; the name and icon once the meta section at the end has.
class Stream {
public:
    // Hand over the data in order, from offset 0. Returns false, with a
    // reason, when it is not a CIA or arrives out of order.
    bool feed(uint64_t offset, const uint8_t *data, size_t size,
              std::string &error);
    // The title ID and version are known.
    bool ready() const { return ready_; }
    // Everything fed until ready, to give to the installer first. Empty
    // afterwards.
    std::vector<uint8_t> takePrefix();
    // Title ID, version and seed flag once ready; the name and icon too
    // once the meta section has passed.
    const Info &info() const { return info_; }

private:
    std::vector<uint8_t> prefix_;
    std::vector<uint8_t> meta_;
    Layout layout_;
    bool haveLayout_ = false;
    uint64_t prefixNeeded_ = 0;
    bool ready_ = false;
    uint64_t next_ = 0;
    uint64_t metaStart_ = 0;
    size_t metaGot_ = 0;
    Info info_;
};

// --- Title IDs ---------------------------------------------------------------

std::string titleIdText(uint64_t titleId); // 16 upper-case hex digits
// "1.2.0 (v1056)"
std::string versionText(uint16_t version);

// 3dsDL's own title. From 0.8.1 on, its CIAs carry a build number instead of
// major.minor.micro, so that every build, pre-releases included, installs
// over the one before (see scripts/version.sh):
// major * 16384 + minor * 256 + patch * 16 + step, where step is the
// pre-release's last number (1 to 14) and 15 for the release.
constexpr uint64_t OwnTitleId = 0x0004000003D5D100ull;

// The build number for a version such as "0.8.1-beta.2", or -1 when the
// version has no build number (no last number from 1 to 14, or too big).
int buildNumber(const std::string &version);

// versionText, except for 3dsDL's own builds from 0.8.1 on: the version
// running, when the number is its own, and otherwise the version the number
// stands for, "0.8.1 (v2079)" or "0.8.1-pre.2 (v2066)". The label itself is
// not in the number. Numbers below 2048 are older 3dsDL releases, which used
// major.minor.micro.
std::string versionText(uint64_t titleId, uint16_t version,
                        const std::string &running = "");

enum class Kind { Application, Update, Dlc, Demo, DsiWare, DownloadPlay, System };
Kind kind(uint64_t titleId);
const char *kindName(Kind kind);

// System titles can brick a console when they go wrong, so 3dsDL does not
// install them.
bool isSystemTitle(uint64_t titleId);
// DSiWare, download play children and a few others belong in NAND, the way
// FBI decides it.
bool installsToNand(uint64_t titleId);
bool isNew3dsOnly(uint64_t titleId);
// The game an update or DLC belongs to, or titleId itself.
uint64_t baseTitle(uint64_t titleId);

} // namespace cia
