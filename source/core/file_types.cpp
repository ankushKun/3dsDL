#include "file_types.hpp"

#include "naming.hpp"
#include "text.hpp"

#include <algorithm>

namespace filetype {
namespace {

template <size_t N>
bool oneOf(const std::string &value, const char *const (&options)[N])
{
    return std::any_of(std::begin(options), std::end(options),
                       [&](const char *option) { return value == option; });
}

const char *const TextExtensions[] = {
    "txt",  "md",   "markdown", "ini",  "cfg",  "conf", "config", "json",
    "xml",  "yml",  "yaml",     "toml", "log",  "csv",  "tsv",    "nfo",
    "lua",  "py",   "sh",       "bat",  "js",   "css",  "html",   "htm",
    "c",    "h",    "cpp",      "hpp",  "cc",   "rs",   "go",     "java",
    "properties", "rst", "diff", "patch", "srt", "ass", "lst",    "list",
    "gm9",  "pal",  "sfv",      "env",  "rsf",  "svg"};
const char *const ImageExtensions[] = {"png", "jpg", "jpeg", "bmp", "gif",
                                       "tga"};
const char *const AudioExtensions[] = {"mp3",  "ogg", "opus", "wav", "flac",
                                       "m4a",  "aac", "bcstm", "bcwav",
                                       "mod",  "xm",  "it",   "s3m"};
const char *const VideoExtensions[] = {"mp4", "mkv", "avi", "webm", "mov",
                                       "moflex", "3gp"};
const char *const ArchiveExtensions[] = {"zip", "7z",  "rar", "tar", "tgz",
                                         "tbz", "tbz2", "txz", "tzst"};
// Compressed tars by their last extension, after ".tar".
const char *const TarFilters[] = {"gz", "bz2", "xz", "lzma", "zst"};

} // namespace

Kind fromName(const std::string &name)
{
    const std::string extension = text::lower(naming::extension(name));
    const std::string bare =
        extension.empty() ? "" : extension.substr(extension.front() == '.');
    if (bare == "cia" || bare == "zcia") {
        return Kind::Cia;
    }
    if (bare == "tik") {
        return Kind::Ticket;
    }
    if (bare == "3dsx") {
        return Kind::Homebrew;
    }
    if (bare == "smdh" || bare == "icn") {
        return Kind::Smdh;
    }
    if (bare == "3ds" || bare == "cci") {
        return Kind::CartridgeImage;
    }
    if (bare == "nds" || bare == "dsi" || bare == "srl") {
        return Kind::Nds;
    }
    if (bare == "firm") {
        return Kind::Firm;
    }
    if (oneOf(bare, ArchiveExtensions) ||
        (oneOf(bare, TarFilters) &&
         text::endsWithNoCase(name.substr(0, name.size() - extension.size()),
                              ".tar"))) {
        return Kind::Archive;
    }
    if (oneOf(bare, ImageExtensions)) {
        return Kind::Image;
    }
    if (oneOf(bare, TextExtensions)) {
        return Kind::Text;
    }
    if (oneOf(bare, AudioExtensions)) {
        return Kind::Audio;
    }
    if (oneOf(bare, VideoExtensions)) {
        return Kind::Video;
    }
    return Kind::Other;
}

const char *label(Kind kind)
{
    switch (kind) {
    case Kind::Folder:
        return "Folder";
    case Kind::Cia:
        return "CIA installer";
    case Kind::Ticket:
        return "Ticket";
    case Kind::Homebrew:
        return "Homebrew app";
    case Kind::Smdh:
        return "Title icon (SMDH)";
    case Kind::CartridgeImage:
        return "3DS game image";
    case Kind::Nds:
        return "DS game";
    case Kind::Firm:
        return "Firmware (FIRM)";
    case Kind::Image:
        return "Picture";
    case Kind::Text:
        return "Text";
    case Kind::Archive:
        return "Archive";
    case Kind::Audio:
        return "Audio";
    case Kind::Video:
        return "Video";
    case Kind::Other:
        break;
    }
    return "File";
}

bool hasTitleMeta(Kind kind)
{
    return kind == Kind::Homebrew || kind == Kind::Smdh ||
           kind == Kind::CartridgeImage || kind == Kind::Nds ||
           kind == Kind::Ticket;
}

} // namespace filetype
