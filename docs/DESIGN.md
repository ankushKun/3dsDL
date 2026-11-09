# 3dsDL System Look

The design language from 0.7 on. The mockups for every screen are on the
design canvas at https://claude.ai/artifact/G2ZpjCidUfCz82fapdc1Ke (private
until shared). This file is the version the code follows. All sizes are
console pixels; the canvas draws the screens at 2x.

## The goal

3dsDL should look and behave like software that came with the console. The
reference is the family of system applets: System Settings and Data
Management first, then the error and confirmation dialogs, the HOME Menu
status bar and the green selection cursor. Values below were sampled from
real captures of those applets, not from memory.

3dsDL keeps its own identity the way each system app does: one accent colour
(3dsDL blue, `#1C93C2`, where Data Management uses green) and its own icons.
No Nintendo logos, mascots or artwork are used. On the console the type is
the system font itself, which is what makes it read as native.

## Rules

1. **The top screen shows, the bottom screen does.** Titles, previews and
   facts go up top. Everything interactive is on the bottom
   screen. Nothing on the top screen responds to input.
2. **Few, large controls.** Embossed cream buttons 26 to 40 px tall, at most
   four or five choices per screen. Long lists page through a recessed panel
   with a page counter instead of scrolling. An options sheet (below) holds
   more, grouped so it reads as three: what the target is for, then two rows
   of icon buttons that never move.
3. **Back and OK live in the bottom corners.** Grey tabs, Back (or Cancel or
   Close) on the left, the screen's main action on the right. The home screen
   has one full-width Close bar instead. Tabs are pressed by their key or by
   touch, and every tab shows its key as a cap: B on the left, the action's
   key (X, Y or START) on the right.
4. **The green ring is the D-pad.** Touch and buttons work everywhere. The
   ring marks where A goes, on content controls and dialog buttons only; it
   never lands on a tab. A chosen option turns yellow with orange brackets,
   and picking one takes effect at once.
5. **Questions are system dialogs.** A cream box over the dimmed bottom
   screen with a gold button strip. Destructive questions get a red button
   and a dark Cancel strip.
6. **Words like the system's.** Title Case on buttons ("Install and Delete
   File"), "Please wait..." while busy, no jargon in the first line of an
   error; the technical detail goes on the top screen.
7. **Facts, not explanations.** No taglines, no help paragraphs for the
   highlighted control, no footnotes, no subtitles that repeat what is
   already on screen. A screen shows what is true (values, sizes, paths,
   versions, counts) as tag rows (`ui::drawFacts`) and lets labels say what
   happens ("Install and Delete File"). A top screen with nothing to add
   shows the section's icon (`ui::drawEmblem`), never a repeat of the
   bottom screen. When a screen would not make sense
   without a sentence, fix the label or the layout instead. Sentences are
   kept for three things only: the question in a dialog, an error or result,
   and a warning that data will be lost, folded into the question where it
   fits. Empty states are one short line.

## Tokens

### Colour

| Role | Value |
| --- | --- |
| Top screen | vertical `#F2EBB2` to `#F2E9A3` |
| Bottom screen | vertical `#F1E9A3` to `#F0E694` |
| Info panel | `#DCD49A`, radius 5 |
| Recess (lists, option groups) | `#D9D3A0`, radius 8, 1 px inner shadow |
| Dotted divider | dots `#B0A66E`, about 2.5 px, 8 px apart |
| Ink | `#615A4B` panels, `#756E60` buttons, `#141414` dialogs, `#8C8466` secondary |
| 3dsDL blue | `#1C93C2` titles and the app's icons |
| Focus ring | `#3CDC78`, 3 to 4 px, glow `#A0FAA0` |
| Label tag | `#609CB4`, white text; green `#3FA65A` for good news |
| Count badge | `#E69614`; green, red `#E0301A`, grey `#ACA27A` variants |
| Progress | track `#CFC795`, fill `#FFCC22` to `#FF9900` |
| Nav tab | `#787878` to `#686868`, 2 px top highlight `#C4C4C4`, white text |
| Dialog | edges `#FFFDF0`, centre `#E4E3DD`; strip `#FFFDF2`, `#FFF0B4`, `#FAE29A`, `#AE8D67`; dividers `#555553` and `#191817` |
| Dim | `rgba(20,18,8,.48)` |

### Buttons

| Kind | Fill | Edge | Text |
| --- | --- | --- | --- |
| Normal | `#FEFCF5` to `#E0DCCB`, darkening to `#B6B192` over the last 12 px | 1 px `#B3AA83` sides, `#8F8659` bottom, white top highlight, 3 px soft shadow | `#756E60` |
| Chosen | `#FFED7F` to `#C9BF49` | orange brackets `#E89436` at both ends | `#E07A10` |
| Current | `#80B6C8` to `#557E86` | `#4E7A86` | white |
| Destructive | `#FE3014` to `#B31B0B` | `#9A1A0C` | white |
| Disabled | none | 1.5 px dashed `#CFC48A` | `#CFC48A` |

Radius 9 for big buttons, 8 small, 10 for menu tiles. Pressed: 10% darker
and 1 px lower; actions fire on release.

### Type (system font)

| Role | Cap height | Use |
| --- | --- | --- |
| Hero | 19 | the app name on Home |
| Title | 16 | the title row on the top screen |
| Button | 15 | big buttons, nav tabs (14) |
| Body | 13 | panels and dialogs, 22 px line pitch |
| Row, meta | 10 | list rows, sizes, tags |
| Status | 11 | the clock |

Calibrate citro2d scales against these cap heights on the console; the
first guess is cap 15 at scale 0.72.

## Layout

### Top screen

- Status bar, y 0 to 17: Wi-Fi bars at x 3, the connection pill at x 27
  ("Connected" in blue, "Not Connected" in grey), the SD card's free space
  ("SD 8.00 GiB" in the soft ink, orange under 256 MiB) just right of the
  pill, date and time right-aligned to x 362, battery at x 371.
- Title row, y 24 to 50: icon and title centred, in 3dsDL blue.
- Dotted divider at y 57. Over a sorted list (Files, Titles) its right end gives way to the order, "Largest First", in small
  soft ink.
- Info panel at x 17 to 383, y 69 to 225. Content screens (text editor, hex
  view, pictures) use the whole area below the title row instead.
- Shoulder keys, y 225 to 240: when L or R has a job, a small grey tab
  in the corner nearest its button names it, built as the nav tabs are:
  flush with the screen's edges, its inner top corner rounded, the key on
  the outside as HOME Menu draws L and R: the button's white outline with
  its letter, filled in while the button is held. A key that goes somewhere takes a word (Files:
  L Favourites); one that toggles shows the state it changes instead, so
  the two never read alike (Files: R as a star, outlined when R would add
  the entry, gold when it would take it away). They name buttons and take
  no touch, so the bottom screen keeps a way to do the same. An options sheet's two pages are the
  exception: L and R sit at the ends of its segments, which take touch, and
  the corners stay empty.

### Bottom screen

- Content from y 6 to 202.
- Nav tabs at y 209 to 239, 126 px wide, the inner top corner rounded 22.
- A page counter ("1/3") sits between the tabs at the bottom right, with a
  small label to its left.

### Lists

A recessed panel holds rows 27 px tall on a 31 px pitch: white pills with a
17 px icon, the name and a right-aligned size or badge, with a small gold
star before it on Files' favourites. Six rows fit; the D-pad's left and
right, L/R and the counter page through the rest. A list that gives L or R
a job of its own, as Files does, pages with the D-pad only. In every list a tap highlights a row and
a tap on the highlighted row opens it. Selected rows turn light blue and their
icon becomes a blue check; in Files a tap on a row's icon or SELECT selects
it, and the words are Select, Deselect and Select All (the code calls these
rows marked, as highlighted is the cursor's row). Options act on the row the
cursor is on, and on the whole selection when that row is selected. The scrollbar's column takes a drag: the thumb follows the stylus, and
a touch on the track brings the thumb's middle there. A list that fills in
while open keeps the row at the top of the view in place as rows arrive above
it; only a list nobody has touched stays at its first row. A list can put
actions at the right end of its heading for what changes the whole list,
each a key cap and a word in the heading's ink (Files' and Titles' X Sort), never a pill. A row on its way carries a thin progress bar along its foot.

### Options sheets

What can be done with something (a file, a folder, a selection) is an
options sheet (`ui::runSheet`), not a wall of text buttons:

- **What it is for** leads: at most two wide buttons, a 20 px icon beside the
  label (Install and Install and Delete for a CIA, Open and Extract for an
  archive, Edit for text, Open for anything else). Two share the row by the
  length of their labels.
- **Two rows of icon buttons** follow, three or four to a row, each a 22 px
  icon over a one-word label. Their places never move: what does not apply
  is dimmed in place (the dashed disabled style), so Paste is always third
  and Delete always last, bottom right, its label red. A count rides on a
  corner badge (Paste's waiting items, Install's CIAs).
- **Two targets, two pages.** The entry and the folder it is in are pages of
  one sheet, switched by L and R or a tap on a segment at the top: a
  recessed track with the shown page raised as a button, its icon and name,
  and the L and R caps at the track's ends, as the system's tab bars show
  them. The top screen shows the shown page's target. The ring stays in the
  page; segments take touch and the shoulder keys only.
- Layout: segments y 6 to 34; the lead row 36 px tall; rows 46 px; 12 px
  between groups and 8 px between buttons; the block is centred in what is
  left above the tabs.

Lists of verbs, such as a title's page, are lists: the action as the row's
label with its fact on the right (Save Data, 2 backups; Delete DLC, 240 MiB),
the destructive ones last in red with a red trash icon. A list hides what
does not apply instead of dimming it.

Short menus of up to four actions (`ui::menu`) give each button an icon; in
one menu, all have one or none do.

### Dialogs

Box at x 10, y 12, 300 by 216, radius 13, over the dimmed bottom screen. The
message is centred in body type. The strip is 39 px with one to three
buttons; the focused one carries the green ring. Destructive dialogs swap the
strip for a red button and a 30 px dark Cancel strip.

## Screens

| Board | Top screen | Bottom screen |
| --- | --- | --- |
| Home | app icon, name, download folder and version | Settings and Queue bars, six tiles (Scan QR Code, Enter URL, Receive; History, Files, Titles), Close bar |
| Scan QR code | camera with corner brackets, status strip at the bottom | title, Type a URL Instead, Back |
| Confirm download | file, host, URL, Save to and HTTPS tags | Download, Add to Queue, Change Folder, Cancel |
| Downloading | file, progress, amounts, speed and its connections tag, the time left across from them (units::timeLeft, as every progress screen and the page say it) | saving to, Cancel |
| File exists | which file and what each choice does | dialog: Keep Both / Replace / Cancel |
| Install prompt | icon, name, publisher, Title ID, Version, Size | Install, Install and Delete File, Keep File |
| Installing | icon, name, progress | do not turn off |
| Installed | icon, name, time taken | dialog: Close / Launch |
| Files | details of the highlighted entry; a picture's preview, a song's cover; L Favourites and R's star in the corner tabs | path, X Sort, six rows, Back, page counter, Options |
| Already here | the folder, how many clash and their names, files and folders tagged | Replace All (Merge All for folders, Replace and Merge for both), Keep Both, Skip These, Ask for Each, Cancel; one clash asks Keep Both / Replace (Merge) / Skip |
| Favourites | details of the highlighted favourite, as Files shows it; R's gold star in the corner tab | starred folders and files in the order they were starred, each with its folder, or Missing; Back, R Remove; A opens a folder, or a file's folder with it highlighted |
| Options | the entry the sheet acts on as Files shows it, or what the marked items add up to; on the folder's page its path, order, count, CIAs and clipboard | segments for the entry and its folder (L, R); the entry's page: what it is for, then Cut, Copy, Paste, Rename / Select, Star, Info, Delete; the folder's: New Folder, New File, then Paste, Select All, Install / Star, Info, Favourites |
| Info | the entry's type, size, bytes and date, or the totals | dialog: OK / SHA-256 / Hex View for a file, OK otherwise |
| Picture viewer | the picture, nothing else | name, size, position, Previous / Next |
| Song viewer | title, cover, artist, album, length, track | name, format, position, Previous / Next |
| Text editor | ruled page with line numbers, current line in blue | the full current line, Edit / New Line / Delete, Close / Save |
| Hex view | offset, bytes, characters | offset, page buttons |
| ZIP contents | totals and the selected entry | entries, Extract Here / Extract to New Folder |
| CIA details | icon, name, Title ID, Version, Size | Install, Install and Delete File, Delete File |
| Delete | what goes and how big it is | destructive dialog |
| History | the highlighted download in full | rows with Installed, Resume, Saved, Failed badges |
| Receive | QR code for the page, the address in large type and http://3ds.local under it; while a file is on its way, its progress as Downloading shows it, with the address at the end of the dotted line | what came in and went out since 3dsDL started, newest first: a bar on what is on its way, the size of what arrived, Installed, Failed, Sent, Queued and Stopped badges, Back, X Clear once something is done (it drops what is done, keeps what is on its way, and leaves the files) |
| Tickets without a backup | for one: its software (installed, remembered from when it was, from a Checkpoint folder or a CIA on the card), Title ID, bought here, issued for, a CIA that restores it, why no backup; for several: each one's name and whether it was bought here or has a CIA | destructive dialog: Delete Anyway / Keep, starting on Keep |
| Titles | the highlighted row: icon, name, publisher, Title ID, version (its update's when installed), size of the game with its update and DLC; for what is left of software that is gone, what is left | heading with the count and size and X Sort; software rows with sizes, then leftovers with a badge (Unused, Update, DLC, Ticket, Extra Data, Unfinished); Back, Y Options |
| Title page | the title as the list shows it | a list of verbs: Launch, Save Data, Extra Data, Get Seed, Export Ticket, then in red Remove Unfinished Install, Delete Update, Delete DLC, Delete Extra Data, Delete Ticket, Delete Software |
| Titles options | seeddb.bin, saved deleted tickets, unfinished installs, unused tickets | Add Seeds, Restore Deleted Tickets, Remove Unfinished Installs, Delete Unused Tickets |
| Received row | the row's name and facts | Install, Install and Delete File, Show in Files |
| Sent links | names and the host each link leads to after its redirects, who sent them, where they go | dialog: Download / Add to Queue / Cancel, starting on Cancel unless 3dsDL's own page sent them; Don't Allow in place of Add to Queue once a device's links were cancelled |
| Sent replace | the file, its host, the sender, the app, its folder and the name the new one was kept as | dialog: Keep Both / Replace, starting on Keep Both; only for the same app |
| Sent extract | what the archive holds, the one folder in /3ds/ it goes into, and the new files it adds to a folder already there | dialog: Extract / Not Now, starting on Not Now unless the console said yes before the download |
| Settings | the highlighted section's current values | About and Updates bars, four tiles, Back / Report Issue |
| Report an Issue | QR code, name and address of the highlighted place | Report a Bug, Discord Server, Developer on X; the Details row to mention, and the Crash Log row when there is one; Back |
| Setting choice | the setting's icon | recessed options, the current one chosen |
| Console clock | console time, internet time, zone | Set the Clock, Change Time Zone |
| Download folder | current and selected folder | presets and recent folders |
| Error | what was kept, the technical detail | dialog: Close / Retry |
| Please wait | what is happening | small dialog with the spinner |

### Notifications

News that is not a question, such as a new version of 3dsDL or a star
added in Files, slides down from under the status bar over whatever the top
screen shows, like the HOME Menu's notifications. A card 336 by 50 at y 22: white to cream, radius 12, a
48 px app-coloured square with a white icon, a title in small type and one
line under it. A thin line under the text runs out over the six seconds it
stays, then the card slides back up. A notice that arrives while one is
down takes its place without sliding in again. It waits while a dialog is open. The
control it points to (Settings, then Updates) carries an orange "New" badge
on its top right corner, drawn over the focus ring.

## Motion and sound

- The focus ring glides to its target in 80 ms; its glow breathes between
  60% and 100% over 1.2 s.
- Dialogs dim the screen in 120 ms and scale from 92% to 100% in 140 ms.
- Screens cross-fade in 100 ms. Progress bars ease.
- Sounds: a short tick when the cursor moves, rising and falling two-note
  blips for choosing and going back, a low tone when an error dialog
  opens and a three-note chime with notifications. They are synthesised
  at start-up (sine with a little octave, soft attack, exponential fade)
  and on by default, with a switch in Other Settings.
- Text never ends in "..." where it matters: a name, path or value too long
  for its box waits 1.4 s, slides to its end at 32 px a second inside the
  box, rests 1.1 s and starts over. Wrapped text that runs out of lines
  scrolls the rest on its last line. In lists only the highlighted row
  scrolls, as on the system's lists; the others keep "...".

### Notification LED

With the lid closed, when nobody can see the screens, the notification LED
says how 3dsDL's work is going. With the lid open it stays off, and opening
it clears what it said.

| Shows | When | Pattern |
| --- | --- | --- |
| Amber | a question waits on the console (Allow, Install, Replace) | on half a second, off half a second |
| Blue | a download, install, extraction, file job, busy task or Receive transfer runs | one slow breath every 3 s, never dark |
| Green | everything that ended since the lid closed went through | two soft pulses, then dark, every 3.5 s |
| Red | something that ended since the lid closed did not | three short blinks, then dark, every 2.5 s |

A question comes first, then work, then how it went; one failure stays red
over later successes until the lid opens, so a batch left alone says if
any of it went wrong. Red never blinks steadily, as the battery warning
does on this LED and the power LED together. 3dsDL writes a pattern only
when what it shows changes, so a SpotPass or StreetPass notice stays until
there is something of its own to show, and clears its own when the HOME
Menu takes over or it closes.

## How the code draws it

`source/app/ui.cpp` is the toolkit; every screen uses it.

- **Shapes are textured quads.** `scripts/make-ui-icons.py` renders the
  icons and, in the last row of the same atlas (`romfs/ui/icons.png`), a
  white disc and four rings. A rounded rectangle is plain bands split at its
  gradient stops plus four anti-aliased quarter discs; a ring is straight
  bars plus quarter rings. Colours, gradients included, are per vertex, so a
  whole screen of shapes is one batch.
- **No proctex.** citro2d tints images through procedural textures, which
  Azahar does not emulate (the same reason its circle primitive draws
  nothing there). The toolkit sets a text-style combiner instead: colour
  from the vertex, coverage from the texture. Real pictures go through
  `ui::drawImage`, which lets citro2d set up its own combiner again.
- **Dialogs dim the real screen.** `choose` copies both framebuffers into
  textures when it opens and draws the box over a dimmed copy.
- **Touch is on release.** Buttons and tabs highlight while pressed and act
  when the stylus lifts over them, as the system does.
- **Scrolling.** `ui::Scroller` drags, flicks with momentum and springs back
  at the ends; lists clip their rows with the scissor test. The text
  editor and hex view scroll from a drag anywhere on the touch screen
  outside their buttons, and pictures change with a sideways swipe.
- **Long press** on a Files row opens its options.

In Azahar the busiest screens draw in under 2 ms of CPU a frame per screen.
The console pass still has to confirm 60 frames a second on an Old 3DS,
the text sizes against real captures, and the focus ring's thickness.

## Building it (done in 0.6)

1. **Drawing primitives.** Rounded rectangles from rectangles and corner
   triangle fans (the emulator skips citro2d's circle primitive), vertical
   gradients from per-corner colours, soft shadows as stacked translucent
   rounded rects, the dotted divider, the focus ring with its glow.
2. **Icon atlas.** The icons on the canvas exported to PNG at 17, 22 and
   30 px, packed into one `.t3x` with tex3ds under `gfx/`, loaded from
   romfs.
3. **Components.** Status bar (Wi-Fi strength, clock, battery from PTMU),
   title row, info panel, button and tile, nav tabs, recessed list with
   paging, label tag and badge, progress bar, dialog with strip and the
   destructive variant, the spinner.
4. **Screens.** Port each board in the order of the table, checking each in
   Azahar against the canvas.
5. **Calibration on a console.** Text scales, the focus ring thickness and
   the button gradient read differently on the real LCDs.
