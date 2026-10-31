# Roadmap

## 0.2: Hardware stabilization and new interface

- [x] Move camera DMA to linear memory and invalidate its CPU cache.
- [x] Replace the inherited console UI with Citro2D.
- [x] Add URL confirmation and file-conflict dialogs.
- [x] Add touch and button navigation.
- [ ] Verify camera orientation, colors, and QR decoding on hardware.
- [ ] Verify the interface on New and Old Nintendo 3DS hardware.

Both hardware checks moved into the 1.0 hardware pass, which now has a written
plan in [docs/HARDWARE.md](docs/HARDWARE.md).

## 0.3: Download quality of life

- [x] Show speed, ETA, transferred size, and available SD space.
- [x] Add download history, retry, destination presets, and collision prompts.
- [x] Prevent sleep during active downloads.
- [x] Add optional HTTP range resume.
- [x] Make archive extraction explicitly opt-in. ZIP extraction is off by
      default and always asks before touching anything.

0.3.0 also adds a console clock check that reads internet time over NTP and
sets the clock for a chosen time zone, because HTTPS fails when the date is
wrong. It also fixes the problems found while reviewing 0.2.0: the camera feed
could freeze for good, the vendored QR decoder could overrun a buffer on noisy
images, a finished download was deleted when its name was rejected, dialogs
ran off the screen and could not be dismissed by touch, and the conflict
prompt stalled the connection while it waited.

## 0.4: Faster downloads

- [x] Ask for larger socket buffers, which widens each connection's TCP
      window (0.3.2).
- [x] Split large downloads across up to four connections when the server
      accepts byte ranges, with resume that remembers which parts are done.
- [x] Run the download on the New 3DS's third core and redraw the progress
      screen less often.

The socket buffer size and the idea that the 3DS caps each connection come
from Colby Shores' measurements in
[Steveice10/FBI#4](https://github.com/Steveice10/FBI/pull/4).

## 0.5: Installing CIAs

- [x] Install a CIA after it downloads, or from Files, through the AM
      service, with INSTALL & DELETE, a version check before replacing a
      newer title, seeds for newer games, and LAUNCH afterwards.
- [x] A Files screen that lists files as well as folders, with CIA details.
- [ ] Hardware pass for installing (items 29 to 36 in
      [docs/HARDWARE.md](docs/HARDWARE.md)).

FBI is no longer maintained, so 3dsDL is taking over its job of installing
what it downloads. The longer plan, including a full file manager and title
management, is in [docs/INSTALLER-PLAN.md](docs/INSTALLER-PLAN.md).

## 0.6: A file manager and the System Look

- [x] Copy, cut, paste, rename and delete files and whole folders, with
      progress, cancel, and a choice when a name is already taken.
- [x] Mark several entries, new folder, new file, sort by name, size, date
      or type, and hidden files.
- [x] Picture viewer (PNG, JPEG, BMP, GIF, TGA), a line editor for text and
      config files, ZIP contents, a hex view, and icons and titles for
      `.3dsx`, `.3ds`, `.nds` and `.tik` files.
- [x] Install several CIAs at once, or every CIA in a folder.
- [x] Every screen redrawn in the style of the system applets (System
      Settings, Data Management), with touch scrolling that flicks. See
      [docs/DESIGN.md](docs/DESIGN.md).
- [ ] Hardware pass for Files (items 37 to 44 in
      [docs/HARDWARE.md](docs/HARDWARE.md)), and 60 frames a second on an
      Old 3DS with the new interface.

## 0.7: Software updates

- [x] Check GitHub for a newer release when 3dsDL opens, on a background
      thread, and show a notification over the top screen.
- [x] Settings, Updates: the installed and newest versions, the release
      notes, and Check at Startup.
- [x] Update in place, checked against the release's `3dsDL.sha256`: the
      CIA installs over itself and restarts, the `.3dsx` replaces the file
      the Homebrew Launcher started.
- [ ] Hardware pass for updating both builds on a console.

## 0.7: Titles and their data

- [x] Titles on Home, in the spirit of System Settings, Data Management:
      Software, Updates and DLC, Extra Data and Tickets, with icons and names
      read from the console. System titles never appear.
- [x] Launch, Get Seed, Delete, Delete with Ticket and Delete Extra Data for
      a title; Delete and Delete Unused for tickets.
- [x] Clean Up: unfinished installs that make reinstalls fail, and unused
      tickets.
- [x] Seeds from a seeddb.bin, only for installed games.
- [ ] Hardware pass (items 45 to 52 in [docs/HARDWARE.md](docs/HARDWARE.md)).
- System save data is left out on purpose: deleting it is the classic way
  to break a console, and Checkpoint covers saves.

## 0.7: Beyond FBI

- [x] Receive: a web page on the console for a phone or computer on the
      same Wi-Fi (QR code on the top screen), and FBI's remote install
      protocol on port 5000 for servefiles.py, sendurls.py and Boop.
- [x] Queue: Add to Queue on the confirm screen, scanning straight back to
      the camera, and Download All without questions in between.
- [x] Install Directly when a CIA does not fit on the SD card, streamed into
      the install service with nothing saved.
- [x] SHA-256 of any file in Files, checked against a .sha256 list next to
      it.
- [ ] Hardware pass (items 53 to 58 in [docs/HARDWARE.md](docs/HARDWARE.md)).

## 0.7: Saves, tickets and polish

- [x] Save data and extra data: back up, restore and look inside, in
      Checkpoint's folders and the way Checkpoint does it (names, the
      extra data ID table, clearing the secure value, skipping files the
      system holds open), so backups move freely between the two apps.
      A restore backs up what is there first.
- [x] Install .tik files from Files; export a title's ticket from Tickets.
- [x] History names downloads that never finished after the file the
      server named.
- [x] Interface sounds, made in code, with a switch in Other Settings.
- [ ] Hardware pass (items 59 to 66 in [docs/HARDWARE.md](docs/HARDWARE.md)).
- DSiWare saves, DS game cards and GBA Virtual Console saves are left to
  Checkpoint for now; they need different routes.

## 0.8: Files over Wi-Fi

- [x] A Files tab on the Receive page: browse the SD card, upload files and
      folders (streamed to the card, up to 4 GiB), download, open, edit
      text, make folders, rename, move and delete, without FTPd.
- [x] Each device has to be allowed on the console first; the API only
      answers its own page, so other websites cannot reach the card.
- [x] Send uploaded CIAs to the console to install.
- [x] Progress for each transfer on the touch screen, and no Sleep Mode
      while one runs.
- [x] Answer for 3ds.local over multicast DNS, with 3ds-2.local and so on
      when the name is taken.
- [x] The page follows the console (waiting for a choice, installing,
      the result), shows the real icons and details of CIAs and homebrew,
      and keeps uploads in view at the top of the list.
- [x] Pre-release versions (`0.8.1-beta.1`): CI publishes them as GitHub
      pre-releases, each build's CIA number is higher than the last so it
      installs over it, and the updater offers the release to anyone on a
      pre-release of it.
- [x] 0.8.1: Reinstall a CIA that is already installed, backing up its save
      first, instead of failing with 0xC8E083FC; 3dsDL never removes itself
      while it runs; and its own versions read as versions.
- [x] 0.8.2: Big folders open at once and fill in; Files and Titles sort
      both ways, with the order on the top screen; Delete Unused Tickets
      never touches installed software; Start opens a .3dsx; a crash leaves
      a log with symbols to read it.
- [x] 0.9.0: The 3ds.local page is a file browser with copy, merge and a
      code editor; Files has favourites, marks with a tap and asks once
      about names taken; 7z, RAR and tar archives; .zcia installs; work
      goes on with the lid closed; websites can send to a folder; log.txt
      for bug reports.
- [ ] Hardware pass (items 58a to 58f in [docs/HARDWARE.md](docs/HARDWARE.md)).
      Partly done on 2026-10-20: see the results there. Multicast works,
      so 3ds.local does.

## 1.0: Stable release

- [x] Host-side URL and filename tests.
- [x] Redirect, TLS, chunked-transfer, cancellation, and SD failure tests.
- [ ] New/Old 3DS hardware compatibility matrix. The checklist and matrix are
      ready in [docs/HARDWARE.md](docs/HARDWARE.md) and need runs on real
      consoles.
- [x] Reproducible CI builds, checksums, and release artifacts. The 3DSX is
      reproducible byte for byte. The CIA is too, apart from the ticket ID,
      title key and content ID that makerom randomises on every run.

## Ideas for after 1.0

- Receive in the background on the home screen, not only on Receive.
- Resume a direct install: AM cannot take data out of order, so it would
  need a server that resends from the last byte the install accepted.
