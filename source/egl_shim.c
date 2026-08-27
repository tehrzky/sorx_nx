/* egl_shim.c -- dlopen/dlsym bridge to mesa EGL/GLES. SDL's Android backend
 * dlopen()s libEGL.so and resolves egl* by name; we answer with mesa.
 * MIT license; see LICENSE. */

#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "util.h"
#include "egl_shim.h"

extern int screen_width, screen_height;

#define FAKE_DL_HANDLE ((void *)0xE61D1B)

typedef void (*generic_func)(void);
typedef struct { const char *name; generic_func fn; } EglEntry;

#define E(sym) { #sym, (generic_func)sym }

// ---------------------------------------------------------------------------
// NEW: callback for surface recreation (fixes black screen on pak switch)
// ---------------------------------------------------------------------------
static void (*s_surface_cb)(void) = NULL;
static int s_in_surface_cb = 0;

void egl_shim_set_surface_cb(void (*cb)(void)) { s_surface_cb = cb; }

// ---------------------------------------------------------------------------

static EGLDisplay eglGetDisplay_log(EGLNativeDisplayType id) {
  EGLDisplay d = eglGetDisplay(id);
#if VERBOSE_EGL
  debugPrintf("[egl] eglGetDisplay(%p) -> %p\n", (void *)id, (void *)d);
#endif
  return d;
}

static EGLBoolean eglInitialize_log(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  EGLBoolean ok = eglInitialize(dpy, major, minor);
#if VERBOSE_EGL
  debugPrintf("[egl] eglInitialize(%p) -> %d (ver %d.%d) err=0x%x\n", (void *)dpy, ok,
              major ? *major : -1, minor ? *minor : -1, eglGetError());
#endif
  return ok;
}

static EGLBoolean eglChooseConfig_log(EGLDisplay dpy, const EGLint *attrs, EGLConfig *configs,
                                       EGLint config_size, EGLint *num_config) {
  EGLBoolean ok = eglChooseConfig(dpy, attrs, configs, config_size, num_config);
#if VERBOSE_EGL
  debugPrintf("[egl] eglChooseConfig -> %d, num_config=%d err=0x%x\n", ok,
              num_config ? *num_config : -1, eglGetError());
  if (attrs) {
    debugPrintf("[egl]   requested attrs:");
    for (int i = 0; attrs[i] != EGL_NONE; i += 2) debugPrintf(" [0x%x=0x%x]", attrs[i], attrs[i + 1]);
    debugPrintf("\n");
  }
  if (ok && num_config && *num_config > 0 && configs) {
    EGLint r, g, b, a, d, s, surf, renderable;
    eglGetConfigAttrib(dpy, configs[0], EGL_RED_SIZE, &r);
    eglGetConfigAttrib(dpy, configs[0], EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib(dpy, configs[0], EGL_BLUE_SIZE, &b);
    eglGetConfigAttrib(dpy, configs[0], EGL_ALPHA_SIZE, &a);
    eglGetConfigAttrib(dpy, configs[0], EGL_DEPTH_SIZE, &d);
    eglGetConfigAttrib(dpy, configs[0], EGL_STENCIL_SIZE, &s);
    eglGetConfigAttrib(dpy, configs[0], EGL_SURFACE_TYPE, &surf);
    eglGetConfigAttrib(dpy, configs[0], EGL_RENDERABLE_TYPE, &renderable);
    debugPrintf("[egl]   chosen[0]: RGBA=%d%d%d%d depth=%d stencil=%d surface_type=0x%x renderable=0x%x\n",
                r, g, b, a, d, s, surf, renderable);
  }
#endif
  return ok;
}

static EGLSurface eglCreateWindowSurface_log(EGLDisplay dpy, EGLConfig cfg,
                                              EGLNativeWindowType win, const EGLint *attrs) {
  EGLSurface s = eglCreateWindowSurface(dpy, cfg, win, attrs);
#if VERBOSE_EGL
  debugPrintf("[egl] eglCreateWindowSurface(win=%p) -> %p err=0x%x\n", (void *)win, (void *)s, eglGetError());
#endif
  if (s != EGL_NO_SURFACE && s_surface_cb && !s_in_surface_cb) {
    s_in_surface_cb = 1;
    s_surface_cb();
    s_in_surface_cb = 0;
  }
  return s;
}

static EGLContext eglCreateContext_log(EGLDisplay dpy, EGLConfig cfg, EGLContext share, const EGLint *attrs) {
  EGLContext c = eglCreateContext(dpy, cfg, share, attrs);
#if VERBOSE_EGL
  debugPrintf("[egl] eglCreateContext -> %p err=0x%x\n", (void *)c, eglGetError());
#endif
  return c;
}

static EGLBoolean eglMakeCurrent_log(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  EGLBoolean ok = eglMakeCurrent(dpy, draw, read, ctx);
#if VERBOSE_EGL
  debugPrintf("[egl] eglMakeCurrent(draw=%p read=%p ctx=%p) -> %d err=0x%x\n",
              (void *)draw, (void *)read, (void *)ctx, ok, eglGetError());
#endif
  return ok;
}

static EGLBoolean eglSwapBuffers_log(EGLDisplay dpy, EGLSurface surf) {
#if VERBOSE_EGL
  static int count = 0;
  int verbose = (count < 10) || (count % 60 == 0);
  if (verbose) {
    GLint fbo = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    debugPrintf("[egl] eglSwapBuffers #%d starting... (bound fbo=%d%s)\n", count, fbo,
                fbo == 0 ? " default/screen" : " OFFSCREEN");
    if (screen_width > 0 && screen_height > 0) {
      static uint8_t row[4096 * 4];
      int w = screen_width < 4096 ? screen_width : 4096;
      glReadPixels(0, screen_height / 2, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
      uint8_t minv = 255, maxv = 0;
      int nonbg = 0;
      for (int i = 0; i < w * 4; i++) {
        if (row[i] < minv) minv = row[i];
        if (row[i] > maxv) maxv = row[i];
      }
      for (int x = 0; x < w; x++) {
        if (row[x*4+0] || row[x*4+1] || row[x*4+2]) nonbg++;
      }
      uint8_t amin = 255, amax = 0;
      for (int x = 0; x < w; x++) {
        if (row[x*4+3] < amin) amin = row[x*4+3];
        if (row[x*4+3] > amax) amax = row[x*4+3];
      }
      debugPrintf("[egl]   readback center scanline (y=%d, w=%d): byte range [%u..%u], nonbg-pixels=%d/%d, alpha range [%u..%u]\n",
                  screen_height / 2, w, minv, maxv, nonbg, w, amin, amax);
    }
  }
#endif

  EGLBoolean ok = eglSwapBuffers(dpy, surf);
#if VERBOSE_EGL
  EGLint err = eglGetError();
  if (verbose || !ok || err != EGL_SUCCESS) {
    debugPrintf("[egl] eglSwapBuffers #%d -> %d err=0x%x\n", count, ok, err);
  }
  count++;
#endif
  return ok;
}

static const EglEntry egl_table[] = {
  E(eglGetError),
  { "eglGetDisplay", (generic_func)eglGetDisplay_log },
  { "eglInitialize", (generic_func)eglInitialize_log },
  E(eglTerminate),
  E(eglQueryString),
  E(eglGetConfigs),
  { "eglChooseConfig", (generic_func)eglChooseConfig_log },
  E(eglGetConfigAttrib),
  { "eglCreateWindowSurface", (generic_func)eglCreateWindowSurface_log },
  E(eglCreatePbufferSurface),
  E(eglCreatePixmapSurface),
  E(eglDestroySurface),
  E(eglQuerySurface),
  E(eglBindAPI),
  E(eglQueryAPI),
  E(eglWaitClient),
  E(eglReleaseThread),
  E(eglCreatePbufferFromClientBuffer),
  E(eglSurfaceAttrib),
  E(eglBindTexImage),
  E(eglReleaseTexImage),
  E(eglSwapInterval),
  { "eglCreateContext", (generic_func)eglCreateContext_log },
  E(eglDestroyContext),
  { "eglMakeCurrent", (generic_func)eglMakeCurrent_log },
  E(eglGetCurrentContext),
  E(eglGetCurrentSurface),
  E(eglGetCurrentDisplay),
  E(eglQueryContext),
  E(eglWaitGL),
  E(eglWaitNative),
  { "eglSwapBuffers", (generic_func)eglSwapBuffers_log },
  E(eglCopyBuffers),
  E(eglGetProcAddress),
};

static void *s_native_main_addr = NULL;

void egl_shim_set_native_main(void *addr) { s_native_main_addr = addr; }

void *dlopen_fake(const char *filename, int flag) {
  (void)filename; (void)flag;
  return FAKE_DL_HANDLE;
}

void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol)
    return NULL;
  if (strcmp(symbol, "SDL_main") == 0)
    return s_native_main_addr;
  for (unsigned i = 0; i < sizeof(egl_table) / sizeof(*egl_table); i++) {
    if (strcmp(symbol, egl_table[i].name) == 0)
      return (void *)egl_table[i].fn;
  }
  return (void *)eglGetProcAddress(symbol);
}

int dlclose_fake(void *handle) { (void)handle; return 0; }

char *dlerror_fake(void) { return NULL; }
