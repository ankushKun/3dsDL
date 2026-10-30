#pragma once

#include "icon_atlas.hpp"

#include <3ds.h>
#include <citro2d.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

// The Citro2D toolkit every 3dsDL screen is drawn with, in the System Look
// (docs/DESIGN.md): khaki screens, embossed cream buttons, grey tabs in the
// bottom corners, system-style dialogs and a green ring for the D-pad. All
// sizes are console pixels.
namespace ui {

constexpr float TopWidth = 400.0f;
constexpr float BottomWidth = 320.0f;
constexpr float ScreenHeight = 240.0f;

namespace color {
constexpr u32 rgb(uint32_t value, uint8_t alpha = 0xff)
{
    return C2D_Color32((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff,
                       alpha);
}

constexpr u32 TopA = rgb(0xF2EBB2);
constexpr u32 TopB = rgb(0xF2E9A3);
constexpr u32 BottomA = rgb(0xF1E9A3);
constexpr u32 BottomB = rgb(0xF0E694);
constexpr u32 Panel = rgb(0xDCD49A);
constexpr u32 Recess = rgb(0xD9D3A0);
constexpr u32 Dot = rgb(0xB0A66E);

constexpr u32 Ink = rgb(0x615A4B);          // panels
constexpr u32 InkStrong = rgb(0x4F493B);    // names
constexpr u32 InkButton = rgb(0x756E60);    // button labels
constexpr u32 InkDialog = rgb(0x141414);    // dialogs
constexpr u32 InkSoft = rgb(0x8C8466);      // secondary
constexpr u32 Counter = rgb(0xB1A96D);

constexpr u32 Accent = rgb(0x1C93C2); // 3dsDL blue
constexpr u32 Tag = rgb(0x609CB4);
constexpr u32 Focus = rgb(0x3CDC78);
constexpr u32 FocusGlow = rgb(0xA0FAA0);
constexpr u32 Badge = rgb(0xE69614);
constexpr u32 Good = rgb(0x3FA65A);
constexpr u32 Bad = rgb(0xE0301A);
constexpr u32 Neutral = rgb(0xACA27A);
constexpr u32 Caution = rgb(0xB8621A); // warning text on khaki

// Icon colours, one per kind of thing, as on the canvas.
constexpr u32 IconBlue = Accent;
constexpr u32 IconTeal = rgb(0x1E9E84);
constexpr u32 IconRed = rgb(0xD2603A);
constexpr u32 IconOrange = rgb(0xE9890E);
constexpr u32 IconGrey = rgb(0x7D7563);
constexpr u32 IconGreen = rgb(0x32A865);
constexpr u32 IconPink = rgb(0xC05BA8);
constexpr u32 IconGold = rgb(0xC9A227);

// Older names, kept so text colours read the same everywhere.
constexpr u32 Text = Ink;
constexpr u32 Muted = InkSoft;
constexpr u32 Primary = Accent;
constexpr u32 Success = Good;
constexpr u32 Warning = Caution;
constexpr u32 Danger = Bad;
} // namespace color

// Text sizes as citro2d scales of the system font, matching the cap
// heights measured from the system applets.
namespace type {
constexpr float Hero = 0.95f;   // cap 19
constexpr float Title = 0.78f;  // cap 16
constexpr float Button = 0.72f; // cap 15
constexpr float Tab = 0.6f;     // cap 14
constexpr float Body = 0.62f;   // cap 13
constexpr float Small = 0.54f;
constexpr float Row = 0.52f;    // cap 10
constexpr float Tiny = 0.44f;
} // namespace type

struct Rect {
    float x;
    float y;
    float w;
    float h;
    bool contains(const touchPosition &touch) const;
    Rect inset(float by) const { return {x + by, y + by, w - 2 * by, h - 2 * by}; }
};

bool init();
void shutdown();

// False once the system has asked 3dsDL to close. Use this instead of calling
// aptMainLoop() directly so every loop sees the request.
bool keepRunning();

struct Input {
    u32 down = 0;
    u32 repeat = 0;
    u32 held = 0;
    u32 up = 0;
    touchPosition touch {}; // the last touched point, also on release
    // A touch that started inside rect this frame.
    bool tapped(const Rect &rect) const;
};
Input readInput();
// Call as a screen opens. It ignores touch until it settles and the stylus
// lifts, and fades in when id differs from the last screen's.
void enterScreen(const std::string &id);
// Only the touch guard, for a dialog drawn over the current screen.
void lockTouch();

// Milliseconds since 3dsDL started, for animation.
uint64_t now();

// --- Frames ------------------------------------------------------------------

// Clears both screens, starts the top screen and paints its background.
void beginFrame();
void beginTop();
void beginBottom(); // also paints the bottom background
void endFrame();
void drawTopBackground();
void drawBottomBackground();

// --- Text --------------------------------------------------------------------

float textWidth(const std::string &value, float scale);
float lineHeight(float scale);
// Cuts value with "..." at the end so it fits maxWidth.
std::string fitEnd(const std::string &value, float scale, float maxWidth);
// Cuts value in the middle, keeping both ends: "sdmc:/a/...e/file.zip".
std::string fitMiddle(const std::string &value, float scale, float maxWidth);
// Breaks value into lines no wider than maxWidth, preferring spaces and URL
// separators. The last line ends in "..." when maxLines cuts text off.
std::vector<std::string> wrap(const std::string &value, float scale,
                              float maxWidth, size_t maxLines);

void drawText(const std::string &value, float x, float y, float scale,
              u32 textColor, u32 flags = 0);
// Draws wrapped text and returns the y coordinate below it.
float drawWrapped(const std::string &value, float x, float y, float scale,
                  u32 textColor, float maxWidth, size_t maxLines,
                  u32 flags = 0);
// Centres text vertically in rect, horizontally too unless left is set.
void drawCentered(const std::string &value, const Rect &rect, float scale,
                  u32 textColor, bool left = false);
// Text in a box maxWidth wide (flags align it as for drawText). Text that
// is too long scrolls: it waits, slides to its end inside the box, rests
// and starts over, so all of it can be read without cutting it off.
void drawMarquee(const std::string &value, float x, float y, float scale,
                 u32 textColor, float maxWidth, u32 flags = 0);
// drawCentered for text that may be too long for rect: it scrolls.
void drawCenteredMarquee(const std::string &value, const Rect &rect,
                         float scale, u32 textColor, bool left = false);

// --- Shapes ------------------------------------------------------------------

namespace corner {
constexpr unsigned TopLeft = 1;
constexpr unsigned TopRight = 2;
constexpr unsigned BottomLeft = 4;
constexpr unsigned BottomRight = 8;
constexpr unsigned Top = TopLeft | TopRight;
constexpr unsigned All = 15;
} // namespace corner

struct Stop {
    float at; // 0 at the top edge, 1 at the bottom
    u32 color;
};

void fillRound(const Rect &rect, float radius, u32 fill,
               unsigned corners = corner::All);
// A vertical gradient through stops.
void fillRound(const Rect &rect, float radius,
               std::initializer_list<Stop> stops,
               unsigned corners = corner::All);
// A ring of width inside rect. openBottom leaves out the bottom edge and
// the bottom corners, for shapes that run off the screen.
void strokeRound(const Rect &rect, float radius, float width, u32 stroke,
                 unsigned corners = corner::All, bool openBottom = false);
void drawShadow(const Rect &rect, float radius, float spread, u32 shadow);
void drawDots(float y, float left = 18, float right = 382);

// --- Icons and pictures --------------------------------------------------------

void drawIcon(Icon icon, float x, float y, float size, u32 tint);
// Draws a picture or title icon. Use this rather than C2D_DrawImageAt, which
// would pick up the shapes' colouring.
void drawImage(const C2D_Image &image, float x, float y);

// Puts a 48x48 RGB565 icon (row by row, see smdh::Info) into the shared icon
// texture and returns it for drawing, or nullptr when pixels is not an icon.
// Only one icon can be shown at a time.
const C2D_Image *showIcon(const std::vector<uint16_t> &pixels);
// Draws a title icon framed like a HOME Menu icon, or a stand-in glyph when
// pixels is empty.
void drawTitleIcon(const std::vector<uint16_t> &pixels, float x, float y,
                   Icon fallback = Icon::Package);

// Puts a picture (see picture::Picture, at most 400x240) into the shared
// picture texture and returns it for drawing at its own size, or nullptr
// when it is empty or too big.
const C2D_Image *showPicture(const std::vector<uint16_t> &pixels,
                             uint32_t width, uint32_t height);

// --- Components ----------------------------------------------------------------

// Wi-Fi, connection, date, time and battery along the top of the top screen.
void drawStatusBar();
// Names the screen just entered in log.txt, once. drawTopChrome names it by
// its title; a screen without one calls this while drawing.
void nameScreen(const std::string &name);
// Top screen background, status bar, icon and title, and the dotted line.
// order is how the list on the bottom screen is sorted ("Largest First"),
// set into the right end of the dotted line, over the panel's corner.
void drawTopChrome(const std::string &title, Icon icon = Icon::None,
                   u32 iconColor = color::Accent,
                   const std::string &order = "");
// The khaki information panel on the top screen (x 17..383, y 69..225 by
// default).
constexpr Rect TopPanel = {17, 69, 366, 156};
void drawPanel(const Rect &rect = TopPanel);
// What L or R does, for drawShoulderKeys: a word, an icon, or both.
struct ShoulderKey {
    std::string label;
    Icon icon = Icon::None;
    u32 iconColor = color::rgb(0xFFFFFF);
};
// L's and R's jobs as small grey tabs in the top screen's bottom corners,
// nearest their buttons and under TopPanel. They name buttons and take no
// touch, so the bottom screen keeps a way to do the same. A key with
// neither label nor icon is left out.
void drawShoulderKeys(const ShoulderKey &left, const ShoulderKey &right = {});
void drawRecess(const Rect &rect);
// Centred, wrapped text in a panel; returns the y below it.
float drawPanelText(const std::string &value, const Rect &rect,
                    float scale = type::Body, u32 textColor = color::Ink,
                    size_t maxLines = 5);

enum class Style { Normal, Chosen, Current, Danger, Disabled };

// labelColor 0 keeps the style's own colour.
void drawButton(const Rect &rect, const std::string &label,
                Style style = Style::Normal, float scale = type::Button,
                Icon icon = Icon::None, u32 iconColor = color::IconGrey,
                bool pressed = false, u32 labelColor = 0);
// A big menu button with a 30 px icon over one or two lines of label.
void drawTile(const Rect &rect, const std::string &label, Icon icon,
              u32 iconColor, bool pressed = false);
// The D-pad ring around rect, breathing gently.
void drawFocus(const Rect &rect, float radius, float gap = 3.5f,
               float width = 3.0f);

enum class Tab { Left, Right, Full };
Rect tabRect(Tab tab);
// Where drawTab puts the tab for this label and key: the right tab grows
// towards the middle, up to 150 px, when a long action needs it.
Rect tabRect(Tab tab, const std::string &label, const std::string &key = "");
// key is the button that also presses the tab, drawn as a small white key
// cap before the label (the system font's own symbol for A, B, X and Y when
// it has them). Tabs never take the D-pad ring, so every tab shows its key.
void drawTab(Tab tab, const std::string &label, const std::string &key = "",
             bool pressed = false);

// A blue label tag; returns its width.
float drawTag(float x, float y, const std::string &label,
              u32 fill = color::Tag);
// A count or status badge right-aligned at right; returns its width.
float drawBadge(float right, float y, const std::string &label,
                u32 fill = color::Badge);
// fraction < 0 draws an indeterminate, moving bar.
void drawProgressBar(const Rect &rect, float fraction);
void drawSpinner(float centerX, float centerY, float radius = 14);
// A label tag and a value on one line, as the system's settings lists do.
// Returns the y of the next line.
float drawTagRow(float x, float y, float tagWidth, const std::string &tag,
                 const std::string &value, u32 fill = color::Tag);

// A fact about what is on screen: "Installed" and "0.8.2". Screens show
// facts rather than sentences explaining themselves (docs/DESIGN.md).
struct Fact {
    std::string tag;
    std::string value;
    u32 fill = color::Tag;
};
// Tag rows centred in area, the tags as wide as the widest and at least
// minTagWidth. Returns the y below them.
float drawFacts(const std::vector<Fact> &facts, const Rect &area = TopPanel,
                float minTagWidth = 76);
// A large icon centred in area, for a top screen whose bottom screen
// already shows everything there is to say.
void drawEmblem(Icon icon, u32 iconColor, const Rect &area = TopPanel);

// --- Dialogs -------------------------------------------------------------------

enum class Tone { Info, Success, Warning, Error };

struct Paragraph {
    std::string text;
    u32 textColor = color::Text;
    float scale = 0.5f;
    size_t maxLines = 3;
};

struct Dialog {
    std::string header; // the top screen's title while the dialog is open
    std::string title;  // the question, in the dialog box
    Tone tone = Tone::Info;
    // Explanations, on the top screen's panel.
    std::vector<Paragraph> body;
    std::vector<std::string> buttons; // one to three
    int initial = 0;
    int dangerous = -1;    // that button is red, the others a dark strip
    int cancelButton = -1; // returned for B; -1 means B returns -1
    // A 48x48 picture shown on the top screen, such as a title's icon.
    const C2D_Image *icon = nullptr;
    // Draws the whole top screen instead of header, body and icon.
    std::function<void()> drawTop;
};

// Shows dialog over the current screens until a button is chosen. Returns
// its index, or -1 for B (when cancelButton is -1) or when the app is
// closing.
int choose(const Dialog &dialog);
void message(const std::string &title, const std::string &text,
             Tone tone = Tone::Info, const std::string &detail = "");

// --- Screens of buttons ----------------------------------------------------------

struct Control {
    Rect rect;
    std::string label;
    Icon icon = Icon::None;
    u32 iconColor = color::IconGrey;
    bool tile = false; // big menu tile with the icon above the label
    Style style = Style::Normal;
    float scale = type::Button;
    bool enabled = true;
    u32 labelColor = 0; // 0 keeps the style's own colour
    // A small badge such as "New" on the top right corner.
    std::string badge;
    u32 badgeColor = color::Badge;
};

struct ScreenSpec {
    std::vector<Control> controls;
    std::string back;      // left tab, "" for none; B always returns Back
    std::string action;    // right tab, "" for none
    std::string actionKey; // key cap shown on the right tab, e.g. "START"
    u32 actionKeys = 0;    // keys that also press the right tab; set one,
                           // as the D-pad cannot reach tabs
    bool closeBar = false; // one full-width START Close bar instead of tabs;
                           // B does nothing there
    u32 extraKeys = 0;     // other keys that end the screen
    std::function<void()> drawTop;
    // Drawn on the bottom screen under the controls.
    std::function<void()> drawBottom;
    // Called every frame before anything else; returns true when it changed
    // the controls (enabled, labels, badges), for screens whose data is
    // still loading.
    std::function<bool()> update;
};

constexpr int ScreenBack = -1;
constexpr int ScreenAction = -2;
constexpr int ScreenKey = -3;
constexpr int ScreenClosed = -4;

struct ScreenResult {
    int choice = ScreenClosed; // control index, or one of the above
    u32 key = 0;               // for ScreenKey
};

// Shows the controls until one is chosen by A or by touch (on release, as
// on the console), or a tab by its key or touch. focus is the control the
// D-pad ring starts on and ends on; the ring never goes to a tab.
ScreenResult runScreen(const ScreenSpec &spec, int &focus);

// Two columns of small buttons, or one column of big ones for up to four.
// Returns the chosen index or -1.
struct MenuItem {
    std::string label;
    bool danger = false;
    bool enabled = true;
};
int menu(const std::vector<MenuItem> &items,
         const std::function<void(int)> &drawTop,
         const std::string &back = "Back");

// One choice out of several, the current one drawn as chosen. Picking one
// returns its index at once; Back returns -1.
int chooseOption(const std::string &title,
                 const std::vector<std::string> &options, int current,
                 Icon icon = Icon::Gear, u32 iconColor = color::IconGrey);

// --- Lists ---------------------------------------------------------------------

struct ListItem {
    std::string label;
    std::string detail; // short text on the right
    u32 labelColor = color::InkButton;
    bool marked = false; // light blue, for multiple selection
    Icon icon = Icon::None;
    u32 iconColor = color::IconGrey;
    std::string badge; // replaces detail with a coloured badge
    u32 badgeColor = color::Badge;
    // A small icon before the detail or badge, such as a favourite's star.
    Icon flag = Icon::None;
    u32 flagColor = color::IconGold;
    // From 0 to 1: a thin bar along the foot of the row, for something on
    // its way. Below 0 draws none.
    float progress = -1;
    // Names the row, so the highlight follows it when update reorders the
    // rows; "" for lists that never change while open.
    std::string key;
};

// An action at the right end of a list's heading, as a key cap and a word
// like the tabs but plain, such as Files' Sort. Its keys or a tap on it
// close the list, reporting its key.
struct HeadingAction {
    std::string label;
    std::string key; // its key cap
    u32 keys = 0;
};

struct ListSpec {
    std::string header;   // unused, kept for older callers
    std::string title;    // the top screen's title when drawTop is unset
    // A small line above the list on the bottom screen, for what the top
    // screen does not already say, such as the folder being browsed.
    std::string heading;
    // Actions at the heading's right end, left to right.
    std::vector<HeadingAction> headingActions;
    std::vector<ListItem> items;
    std::string emptyText;
    std::string back = "Back";
    std::string action;    // right tab
    std::string actionKey; // its key cap
    u32 actionKeys = 0;    // keys that press it; reported as the result key
    std::string hint;      // between the tabs, e.g. "2 marked"
    u32 extraKeys = 0;     // keys besides A and B that close the list
    // A long press on a row reports this key with that row highlighted.
    u32 longPressKey = 0;
    // Draws the top screen for the highlighted item (index -1 when empty).
    std::function<void(int)> drawTop;
    // For lists that fill in while open: called every frame before drawing,
    // it may change items and returns true when it did.
    std::function<bool()> update;
    // While true, a spinner shows that more rows are on their way.
    std::function<bool()> loading;
};

struct ListResult {
    u32 key = 0;    // KEY_A, KEY_B, an action or extra key; 0 when closing
    int index = -1; // highlighted item
};

// Rows scroll smoothly: drag them, flick them, or move with the D-pad, and
// with L and R unless the list's keys give them a job. A tap highlights a
// row and a tap on the highlighted row opens it. offset, when given, is
// where the rows are scrolled to, below 0 for none yet: the list opens
// there, with the highlighted row in view, and leaves it there, so a screen
// that closes the list for a key and opens it again stays in place.
ListResult runList(const ListSpec &spec, int &cursor, float *offset = nullptr);

// Touch scrolling with momentum for anything taller than its box.
class Scroller {
public:
    // Call once a frame. area is where a drag starts; content and viewport
    // are heights. Returns true on the frame a touch ended without moving
    // (a tap), with the touched point in tapPoint.
    bool update(const Input &input, const Rect &area, float content,
                float viewport);
    float offset() const { return offset_; }
    void setOffset(float value) { offset_ = target_ = value; velocity_ = 0; }
    // Moves the content by, as when rows are added above the view, without
    // stopping a flick or a drag.
    void shift(float by)
    {
        offset_ += by;
        target_ += by;
    }
    // Eases towards keeping [top, top + height) visible.
    void reveal(float top, float height, float viewport);
    bool dragging() const { return dragging_; }
    bool moved() const { return moved_; }
    // True once while a touch is held still for half a second.
    bool longPressed() const { return longPressed_; }
    touchPosition tapPoint {};
    touchPosition pressPoint {};

private:
    float offset_ = 0;
    float target_ = 0;
    float velocity_ = 0;
    bool dragging_ = false;
    bool moved_ = false;
    bool longPressed_ = false;
    bool longPressFired_ = false;
    bool following_ = false;
    float startY_ = 0;
    float lastY_ = 0;
    uint64_t downTime_ = 0;
};

// --- Progress, busy and scanner layouts ----------------------------------------

struct ProgressView {
    std::string title; // the top screen's title row: "Downloading"
    Icon icon = Icon::Download;
    u32 iconColor = color::IconGreen;
    std::string name;   // the file or title
    Icon nameIcon = Icon::Package; // beside name when there is no titleIcon
    u32 nameIconColor = color::Accent;
    std::string source; // under the name: host or version
    std::vector<uint16_t> titleIcon; // 48x48 when a title is known
    float fraction = -1.0f;
    std::string amounts; // "3.10 MiB of 4.42 MiB"
    std::string speed;   // "1.24 MiB/s"
    std::string tag;     // "4 connections", beside the speed
    std::string left;    // "9 s left" (units::timeLeft), at the line's end
    std::string note;    // green, e.g. "Continuing from 12.0 MiB"
    std::string message; // on the bottom panel: where it goes, or a warning
    bool cancellable = true; // a Cancel button under the panel
    bool stopping = false;
};

extern const Rect ProgressCancelButton;
void drawProgress(const ProgressView &view);
// Only the top screen's panel of drawProgress, under a title row drawn first.
void drawProgressPanel(const ProgressView &view);

// A "Please wait..." dialog over the current screens.
void drawBusy(const std::string &header, const std::string &title);

struct BusyView {
    std::string header;
    std::string title; // on the top screen's panel
    bool progress = false; // a bar, filled to fraction, with amount under it
    float fraction = 0;
    std::string amount; // "3 of 12"
    std::string left;   // "9 s left" (units::timeLeft), across from amount
    bool cancellable = false; // a Cancel button along the bottom of the box
    bool stopping = false;    // it was pressed and the job is finishing
    bool pressed = false;     // it is held down
};
// Where the Cancel button of a cancellable busy dialog is.
extern const Rect BusyCancelButton;
void drawBusy(const BusyView &view);

extern const Rect ScannerCancelButton;
extern const Rect ScannerTypeButton;
void drawScanner(const C2D_Image *preview, const std::string &status,
                 u32 statusColor);

// Copies what both screens show now, for dialogs to dim behind them.
void captureBackdrop();

// --- Notifications -------------------------------------------------------------

struct Notice {
    std::string title; // "Update Available"
    std::string text;  // one line under it
    Icon icon = Icon::AppMark;
    u32 iconColor = color::Accent;
};

// Slides notice down from under the status bar over whatever the top screen
// shows, keeps it there for a few seconds and slides it away, the way the
// HOME Menu shows its notifications. A new notice replaces the current one,
// in its place when that one is down.
void notify(const Notice &notice);

// Runs at the start of every frame on the main thread, whichever screen is
// open. For picking up the results of background work.
void setFrameHook(std::function<void()> hook);

} // namespace ui
