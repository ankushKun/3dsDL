#include "installer.hpp"

#include "cia.hpp"
#include "fs_util.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace installer {
namespace {

// Big enough that the SD card and AM each see few, large requests.
constexpr size_t ChunkSize = 512 * 1024;
constexpr size_t SeedSize = 16;
// AM's "invalid or outdated title version" (0xD8E08027), and its "this
// version is already installed" (0xC8E083FC).
constexpr int VersionRefused = 39;
constexpr int SameVersion = 1020;

// Nintendo's seed server presents a certificate from Nintendo's own CA,
// which is not in the Mozilla bundle. Its public key is pinned instead;
// the certificate is valid until 2038.
constexpr const char *SeedServerKey =
    "sha256//G2ME0Jc6NyYHoLAj46YE32ygTCPG6xPvZxQhtXn3Eqs=";

bool started = false;
::Result startResult = 0;

std::string hexCode(::Result code)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                  static_cast<unsigned long>(code));
    return buffer;
}

// FS_Path wants UTF-16 without the "sdmc:" device name.
class SdPath {
public:
    explicit SdPath(const std::string &path)
    {
        std::string local = path;
        if (local.compare(0, 5, "sdmc:") == 0) {
            local.erase(0, 5);
        }
        units_.assign(local.size() + 1, 0);
        const ssize_t count = utf8_to_utf16(
            units_.data(), reinterpret_cast<const uint8_t *>(local.c_str()),
            local.size());
        valid_ = count >= 0;
        units_.resize(valid_ ? static_cast<size_t>(count) + 1 : 1);
        units_.back() = 0;
    }

    bool valid() const { return valid_; }
    FS_Path path() const
    {
        return {PATH_UTF16, static_cast<u32>(units_.size() * 2),
                units_.data()};
    }

private:
    std::vector<uint16_t> units_;
    bool valid_ = false;
};

class FileHandle {
public:
    FileHandle() = default;
    ~FileHandle()
    {
        if (handle_ != 0) {
            FSFILE_Close(handle_);
        }
    }
    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;

    ::Result open(const std::string &path)
    {
        const SdPath local(path);
        if (!local.valid()) {
            return MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_APPLICATION,
                              RD_INVALID_COMBINATION);
        }
        return FSUSER_OpenFileDirectly(&handle_, ARCHIVE_SDMC,
                                       fsMakePath(PATH_EMPTY, ""),
                                       local.path(), FS_OPEN_READ, 0);
    }
    Handle get() const { return handle_; }

private:
    Handle handle_ = 0;
};

bool isVersionRefusal(::Result code)
{
    return (R_MODULE(code) == RM_AM || R_MODULE(code) == RM_AM_LOW) &&
           (R_DESCRIPTION(code) == VersionRefused ||
            R_DESCRIPTION(code) == SameVersion);
}

Result failure(::Result code, const std::string &what)
{
    Result result;
    result.code = code;
    result.versionRefused = isVersionRefusal(code);
    result.message = what + " " + describe(code);
    return result;
}

// Opens the CIA inside a .zcia. False, leaving reader alone, for a plain
// CIA; false with error set when the .zcia cannot be read.
bool openCompressed(Handle file, u64 size, z3ds::Reader &reader,
                    std::string &error)
{
    uint8_t header[z3ds::HeaderSize];
    u32 read = 0;
    z3ds::Header parsed;
    if (size < sizeof(header) ||
        R_FAILED(FSFILE_Read(file, &read, 0, header, sizeof(header))) ||
        read != sizeof(header) ||
        !z3ds::parseHeader(header, sizeof(header), parsed)) {
        return false;
    }
    if (!z3ds::holdsCia(parsed)) {
        error = "This compressed file does not hold a CIA.";
        return false;
    }
    const auto source = [file](uint64_t offset, void *buffer, size_t count) {
        u32 got = 0;
        return R_SUCCEEDED(FSFILE_Read(file, &got, offset, buffer,
                                       static_cast<u32>(count))) &&
               got == count;
    };
    return reader.open(source, size, error);
}

// FBI's FSUSER_AddSeed, which libctru does not wrap.
::Result addSeed(uint64_t titleId, const uint8_t *seed)
{
    u32 *command = getThreadCommandBuffer();
    command[0] = IPC_MakeHeader(0x87a, 6, 0);
    command[1] = static_cast<u32>(titleId);
    command[2] = static_cast<u32>(titleId >> 32);
    std::memcpy(&command[3], seed, SeedSize);
    const ::Result sent = svcSendSyncRequest(*fsGetSessionHandle());
    return R_FAILED(sent) ? sent : static_cast<::Result>(command[1]);
}

bool readSeedFile(const std::string &path, uint8_t *seed)
{
    std::string contents;
    if (!fs::readFile(path, contents) || contents.size() != SeedSize) {
        return false;
    }
    std::memcpy(seed, contents.data(), SeedSize);
    return true;
}

// The country code Nintendo's server expects for the console's region.
const char *seedCountry()
{
    static const char *const Countries[] = {"JP", "US", "GB", "GB",
                                            "HK", "KR", "TW"};
    u8 region = 1;
    if (R_SUCCEEDED(cfguInit())) {
        if (R_FAILED(CFGU_SecureInfoGetRegion(&region))) {
            region = 1;
        }
        cfguExit();
    }
    return region < 7 ? Countries[region] : "US";
}

size_t collect(char *data, size_t size, size_t count, void *output)
{
    std::string &body = *static_cast<std::string *>(output);
    if (body.size() + size * count > 64) {
        return 0; // a seed is 16 bytes; anything this big is not one
    }
    body.append(data, size * count);
    return size * count;
}

// Ends a transfer once stop is set.
int stopWhenAsked(void *stop, curl_off_t, curl_off_t, curl_off_t,
                  curl_off_t)
{
    return static_cast<const std::atomic<bool> *>(stop)->load() ? 1 : 0;
}

bool downloadSeed(uint64_t titleId, uint8_t *seed, std::string &message,
                  const std::atomic<bool> *stop)
{
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        message = "Could not start the seed download.";
        return false;
    }
    const std::string link =
        "https://kagiya-ctr.cdn.nintendo.net/title/0x" +
        cia::titleIdText(titleId) + "/ext_key?country=" + seedCountry();
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, link.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, SeedServerKey);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    if (stop != nullptr) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stopWhenAsked);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA,
                         const_cast<std::atomic<bool> *>(stop));
    }
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (code == CURLE_ABORTED_BY_CALLBACK) {
        message = "The seed lookup was stopped.";
        return false;
    }
    if (code != CURLE_OK) {
        message = std::string("Could not reach Nintendo's seed server: ") +
                  curl_easy_strerror(code) + ".";
        return false;
    }
    if (status == 404) {
        message = "Nintendo's server has no seed for this title.";
        return false;
    }
    if (status != 200 || body.size() != SeedSize) {
        message = "Nintendo's seed server answered with HTTP " +
                  std::to_string(status) + ".";
        return false;
    }
    std::memcpy(seed, body.data(), SeedSize);
    return true;
}

// Creates the SD card's title database when AM says there is none, as on
// a card that has never had a title installed, right before an install
// needs it, the way FBI does. Never at startup: a database that exists but
// is not available yet must not be replaced by an empty one.
void prepareTitleDatabase(FS_MediaType media)
{
    bool available = true;
    if (media == MEDIATYPE_SD &&
        R_SUCCEEDED(AM_QueryAvailableExternalTitleDatabase(&available)) &&
        !available) {
        AM_InitializeExternalTitleDatabase(false);
    }
}

} // namespace

void init()
{
    startResult = amInit();
    started = R_SUCCEEDED(startResult);

}

void exit()
{
    if (started) {
        amExit();
        started = false;
    }
}

bool available()
{
    return started;
}

std::string unavailableReason()
{
    return "3dsDL could not open the install service (" +
           hexCode(startResult) +
           "). Run the CIA version of 3dsDL, or the .3dsx from the Homebrew "
           "Launcher with Luma3DS.";
}

FS_MediaType destination(uint64_t titleId)
{
    return cia::installsToNand(titleId) ? MEDIATYPE_NAND : MEDIATYPE_SD;
}

Result installCia(const std::string &path, uint64_t titleId,
                  Observer &observer)
{
    if (!started) {
        Result result;
        result.message = unavailableReason();
        return result;
    }

    FileHandle file;
    ::Result code = file.open(path);
    if (R_FAILED(code)) {
        return failure(code, "Could not open the CIA file.");
    }
    u64 size = 0;
    code = FSFILE_GetSize(file.get(), &size);
    if (R_FAILED(code)) {
        return failure(code, "Could not read the CIA file.");
    }
    // A .zcia is decompressed on its way to AM.
    z3ds::Reader compressed;
    std::string error;
    const bool isCompressed =
        openCompressed(file.get(), size, compressed, error);
    if (!error.empty()) {
        Result result;
        result.message = error;
        return result;
    }
    if (isCompressed) {
        size = compressed.size();
    }

    std::unique_ptr<uint8_t, decltype(&std::free)> buffer(
        static_cast<uint8_t *>(std::malloc(ChunkSize)), &std::free);
    if (!buffer) {
        Result result;
        result.message = "Not enough memory to install.";
        return result;
    }

    const u64 startTime = osGetTime();
    Handle target = 0;
    prepareTitleDatabase(destination(titleId));
    code = AM_StartCiaInstall(destination(titleId), &target);
    if (R_FAILED(code)) {
        return failure(code, "The install could not start.");
    }

    observer.onProgress(0, size);
    u64 offset = 0;
    while (offset < size) {
        if (observer.cancelRequested()) {
            AM_CancelCIAInstall(target);
            Result result;
            result.outcome = Outcome::Cancelled;
            result.message = "The install was cancelled. Nothing was "
                             "changed.";
            return result;
        }
        const u32 wanted = static_cast<u32>(
            std::min<u64>(ChunkSize, size - offset));
        u32 read = 0;
        if (isCompressed) {
            if (!compressed.read(offset, buffer.get(), wanted, error)) {
                AM_CancelCIAInstall(target);
                Result result;
                result.message = error;
                result.compressed = true;
                return result;
            }
            read = wanted;
        } else {
            code = FSFILE_Read(file.get(), &read, offset, buffer.get(), wanted);
            if (R_SUCCEEDED(code) && read != wanted) {
                code = MAKERESULT(RL_PERMANENT, RS_INVALIDSTATE,
                                  RM_APPLICATION, RD_NO_DATA);
            }
            if (R_FAILED(code)) {
                AM_CancelCIAInstall(target);
                return failure(code, "Reading the CIA file failed.");
            }
        }
        u32 written = 0;
        code = FSFILE_Write(target, &written, offset, buffer.get(), read, 0);
        if (R_FAILED(code)) {
            AM_CancelCIAInstall(target);
            return failure(code, "The install failed.");
        }
        offset += read;
        observer.onProgress(offset, size);
    }

    code = AM_FinishCiaInstall(target);
    if (R_FAILED(code)) {
        return failure(code, "The install could not be finished.");
    }

    Result result;
    result.outcome = Outcome::Installed;
    result.bytes = size;
    result.seconds = static_cast<double>(osGetTime() - startTime) / 1000.0;
    result.compressed = isCompressed;
    return result;
}

StreamInstall::~StreamInstall()
{
    cancel();
}

bool StreamInstall::begin()
{
    const cia::Info &info = stream_.info();
    if (cia::isSystemTitle(info.titleId)) {
        result_.message = "This is a system title. 3dsDL does not install "
                          "them.";
        return false;
    }
    startTime_ = osGetTime();
    prepareTitleDatabase(destination(info.titleId));
    const ::Result code = AM_StartCiaInstall(destination(info.titleId), &target_);
    if (R_FAILED(code)) {
        result_ = failure(code, "The install could not start.");
        return false;
    }
    started_ = true;
    const std::vector<uint8_t> prefix = stream_.takePrefix();
    u32 done = 0;
    const ::Result wrote = FSFILE_Write(target_, &done, 0, prefix.data(),
                                        static_cast<u32>(prefix.size()), 0);
    if (R_FAILED(wrote)) {
        result_ = failure(wrote, "The install failed.");
        cancel();
        return false;
    }
    written_ = prefix.size();
    return true;
}

bool StreamInstall::write(uint64_t offset, const void *data, size_t size)
{
    if (!started_ && target_ == 0 && !result_.message.empty()) {
        return false; // already failed
    }
    if (!available()) {
        result_.message = unavailableReason();
        return false;
    }
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    if (sniffed_) {
        return pass(offset, bytes, size);
    }
    // The first four bytes tell a .zcia from a CIA.
    constexpr size_t MagicSize = 4;
    if (offset != sniff_.size()) {
        result_.message = "The download arrived out of order.";
        return false;
    }
    const size_t used = std::min(size, MagicSize - sniff_.size());
    sniff_.insert(sniff_.end(), bytes, bytes + used);
    if (sniff_.size() < MagicSize) {
        return true;
    }
    sniffed_ = true;
    compressed_ = std::memcmp(sniff_.data(), "Z3DS", MagicSize) == 0;
    const std::vector<uint8_t> first = std::move(sniff_);
    sniff_.clear();
    return pass(0, first.data(), first.size()) &&
           (used == size || pass(first.size(), bytes + used, size - used));
}

// Hands on what arrived, decompressing a .zcia's bytes first.
bool StreamInstall::pass(uint64_t offset, const uint8_t *data, size_t size)
{
    if (!compressed_) {
        return take(offset, data, size);
    }
    if (offset != fed_) {
        result_.message = "The download arrived out of order.";
        cancel();
        return false;
    }
    fed_ += size;
    std::string error;
    const auto out = [this](const uint8_t *bytes, size_t count) {
        const bool ok = take(unpacked_, bytes, count);
        unpacked_ += count;
        return ok;
    };
    if (unpack_.feed(data, size, out, error)) {
        return true;
    }
    if (!error.empty()) { // otherwise take() has said why
        result_.message = error;
        cancel();
    }
    return false;
}

// Gives the CIA's bytes to AM, once its title ID is known.
bool StreamInstall::take(uint64_t offset, const uint8_t *data, size_t size)
{
    std::string error;
    if (!stream_.feed(offset, data, size, error)) {
        result_.message = error;
        cancel();
        return false;
    }
    if (!started_) {
        return !stream_.ready() || begin();
    }
    if (offset < written_) {
        return true; // already handed over as part of the prefix
    }
    u32 done = 0;
    const ::Result code = FSFILE_Write(target_, &done, offset, data,
                                       static_cast<u32>(size), 0);
    if (R_FAILED(code)) {
        result_ = failure(code, "The install failed.");
        cancel();
        return false;
    }
    written_ = offset + size;
    return true;
}

Result StreamInstall::finish()
{
    if (!started_) {
        if (result_.message.empty()) {
            result_.message = "The download ended before the CIA's header "
                              "arrived.";
        }
        return result_;
    }
    if (compressed_ && !unpack_.done()) {
        cancel();
        result_ = Result();
        result_.message = "The download ended before the whole CIA arrived.";
        return result_;
    }
    const ::Result code = AM_FinishCiaInstall(target_);
    started_ = false;
    target_ = 0;
    if (R_FAILED(code)) {
        result_ = failure(code, "The install could not be finished.");
        return result_;
    }
    result_ = Result();
    result_.outcome = Outcome::Installed;
    result_.bytes = written_;
    result_.seconds = static_cast<double>(osGetTime() - startTime_) / 1000.0;
    result_.compressed = compressed_;
    return result_;
}

void StreamInstall::cancel()
{
    if (started_) {
        AM_CancelCIAInstall(target_);
        started_ = false;
        target_ = 0;
    }
}

bool installedVersion(uint64_t titleId, uint16_t &version)
{
    if (!started) {
        return false;
    }
    u64 id = titleId;
    AM_TitleInfo info {};
    if (R_FAILED(AM_GetTitleInfo(destination(titleId), 1, &id, &info))) {
        return false;
    }
    version = info.version;
    return true;
}

bool removeTitle(uint64_t titleId, std::string &error)
{
    if (cia::isSystemTitle(titleId)) {
        error = "System titles are never removed.";
        return false;
    }
    const ::Result code = AM_DeleteTitle(destination(titleId), titleId);
    if (R_FAILED(code)) {
        error = "Could not remove the installed title. " + describe(code);
        return false;
    }
    return true;
}

int64_t requiredSpace(const std::string &path, const cia::Info &info)
{
    if (info.compressed) {
        return static_cast<int64_t>(info.contentSize);
    }
    if (!started) {
        return -1;
    }
    FileHandle file;
    u64 space = 0;
    if (R_FAILED(file.open(path)) ||
        R_FAILED(AM_GetCiaRequiredSpace(&space, destination(info.titleId),
                                        file.get()))) {
        return -1;
    }
    return static_cast<int64_t>(space);
}

bool installSeed(uint64_t titleId, std::string &message,
                 const std::atomic<bool> *stop)
{
    const std::string name = cia::titleIdText(titleId) + ".dat";
    uint8_t seed[SeedSize];
    std::string source;
    if (readSeedFile("sdmc:/3ds/3dsDL/seeds/" + name, seed) ||
        readSeedFile("sdmc:/fbi/seed/" + name, seed)) {
        source = "from the SD card";
    } else if (downloadSeed(titleId, seed, message, stop)) {
        source = "from Nintendo";
    } else {
        return false;
    }

    const ::Result code = addSeed(titleId, seed);
    if (R_FAILED(code)) {
        message = "The seed could not be added. " + describe(code);
        return false;
    }
    message = "Added the game's seed " + source + ".";
    return true;
}

bool addSeed(uint64_t titleId, const uint8_t *seed, std::string &error)
{
    const ::Result code = addSeed(titleId, seed);
    if (R_FAILED(code)) {
        error = "The seed could not be added. " + describe(code);
        return false;
    }
    return true;
}

bool launch(uint64_t titleId, std::string &error)
{
    // The jump carries no parameters; both buffers stay zero.
    static u8 parameters[0x300];
    static u8 hmac[0x20];
    ::Result code = APT_PrepareToDoApplicationJump(
        0, titleId, static_cast<u8>(destination(titleId)));
    if (R_SUCCEEDED(code)) {
        code = APT_DoApplicationJump(parameters, sizeof(parameters), hmac);
    }
    if (R_FAILED(code)) {
        error = "Could not start the title. " + describe(code);
        return false;
    }
    return true;
}

std::string describe(::Result code)
{
    const int module = R_MODULE(code);
    const int description = R_DESCRIPTION(code);
    std::string text;
    if (module == RM_AM || module == RM_AM_LOW) {
        switch (description) {
        case 4:
            text = "The install service was busy with something else. "
                   "Restart the console and try again.";
            break;
        case 37:
            text = "The CIA's contents are damaged.";
            break;
        case 39:
            text = "The install service refused this version. The same or "
                   "a newer version may already be installed.";
            break;
        case 43:
            text = "The SD card's title database is missing.";
            break;
        case 44:
            text = "System titles cannot be removed.";
            break;
        case 53:
            text = "There is not enough space left in the DSiWare area of "
                   "the system memory.";
            break;
        case SameVersion:
            text = "This version is already installed.";
            break;
        case 106:
            text = "The CIA failed a signature or hash check. The file is "
                   "probably damaged; download it again.";
            break;
        default:
            break;
        }
    } else if (module == RM_FS && description == RD_NOT_FOUND) {
        text = "The file was not found.";
    }
    if (text.empty()) {
        text = "Error " + hexCode(code) + ".";
    } else {
        text += " (" + hexCode(code) + ")";
    }
    return text;
}

} // namespace installer
