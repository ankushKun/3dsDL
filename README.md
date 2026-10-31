<p align="center">
  <img src="resources/banner-3dsdl-source.png" width="560" alt="3dsDL">
</p>

<p align="center">
  <b>Download files straight onto your Nintendo 3DS, from a link, a QR code or your phone.</b>
</p>

<p align="center">
  <a href="https://github.com/ankushKun/3dsDL/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/ankushKun/3dsDL?label=release&color=1c93c2"></a>
  <a href="https://github.com/ankushKun/3dsDL/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/ankushKun/3dsDL/total?color=3fa65a"></a>
  <a href="https://github.com/ankushKun/3dsDL/actions/workflows/ci.yml"><img alt="CI" src="https://img.shields.io/github/actions/workflow/status/ankushKun/3dsDL/ci.yml?branch=main&label=CI"></a>
  <a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-8c8466"></a>
</p>

<p align="center">
  <a href="https://ankush.one/3dsDL/">Website</a> &nbsp;·&nbsp;
  <a href="https://github.com/ankushKun/3dsDL/releases/latest">Download</a> &nbsp;·&nbsp;
  <a href="docs/USAGE.md">Guide</a> &nbsp;·&nbsp;
  <a href="ROADMAP.md">Roadmap</a>
</p>

3dsDL downloads files onto the SD card while the console stays on, with no card reader and no computer. Scan a QR code, type a link or send one from your phone, and it saves the file, installs it if it's a CIA, and lets you manage what's on the card. It's built to replace FBI, which is no longer maintained, and it looks and feels like the console's own system software.

<p align="center">
  <img src="docs/images/home.png" width="250" alt="The home screen">
  <img src="docs/images/downloading.png" width="250" alt="A download in progress">
  <img src="docs/images/install.png" width="250" alt="Installing a downloaded CIA">
  <br>
  <img src="docs/images/files.png" width="250" alt="Files, showing a CIA's details">
  <img src="docs/images/titles.png" width="250" alt="The Titles screen">
  <img src="docs/images/receive.png" width="250" alt="Receiving links from a phone">
</p>

## Features

- Downloads over verified HTTPS, split across up to four connections when the server allows it (fewer if you set a lower limit in Settings), with resume after a dropped connection.
- Installs CIAs and Azahar's compressed `.zcia` files with their seeds, or streams them straight into the install service when the SD card is too full.
- Receives links from a phone on the same Wi-Fi, from websites (straight into `/3ds/`, after asking), and from FBI's `servefiles.py`, `sendurls.py` and Boop.
- Manages the SD card from a phone or computer's browser: upload, download, edit, move, copy and delete files, and install uploaded CIAs, with no FTP app. Also answers at `http://3ds.local/`.
- A queue for scanning many QR codes in a row and downloading them together.
- Files: a file manager with favourites, a picture viewer, a text editor, ZIP extraction, a hex view and SHA-256 checks.
- Titles: launch and delete installed software, manage tickets and seeds, and clear stuck installs.
- Save data and extra data backup, restore and browsing, compatible with Checkpoint's backups.
- Updates itself from GitHub releases, checking each download against its SHA-256 checksum.

See the [guide](docs/USAGE.md) for how each part works, the controls and the settings.

## Install

You need a 3DS with custom firmware ([3ds.hacks.guide](https://3ds.hacks.guide/)) and Wi-Fi.

<p align="center">
  <img src="site/qr/install-cia.svg" width="180" alt="QR code for the latest 3dsDL.cia"><br>
  <sub>In FBI, choose Remote Install → Scan QR Code and scan this.</sub>
</p>

- **CIA:** scan the code above in FBI, or install `3dsDL.cia` from the [latest release](https://github.com/ankushKun/3dsDL/releases/latest).
- **3DSX:** copy `3dsDL.3dsx` to `sdmc:/3ds/3dsDL/` and open it from the Homebrew Launcher.

### Updating

You only install 3dsDL once. It checks GitHub for a new version when it opens and shows a notification when there is one. **Settings → Updates** downloads the right build, checks it against the release's SHA-256 checksum and installs it, without FBI or a computer. The one exception is the 0.7.0 CIA, which crashes at start-up: install the latest release with FBI once.

> [!NOTE]
> 3dsDL is in development. Version 0.9.0 runs on real consoles, and its Files over Wi-Fi page was tested on one from a Mac: uploads, downloads, installs and `3ds.local`. Save and extra data restore and ticket export are the least tested parts, so keep a Checkpoint backup of anything important for now. The full console test plan is in [docs/HARDWARE.md](docs/HARDWARE.md).

## Building

```sh
git clone https://github.com/ankushKun/3dsDL.git
cd 3dsDL
./scripts/build.sh        # Docker build; results in dist/
make -C tests check       # host tests
```

Build options, building without Docker and the project layout are in the [guide](docs/USAGE.md#building).

## Credits

- [Multidownload](https://github.com/hax0kartik/Multidownload) by hax0kartik, which 3dsDL grew out of.
- [FBI](https://github.com/Steveice10/FBI) by Steveice10, whose install sequence, seed handling and remote install protocol 3dsDL follows.
- [Checkpoint](https://github.com/BernardoGiordano/Checkpoint) by Bernardo Giordano, whose backup format and restore steps 3dsDL matches.
- [Colby Shores](https://github.com/colbyshores), for the socket buffer and TLS randomness findings.
- [libcurl](https://curl.se/), [Mbed TLS](https://github.com/Mbed-TLS/mbedtls), the [Mozilla CA bundle](https://curl.se/docs/caextract.html), [quirc](https://github.com/dlbeer/quirc) ([license](LICENSE-quirc.md)), [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library), [stb_image](https://github.com/nothings/stb), [libjpeg-turbo](https://libjpeg-turbo.org/), [CodeMirror](https://codemirror.net/) for the page's editor, and [devkitPro](https://devkitpro.org/) with libctru and Citro2D.

## License

GPLv3. Bundled components keep their own licenses.

<p align="center">
  <img src="resources/icon-3dsdl.png" width="40" alt=""><br>
  <sub>Made by <a href="https://ankush.one">ankushKun</a></sub>
</p>
