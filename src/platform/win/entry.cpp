// Windows entry of the client DLL: it is injected into the game process (LoadLibrary), so DllMain only starts the
// start-up thread (slinky_setup runs JNI and file I/O, which must not happen under the loader lock).
#include "slinky/platform.h"
#include "slinky/win32.h"

extern "C" void start_unload_thread(HMODULE self);   // protected/input.cpp: unload thread (teardown, then FreeLibraryAndExitThread)

static DWORD start_thread(LPVOID self) {
    if (slinky_setup(self)) start_unload_thread((HMODULE)self);   // failed: report was printed, take the DLL out again
    return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, void *) {
    if (reason == 1 /* DLL_PROCESS_ATTACH */) {
        DisableThreadLibraryCalls(self);
        CloseHandle(CreateThread(0, 0, start_thread, self, 0, 0));
    }
    return 1;
}

// ---- OpenGL: wglGetProcAddress for extensions, opengl32 itself for GL 1.1 ----
typedef void *(WINAPI *WglGetProcAddress)(const char *);
extern "C" void *slinky_gl_get_proc(const char *name) {
    static HMODULE gl;
    static WglGetProcAddress wgl;
    if (!gl) {
        gl = GetModuleHandleA("opengl32");
        if (!gl) gl = LoadLibraryA("opengl32.dll");
        if (!gl) return 0;
        wgl = (WglGetProcAddress)GetProcAddress(gl, "wglGetProcAddress");
    }
    void *p = wgl ? wgl(name) : 0;
    if (p && p != (void *)1 && p != (void *)2 && p != (void *)3 && p != (void *)-1) return p;   // wgl failure values
    return GetProcAddress(gl, name);
}
