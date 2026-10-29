# Hardware test plan

The host tests cover the download engine, file naming, ZIP extraction and QR
decoding, but nothing in them touches the camera, the GPU, the software
keyboard, Wi-Fi or sleep mode. This checklist covers those parts on real
consoles. Run it on at least one Old and one New 3DS before tagging 1.0, and
record each run in the matrix at the bottom.

Use both builds when you can: `3dsDL.3dsx` from the Homebrew Launcher and
`3dsDL.cia` from the HOME Menu. They differ in memory layout, service access
and (on New 3DS) CPU speed.

## Preparation

- A computer on the same network running the fixture server:
  `python3 tests/fixtures/server.py --cert cert.pem --key key.pem --ready ports.json`
  (see `tests/run_integration.sh` for how to make a throwaway certificate).
  Its `/file/<size>` endpoint takes the query options listed at the top of
  `server.py`, which makes drops, stalls and slow transfers easy to trigger.
- A few QR codes, for example made with
  `qrencode -t ansiutf8 "https://…"` and shown on a monitor:
  a short `https://` link, a long link (around 200 characters), an upper-case
  `HTTPS://EXAMPLE.COM/FILE.ZIP` link, and a QR code that is not a link
  (a Wi-Fi code or plain text).
- A small ZIP with a folder inside, and a large file (at least 50 MiB).

## Checklist

### Start-up and home screen

1. The app starts without a "Startup failed" dialog.
2. The top screen shows the download folder, the free space and the Wi-Fi
   state. Turning Wi-Fi off in the HOME Menu settings changes it within a
   couple of seconds of returning.
3. D-Pad, Circle Pad and touch all move between and open the six buttons.
   START exits.
4. Settings from 0.2 (`sdmc:/3dsDL.cfg`) are migrated to
   `sdmc:/3ds/3dsDL/settings.ini` and the old file is removed.

### QR scanner

5. The preview is upright, not mirrored, with natural colours, and keeps
   updating for at least two minutes (watch for it freezing).
6. Each test code is read. The non-link code shows "That QR code does not
   hold a web link" and scanning continues.
7. Close the lid while scanning, open it again: the preview recovers, or a
   clear "camera stopped" error appears (no hang).
8. Press HOME while scanning, return: the preview recovers.
9. B, START and the CANCEL button all leave the scanner.
10. Scan, cancel, then open ENTER URL: the keyboard works and nothing on
    screen is corrupted afterwards.

### Downloads

11. The confirm screen shows the host, the full link (wrapped), the folder and
    the http warning for plain-http links.
12. A large download (several MiB, from GitHub for example) shows speed, time
    left, size and percentage, and its status says "4 connections". The
    finished message gives the average speed; write it in the matrix. Then
    check the file is intact: a CIA installs with FBI, a video plays to the
    end.
13. Cancelling asks first; "KEEP GOING" continues, "STOP" removes the partial
    file.
14. Close the lid during a download: the screens go dark within a quarter
    second and the download keeps going. Open it: the screens light again.
    Close it once more and wait past the end: the console sleeps about 30 s
    after the download ends (log.txt: lid.closed, lid.opened, lid.sleep).
    Run a queue of three with the lid closed: all three finish.
15. Use `drop_at` on the fixture server with a file under 1 MiB, which uses
    one connection: the failure says the connection dropped and RETRY
    continues from the partial size ("Continuing from…"). With a larger file
    the dropped connection is reopened and the download simply finishes.
16. Press HOME during a download, then close 3dsDL from the HOME Menu. Start
    it again: History lists the download as resumable and RESUME continues it.
    Try it once with a large download split across connections too, and
    check the finished file as in item 12.
17. Download the same file twice: RENAME creates `name (1).ext`, OVERWRITE
    replaces it, CANCEL keeps the old file.
18. With Wi-Fi off, starting a download warns first.
19. Set the console clock to 2010, then download over HTTPS: the error says
    the certificate is not valid yet, shows the console's time and offers
    CHECK CLOCK.
20. Settings → Console clock shows the console time, the actual time and the
    drift. TIME ZONE previews the local time for each zone. SET CLOCK sets it
    (CIA build) or explains the Rosalina alternative when refused (3dsx).
    Afterwards the HOME Menu clock matches and HTTPS works again.

### Folders, history, settings, ZIP

21. Presets create their folder when it is missing. The browser can open
    folders, go up, make a new folder (X) and choose a folder (Y).
22. Typing a path outside `sdmc:/` is refused; a missing folder offers to be
    created.
23. History shows saved, failed and cancelled entries with the right details;
    REMOVE and "Clear all" work and survive a restart.
24. With "Offer to extract ZIP files" on, a ZIP download offers HERE / NEW
    FOLDER / NO, asks about existing files, shows progress, and DELETE ZIP
    removes the archive. With it off, nothing is offered.

### HOME Menu banner and icon

Release builds use the animated 3D banner (see `resources/banner/README.md`).
A bad one crashes or freezes the HOME Menu, so check these on a console
whenever the banner changes. `THREEDSDL_FLAT_BANNER=1` builds with the flat
picture instead.

25. After installing the CIA, the HOME Menu shows 3dsDL's icon (a cyan card
    with a yellow arrow). The Homebrew Launcher shows the same icon for the
    `.3dsx`.
26. Selecting 3dsDL plays the jingle once and shows the banner: the arrow
    drops into the card, the card squashes, sparkles burst and the wordmark
    bobs. It loops without a jump, and nothing is black or flickers.
27. With the 3D slider up, the wordmark and sparkles stand in front of the
    screen and the card sits behind them.
28. Moving the cursor on and off 3dsDL several times, and opening and closing
    the app, never crashes or freezes the HOME Menu. If it does, hold POWER
    to switch off if needed, then open FBI without highlighting 3dsDL and
    install a flat banner build (`THREEDSDL_FLAT_BANNER=1`) over it. If the HOME Menu starts
    with 3dsDL already highlighted and crashes again, starting once without
    the SD card should move the cursor elsewhere.

### Installing CIAs

These need a few CIAs: a small homebrew app (3dsFM works), a retail game
from 2015 or later that uses a seed, an update and a DLC for a game that is
installed, and a game older than the installed version.

29. Downloading a `.cia` offers INSTALL, INSTALL & DELETE and KEEP FILE with
    the icon, name, version and installed version. INSTALL installs it and
    the title appears on the HOME Menu; INSTALL & DELETE also removes the
    file. The "After downloading a CIA" setting skips the question.
30. Files lists folders and files, shows a CIA's details on the top screen and
    installs or deletes it. B goes up and lands on the folder just left.
31. A game that needs a seed starts after installing (the success message says
    where the seed came from). Try it once with
    `sdmc:/3ds/3dsDL/seeds/<title id>.dat` and once from Nintendo's server.
32. Installing the same version again works, or asks to replace it. Note
    which one, and whether the save survives REPLACE. An older version asks
    first and warns about save data. A CIA that shares an installed
    title's ID under another name, like a ROM hack, asks to replace the
    installed one by name, showing both names.
33. An update and a DLC install over their game, and LAUNCH opens the game.
34. An install has no Cancel, B does nothing and HOME is held back until it
    ends. Stopping one used to leave the SD card unreadable until a restart
    (issue #15). A direct install keeps Cancel until the CIA's header has
    arrived, then loses it.
35. A CIA bigger than the free space is refused before anything is written.
36. LAUNCH after installing starts the title, from both the `.3dsx` and the
    CIA build. Installing 3dsDL's own CIA over itself from the CIA build works.
36a. A `.zcia` made by Azahar (`azahar -c game.cia`, or its Compress menu)
    shows "ZCIA" with its icon and name in Files and installs from there,
    from the Receive page and after downloading it, and the game starts.
    Note how long a game of 1 GiB or more takes next to its `.cia`.

### Files

Use a folder with a few of each: big phone photos (JPEG over 12 MP), a PNG
with transparency, a GIF, `.ini` and `.txt` files with Windows line endings,
a ZIP, a `.3dsx`, a decrypted and an encrypted `.3ds`, an `.nds` and a folder
with a few hundred files.

37. Scrolling a folder of a few hundred files stays smooth on an Old 3DS, and
    the top screen keeps up with the highlighted entry.
38. Pictures open, fit the screen, and L/R browse the folder. A 12 MP photo
    opens in a few seconds without running out of memory, on Old and New.
39. Editing a line of an `.ini` and saving changes only that line (compare
    the file on a computer); a new file gets a line break at the end. B with
    unsaved changes asks first. The keyboard shows the line's current text.
40. Copy and paste a folder of a few hundred MiB: progress moves, cancel
    stops it and leaves the finished files, and the copy matches the
    original. Paste into the same folder makes "name (1)".
41. Cut and paste moves instantly, also between folders, and never into
    itself. Something that could not move stays ready to paste.
42. Delete a folder with files inside: it says how much is inside first, then
    removes everything.
43. Rename, including changing only the case of a name (a.txt to A.txt).
44. Mark several CIAs and install them: each installs, and the summary counts
    them.

### Software updates and Titles

45. With an older build installed, 3dsDL shows the Update Available
    notification on Home and the New badges. Settings, Updates installs the
    new CIA over itself and restarts into it; with the .3dsx it replaces the
    file and closes, and the Homebrew Launcher starts the new version.
46. Titles lists every installed game, update and DLC with the right icon,
    name, version and size, including DSiWare in the system memory, and no
    system titles. The CIA build can read every icon (FS access rights).
47. Launch from Software starts the game. Get Seed on a seed-encrypted game
    adds its seed, and the game then starts.
48. Delete an update: the game stays and goes back to its first version.
    Delete with Ticket on a test game removes it and its ticket.
49. Extra Data lists the SD card's extra data with names and icons, even for
    games that are no longer installed; Delete removes one and the game
    recreates it on its next start.
50. Tickets marks tickets of uninstalled software as Unused, and Delete Unused
    removes only those.
51. Interrupt a CIA install (cancel it, or pull the power in a test), then
    Clean Up shows an unfinished install; Remove Unfinished Installs clears
    it and the CIA installs again.
52. Seeds imports from a seeddb.bin in sdmc:/ only the seeds of installed
    games.

### Queue, Receive and direct install

53. Scan three QR codes with Add to Queue: the camera comes back after each,
    Home shows 3 on Queue, and Download All fetches them in order.
54. Receive shows the console's address and a QR code. A phone that scans it
    gets the page; links pasted there arrive and download.
55. servefiles.py from a computer sends a CIA to port 5000: it downloads and
    installs, and servefiles.py exits by itself afterwards.
56. Fill the SD card until a CIA does not fit: the error offers Install
    Directly, the CIA installs, and nothing is left on the card.
57. Install Directly of an older version than the installed one is refused
    with a clear message, and the installed copy still starts.
58. SHA-256 of a downloaded 3dsDL.cia next to 3dsDL.sha256 says it matches.
58a. Files on the Receive page: the console asks Allow or Don't Allow for
    the phone; Don't Allow keeps the page waiting until Receive reopens.
58b. Upload a CIA of 1 GiB or more from a phone and from a computer: the
    touch screen shows progress, the console does not sleep with the lid
    closed, and the file's SHA-256 matches. Note the speed.
58b2. Upload a folder of a few thousand small files from a computer and close
    the lid partway (issue #13): every file arrives, the screens light when
    the lid opens, and with the lid left closed the console sleeps about
    30 s after the last file. Leave the page open and idle with the lid
    closed: the console still sleeps (the status poll does not count).
58c. With Offer to Install ticked, uploading two CIAs asks once on the
    console and installs both.
58d. Download, rename, move, delete and edit a text file from the page, and
    upload a folder of homebrew into sdmc:/3ds/.
58e. Close the page halfway through an upload: nothing is left behind, and
    the next upload works.
58f. http://3ds.local/ opens on a Mac or iPhone and on Windows; with two
    consoles on Receive the second answers to 3ds-2.local. Record whether
    the console joined the multicast group at all (the line about .local
    is missing from the top screen if it did not).

### Saves, tickets and sounds

59. Save Data lists the installed games that have saves, and a 3DS game card
    in the slot. A New Backup appears in sdmc:/3ds/Checkpoint/saves/ under
    the same folder name Checkpoint uses for that game.
60. Checkpoint lists 3dsDL's backup, and 3dsDL lists Checkpoint's; each
    restores the other's, and the game starts with the restored progress.
61. Restore on a game with anti-tamper (a Pokemon game, for example) does
    not make the game call its save corrupted (secure value cleared).
62. Restore first makes a "Before Restore" backup; a backup folder with
    ._ files from a Mac still restores.
63. Extra Data backs up and restores, for example a game's downloaded
    content, and HOME Menu's own extra data is skipped where it is in use.
64. Browse a save, view a file as hex and copy a folder to the SD card.
65. Install a .tik from Files; export a title's ticket from Tickets and the
    .tik installs again (export is not available in Azahar).
66. Sounds play on moving, choosing and going back, and stop with Sounds
    off in Other Settings; without sdmc:/3ds/dspfirm.cdc the setting says
    unavailable and nothing else changes.

## Results so far

On 2026-10-29 test builds 0.8.3-beta.1 and beta.2 ran on a New 3DS
(11.17.0-50U) for issue #13.

- 14 in part: with beta.1 the lid closed during a 100 MiB download from
  proof.ovh.net with 4 connections, the screens went dark and the console
  stayed awake, but the download failed two seconds later with curl error
  7: refusing sleep does not stop NDM dropping Wi-Fi for StreetPass. With
  beta.2, which also holds NDM's infrastructure state, the download kept
  going with the lid closed. The sleep after 30 s and a queue were not
  tried.

On 2026-10-12 the 0.3.0 build was confirmed on a console for downloads over
the internet (items 11 and 12), installing the CIA with FBI, and ZIP
extraction (item 24). The console model was not recorded, so the matrix
below is still open.

On 2026-10-20 the 0.8.1 pre-releases ran on the same console. Installing a
CIA of the version already installed had failed with 0xC8E083FC (AM will
not install the same version again); sending the running 3dsDL's own CIA
again now says it is already installed, and the Receive page reports the
same. 0.8.1-beta.2 installed over 0.8.1-beta.1 from inside 3dsDL, which the
build numbers make possible. Reinstall, with the save backed up first, was
not tried on a game yet.

On 2026-10-19 and 20 the Files over Wi-Fi pre-releases (0.7.9 to 0.7.13,
which became 0.8.0) ran on a console, tested from a Mac on the same Wi-Fi.
The console model was not recorded.

- 58a in part: Allow worked and lasted until Receive closed. Don't Allow was
  not tried.
- 58b in part: 16 MiB uploads ran at 0.90 to 0.99 MB/s and the download back
  at 0.98 MB/s, byte for byte the same file. A 1 GiB file, a phone, and the
  lid closed during a transfer were not tried.
- 58c in part: a CIA uploaded from the Mac and installed after Install on the
  console, five times (3dsDL over itself). Two at once was not tried.
- 58d in part: folder listings took 35 to 220 ms; a text file was written and
  deleted, and file dates matched the Mac's clock to the minute.
- 58f in part: http://3ds.local/ opened on the Mac, so the console's network
  service does deliver multicast. Windows, an iPhone and two consoles at once
  were not tried.

That run found three problems, all fixed before 0.8.0: every web request took
one to two seconds and uploads crawled at 21 KB/s, because the listener's and
the mDNS responder's blocking waits held up each other inside the network
service; the end of every answer was lost once client sockets became
non-blocking, because the service drops unsent data when such a socket
closes; and no file ever showed a date, because stat() on the SD card always
reports 0. 3ds.local also took five seconds to look up on the Mac until the
console answered its IPv6 question.

On 2026-10-19, before any console run, the CIA installer was tried in Azahar
2126.2 with the `.3dsx`: install from Files, download then INSTALL & DELETE
from a local server, same-version reinstall, History marking the download as
installed, and LAUNCH (items 29, 30 and 36 in part). Azahar's AM is more
forgiving than a console's, so items 31 to 35 still need hardware.

On 2026-10-18 a test build of 0.4.0 (pre-release `test-fast-downloads-1`)
downloaded a 38 MB video over 4 connections at an average of 500 to 600
KiB/s (item 12). The single-connection speed of 0.3.2 on the same file and
network was not measured, so the gain is not known exactly.

The same day, the first 3D banner build (commit 539b3d8) crashed the HOME
Menu every time 3dsDL was selected (item 28). Its CGFX left every mesh's node
name blank; `build-cgfx.py` now fills them in, and the flat banner is the
default until a 3D build passes items 26 to 28. The second 3D build (with the
node names) still crashed: Luma reported a data abort in `menu` (HOME Menu)
reading address 0x8 through a null pointer. Its bone lookup tree was stale
after pycgfx moved the wordmark's bone last, so lookups for five bones landed
on the wrong entries. `build-cgfx.py` now rebuilds the tree and checks every
lookup. The third 3D build (commit 155e8a5) got further: the banner drew its
first frame correctly, then the HOME Menu froze (no input, music still
playing). That build was also the first with a blended wordmark marked
translucent, parts scaled to 0.001 and 22.05 kHz sound; the fourth reverts
all three, and a still version of it (no animation) was built alongside to
tell an animation problem from anything else. The fourth, animated
(test-banner-5), worked: no crash or freeze, and the banner animates. Its one
flaw was the wordmark vanishing when seen from behind, so the wordmark is now
a billboard that always faces the viewer. That build (test-banner-7, commit
c9bd54f) passed: the banner animates, the wordmark stays visible,
and the HOME Menu neither crashes nor freezes. It became the default for
0.3.1.

## Compatibility matrix

| Console | System version | Luma3DS | Build | Checklist | Download speed | Tester, date | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| New 3DS XL | | | 3dsx | not run | | | |
| New 3DS XL | | | cia | not run | | | |
| Old 3DS / 2DS | | | 3dsx | not run | | | |
| Old 3DS / 2DS | | | cia | not run | | | |

Mark the checklist column with the item numbers that failed, or "all pass".
