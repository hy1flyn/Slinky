// The loader's terminal UI, reproduced from the reference Java launcher
// (Slinky-source/native/launcher.cpp).
//
// Structure taken from the reference:
//   - an ASCII dachshund mascot in five poses (four walking, one dead-eye failure)
//     drawn above every page, with a randomly mixed '-'/'_' ground that scrolls
//   - each stage is a page: mascot, then "> HEADING", then value rows, then messages
//   - the run happens on the alternate screen buffer so scrollback survives
//
// The wording is the reference's English text verbatim.
#pragma once

#include <chrono>
#include <string>

namespace Tui {

// ---- palette (values copied byte for byte from the reference) ----
bool EnableAnsi();          // Windows: turn on VT processing; Linux: no-op
bool StdoutIsTty();

const char *reset();
const char *dim();
const char *muted();
const char *text();
const char *white();
const char *orange();
const char *green();
const char *red();
const char *errorRed();
const char *yellow();

class Screen {
public:
    explicit Screen(bool animate);
    ~Screen();

    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    bool Animated() const { return animate_; }

    // ---- page model ----
    // Switches to the alternate screen buffer for the whole run (reference: enterScreen).
    void EnterScreen();
    void LeaveScreen();

    // Clears the screen and redraws the mascot at the top, then leaves the cursor
    // directly under it. Everything written next belongs to this page.
    void NewPage();

    // "> HEADING", a blank line after it.
    void Section(const char *title, const char *title_color = nullptr);

    // "Label - value": grey label, dimmed separator, value in `colour`, blank line after.
    void Detail(const std::string &label, const std::string &value, const char *colour);

    // The same row entirely in one colour, as the reference's plainDetail.
    void PlainDetail(const char *label, const char *value, const char *colour);

    // One message line in `colour`, followed by a blank line.
    void Emit(const char *colour, const std::string &value);

    // The reference's support footer, used by error pages that end the run.
    void SupportNote();

    // ---- mascot ----
    void RepaintMascot();
    void SetFailed(bool failed);
    void SetWalking(bool walking);

    // ---- pacing ----
    // Holds for at least `seconds` while the mascot keeps walking (reference: holdStage).
    void HoldStage(double seconds);
    // The millisecond variant, used by the brief INITIALIZING flash.
    void FlashStage(int millis);

    // Success line with a live countdown, rewritten in place each second.
    void CountdownClose(int seconds, const std::string &message, bool line_already_printed = false);

    // Keeps a failure page on screen until a key is pressed.
    bool WaitForDismissal();

    // Puts whatever is buffered on screen. Pages that end the run must call this,
    // because nothing repaints afterwards (reference: composingPage/flushPage).
    void FlushPage();

private:
    std::string MascotFrame() const;
    std::string GroundRow() const;
    void Write(const std::string &text);

    bool animate_;
    bool failed_ = false;
    bool walking_ = true;
    bool on_alternate_screen_ = false;
    bool composing_ = false;      // buffering a page so it appears in one write
    int frame_ = 0;
    int ground_offset_ = 0;
    std::string ground_tile_;
    std::string page_buffer_;
    std::string last_page_;       // last frame written, so a redraw of nothing is skipped
};

}  // namespace Tui
