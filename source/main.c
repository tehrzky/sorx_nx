/* main.c -- OpenBOR Switch wrapper entry point.
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "patch.h"
#include "egl_shim.h"
#include "libc_shim.h"
#include "gpuarena.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module hidapi_mod, sdl2_mod, openbor_mod;

#define SO_HEAP_RESERVE (64 * 1024 * 1024)

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_reserve = SO_HEAP_RESERVE;
  if (so_reserve > size / 2)
    so_reserve = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t fake_heap_size = size - so_reserve;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(HIDAPI_SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", HIDAPI_SO_NAME);
  if (stat(SDL2_SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", SDL2_SO_NAME);
  if (stat(OPENBOR_SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", OPENBOR_SO_NAME);
  mkdir("data", 0777);
  mkdir("Paks", 0777);
  mkdir("extracted", 0777);
}

static void resolve_data_root(void) {
  char cwd[256];
  if (!getcwd(cwd, sizeof(cwd)) || !cwd[0]) return;
  char *colon = strchr(cwd, ':');
  char *base = colon ? colon + 1 : cwd;
  if (!base[0]) return;
  size_t l = strlen(base);
  while (l > 1 && base[l - 1] == '/') base[--l] = 0;
  // Always use the actual NRO directory, regardless of what config.txt says
  snprintf(config.data_root, sizeof(config.data_root), "%s", base);
  snprintf(config.save_root, sizeof(config.save_root), "%s/save", base);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
}

static int  (*e_nativeSetupJNI)(void *env);
static int  (*e_audioSetupJNI)(void *env);
static int  (*e_controllerSetupJNI)(void *env);
static void (*e_onNativeSurfaceCreated)(void *env, void *cls);
static void (*e_onNativeSurfaceChanged)(void *env, void *cls);
static void (*e_onNativeSurfaceDestroyed)(void *env, void *cls);
static void (*e_onNativeResize)(void *env, void *cls);
static void (*e_nativeRunMain)(void *env, void *cls, void *library, void *function, void *args);
static void (*e_nativePause)(void *env, void *cls);
static void (*e_nativeResume)(void *env, void *cls);
static void (*e_nativeQuit)(void *env, void *cls);
static void (*e_onNativeKeyDown)(void *env, void *cls, int keycode);
static void (*e_onNativeKeyUp)(void *env, void *cls, int keycode);
static int  (*e_JNI_OnLoad)(void *vm, void *reserved);
static int  (*e_nativeAddJoystick)(void *env, void *cls, int device_id, void *name, void *desc,
                                    int vendor_id, int product_id, int is_accelerometer,
                                    int button_mask, int naxes, int axis_mask, int nhats);
static void (*e_onNativePadDown)(void *env, void *cls, int device_id, int keycode);
static void (*e_onNativePadUp)(void *env, void *cls, int device_id, int keycode);
static void (*e_onNativeJoy)(void *env, void *cls, int device_id, int axis, float value);
static void (*e_onNativeHat)(void *env, void *cls, int device_id, int hat_id, int x, int y);
static void (*e_onNativeTouch)(void *env, void *cls, int touch_device_id, int pointer_finger_id,
                                int action, float x, float y, float pressure);
static void (*e_nativeSetScreenResolution)(void *env, void *cls, int surfaceWidth, int surfaceHeight,
                                            int deviceWidth, int deviceHeight, int format, float rate);

static void resolve_entry_points(void) {
  e_JNI_OnLoad               = (void *)so_try_find_addr_rx(&sdl2_mod, "JNI_OnLoad");
  e_nativeSetupJNI           = (void *)so_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativeSetupJNI");
  e_audioSetupJNI            = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLAudioManager_nativeSetupJNI");
  e_controllerSetupJNI       = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_nativeSetupJNI");
  e_onNativeSurfaceCreated   = (void *)so_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeSurfaceCreated");
  e_onNativeSurfaceChanged   = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeSurfaceChanged");
  e_onNativeSurfaceDestroyed = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeSurfaceDestroyed");
  e_onNativeResize           = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeResize");
  e_nativeRunMain            = (void *)so_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativeRunMain");
  e_nativePause              = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativePause");
  e_nativeResume             = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativeResume");
  e_nativeQuit               = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativeQuit");
  e_onNativeKeyDown          = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeKeyDown");
  e_onNativeKeyUp            = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeKeyUp");
  e_nativeAddJoystick        = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_nativeAddJoystick");
  e_onNativePadDown          = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_onNativePadDown");
  e_onNativePadUp            = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_onNativePadUp");
  e_onNativeJoy              = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_onNativeJoy");
  e_onNativeHat              = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_onNativeHat");
  e_onNativeTouch            = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeTouch");
  e_nativeSetScreenResolution = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativeSetScreenResolution");
  imports_set_real_rendercopy((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_RenderCopy"));
  imports_set_real_getdesktopdisplaymode((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_GetDesktopDisplayMode"));
  imports_set_real_createtexture((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_CreateTexture"));
  imports_set_real_createtexturefromsurface((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_CreateTextureFromSurface"));
  imports_set_real_updatetexture((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_UpdateTexture"));
  imports_set_real_destroytexture((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_DestroyTexture"));
  imports_set_real_renderclear((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_RenderClear"));
  imports_set_real_renderpresent((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_RenderPresent"));
  imports_set_real_createthread((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_CreateThread"));
  imports_set_real_waitthread((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_WaitThread"));
  imports_set_real_lockmutex((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_LockMutex"));
  imports_set_real_condwait((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_CondWait"));
}

#define JOY_DEVICE_ID 0

#define AKEYCODE_DPAD_UP    19
#define AKEYCODE_DPAD_DOWN  20
#define AKEYCODE_DPAD_LEFT  21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_ENTER      66
#define AKEYCODE_ESCAPE     111
#define AKEYCODE_A          29
#define AKEYCODE_S          47
#define AKEYCODE_Z          54
#define AKEYCODE_X          52
#define AKEYCODE_D          32
#define AKEYCODE_F          34

static PadState pad;

static struct { u64 sw; int key; } s_btnmap[] = {
  { HidNpadButton_A,      AKEYCODE_A },
  { HidNpadButton_X,      AKEYCODE_S },
  { HidNpadButton_B,      AKEYCODE_S },
  { HidNpadButton_Y,      AKEYCODE_Z },
  { HidNpadButton_L,      AKEYCODE_X },
  { HidNpadButton_R,      AKEYCODE_D },
  { HidNpadButton_ZL,     AKEYCODE_F },
  { HidNpadButton_ZL,     AKEYCODE_F },
  { HidNpadButton_ZR,     AKEYCODE_ESCAPE },
  { HidNpadButton_Plus,   AKEYCODE_ENTER },
  { HidNpadButton_Minus,  AKEYCODE_ESCAPE },
  { HidNpadButton_Up,     AKEYCODE_DPAD_UP },
  { HidNpadButton_Down,   AKEYCODE_DPAD_DOWN },
  { HidNpadButton_Left,   AKEYCODE_DPAD_LEFT },
  { HidNpadButton_Right,  AKEYCODE_DPAD_RIGHT },
};

static u64 s_prev_buttons = 0;

static void poll_input(void) {
  void *cls = jni_activity_class();
  padUpdate(&pad);
  const u64 cur = padGetButtons(&pad);

  for (unsigned i = 0; i < sizeof(s_btnmap) / sizeof(*s_btnmap); i++) {
    const u64 m = s_btnmap[i].sw;
    if ((cur & m) && !(s_prev_buttons & m)) {
      debugPrintf("[input] Down %d (key fn=%p pad fn=%p)\n", s_btnmap[i].key, (void *)e_onNativeKeyDown, (void *)e_onNativePadDown);
      if (e_onNativeKeyDown) e_onNativeKeyDown(fake_env, cls, s_btnmap[i].key);
      if (e_onNativePadDown) e_onNativePadDown(fake_env, cls, JOY_DEVICE_ID, s_btnmap[i].key);
    } else if (!(cur & m) && (s_prev_buttons & m)) {
      debugPrintf("[input] Up %d (key fn=%p pad fn=%p)\n", s_btnmap[i].key, (void *)e_onNativeKeyUp, (void *)e_onNativePadUp);
      if (e_onNativeKeyUp) e_onNativeKeyUp(fake_env, cls, s_btnmap[i].key);
      if (e_onNativePadUp) e_onNativePadUp(fake_env, cls, JOY_DEVICE_ID, s_btnmap[i].key);
    }
  }
  s_prev_buttons = cur;

  if (e_onNativeJoy) {
    HidAnalogStickState l = padGetStickPos(&pad, 0);
    e_onNativeJoy(fake_env, cls, JOY_DEVICE_ID, 0, l.x / 32767.0f);
    e_onNativeJoy(fake_env, cls, JOY_DEVICE_ID, 1, -l.y / 32767.0f);
  }

  static int was_up = 0, was_down = 0, was_left = 0, was_right = 0;
  HidAnalogStickState l = padGetStickPos(&pad, 0);
  const int dz = 0x1A00;
  int is_up = l.y > dz, is_down = l.y < -dz, is_left = l.x < -dz, is_right = l.x > dz;
  struct { int is, was, key; } axes[4] = {
    { is_up, was_up, AKEYCODE_DPAD_UP },
    { is_down, was_down, AKEYCODE_DPAD_DOWN },
    { is_left, was_left, AKEYCODE_DPAD_LEFT },
    { is_right, was_right, AKEYCODE_DPAD_RIGHT },
  };
  for (int i = 0; i < 4; i++) {
    if (axes[i].is && !axes[i].was) {
      if (e_onNativeKeyDown) e_onNativeKeyDown(fake_env, cls, axes[i].key);
      if (e_onNativePadDown) e_onNativePadDown(fake_env, cls, JOY_DEVICE_ID, axes[i].key);
    } else if (!axes[i].is && axes[i].was) {
      if (e_onNativeKeyUp) e_onNativeKeyUp(fake_env, cls, axes[i].key);
      if (e_onNativePadUp) e_onNativePadUp(fake_env, cls, JOY_DEVICE_ID, axes[i].key);
    }
  }
  was_up = is_up; was_down = is_down; was_left = is_left; was_right = is_right;

  if (e_onNativeHat) {
    static int hat_x = 0, hat_y = 0;
    int nx = ((cur & HidNpadButton_Right) || is_right) - ((cur & HidNpadButton_Left) || is_left);
    int ny = ((cur & HidNpadButton_Up) || is_up) - ((cur & HidNpadButton_Down) || is_down);
    if (nx != hat_x || ny != hat_y) {
      hat_x = nx; hat_y = ny;
      e_onNativeHat(fake_env, cls, JOY_DEVICE_ID, 0, hat_x, hat_y);
    }
  }

  {
    static int was_touching = 0;
    HidTouchScreenState ts = {0};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
      float nx = (float)ts.touches[0].x / 1280.0f;
      float ny = (float)ts.touches[0].y / 720.0f;
      int action = was_touching ? 2 : 0;
      if (!was_touching)
        debugPrintf("[input] touch DOWN raw=(%u,%u) norm=(%.3f,%.3f) fn=%p\n",
                    ts.touches[0].x, ts.touches[0].y, nx, ny, (void *)e_onNativeTouch);
      if (e_onNativeTouch) e_onNativeTouch(fake_env, cls, 0, 0, action, nx, ny, 1.0f);
      was_touching = 1;
    } else if (was_touching) {
      debugPrintf("[input] touch UP fn=%p\n", (void *)e_onNativeTouch);
      if (e_onNativeTouch) e_onNativeTouch(fake_env, cls, 0, 0, 1, 0.0f, 0.0f, 0.0f);
      was_touching = 0;
    }
  }
}

static Thread s_sdl_thread;
static volatile int s_sdl_thread_done = 0;

static void sdl_thread_fn(void *arg) {
  (void)arg;
  tls_setup_guard();
  void *cls = jni_activity_class();
  debugPrintf(">> SDL thread: nativeRunMain...\n");
  e_nativeRunMain(fake_env, cls, jni_new_string(OPENBOR_SO_NAME), jni_new_string("SDL_main"), NULL);
  debugPrintf(">> SDL thread: SDL_main returned\n");
  s_sdl_thread_done = 1;
}

// One call per *.pak (not per asset, see vpak_index_all()'s own comment --
// indexing no longer touches individual asset bytes at all, so there's no
// long silent gap left to paper over here): total is small (MAX_CATALOGS
// is 8), so just print each catalog by name as it starts, plus a final
// summary line. This is the only point in the whole run where an on-screen
// progress line is safe to draw at all -- see this function's call site
// below for why (SDL/EGL hasn't touched the screen yet here; it has for
// the entire rest of the process's life after that point).
static void extract_progress_cb(uint32_t done, uint32_t total, const char *name) {
  if (done >= total)
    printf("Found %u game(s).\n", (unsigned)total);
  else
    printf("Indexing... (%u/%u) %s\n", (unsigned)(done + 1), (unsigned)total, name);
  consoleUpdate(NULL);
}

int main(void) {
  cpu_boost(1);

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();
  resolve_data_root();
  mkdir(config.save_root, 0777);
  setenv("HOME", config.save_root, 1);
  chdir(config.data_root);

  // Index game catalogs before any SDL/EGL context exists.
  // Fast every boot (see vpak_index_all()'s own comment): this only reads
  // each pak's small catalog header now, never the game's actual asset
  // bytes, so it stays quick even with several large games installed side
  // by side.
  vpak_index_all(NULL);

  set_screen_size(config.screen_width, config.screen_height);

  debugPrintf("== OpenBOR Switch wrapper booting; data_root=%s ==\n", config.data_root);

  void *base = heap_so_base;
  size_t remaining = heap_so_limit;

  if (so_load(&hidapi_mod, HIDAPI_SO_NAME, base, remaining) < 0)
    fatal_error("Could not load\n%s.", HIDAPI_SO_NAME);
  base = (char *)base + hidapi_mod.load_size; remaining -= hidapi_mod.load_size;
  sorx_resolve_imports(&hidapi_mod);

  if (so_load(&sdl2_mod, SDL2_SO_NAME, base, remaining) < 0)
    fatal_error("Could not load\n%s.", SDL2_SO_NAME);
  base = (char *)base + sdl2_mod.load_size; remaining -= sdl2_mod.load_size;
  sorx_resolve_imports(&sdl2_mod);

  if (so_load(&openbor_mod, OPENBOR_SO_NAME, base, remaining) < 0)
    fatal_error("Could not load\n%s.", OPENBOR_SO_NAME);
  base = (char *)base + openbor_mod.load_size; remaining -= openbor_mod.load_size;
  sorx_resolve_imports(&openbor_mod);

  debugPrintf("== all modules loaded + resolved ==\n");

  so_patch(&openbor_mod);

  resolve_entry_points();
  if (!e_nativeSetupJNI || !e_onNativeSurfaceCreated || !e_nativeRunMain)
    fatal_error("Could not resolve SDLActivity entry points.");



  uintptr_t sdl_main_addr = so_try_find_addr_rx(&openbor_mod, "SDL_main");
  if (!sdl_main_addr)
    fatal_error("Could not find SDL_main in\n%s.", OPENBOR_SO_NAME);
  egl_shim_set_native_main((void *)sdl_main_addr);

  so_finalize(&hidapi_mod);  so_flush_caches(&hidapi_mod);
  so_finalize(&sdl2_mod);    so_flush_caches(&sdl2_mod);
  so_finalize(&openbor_mod); so_flush_caches(&openbor_mod);
  debugPrintf("== so_finalize ok; running init_arrays ==\n");

  tls_setup_guard();
  so_execute_init_array(&hidapi_mod);
  so_execute_init_array(&sdl2_mod);
  so_execute_init_array(&openbor_mod);
  so_free_temp(&hidapi_mod);
  so_free_temp(&sdl2_mod);
  so_free_temp(&openbor_mod);
  debugPrintf("== init_arrays done ==\n");

  gpua_enable();

  jni_init();

  void *cls = jni_activity_class();
  if (e_JNI_OnLoad) { debugPrintf(">> JNI_OnLoad...\n"); e_JNI_OnLoad(fake_vm, NULL); }
  debugPrintf(">> nativeSetupJNI...\n");
  e_nativeSetupJNI(fake_env);
  if (e_audioSetupJNI) e_audioSetupJNI(fake_env);
  if (e_controllerSetupJNI) e_controllerSetupJNI(fake_env);
  if (e_nativeAddJoystick) {
    e_nativeAddJoystick(fake_env, cls, JOY_DEVICE_ID, jni_new_string("Switch Controller"),
                         jni_new_string("Switch Controller"), 0, 0, 0,
                         0xFFFFFFFF, 2, 0x3, 1);
    debugPrintf(">> nativeAddJoystick done\n");
  }
  debugPrintf(">> resolved: onNativeTouch=%p onNativeKeyDown=%p onNativePadDown=%p\n",
              (void *)e_onNativeTouch, (void *)e_onNativeKeyDown, (void *)e_onNativePadDown);
  debugPrintf(">> onNativeSurfaceCreated...\n");
  e_onNativeSurfaceCreated(fake_env, cls);
  if (e_nativeSetScreenResolution)
    e_nativeSetScreenResolution(fake_env, cls, screen_width, screen_height,
                                 screen_width, screen_height, 1, 60.0f);
  debugPrintf(">> nativeSetScreenResolution(%d,%d)%s\n", screen_width, screen_height,
              e_nativeSetScreenResolution ? "" : " -- NOT FOUND");
  if (e_onNativeResize) e_onNativeResize(fake_env, cls);
  if (e_onNativeSurfaceChanged) e_onNativeSurfaceChanged(fake_env, cls);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  if (R_FAILED(threadCreate(&s_sdl_thread, sdl_thread_fn, NULL, NULL, 4 * 1024 * 1024, 0x3B, -2)))
    fatal_error("Could not create the SDL thread.");
  threadStart(&s_sdl_thread);

  if (e_nativeResume) e_nativeResume(fake_env, cls);

  int s_focused = 1, s_paused = 0;
  uint64_t loop_iters = 0;
  debugPrintf(">> entering main loop, initial appletGetFocusState()=%d (InFocus=%d)\n",
              appletGetFocusState(), AppletFocusState_InFocus);

  uint64_t last_pak = pak_bytes_total();
  int boosted = 1;
  int idle_frames = 0;
#define BOOST_IDLE_TICKS 900
    int frame_counter = 0;
    while (appletMainLoop() && !s_sdl_thread_done) {
    // Re-assert screen resolution every ~1 second in case SDL's Android
    // surface globals were reset during pak switch / surface recreation.
    // Doing this from the main thread avoids EGL-thread safety issues.
    if (++frame_counter % 60 == 0 && e_nativeSetScreenResolution) {
      e_nativeSetScreenResolution(fake_env, cls, screen_width, screen_height,
                                   screen_width, screen_height, 1, 60.0f);
    }

    AppletFocusState fs = appletGetFocusState();
    int focused = (fs == AppletFocusState_InFocus);
    if (focused != s_focused) {
      debugPrintf("[focus] changed: %d -> %d (fs=%d) at iter %llu\n", s_focused, focused, fs, (unsigned long long)loop_iters);
      s_focused = focused;
      if (!s_focused && !s_paused && e_nativePause) { e_nativePause(fake_env, cls); s_paused = 1; }
      else if (s_focused && s_paused && e_nativeResume) { e_nativeResume(fake_env, cls); s_paused = 0; }
    }
    if (s_focused) poll_input();
#if VERBOSE_IO
    if (loop_iters % 120 == 0) debugPrintf("[main] loop heartbeat iter=%llu focused=%d\n", (unsigned long long)loop_iters, s_focused);
#endif
    if (loop_iters % 312 == 0) log_cpu_clock_periodic();
    uint64_t cur_pak = pak_bytes_total();
    if (cur_pak - last_pak > 64 * 1024 || g_video_playing) {
      idle_frames = 0;
      if (!boosted) {
        cpu_boost(1);
        boosted = 1;
        debugPrintf("[boost] -> FastLoad at iter %llu (pak served=%lluMB)%s\n",
                    (unsigned long long)loop_iters, (unsigned long long)(cur_pak >> 20),
                    g_video_playing ? " (video playing)" : "");
      }
    } else if (boosted && ++idle_frames > BOOST_IDLE_TICKS) {
      cpu_boost(0);
      boosted = 0;
      debugPrintf("[boost] -> Normal at iter %llu (pak served=%lluMB)\n",
                  (unsigned long long)loop_iters, (unsigned long long)(cur_pak >> 20));
    }
    last_pak = cur_pak;
    loop_iters++;
    svcSleepThread(16 * 1000 * 1000);
  }

  if (e_nativeQuit) e_nativeQuit(fake_env, cls);
  else if (e_onNativeSurfaceDestroyed) e_onNativeSurfaceDestroyed(fake_env, cls);

  for (int i = 0; i < 120 && !s_sdl_thread_done; i++)
    svcSleepThread(16 * 1000 * 1000);

  threadClose(&s_sdl_thread);

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
