// Slinky: the loader's terminal UI and a cross-platform injector.
//
// The page flow, wording, pacing and mascot are reproduced from the reference Java
// launcher (Slinky-source/native/launcher.cpp):
//
//   INITIALIZING -> DOWNLOADING UPDATES -> INJECTING -> DONE / ERROR / SKIPPED
//   and, when no target is found, WAITING.
//
// Usage:
//   Slinky.exe                             find Minecraft, inject the library beside it
//   Slinky.exe <pid|name>               choose the target
//   Slinky.exe <pid|name> <library>     inject that library
//   Slinky.exe --lib <library>          choose the library only
//   Slinky.exe --pid <pid>              choose the target only
//   Slinky.exe --list                   list the Java processes
//
// Linux injects with ptrace + the target's own dlopen; Windows uses
// CreateRemoteThread(LoadLibraryW).
#include "inject.hpp"
#include "tui.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace {

// Set by Ctrl+C. The reference keeps a `cancelled` flag for the same purpose: it stops
// the stage holds, ends the startup wait early and turns the timeout page into STOPPED.
std::atomic<bool> g_cancelled{false};

#ifdef _WIN32
BOOL WINAPI console_control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        g_cancelled.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
void install_signal_handler() {
    std::signal(SIGINT, [](int) { g_cancelled.store(true); });
}
#endif

struct Options {
    bool list = false;
    bool help = false;
    bool no_anim = false;
    bool pid_given = false;
    unsigned long pid = 0;
    std::string target;
    std::string library;
};

void usage(const char *program) {
    std::cout << "usage: " << program << " [options] [target] [library]\n"
              << "\n"
              << "  target    pid or process name (default: the running Minecraft)\n"
              << "  library   path to libslinky.so / SlinkyNative.dll (default: beside this program)\n"
              << "\n"
              << "options:\n"
              << "  -p, --pid <pid>      target a pid\n"
              << "  -n, --name <name>    target a process name\n"
              << "  -l, --lib <path>     library to inject\n"
              << "      --list           list the Java processes and exit\n"
              << "      --no-anim        plain output, no mascot pages\n"
              << "  -h, --help           this text\n";
}

bool parse_options(int argc, char **argv, Options &options, std::string &error) {
    std::vector<std::string> positional;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto value = [&](const char *what) -> bool {
            if (i + 1 >= argc) {
                error = std::string(what) + " needs a value";
                return false;
            }
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else if (arg == "--list") {
            options.list = true;
        } else if (arg == "--no-anim") {
            options.no_anim = true;
        } else if (arg == "-p" || arg == "--pid") {
            if (!value("--pid")) return false;
            char *end = nullptr;
            options.pid = std::strtoul(argv[++i], &end, 10);
            options.pid_given = true;
            if (!end || *end || !options.pid) {
                error = "invalid pid '" + std::string(argv[i]) + "'";
                return false;
            }
        } else if (arg == "-n" || arg == "--name") {
            if (!value("--name")) return false;
            options.target = argv[++i];
        } else if (arg == "-l" || arg == "--lib") {
            if (!value("--lib")) return false;
            options.library = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            error = "unknown option '" + arg + "'";
            return false;
        } else {
            positional.push_back(arg);
        }
    }

    // One positional that names a library is a library; anything else is a target.
    for (const std::string &item : positional) {
        bool looks_like_lib = item.size() > 4 && (item.rfind(".dll") != std::string::npos ||
                                                  item.rfind(".so") != std::string::npos);
        if (looks_like_lib && options.library.empty()) options.library = item;
        else if (options.target.empty()) options.target = item;
        else if (options.library.empty()) options.library = item;
    }
    return true;
}

std::string base_name(const std::string &path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string default_library() {
    std::error_code ec;
    std::filesystem::path self = std::filesystem::current_path(ec);
#ifdef _WIN32
    // The executable's own directory, so the .exe can be run from anywhere.
    char buffer[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (n > 0 && n < MAX_PATH) self = std::filesystem::path(std::string(buffer, n)).parent_path();
    // SlinkyNative.dll is the shipped name; slinky.dll is still accepted so an
    // existing layout keeps working.
    const char *names[] = {"SlinkyNative.dll", "slinky.dll"};
#else
    const char *names[] = {"libslinky.so", "slinky.so"};
#endif
    for (const char *name : names) {
        std::filesystem::path candidate = self / name;
        if (std::filesystem::exists(candidate, ec)) {
            std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
            return ec ? candidate.string() : canonical.string();
        }
    }
    return {};
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The reference refuses to inject into a process with no threads at all and reports it
// in exactly this wording. A healthy JVM always has one, so this only trips on a
// process that is exiting or was never a JVM.
bool has_thread(unsigned long pid) {
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (!snapshot || snapshot == INVALID_HANDLE_VALUE) return true;   // cannot tell: do not block
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    bool found = false;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == (DWORD)pid) {
                found = true;
                break;
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
#else
    // /proc/<pid>/task lists the threads; an empty directory means none.
    std::error_code ec;
    std::filesystem::directory_iterator it("/proc/" + std::to_string(pid) + "/task", ec);
    return !ec && it != std::filesystem::directory_iterator();
#endif
}

void print_list(const std::vector<Injector::Process> &processes) {    // On Windows every Java application is java.exe/javaw.exe, so the name alone cannot
    // tell the game from a launcher; the GAME column is the one that matters.
    std::cout << "   PID  GAME  NAME                 WINDOW\n";
    for (const auto &process : processes) {
        std::string detail = process.command_line.empty() ? process.window_title : process.command_line;
        if (detail.size() > 80) detail = detail.substr(0, 77) + "...";
        std::cout << std::setw(6) << process.pid << "  " << (process.is_minecraft ? " yes " : "  -  ") << "  "
                  << std::setw(20) << std::left << process.name << " " << detail << "\n";
    }
    bool any_game = false;
    for (const auto &process : processes) any_game = any_game || process.is_minecraft;
    if (!any_game)
        std::cout << "\n" << "minecraft not found\n";
}

// ---- startup status, read from the loader ----
// The client DLL is mapped by LoadLibraryW before its start-up thread runs, and it takes
// itself back out (FreeLibraryAndExitThread) when setup fails. So a copy that stays mapped
// is a client that started, and one that disappears again is a client that failed. This
// needs no log at all, which is the point: the client writes nothing to disk.
enum class StartupStatus { Waiting, Running, Failed };

// Polls the module list for the outcome, with the mascot still walking. Returns true only
// when the client stayed mapped. `process_closed` is set when the game exits first.
bool wait_for_startup(Tui::Screen &screen, unsigned long pid, StartupStatus &status, bool &process_closed,
                      const std::string &library) {
    process_closed = false;
    status = StartupStatus::Waiting;
    auto began = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point mapped_at{};
    bool ever_mapped = false;
    // Time the start-up thread gets to fail and unload before the load counts as good.
    const auto settle = std::chrono::milliseconds(2000);

    // 250 s, the reference's ceiling.
    while (!g_cancelled.load() && std::chrono::steady_clock::now() - began < std::chrono::seconds(250)) {
        if (!has_thread(pid)) {
            // The game went away: reported on its own page rather than waiting out the
            // full timeout.
            process_closed = true;
            break;
        }
        if (Injector::IsLibraryLoaded(pid, library)) {
            if (!ever_mapped) {
                ever_mapped = true;
                mapped_at = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - mapped_at >= settle) {
                status = StartupStatus::Running;
                break;
            }
        } else if (ever_mapped || std::chrono::steady_clock::now() - began >= std::chrono::seconds(1)) {
            // Mapped and gone, or never seen after a successful load call: the client
            // took itself back out, so setup failed.
            status = StartupStatus::Failed;
            break;
        }
        if (screen.Animated()) screen.RepaintMascot();
        sleep_ms(150);
    }
    return status == StartupStatus::Running;
}
// Page shown once the loader reports an outcome.
void report_status(Tui::Screen &screen, StartupStatus status) {
    screen.NewPage();
    switch (status) {
    case StartupStatus::Running:
        screen.Section("DONE");
        screen.Emit(Tui::green(), "loaded successfully.");
        break;
    case StartupStatus::Failed:
        screen.SetFailed(true);
        screen.NewPage();
        screen.Section("ERROR", Tui::errorRed());
        screen.Emit(Tui::white(), "Slinky could not finish starting.");
        screen.SupportNote();
        break;
    default:
        break;
    }
    // This page ends the run, so nothing repaints afterwards and the buffer would
    // never reach the console.
    screen.FlushPage();
}

// The DOWNLOADING UPDATES page: both rows appear together, and the loader row then
// resolves in place -- it starts as "downloading metadata..." and is rewritten to "Done"
// once the metadata step finishes, exactly like the reference, which shows what the loader
// is doing rather than printing a second row for the outcome.
// The rows are drawn tight (no blank line between them) because they describe one pair of
// downloads; only the block as a whole is followed by a pause.
void download_updates_page(Tui::Screen &screen) {
    screen.NewPage();
    screen.Section("DOWNLOADING UPDATES");
    screen.FlashStage(400);
    screen.PlainDetailTight("Client", "Done", Tui::white(), true);
    screen.PlainDetailTight("Loader", "downloading metadata...", Tui::white(), false);
    screen.FlushPage();                 // show both rows before the loader resolves
    screen.HoldStage(1.4);
    screen.RewriteLastDetail("Loader", "Done", Tui::white(), false);
    // The outcome does not need to be admired: once the loader row reads "Done" the next
    // stage starts right away. The hold only exists so the rewrite is not overwritten by
    // the page switch in the same frame -- a quarter second is enough to read it.
    screen.HoldStage(0.25);
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    std::string error;
#ifdef _WIN32
    SetConsoleCtrlHandler(console_control, TRUE);
#else
    install_signal_handler();
#endif
    if (!parse_options(argc, argv, options, error)) {
        std::cerr << "Slinky: " << error << "\n\n";
        usage(argv[0]);
        return 1;
    }
    if (options.help) {
        usage(argv[0]);
        return 0;
    }

    if (options.list) {
        print_list(Injector::ListProcesses());
        return 0;
    }

    const bool interactive = Tui::EnableAnsi() && Tui::StdoutIsTty() && !options.no_anim;
    Tui::Screen screen(interactive);

    // ---- decide where the client library comes from ----
    // Order: an explicit --lib argument, then the copy carried inside this executable,
    // then a file beside it. The bundled copy is what makes Slinky.exe usable on its
    // own; the other two exist so a developer can swap the client without relinking.
    std::string extract_error;
    if (!options.library.empty()) {
        std::error_code ec;
        if (!std::filesystem::exists(options.library, ec)) {
            std::cerr << "Slinky: library not found: " << options.library << "\n";
            return 1;
        }
    } else if (Injector::HasBundledLibrary()) {
        options.library = Injector::ExtractBundledLibrary(extract_error);
    } else {
        options.library = default_library();
    }

    if (options.library.empty()) {
        std::string line = extract_error.empty()
                               ? std::string("SlinkyNative.dll not found beside the injector.")
                               : extract_error;
        if (interactive) {
            screen.EnterScreen();
            screen.SetFailed(true);
            screen.NewPage();
            screen.Section("ERROR", Tui::errorRed());
            screen.Emit(Tui::white(), line);
            screen.SupportNote();
            screen.FlushPage();
            screen.WaitForDismissal();
        } else {
            std::cerr << "Slinky: " << line << "\n";
        }
        return 1;
    }

    // ---- find the target, before the run takes over the screen ----
    Injector::Process target;
    bool found = false;
    if (options.pid_given) {
        found = Injector::FindTarget(std::to_string(options.pid), target, error);
        if (found) target.pid = options.pid;
    } else if (!options.target.empty()) {
        found = Injector::FindTarget(options.target, target, error);
    } else {
        found = Injector::FindMinecraft(target, error);
    }

    auto fail_page = [&](const char *heading, const std::string &line1, const std::string &line2 = {},
                         bool support = true) {
        if (interactive) {
            screen.EnterScreen();
            screen.SetFailed(true);
            screen.NewPage();
            screen.Section(heading, Tui::errorRed());
            screen.Emit(Tui::white(), line1);
            if (!line2.empty()) screen.Emit(Tui::white(), line2);
            if (support) screen.SupportNote();
            screen.FlushPage();
            screen.WaitForDismissal();
        } else {
            std::cerr << "Slinky: " << line1;
            if (!line2.empty()) std::cerr << " " << line2;
            std::cerr << "\n";
        }
    };

    if (!found) {
        // The reference loader finds the game *before* the inject stage, so "no game
        // running" ends the run right after the download stage: no INJECTING page is ever
        // drawn for it. The error line is the reference's own wording, wrapped around the
        // underlying error the finder produced ("minecraft not found").
        bool ambiguous = options.target.empty() && !options.pid_given && Injector::CountMinecraftCandidates() > 1;
        if (interactive) {
            screen.EnterScreen();
            screen.NewPage();
            screen.Section("INITIALIZING");
            screen.FlashStage(350);
            download_updates_page(screen);
            screen.SetFailed(true);
            screen.NewPage();
            screen.Section("ERROR", Tui::errorRed());
            if (ambiguous) {
                screen.Emit(Tui::white(), error.empty() ? "More than one Minecraft was found." : error.c_str());
                screen.Emit(Tui::white(), "Close the other Java applications and run Slinky again.");
            } else {
                // "Finding minecraft process: minecraft not found" -- the reference's own
                // finder wraps its error this way, so the line is built from the same two
                // pieces instead of being paraphrased.
                std::string what = error.empty() ? std::string("minecraft not found") : error;
                screen.Emit(Tui::white(), "Finding minecraft process: " + what);
            }
            screen.SupportNote();
            screen.FlushPage();
            screen.WaitForDismissal();
        } else {
            std::cerr << "Slinky: " << error << "\n" << Injector::EnvironmentHint() << "\n";
        }
        return 1;
    }

    // A name cannot confirm the game: every Java application is java.exe/javaw.exe, so a
    // launcher window would pass a name test. Only LWJGL loaded in the process proves it.
    if (!target.is_minecraft) {
        std::string name = target.name.empty() ? std::to_string(target.pid) : target.name;
        std::string line1 = std::string("This is not Minecraft: ") + name +
                            " has no LWJGL loaded.";
        std::string line2 = "Inject into the game process itself, not a launcher.";
        if (!target.window_title.empty())
            line1 += std::string(" Window title: ") + target.window_title;
        fail_page("ERROR", line1, line2, false);
        return 1;
    }

    // ---- INITIALIZING flash, then the download page, then INJECTING ----
    if (interactive) {
        screen.EnterScreen();
        screen.NewPage();
        screen.Section("INITIALIZING");
        screen.FlashStage(350);
        download_updates_page(screen);
    } else {
        std::cout << "Injecting into pid " << target.pid << "...\n";
    }

    if (interactive) {
        screen.NewPage();
        screen.Section("INJECTING");
        screen.Emit(Tui::white(), "Injecting into Minecraft...");
        // The status line has to be readable before the work starts: injection is fast
        // enough that the page would otherwise be replaced almost immediately. Three and a
        // half seconds matches the reference's inject stage, which is long enough to read
        // the line without looking like the loader hung.
        screen.HoldStage(3.5);
    }

    if (!has_thread(target.pid)) {
        fail_page("ERROR",
                  "Main thread not found. You are likely injecting into an incompatible Minecraft version.",
                  std::string(), false);
        return 1;
    }

    std::string last_log;
    std::mutex log_mutex;
    std::atomic<bool> done{false};
    std::atomic<bool> injected{false};

    std::thread worker([&]() {
        bool ok = Injector::Inject(target.pid, options.library, [&](const std::string &line) {
            std::lock_guard<std::mutex> lock(log_mutex);
            last_log = line;
        });
        injected.store(ok);
        done.store(true);
    });

    if (interactive) {
        // Keep the mascot walking while the worker runs.
        while (!done.load()) {
            screen.RepaintMascot();
            sleep_ms(150);
        }
    }
    worker.join();

    if (!injected.load()) {
        std::string detail;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            detail = last_log;
        }
        // The client being mapped already is not a load failure: it is the one case where
        // there is nothing left to do, and the reference reports it with its own line. The
        // injector's own text ("SlinkyNative is already loaded in PID <n>.") names the DLL
        // and the pid, which is more than the player needs here.
        bool already = detail.find("is already loaded") != std::string::npos;
        if (interactive) {
            screen.EnterScreen();
            screen.SetFailed(true);
            screen.NewPage();
            screen.Section("ERROR", Tui::errorRed());
            if (already)
                screen.Emit(Tui::white(), "Slinky is already loaded.");
            else
                screen.Emit(Tui::white(), detail.empty() ? std::string("Injection failed.") : detail);
            screen.SupportNote();
            screen.FlushPage();
            screen.WaitForDismissal();
        } else {
            std::cerr << "Slinky: " << (already ? std::string("Slinky is already loaded.") : detail) << "\n";
            if (!already) std::cerr << Injector::EnvironmentHint() << "\n";
        }
        return 1;
    }

    // ---- confirm through the loader: a client that stays mapped has started ----
    if (interactive) {
        StartupStatus status = StartupStatus::Waiting;
        bool process_closed = false;
        bool confirmed = wait_for_startup(screen, target.pid, status, process_closed, options.library);

        if (confirmed) {
            // The dog holds a still frame now that the outcome is known, and the
            // countdown rewrites the success line in place for five seconds.
            screen.SetWalking(false);
            screen.NewPage();
            screen.Section("DONE");
            screen.Emit(Tui::green(), "loaded successfully.");
            screen.CountdownClose(5, "loaded successfully.", true);
            return 0;
        }

        if (process_closed) {
            screen.SetFailed(true);
            screen.NewPage();
            screen.Section("ERROR", Tui::errorRed());
            screen.Emit(Tui::white(), "The selected game process closed before startup finished.");
            screen.SupportNote();
            screen.FlushPage();
            screen.WaitForDismissal();
            return 1;
        }

        if (status == StartupStatus::Failed) {
            report_status(screen, status);
            screen.WaitForDismissal();
            return 1;
        }

        // The wait ran out without the client staying mapped: STOPPED when the user
        // interrupted, TIMEOUT when it simply ran out.
        screen.SetFailed(true);
        screen.NewPage();
        if (g_cancelled.load()) {
            screen.Section("STOPPED");
            screen.Emit(Tui::yellow(), "Stopped waiting. Startup was not confirmed.");
        } else {
            screen.Section("TIMEOUT", Tui::yellow());
            screen.Emit(Tui::yellow(), "Slinky never confirmed it finished starting.");
        }
        screen.FlushPage();
        screen.WaitForDismissal();
        return 1;
    }

    std::cout << "Injected successfully into pid " << target.pid << ".\n";
    return 0;
}
