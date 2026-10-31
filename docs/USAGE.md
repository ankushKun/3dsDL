# Using 3dsDL

## Downloading

Pick **Scan QR Code** or **Enter URL** on the home screen, check the link and folder, and press **Download**. <kbd>B</kbd> cancels at any time. Closing the lid turns the screens off and the download keeps going; the console sleeps half a minute after it ends. If the connection drops, **Retry** continues from where it stopped, and History can resume it later, as long as the server confirms the file hasn't changed.

If a file with the same name is already there, you choose **Keep Both**, **Replace** or **Cancel**. When a CIA finishes, you can install it, install it and delete the file, or keep it; Settings can make that choice automatic. A CIA that doesn't fit on the SD card can be installed directly: it streams into the install service without being saved, and starts over if the connection drops.

Links have to point at the file itself (`http://` or `https://`). Download pages that need a browser, a login or a click won't work. To send a link from a computer, turn it into a QR code in the terminal and scan it off the screen:

```sh
qrencode -t ansiutf8 "https://example.com/some-homebrew.3dsx"
```

## Queue and Receive

**Add to Queue** on the confirm screen saves a link for later. From the scanner the camera comes straight back, so a page of codes goes quickly. **Queue** on the home screen downloads them all with **Download All**, keeping both files on a name clash and asking about CIAs once.

**Receive** listens for links and files while it's open. Scan its QR code with a phone on the same Wi-Fi, or type the address it shows into a browser. Most computers and iPhones can also open `http://3ds.local/` (`3ds-2.local` if another console already uses the name); some Android phones can't, which is why the QR code uses the address. Paste links into the page's **Send Links** and press **Send**. FBI's `servefiles.py`, `sendurls.py` and Boop take the same address, as they did with FBI. CIAs they send install without a copy being kept, or download instead if you choose.

A website can send a link as well, with `http://3ds.local/send?links=<link>` (the link URL-encoded), at most 20 links at once. Before it asks, the console checks where each link leads once its redirects are followed, and names that host. A link never leads back to the console, and one from another site never into your home network. Links from anywhere but 3dsDL's own page start on **Cancel**, since a site can send them without being asked. After you cancel a device's links, its next ones offer **Don't Allow**, which turns that device's links away until you leave Receive (its use of the files is asked about separately). Three more fields say where links go:

- `&folder=/3ds/<app>/` saves apps (`.3dsx`, `.cia`) and archives in `/3ds/` or `/cias/`, or one folder in them, instead of the Download Folder. Anything else the link turns out to be still goes to the Download Folder, and any other folder is refused.
- `&replace=1`, with a folder, asks whether a `.3dsx` or `.cia` replaces an older copy of the same app: the same title ID, or the same title and publisher. A different app with the same name keeps both, without asking.
- `&extract=1` on a link to an archive asks whether to extract it into one folder in `/3ds/`: the app in the archive's own `3ds` folder, its one top folder, or all of it in a folder of its name. A folder already there must hold the same app, and the question says what the archive adds to it. Nothing already there is overwritten, and a loose file goes into `/3ds/` itself only if it is a `.3dsx` or `.cia`; an app the archive updates is put beside the old one and gets the same question as `&replace=1`.

A file that ends up coming from another site than the one its link was checked to lead to goes to the Download Folder, without any of this. **Add to Queue** keeps only the folder.

Everything that comes in or goes out stays listed on the touch screen until 3dsDL closes, newest first: each file, each folder dropped on the page as one row, and each link, with how it went. Open a row to see where it was saved and who sent it, to install a CIA, or to show it in Files.

The page is also a file browser for the SD card, with no FTP app needed: the SD card's usual folders and Files' favourites on the left, the folder in the middle, and what is selected on the right (on a phone the left side slides in, and a bar at the bottom holds the actions). The first time a device opens a folder, the console asks whether to allow that device, and the answer lasts until you leave Receive. From the page you can:

- upload files or whole folders with **Upload** or by dropping them on the folder or on a folder in the sidebar, up to the 4 GiB a FAT32 card allows, with progress, speed and cancel for each file under **Transfers**;
- download files, open pictures and text in the browser, and edit text files in place;
- select several entries with Shift and Cmd or Ctrl clicks, a box drawn with the mouse, or a long press on a phone;
- move or copy them with **Move To** and **Copy To**, which pick the folder in columns, by dragging them onto a folder (hold Option, or Ctrl on Windows, to copy), or with cut, copy and paste on the keyboard. When names are already taken the page asks once: **Skip**, **Keep Both**, or **Replace**, which merges a folder into one of the same name;
- make folders, rename and delete, one entry or several at once;
- send CIAs to the console to install. With **Offer to install CIAs** ticked, the console asks once a batch of uploads finishes.

The page follows the console: the status under 3dsDL's name says when it is waiting for you to choose on the 3DS, installing or downloading, and how that ended. A CIA or homebrew shows its real icon, title, title ID and version. There are icon and list views, sorting by name, kind, size or date, search, hidden files, a picture viewer, keyboard shortcuts on a computer, and a warning before you change `boot.firm`, `luma` or `Nintendo 3DS`.

One transfer runs at a time, and the console shows it on the top screen. While the page is sending, reading or comparing files the console stays out of Sleep Mode, also with the lid closed and between the files of a batch, and sleeps half a minute after the last request. Uploads go to a hidden `.3dsdl-upload.part` file and only take their real name once every byte has arrived. From a computer, `curl` works too:

```sh
curl -T game.cia -H 'X-Requested-With: 3dsDL' 'http://192.168.1.20/api/file?path=/cia/game.cia'
```

## Files

Files browses the SD card. Tap a row to select it and tap again (or press <kbd>A</kbd>) to open it. Pictures fill the top screen, text and config files open in a line editor, ZIPs show their contents, CIAs (Azahar's compressed `.zcia` too) and tickets install, and anything else opens as hex.

<kbd>Y</kbd> opens the options for an entry (rename, copy, cut, delete, install, properties, SHA-256), <kbd>SELECT</kbd> or a tap on an entry's icon marks several entries, and <kbd>START</kbd> has the folder options (paste, new folder or file, hidden files). Once something is cut or copied, <kbd>Y</kbd> in any folder starts with Move Here or Paste Here. When names are already taken you answer once for all of them, or for each; a folder meeting a folder of the same name merges into it. <kbd>X</kbd> (or Sort at the right of the folder's path) sorts by name, date, size or type, in either direction. Folders always come before files, and sorting by size adds up each folder in the background. Properties adds up a folder or the marked items.

## Titles

Titles lists what's installed, like Data Management in System Settings.

- **Software** and **Updates and DLC** launch, delete, and fetch a game's seed. <kbd>X</kbd> sorts them by name or size, in either direction, and shows only the SD card or system memory (Software) or only updates or DLC.
- **Save Data** and **Extra Data** back up, restore and browse. Backups go in `sdmc:/3ds/Checkpoint/saves/` and `.../extdata/`, named the way Checkpoint names them, so either app restores the other's. A restore backs up the current data first.
- **Tickets** exports a title's ticket to `sdmc:/3ds/3dsDL/tickets/` or deletes unused ones. Install a `.tik` from Files.
- **Clean Up** removes unfinished installs, which stop a CIA from installing again, and **Seeds** imports a `seeddb.bin`.

System titles never appear, so they can't be deleted from here.

## Controls

| Where | Button | Action |
| --- | --- | --- |
| Everywhere | <kbd>A</kbd> / <kbd>B</kbd> | Choose / back |
| Home | <kbd>START</kbd> | Exit |
| Lists | <kbd>L</kbd> / <kbd>R</kbd> | Page up / down |
| Files | <kbd>X</kbd> / <kbd>Y</kbd> / <kbd>SELECT</kbd> / <kbd>START</kbd> | Sort / options / mark / folder options |
| Text editor | <kbd>A</kbd> / <kbd>X</kbd> / <kbd>Y</kbd> / <kbd>START</kbd> | Edit line / new line / delete line / save |
| Pictures | <kbd>L</kbd> / <kbd>R</kbd> | Previous / next picture |

Everything also works by touch, and lists scroll with a flick.

## Settings

| Setting | Default | |
| --- | --- | --- |
| Downloads → Resume Downloads | On | Keeps partial downloads to continue later |
| Downloads → Max Connections | 4 | The most connections one download is split across, 1 to 4; lower it for servers that refuse several |
| Downloads → Ask to Extract ZIPs | Off | Asks to extract a ZIP after it downloads |
| Downloads → After a CIA Downloads | Ask Each Time | Ask, install, install and delete, or keep |
| Updates → Check at Startup | On | Looks for a new 3dsDL when it opens |
| Other Settings → Sounds | On | Needs `sdmc:/3ds/dspfirm.cdc` |

**Console Clock** compares the console's time with internet time and sets it for your time zone. HTTPS fails when the date is wrong. Setting the clock needs the CIA build; Rosalina (<kbd>L</kbd> + <kbd>Down</kbd> + <kbd>SELECT</kbd>, Miscellaneous options) can also set it.

## Reporting a problem

**Settings → Report Issue** (or **About → Report Issue**) shows a QR code for each place to get help. Scan it with a phone:

- **Report a Bug** opens the [bug report form](https://github.com/ankushKun/3dsDL/issues/new?template=bug.yml) with your 3dsDL version, CIA or 3DSX, console model and system version already filled in.
- **Discord Server** is an [invite](https://discord.gg/axrWTjyAWG) to the developer's server, for questions, ideas and chatting with other users.
- **Developer on X** is [@ankushKun_](https://x.com/ankushKun_), for quick questions or if you don't use GitHub.

A good bug report says what you did, what happened instead, and the exact error on screen. Rosalina takes screenshots of both screens (<kbd>L</kbd> + <kbd>Down</kbd> + <kbd>SELECT</kbd>, Take screenshot) into `sdmc:/luma/screenshots/`. Reports of how 3dsDL runs on an Old 3DS or 2DS are welcome as a [console test report](https://github.com/ankushKun/3dsDL/issues/new?template=console-test.yml).

## Files 3dsDL keeps

| Path | |
| --- | --- |
| `sdmc:/3ds/3dsDL/settings.ini` | Settings |
| `sdmc:/3ds/3dsDL/history.txt` | The last 50 downloads |
| `sdmc:/3ds/3dsDL/seeds/<title id>.dat` | Seeds to use before asking Nintendo's server (FBI's `sdmc:/fbi/seed/` works too) |
| `<folder>/.3dsdl-<id>.part`, `.resume` | A partial download, removed once it finishes |

## Building

`./scripts/build.sh` builds in Docker (Docker Desktop, or Colima on macOS) with a digest-pinned devkitPro image and writes `3dsDL.3dsx`, `3dsDL.cia`, `3dsDL.smdh` and their checksums to `dist/`. The 3DSX is reproducible byte for byte; the CIA differs only in the IDs makerom randomises.

| Variable | Effect |
| --- | --- |
| `THREEDSDL_BUILD_JOBS=4` | More compiler jobs |
| `THREEDSDL_FLAT_BANNER=1` | Flat HOME Menu banner instead of the 3D one |
| `THREEDSDL_BUILD_IMAGE=...` | Another devkitPro image |
| `THREEDSDL_CIA_IMAGE=...` | Another CIA packaging image |
| `THREEDSDL_KEEP_COLIMA=1` | Leave Colima running |

Without Docker, install devkitARM, libctru, Citro2D, Citro3D, 3ds-curl and 3ds-mbedtls with devkitPro's pacman and run `make`.

The host tests (`make -C tests check`) need a C++17 compiler, libcurl and zlib headers, Python 3 and OpenSSL. They run under AddressSanitizer and UBSan against a local HTTP, HTTPS and NTP server.

### Versions and releases

`VERSION` holds a release, `0.8.1`, or a pre-release of one that ends in a number from 1 to 14, `0.8.1-beta.1` or `0.8.1-rc.3`. 3dsDL shows it as written. A CIA's version is a single number that the console only lets go up, the same number again included, so each build gets a build number made from its version (`major × 16384 + minor × 256 + patch × 16 + step`, where step is the pre-release's last number and 15 for the release). Every build installs over the one before it, so keep that last number rising across labels for one version: beta.1, beta.2, rc.3. `scripts/version.sh` prints the numbers for the current `VERSION`.

To publish, commit the new `VERSION` and push a matching tag (`v0.8.0-beta.1`). CI builds it, checks the tag against `VERSION`, and either publishes a GitHub pre-release straight away (testers install it from the release page) or, for a release, creates a draft to review and publish. The updater in 3dsDL only looks at the latest release, so it never offers a pre-release, and someone on `0.8.0-beta.3` is offered `0.8.0` as soon as it is out.

| Folder | |
| --- | --- |
| `source/app/` | 3DS code: screens, UI toolkit, scanner, workers |
| `source/core/` | Portable logic, tested on the host |
| `source/net/` | Download engine, fetch and the link receiver |
| `tests/` | Host tests and the fixture server |
| `site/` | The website |
