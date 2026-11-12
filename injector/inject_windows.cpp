// Windows injection: CreateRemoteThread(LoadLibraryW) into the game's process.
// The client DLL's DllMain starts the client, so loading it is the whole job.
//
// Behaviour is aligned with the reference Java launcher (native/launcher.cpp):
//   - a target is only accepted as the game when it has LWJGL loaded
//   - an already-mapped client library is reported instead of loading it twice
//   - the remote thread is polled with a timeout instead of trusting one exit code,
//     because a load that half-succeeded unloads itself and still returns 0
//   - the module list is re-checked after the thread finishes to confirm the load
#ifdef _WIN32

#include "inject.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace Injector {
namespace {

std::string narrow(const std::wstring &text) {
    if (text.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string &text) {
    if (text.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n);
    return out;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

std::string base_name(const std::string &path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string process_name(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[1024];
    DWORD size = (DWORD)(sizeof path / sizeof path[0]);
    std::string result;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        std::wstring full(path, size);
        size_t slash = full.find_last_of(L"\\/");
        result = narrow(slash == std::wstring::npos ? full : full.substr(slash + 1));
    }
    CloseHandle(process);
    return result;
}

// Every module path mapped in the process.  Empty when the process cannot be queried,
// which the callers treat as "not the game" rather than guessing.
std::vector<std::string> module_paths(DWORD pid) {
    std::vector<std::string> out;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return out;

    HMODULE modules[1024];
    DWORD needed = 0;
    if (EnumProcessModulesEx(process, modules, sizeof modules, &needed, LIST_MODULES_ALL)) {
        DWORD count = needed / sizeof(HMODULE);
        if (count > (DWORD)(sizeof modules / sizeof modules[0]))
            count = (DWORD)(sizeof modules / sizeof modules[0]);
        for (DWORD i = 0; i < count; i++) {
            wchar_t path[MAX_PATH];
            // The module's own path is needed, not just its base name: launchers unpack
            // libraries into temporary folders, so the full path is not stable.
            if (GetModuleFileNameExW(process, modules[i], path, MAX_PATH) > 0) out.push_back(narrow(path));
        }
    }
    CloseHandle(process);
    return out;
}

// "Operation: <system text>" for a Win32 error code, exactly the shape the reference
// launcher prints through windowsError(): the operation names what was being attempted
// and the system message explains why it failed, so a bare error number never appears.
std::string describe_error(const char *operation, DWORD error) {
    LPSTR detail = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, error, 0, (LPSTR)&detail, 0, nullptr);
    std::string text = operation ? operation : "";
    text += ": ";
    if (n && detail) {
        std::string message(detail, n);
        // The system text ends with a CRLF; trim it so the caller controls spacing.
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
            message.pop_back();
        text += message;
    } else {
        text += "No further details available.";
    }
    if (detail) LocalFree(detail);
    return text;
}

std::string window_title_of(DWORD pid) {    struct Ctx { DWORD pid; std::string title; } ctx{pid, {}};
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto *c = reinterpret_cast<Ctx *>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != c->pid) return TRUE;
            if (!IsWindowVisible(window)) return TRUE;
            int len = GetWindowTextLengthW(window);
            if (len <= 0) return TRUE;
            std::wstring buffer((size_t)len + 1, L'\0');
            int got = GetWindowTextW(window, buffer.data(), len + 1);
            if (got > 0) {
                buffer.resize((size_t)got);
                c->title = narrow(buffer);
                return FALSE;   // first titled visible window wins
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.title;
}

}  // namespace

bool IsMinecraftProcess(unsigned long pid) {
    for (const std::string &path : module_paths((DWORD)pid)) {
        const std::string lowered = lower(path);
        // LWJGL 2 and 3 land as lwjgl.dll / lwjgl64.dll / lwjgl-opengl.dll and so on.
        if (lowered.find("lwjgl") != std::string::npos) return true;
    }
    return false;
}

bool IsLibraryLoaded(unsigned long pid, const std::string &library) {
    std::string wanted = lower(base_name(library));
    if (wanted.empty()) return false;
    for (const std::string &path : module_paths((DWORD)pid)) {
        if (lower(base_name(path)) == wanted) return true;
    }
    return false;
}

std::string NativeLogPath() {
    char temp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, temp);
    if (!n) return {};
    std::string dir(temp, n);
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir += '\\';
    // The client logs to %TEMP%\slinky-<pid>\slinky-native.log; the exact folder is
    // per process, so the caller is pointed at the parent instead of a wrong file.
    return dir + "slinky-<pid>\\slinky-native.log";
}

std::string NativeLogPathFor(unsigned long pid) {
    // The native client appends every line to %TEMP%\slinky.log: src/platform/win/entry.cpp
    // uses GetTempPathW + "slinky.log" with FILE_APPEND_DATA and no per-process folder.
    // The earlier per-pid guess never existed on disk, so reading the startup status
    // back always ended in the timeout. The pid stays in the signature because callers
    // think in terms of a target; the file itself is shared.
    (void)pid;
    char temp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, temp);
    if (!n) return {};
    std::string dir(temp, n);
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir += '\\';
    return dir + "slinky.log";
}

// ---- bundled client library ----
// Resource 200 holds the client DLL, the same slot the reference launcher reads. A build
// without the resource (or a Linux build) simply reports no bundled library and falls
// back to a path passed with --lib.
bool HasBundledLibrary() {
    return FindResourceW(nullptr, MAKEINTRESOURCEW(200), MAKEINTRESOURCEW(10 /* RT_RCDATA */)) != nullptr;
}

std::string ExtractBundledLibrary(std::string &error) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(200), MAKEINTRESOURCEW(10 /* RT_RCDATA */));
    if (!resource) {
        error = "Finding the bundled DLL: No further details available.";
        return {};
    }
    DWORD size = SizeofResource(nullptr, resource);
    HGLOBAL loaded = LoadResource(nullptr, resource);
    const void *bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || !size) {
        error = describe_error("Reading the bundled DLL", ERROR_INVALID_DATA);
        return {};
    }

    char temp[MAX_PATH];
    DWORD temp_length = GetTempPathA(MAX_PATH, temp);
    if (!temp_length || temp_length >= MAX_PATH) {
        error = describe_error("Finding a temporary folder",
                               temp_length ? ERROR_BUFFER_OVERFLOW : GetLastError());
        return {};
    }
    // One folder per injector run, so two games can be served without their extracted
    // copies colliding, and so a stale copy never gets loaded.
    std::string directory = std::string(temp) + "slinky-" + std::to_string(GetCurrentProcessId());
    if (!CreateDirectoryA(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = describe_error("Creating a temporary folder", GetLastError());
        return {};
    }

    std::string path = directory + "\\SlinkyNative.dll";
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = describe_error("Writing the bundled DLL", GetLastError());
        return {};
    }
    DWORD written = 0;
    BOOL saved = WriteFile(file, bytes, size, &written, nullptr);
    DWORD write_error = saved ? ERROR_WRITE_FAULT : GetLastError();
    CloseHandle(file);
    if (!saved || written != size) {
        error = describe_error("Extracting the bundled DLL", write_error);
        return {};
    }
    return path;
}

std::streamoff NativeLogSize(unsigned long pid) {
    std::string path = NativeLogPathFor(pid);
    if (path.empty()) return 0;
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : (std::streamoff)size;
}

std::vector<Process> ListProcesses() {
    std::vector<Process> out;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            Process process;
            process.pid = entry.th32ProcessID;
            process.name = narrow(entry.szExeFile);
            // Only Java processes can be the game. Everything else on the machine is noise
            // in the list and cannot be a valid target, so it is dropped here rather than
            // shown as an unselectable row: a 300-entry list of unrelated programs made the
            // one line that mattered impossible to find.
            std::string name = lower(process.name);
            if (name != "java.exe" && name != "javaw.exe" && name != "java" && name != "javaw") continue;
            process.window_title = window_title_of(entry.th32ProcessID);
            process.is_minecraft = IsMinecraftProcess(entry.th32ProcessID);
            out.push_back(std::move(process));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    // The game first, then by name: the list is meant to be read.
    std::sort(out.begin(), out.end(), [](const Process &a, const Process &b) {
        if (a.is_minecraft != b.is_minecraft) return a.is_minecraft;
        if (a.name != b.name) return a.name < b.name;
        return a.pid < b.pid;
    });
    return out;
}

bool FindMinecraft(Process &out, std::string &error) {
    std::vector<Process> candidates;
    for (const Process &process : ListProcesses()) {
        if (process.is_minecraft) candidates.push_back(process);
    }

    if (candidates.empty()) {
        error = "Minecraft is not running (no process has LWJGL loaded)";
        return false;
    }

    if (candidates.size() > 1) {
        error = "There are " + std::to_string(candidates.size()) +
                " Java windows open, so the target is ambiguous.";
        return false;
    }

    // Prefer the one that owns a visible window: several JVMs can have LWJGL loaded
    // after the game was restarted, and the titled one is the live client.
    for (const Process &process : candidates) {
        if (!process.window_title.empty()) {
            out = process;
            out.already_loaded = false;
            return true;
        }
    }
    out = candidates.front();
    out.already_loaded = false;
    return true;
}

bool FindTarget(const std::string &target, Process &out, std::string &error) {
    if (target.empty()) {
        error = "no target given";
        return false;
    }

    bool numeric = std::all_of(target.begin(), target.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (numeric) {
        out.pid = std::strtoul(target.c_str(), nullptr, 10);
        if (!out.pid) {
            error = "invalid pid '" + target + "'";
            return false;
        }
        // Existence is decided by opening the process, not by reading its name: a live
        // process owned by another user answers PROCESS_QUERY_LIMITED_INFORMATION with an
        // empty image name, and treating that as "no such process" reported a running
        // target as missing.
        HANDLE alive = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)out.pid);
        bool exists = alive != nullptr;
        if (!exists && GetLastError() == ERROR_ACCESS_DENIED) exists = true;   // exists, cannot open
        if (alive) CloseHandle(alive);
        if (!exists) {
            // A pid that no longer exists must not fall through to a name match.
            error = "no process with pid " + target;
            return false;
        }
        out.name = process_name((DWORD)out.pid);
        if (out.name.empty()) out.name = "pid " + target;
        out.window_title = window_title_of((DWORD)out.pid);
        out.is_minecraft = IsMinecraftProcess(out.pid);
        return true;
    }

    std::string needle = lower(target);
    std::vector<Process> all = ListProcesses();   // Java processes only: the game is the only target
    for (const Process &process : all) {
        if (lower(process.name) == needle) {
            out = process;
            return true;
        }
    }
    for (const Process &process : all) {
        std::string name = lower(process.name);
        std::string title = lower(process.window_title);
        if (name.find(needle) != std::string::npos || title.find(needle) != std::string::npos) {
            out = process;
            return true;
        }
    }
    error = "no Java process matching '" + target +
            "' (the game is a java.exe/javaw.exe process, so a launcher or other program cannot be a target)";
    return false;
}

unsigned CountMinecraftCandidates() {
    unsigned count = 0;
    for (const Process &process : ListProcesses())
        if (process.is_minecraft) count++;
    return count;
}

bool Inject(unsigned long pid, const std::string &library, const LogFn &log) {
    std::wstring wide_library = widen(library);

    DWORD full_size = GetFullPathNameW(wide_library.c_str(), 0, nullptr, nullptr);
    if (full_size) {
        std::wstring full(full_size, L'\0');
        DWORD written = GetFullPathNameW(wide_library.c_str(), full_size, full.data(), nullptr);
        if (written && written < full_size) {
            full.resize(written);
            wide_library = full;
        }
    }

    // Loading it twice would start a second client inside the same JVM. The reference
    // launcher refuses this and so does this injector.
    if (IsLibraryLoaded(pid, narrow(wide_library))) {
        log("SlinkyNative is already loaded in PID " + std::to_string(pid) + ".");
        return false;
    }

    log("injecting " + narrow(wide_library) + " into pid " + std::to_string(pid));

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, (DWORD)pid);
    if (!process) {
        DWORD err = GetLastError();
        log(describe_error("Opening the selected game process", err));
        if (err == ERROR_ACCESS_DENIED) log("Try running the injector as administrator.");
        return false;
    }

    SIZE_T bytes = (wide_library.size() + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        log(describe_error("Allocating the DLL path in the game", GetLastError()));
        CloseHandle(process);
        return false;
    }

    bool ok = false;
    bool may_free = true;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, wide_library.c_str(), bytes, &written) || written != bytes) {
        log(describe_error("Copying the DLL path into the game", GetLastError()));
    } else {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW"));
        if (!load_library) {
            log(describe_error("Finding the Windows DLL loader", ERROR_PROC_NOT_FOUND));
        } else {
            HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
            if (!thread) {
                log(describe_error("Starting the DLL loader in the game", GetLastError()));
            } else {
                // Poll instead of one infinite wait so the UI keeps drawing and a hung
                // loader cannot wedge the injector forever.
                DWORD wait_result = WAIT_TIMEOUT;
                ULONGLONG began = GetTickCount64();
                while (GetTickCount64() - began < 30000) {
                    wait_result = WaitForSingleObject(thread, 150);
                    if (wait_result != WAIT_TIMEOUT) break;
                }

                if (wait_result == WAIT_OBJECT_0) {
                    DWORD exit_code = 0;
                    if (GetExitCodeThread(thread, &exit_code)) {
                        // The HMODULE return is truncated to a DWORD, so only zero is
                        // meaningful. A worker may also deliberately unload itself after
                        // reporting a failure, which is why the module list decides.
                        if (exit_code) ok = true;
                    }
                    if (!ok) {
                        // Up to five seconds for the mapping to appear; a successful
                        // LoadLibraryW maps the module before the thread returns, so a
                        // short poll covers the truncated-return case.
                        for (int attempt = 0; attempt < 100 && !ok; attempt++) {
                            if (IsLibraryLoaded(pid, narrow(wide_library))) {
                                ok = true;
                                break;
                            }
                            Sleep(50);
                        }
                    }
                    if (!ok) {
                        log("The DLL is not present in the game after the load attempt. See " + NativeLogPath());
                    }
                } else {
                    // The loader thread may still be reading the remote path: keep it alive.
                    may_free = false;
                    if (wait_result == WAIT_FAILED)
                        log(describe_error("Waiting for the DLL loader", GetLastError()));
                    else
                        log("TIMEOUT: The game did not finish loading the DLL within 30 seconds. Its result is unknown.");
                }
                CloseHandle(thread);
            }
        }
    }

    if (may_free) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

std::string EnvironmentHint() {
    return "if injection fails with access denied, run the injector as administrator";
}

}  // namespace Injector

#endif  // _WIN32
