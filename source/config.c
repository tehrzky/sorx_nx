/* config.h -- OpenBOR Switch wrapper configuration.
 * MIT license; see LICENSE. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#define HIDAPI_SO_NAME  "libhidapi.so"
#define SDL2_SO_NAME    "libSDL2.so"
#define OPENBOR_SO_NAME "libopenbor.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "debug.log"

#define DEFAULT_DATA_ROOT "/switch/openbor"
#define DEFAULT_SAVE_ROOT "/switch/openbor/save"
#define LOG_PATH DEFAULT_DATA_ROOT "/debug.log"

#define DEBUG_LOG 0
#define VERBOSE_IO 0
#define VERBOSE_JNI 0
#define VERBOSE_EGL 0

extern int screen_width;
extern int screen_height;

#define ANDROID_PKG "org.openbor.engine"

typedef struct {
  int screen_width;
  int screen_height;
  char data_root[256];
  char save_root[256];
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
