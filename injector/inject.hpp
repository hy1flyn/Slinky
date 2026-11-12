// Process discovery and library injection, with no opinion about the UI.
//
// The target checks mirror the reference Java launcher (Slinky-source/native/launcher.cpp):
// a process name says nothing about whether something is the game, because every Java
// application runs as java.exe or javaw.exe and a launcher such as HMCL looks exactly like
// the game to a name test. What does distinguish the game is LWJGL, the library that hosts
// its window: a launcher never loads it and the game cannot start without it.
#pragma once

#include <functional>
#include <ios>
#include <string>
#include <vector>

namespace Injector {

struct Process {
    unsigned long pid = 0;
    std::string   name;
    std::string   command_line;
    std::string   window_title;   // first visible window title, when one could be read
    bool is_minecraft = false;    // the process has LWJGL loaded (see IsMinecraftProcess)
    bool already_loaded = false;  // the client library is mapped in already
};

using LogFn = std::function<void(const std::string &)>;

std::vector<Process> ListProcesses();

// Finds the running Minecraft JVM.  On Linux that is a java process whose
// command line names the game; on Windows it is a process that has LWJGL loaded,
// preferring one that also owns a visible window.
bool FindMinecraft(Process &out, std::string &error);

// Finds a process by pid, exact name, or case-insensitive substring of the
// name/command line.  Accepts a decimal pid in `target`.
bool FindTarget(const std::string &target, Process &out, std::string &error);

// True when the process has LWJGL mapped.  This is the only reliable "is the game"
// test: launchers are Java windows too and their titles contain the word Minecraft.
bool IsMinecraftProcess(unsigned long pid);

// True when a module whose file name matches `library` is already mapped in `pid`.
// Used both to refuse a second injection and to confirm that one succeeded.
bool IsLibraryLoaded(unsigned long pid, const std::string &library);

// Resolves the path the client reports failures to, so a failed load can be explained.
std::string NativeLogPath();

// The log the native client appends to. It is shared by every process that loaded the
// client, so a caller that wants only its own run must take the size first and read
// from that offset.
std::string NativeLogPathFor(unsigned long pid);

// Current byte length of that log, or 0 when it does not exist yet. Reading the outcome
// of one injection means starting at the size recorded immediately before injecting.
std::streamoff NativeLogSize(unsigned long pid);

// True when this build carries a client library inside the executable.
bool HasBundledLibrary();

// Writes the bundled client library to a temporary folder and returns its path, or an
// empty string when this build has none or it could not be written. This is what lets
// the injector ship as a single file: the DLL never has to sit beside the .exe.
std::string ExtractBundledLibrary(std::string &error);

// How many processes look like the running game. More than one means the automatic
// choice is ambiguous, which the reference reports instead of picking one.
unsigned CountMinecraftCandidates();

// Loads `library` into `pid` through ptrace+dlopen (Linux) or
// CreateRemoteThread+LoadLibraryW (Windows).  Returns false on failure.
bool Inject(unsigned long pid, const std::string &library, const LogFn &log);

// Why injection may fail before it starts, for the error message.
std::string EnvironmentHint();

}  // namespace Injector
