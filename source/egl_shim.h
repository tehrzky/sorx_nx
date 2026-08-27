/* egl_shim.h -- dlopen/dlsym bridge to mesa EGL/GLES.
 * MIT license; see LICENSE. */

#ifndef __EGL_SHIM_H__
#define __EGL_SHIM_H__

void *dlopen_fake(const char *filename, int flag);
void *dlsym_fake(void *handle, const char *symbol);
int dlclose_fake(void *handle);
char *dlerror_fake(void);

void egl_shim_set_native_main(void *addr);
void egl_shim_set_surface_cb(void (*cb)(void));   // NEW: callback on surface recreation

#endif
