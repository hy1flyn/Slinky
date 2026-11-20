// What the portable client code needs from the operating system (implemented in src/platform/<os>/).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// ---- implemented by the platform layer ----
void *slinky_gl_get_proc(const char *name);        // OpenGL entry point by name (needs a current context)
// ---- implemented by the client (src/engine/protected/init.cpp), called by the platform layer ----
int slinky_setup(void *self);                       // start-up; runs on its own thread. 0 = ok, else an error code
int slinky_teardown(void);                          // frees everything slinky_setup built; 0 = hooks still live, keep the DLL loaded
#ifdef __cplusplus
}
#endif
