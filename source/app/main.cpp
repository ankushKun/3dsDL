#include "app.hpp"
#include "awake.hpp"
#include "crash.hpp"
#include "installer.hpp"
#include "logging.hpp"
#include "sound.hpp"
#include "ui.hpp"
#include "updater.hpp"

#include <3ds.h>
#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <string>

u32 __stacksize__ = 0x40000;

namespace {

constexpr size_t SocBufferSize = 0x100000;

std::string failure(const char *operation, Result result)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lX).", operation,
                  static_cast<unsigned long>(result));
    return buffer;
}

struct Services {
    u32 *socBuffer = nullptr;
    bool romfs = false;
    bool soc = false;
    bool sslc = false;
    bool ac = false;
    bool curl = false;

    bool start(std::string &error)
    {
        socBuffer = static_cast<u32 *>(memalign(0x1000, SocBufferSize));
        if (socBuffer == nullptr) {
            error = "Could not allocate the network buffer.";
            return false;
        }

        Result result = romfsInit();
        if (R_FAILED(result)) {
            error = failure("Opening the app's files", result);
            return false;
        }
        romfs = true;

        result = socInit(socBuffer, SocBufferSize);
        if (R_FAILED(result)) {
            error = failure("Starting the network service", result);
            return false;
        }
        soc = true;

        // devkitPro's Mbed TLS gathers its randomness through
        // sslcGenerateRandomData and never checks the result. Without an
        // open SSLC session every TLS connection would be keyed from
        // whatever happened to be on the stack.
        result = sslcInit(0);
        if (R_FAILED(result)) {
            error = failure("Starting the secure random source", result);
            return false;
        }
        sslc = true;

        // Only used for the Wi-Fi indicator, so failing here is not fatal.
        ac = R_SUCCEEDED(acInit());

        // Without AM, downloads still work; installing explains why not.
        installer::init();

        // Without the DSP firmware dump there are no sounds, which is fine.
        sound::init();

        awake::init();

        const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK) {
            error = std::string("Starting libcurl failed: ") +
                    curl_easy_strerror(code);
            return false;
        }
        curl = true;
        return true;
    }

    ~Services()
    {
        awake::exit();
        sound::exit();
        installer::exit();
        if (curl) {
            curl_global_cleanup();
        }
        if (ac) {
            acExit();
        }
        if (sslc) {
            sslcExit();
        }
        if (soc) {
            socExit();
        }
        if (romfs) {
            romfsExit();
        }
        std::free(socBuffer);
    }
};

} // namespace

int main(int argc, char **argv)
{
    // The Homebrew Launcher passes the .3dsx's path, which an update
    // replaces.
    updater::setLaunchPath(argc > 0 ? argv[0] : nullptr);
    crash::install(updater::runningAsCia());
    // Uses the New 3DS's faster clock and L2 cache when available. TLS and QR
    // decoding both benefit; on an Old 3DS this does nothing.
    osSetSpeedupEnable(true);
    gfxInitDefault();
    gfxSet3D(false);
    logging::start(updater::runningAsCia()); // after gfx, for linear memory

    if (!ui::init()) {
        gfxExit();
        return 1;
    }

    int status = 0;
    {
        Services services;
        std::string error;
        if (services.start(error)) {
            App app;
            app.run();
        } else {
            ui::message("Startup failed", error, ui::Tone::Error);
            status = 1;
        }
    }
    logging::finish();

    ui::shutdown();
    gfxExit();
    return status;
}
