#include "tui.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Tui {
namespace {

bool g_colour = false;

// ---- mascot art ----
// Taken from the reference launcher's own string table: four walk poses (the tail
// cycles up / mid / down / mid against a four-beat step) plus a separate dead-eye
// pose. The failure pose is deliberately kept out of the cycle so a successful run
// never blinks a dead eye.
constexpr int kDogRows = 8;         // blank + 4 body + 1 leg + 1 ground + blank
constexpr int kDogWidth = 36;
constexpr int kGroundLen = 64;

const char *const kBlankRow = "                                    ";
const char *const kGroundStatic = "------------------------------------";

const char *const kDogFrames[5][kDogRows] = {
    {kBlankRow,
     "((                  ____      ",
     " \\\\________________( ( e\\____o",
     " (                     _____/ ",
     "  \\___________________/       ",
     "  /   \\           \\  /        ",
     kGroundStatic,
     kBlankRow},
    {kBlankRow,
     " ((                 ____      ",
     " ||________________( ( e\\____o",
     " (                     _____/ ",
     "  \\___________________/       ",
     "   )  |           |   )       ",
     kGroundStatic,
     kBlankRow},
    {kBlankRow,
     "  //                ____      ",
     " //________________( ( e\\____o",
     " (                     _____/ ",
     "  \\___________________/       ",
     "   \\  /           /   \\       ",
     kGroundStatic,
     kBlankRow},
    {kBlankRow,
     " ((                 ____      ",
     " ||________________( ( e\\____o",
     " (                     _____/ ",
     "  \\___________________/       ",
     "  |   )           )   |       ",
     kGroundStatic,
     kBlankRow},
    {kBlankRow,
     "                    ____      ",
     "  _________________( ( x\\____o",
     "//                     _____/ ",
     "||____________________/       ",
     "  |   |           |  |        ",
     kGroundStatic,
     kBlankRow},
};

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

// ---- palette, byte for byte from the reference ----
const char *reset() { return g_colour ? "\x1b[0m" : ""; }
const char *dim() { return g_colour ? "\x1b[38;5;247m" : ""; }
const char *muted() { return g_colour ? "\x1b[38;5;247m" : ""; }
const char *text() { return g_colour ? "\x1b[38;5;246m" : ""; }
const char *white() { return g_colour ? "\x1b[0m" : ""; }
const char *orange() { return g_colour ? "\x1b[38;5;208m" : ""; }
const char *green() { return g_colour ? "\x1b[1;38;5;208m" : ""; }
const char *red() { return g_colour ? "\x1b[38;5;168m" : ""; }
const char *errorRed() { return g_colour ? "\x1b[38;5;210m" : ""; }
const char *yellow() { return g_colour ? "\x1b[38;5;208m" : ""; }

bool EnableAnsi() {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!out || out == INVALID_HANDLE_VALUE || !GetConsoleMode(out, &mode)) return false;
    g_colour = SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
    return g_colour;
#else
    g_colour = true;
    return true;
#endif
}

bool StdoutIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

Screen::Screen(bool animate) : animate_(animate && StdoutIsTty()) {
    // The reference's floor is one irregular line, dashes and underscores mixed at
    // random and different every run.
    std::random_device device;
    std::mt19937 rng(device());
    ground_tile_.reserve(kGroundLen);
    for (int i = 0; i < kGroundLen; i++) ground_tile_.push_back((rng() & 1) ? '-' : '_');
}

Screen::~Screen() { LeaveScreen(); }

void Screen::Write(const std::string &text) {
    if (composing_) {
        page_buffer_ += text;
        return;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

void Screen::FlushPage() {
    if (page_buffer_.empty()) return;
    composing_ = false;
    std::fwrite(page_buffer_.data(), 1, page_buffer_.size(), stdout);
    std::fflush(stdout);
    page_buffer_.clear();
}

std::string Screen::GroundRow() const {
    std::string row;
    row.reserve(kDogWidth);
    const int len = (int)ground_tile_.size();
    const int index = len ? ground_offset_ % len : 0;
    for (int i = 0; i < kDogWidth; i++) row.push_back(len ? ground_tile_[(index + i) % len] : '-');
    return row;
}

std::string Screen::MascotFrame() const {
    if (!animate_) return {};
    const char *const *rows = kDogFrames[failed_ ? 4 : (frame_ % 4)];
    std::string art;
    art.reserve((kDogWidth + 16) * kDogRows);
    for (int r = 0; r < kDogRows; r++) {
        art += "\x1b[2K";           // wipe the row so no old art shows through
        art += white();
        // The single ground row scrolls; every other row is fixed.
        if (!failed_ && r == kDogRows - 2) art += GroundRow();
        else art += rows[r];
        art += "\r\n";
    }
    art += reset();
    return art;
}

void Screen::EnterScreen() {
    if (!animate_ || on_alternate_screen_) return;
    // The reference carries the ESC[?1049h / ESC[?1049l pair, so a run leaves the
    // user's scrollback intact.
    Write("\x1b[?1049h\x1b[?25l");
    on_alternate_screen_ = true;
}

void Screen::LeaveScreen() {
    if (!animate_ || !on_alternate_screen_) return;
    FlushPage();
    std::fwrite("\x1b[?1049l", 1, 8, stdout);
    std::fflush(stdout);
    on_alternate_screen_ = false;
}

void Screen::NewPage() {
    if (!animate_) return;
    EnterScreen();
    // Clear, home, hide the cursor and draw the mascot as one buffered page: writing
    // them separately let the console show a cleared screen and then a half-drawn one.
    composing_ = true;
    page_buffer_.clear();
    Write("\x1b[2J\x1b[H\x1b[?25l");
    Write(MascotFrame());
    last_page_.clear();
}

void Screen::Section(const char *title, const char *title_color) {
    if (!animate_) {
        std::fputs("> ", stdout);
        std::fputs(title, stdout);
        std::fputs("\n\n", stdout);
        std::fflush(stdout);
        return;
    }
    std::string line = orange();
    line += "> ";
    line += title_color ? title_color : orange();
    line += title;
    line += reset();
    line += "\r\n\r\n";
    Write(line);
}

void Screen::Detail(const std::string &label, const std::string &value, const char *colour) {
    if (!animate_) {
        std::fputs(label.c_str(), stdout);
        std::fputs(" - ", stdout);
        std::fputs(value.c_str(), stdout);
        std::fputs("\n\n", stdout);
        std::fflush(stdout);
        return;
    }
    std::string line = muted();
    line += label;
    line += dim();
    line += " - ";
    line += reset();
    line += colour ? colour : white();
    line += value;
    line += reset();
    line += "\r\n\r\n";
    Write(line);
}

void Screen::PlainDetail(const char *label, const char *value, const char *colour) {
    if (!animate_) {
        std::fputs(label, stdout);
        std::fputs(" - ", stdout);
        std::fputs(value, stdout);
        std::fputs("\n\n", stdout);
        std::fflush(stdout);
        return;
    }
    std::string line = colour ? colour : white();
    line += label;
    line += " - ";
    line += value;
    line += reset();
    line += "\r\n\r\n";
    Write(line);
}

void Screen::Emit(const char *colour, const std::string &value) {
    if (!animate_) {
        std::fputs(value.c_str(), stdout);
        std::fputs("\n\n", stdout);
        std::fflush(stdout);
        return;
    }
    // A leading indent is stripped so every message line starts flush left, and a blank
    // line follows so consecutive messages never run together (reference: emit()).
    std::string body = value;
    if (body.compare(0, 2, "  ") == 0) body.erase(0, 2);
    Write(std::string(colour ? colour : white()) + body + reset() + "\r\n\r\n");
}

void Screen::SupportNote() {
    std::string note = white();
    note += "If you need further support, please visit: ";
    note += "https://slinky.gg/contact/";
    note += "\r\n\r\n";
    note += reset();
    Write(note);
}

void Screen::RepaintMascot() {
    if (!animate_) return;
    // The page was buffered; putting it on screen now, before the mascot tick, is what
    // makes a stage switch a single repaint instead of several (reference: flushPage).
    if (composing_) FlushPage();
    // Save the cursor, jump home, redraw the mascot, come back: the dog stays anchored
    // no matter where the text beneath it ended.
    std::string out = "\x1b[s\x1b[H" + MascotFrame() + "\x1b[u";
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    if (walking_ && !failed_) {
        frame_ = (frame_ + 1) % 4;
        ground_offset_ += 1;
    }
}

void Screen::SetFailed(bool failed) {
    failed_ = failed;
    if (failed) walking_ = false;
}

void Screen::SetWalking(bool walking) { walking_ = walking; }

void Screen::HoldStage(double seconds) {
    if (!animate_) return;
    // A floor, not a cap: a stage that genuinely takes longer runs to completion.
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
    while (std::chrono::steady_clock::now() < until) {
        RepaintMascot();
        sleep_ms(150);      // the reference's walk cadence
    }
}

void Screen::FlashStage(int millis) {
    if (!animate_) return;
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(millis);
    while (std::chrono::steady_clock::now() < until) {
        RepaintMascot();
        sleep_ms(150);
    }
}

void Screen::CountdownClose(int seconds, const std::string &message, bool line_already_printed) {
    for (int left = seconds; left > 0; --left) {
        std::string line = std::string(green()) + message;
        line += white();
        line += " Loader will close in ";
        line += orange();
        line += std::to_string(left);
        line += orange();
        line += "s.";
        line += reset();
        if (!animate_) {
            std::fputs(message.c_str(), stdout);
            std::fprintf(stdout, " Loader will close in %ds.\n", left);
            std::fflush(stdout);
        } else {
            if (line_already_printed) {
                // Emit left the cursor two rows below the line: step up onto it, erase
                // it and redraw, so the countdown rewrites in place instead of printing
                // a second copy underneath.
                Write("\x1b[s\x1b[2A\x1b[2K" + line + "\x1b[u");
            } else {
                Write(line + "\r\n\r\n");
                line_already_printed = true;
            }
            FlushPage();
        }
        RepaintMascot();
        sleep_ms(1000);
    }
}

bool Screen::WaitForDismissal() {
    if (!animate_ || !StdoutIsTty()) return true;
    FlushPage();
    std::string note = std::string(white()) + "Press any key to close." + reset() + "\r\n";
    std::fwrite(note.data(), 1, note.size(), stdout);
    std::fflush(stdout);
#ifdef _WIN32
    while (true) {
        if (_kbhit()) {
            _getch();
            return true;
        }
        if (!GetConsoleWindow()) return true;   // the window was closed: nobody can dismiss
        sleep_ms(50);
    }
#else
    sleep_ms(100);
    return true;
#endif
}

}  // namespace Tui
