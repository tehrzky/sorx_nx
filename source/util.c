/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#if DEBUG_LOG

static int s_nxlinkSock = -1;
static FILE *s_log = NULL; // persistent log handle (fast; fflush per line)
static uint64_t s_boot_tick; // armGetSystemTick() as of userAppInit(), our earliest hookable point
static int s_log_fd = -1; // raw fd for s_log, captured once at open time -- see debugPrintf
static Mutex s_log_mutex;

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

// sdmc is mounted by the time userAppInit runs, so open the log once here
// instead of reopening it per line.
void userAppInit(void) {
  s_boot_tick = armGetSystemTick();
  initNxLink();
  s_log = fopen(LOG_PATH, "w");
  if (!s_log) s_log = fopen(LOG_NAME, "w"); // fall back to the launch CWD
  if (s_log) s_log_fd = fileno(s_log); // captured now, before Ikemen's cgo init ever runs
  if (s_log) {
    fputs("== sorx log open ==\n", s_log);
    fflush(s_log);
  }
}

void userAppExit(void) {
  if (s_log) { fclose(s_log); s_log = NULL; }
  deinitNxLink();
}

#endif

// ---------------------------------------------------------------------------
// Dependency-free formatter: after Ikemen's (Go/cgo) init_array runs, cgo's
// Android TLS scan misfires and corrupts newlib's own per-thread state that
// vfprintf/vprintf rely on internally -- confirmed on-device: every call
// into that machinery crashes from that point on, while raw write() and
// plain function calls keep working fine. Everything below only uses
// strlen/memcpy/manual digit conversion and write() -- nothing reentrant.
// ---------------------------------------------------------------------------
static void out_str(char *buf, size_t bufsz, size_t *pos, const char *s) {
  size_t n = strlen(s);
  size_t room = bufsz > *pos + 1 ? bufsz - *pos - 1 : 0;
  if (n > room) n = room;
  memcpy(buf + *pos, s, n);
  *pos += n;
}
static void out_uint(char *buf, size_t bufsz, size_t *pos, unsigned long long v, int base, int upper) {
  char tmp[32], rev[32]; int i = 0;
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  if (v == 0) tmp[i++] = '0';
  while (v > 0) { tmp[i++] = digits[v % base]; v /= base; }
  for (int j = 0; j < i; j++) rev[j] = tmp[i - 1 - j];
  rev[i] = 0;
  out_str(buf, bufsz, pos, rev);
}
static void out_int(char *buf, size_t bufsz, size_t *pos, long long v) {
  if (v < 0) { out_str(buf, bufsz, pos, "-"); out_uint(buf, bufsz, pos, (unsigned long long)(-v), 10, 0); }
  else out_uint(buf, bufsz, pos, (unsigned long long)v, 10, 0);
}
static void out_float(char *buf, size_t bufsz, size_t *pos, double v, int width, int prec) {
  if (prec < 0) prec = 6;
  if (v < 0) { out_str(buf, bufsz, pos, "-"); v = -v; }
  double scale = 1.0;
  for (int i = 0; i < prec; i++) scale *= 10.0;
  unsigned long long scaled = (unsigned long long)(v * scale + 0.5);
  unsigned long long ip = scaled;
  for (int i = 0; i < prec; i++) ip /= 10;
  char intbuf[32]; size_t ipos = 0;
  out_uint(intbuf, sizeof(intbuf), &ipos, ip, 10, 0);
  intbuf[ipos] = 0;
  int len_so_far = (int)ipos + (prec > 0 ? 1 + prec : 0);
  for (int i = len_so_far; i < width; i++) out_str(buf, bufsz, pos, " ");
  out_str(buf, bufsz, pos, intbuf);
  if (prec > 0) {
    out_str(buf, bufsz, pos, ".");
    unsigned long long frac = scaled;
    char fracbuf[32];
    for (int i = prec - 1; i >= 0; i--) { fracbuf[i] = '0' + (int)(frac % 10); frac /= 10; }
    fracbuf[prec] = 0;
    out_str(buf, bufsz, pos, fracbuf);
  }
}
static int safe_vformat(char *buf, size_t bufsz, const char *fmt, va_list ap) {
  size_t pos = 0;
  for (const char *p = fmt; *p && pos + 1 < bufsz; p++) {
    if (*p != '%') { buf[pos++] = *p; continue; }
    p++;
    int width = 0, prec = -1;
    while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
    if (*p == '.') { p++; prec = 0; while (*p >= '0' && *p <= '9') { prec = prec * 10 + (*p - '0'); p++; } }
    int is_ll = 0, is_l = 0;
    if (*p == 'l') { is_l = 1; p++; if (*p == 'l') { is_ll = 1; p++; } }
    switch (*p) {
      case 's': out_str(buf, bufsz, &pos, va_arg(ap, const char *)); break;
      case 'd': case 'i':
        if (is_ll) out_int(buf, bufsz, &pos, va_arg(ap, long long));
        else if (is_l) out_int(buf, bufsz, &pos, va_arg(ap, long));
        else out_int(buf, bufsz, &pos, va_arg(ap, int));
        break;
      case 'u':
        if (is_ll) out_uint(buf, bufsz, &pos, va_arg(ap, unsigned long long), 10, 0);
        else if (is_l) out_uint(buf, bufsz, &pos, va_arg(ap, unsigned long), 10, 0);
        else out_uint(buf, bufsz, &pos, va_arg(ap, unsigned int), 10, 0);
        break;
      case 'x': out_uint(buf, bufsz, &pos, va_arg(ap, unsigned int), 16, 0); break;
      case 'p': out_str(buf, bufsz, &pos, "0x"); out_uint(buf, bufsz, &pos, (unsigned long long)(uintptr_t)va_arg(ap, void *), 16, 0); break;
      case 'c': buf[pos++] = (char)va_arg(ap, int); break;
      case 'f': out_float(buf, bufsz, &pos, va_arg(ap, double), width, prec); break;
      case '%': buf[pos++] = '%'; break;
      default: buf[pos++] = '%'; if (pos + 1 < bufsz) buf[pos++] = *p; break;
    }
  }
  buf[pos] = 0;
  return (int)pos;
}

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  mutexLock(&s_log_mutex);
  char line[512];
  size_t off = 0;

  double elapsed_s = (double)armTicksToNs(armGetSystemTick() - s_boot_tick) / 1e9;
  out_str(line, sizeof(line), &off, "[");
  out_float(line, sizeof(line), &off, elapsed_s, 9, 3);
  out_str(line, sizeof(line), &off, "] ");

  va_list list;
  va_start(list, text);
  off += (size_t)safe_vformat(line + off, sizeof(line) - off, text, list);
  va_end(list);

  if (s_log_fd >= 0) write(s_log_fd, line, off);
  write(1, line, off);
  mutexUnlock(&s_log_mutex);
#endif
  return 0;
}

// Shared TLS block for the loaded modules' stack-protector guard at
// tpidr_el0 + 0x28. Written directly via MSR: this libnx doesn't expose a
// tpidr_el0 (read-write TLS) setter of its own -- armGetTls()/the kernel's
// TLS slot APIs all operate on tpidr**ro**_el0, the separate IPC buffer
// register bionic doesn't touch.
static uint8_t s_tls_block[0x1000] __attribute__((aligned(16)));

void tls_setup_guard(void) {
  *(uint64_t *)(s_tls_block + 0x28) = 0x0123456789ABCDEFull;
  __asm__ __volatile__("msr tpidr_el0, %x0" :: "r"(s_tls_block));
}

// boost the CPU to 1785MHz while loading
//
// On-device evidence (main.c's adaptive-boost logging showed FastLoad
// covering 100% of a whole ~190s loading window, yet the load took just as
// long as an earlier run that only had ~86% coverage) contradicts the
// assumption that CPU clock was the bottleneck here at all -- possible
// explanations include ApmCpuBoostMode_FastLoad not actually raising the
// CPU clock the way requested (libnx docs describe it purely in terms of
// "boost mode", not a guaranteed frequency) or its side effect of
// throttling the GPU to minimum (per libnx's apm.h: "Boost CPU.
// Additionally, throttle GPU to minimum") costing back whatever the CPU
// side gains if the load also does meaningful GPU work. Query and log the
// actual CpuBus clock rate via clkrst (not just our own "boosted" flag)
// whenever this is called, so the next capture gives real hardware
// evidence either way instead of another assumption.
static int s_clkrst_ready = -1; // -1 = not yet tried, 0 = unavailable, 1 = ready
static ClkrstSession s_clkrst_cpu;

static void log_cpu_clock(const char *when) {
  if (s_clkrst_ready == -1) {
    s_clkrst_ready = 0;
    if (R_SUCCEEDED(clkrstInitialize())) {
      if (R_SUCCEEDED(clkrstOpenSession(&s_clkrst_cpu, PcvModuleId_CpuBus, 3)))
        s_clkrst_ready = 1;
      else
        clkrstExit();
    }
  }
  if (s_clkrst_ready != 1) {
    debugPrintf("[clock] %s: clkrst unavailable, can't confirm actual CPU Hz\n", when);
    return;
  }
  u32 hz = 0;
  Result rc = clkrstGetClockRate(&s_clkrst_cpu, &hz);
  if (R_SUCCEEDED(rc))
    debugPrintf("[clock] %s: CpuBus clock = %u Hz (%.1f MHz)\n", when, hz, hz / 1000000.0);
  else
    debugPrintf("[clock] %s: clkrstGetClockRate failed: 0x%x\n", when, rc);
}

void cpu_boost(int on) {
  // The one and only clock sample the previous capture produced (right at
  // process start, immediately after the very first cpu_boost(1)) read
  // 1020 MHz -- the Switch's normal, non-boosted CPU clock -- suggesting
  // appletSetCpuBoostMode(FastLoad) isn't actually raising it. Checking the
  // call's own Result (never done before) is the obvious next thing that
  // was still unverified: if THIS is failing outright, that alone would
  // fully explain a clock reading that never moves.
  Result rc = appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
  char label[64];
  snprintf(label, sizeof(label), "after cpu_boost(%d) rc=0x%x", on, rc);
  log_cpu_clock(label);
}

// Independent of any boost state transition: main.c's loop calls this
// periodically so the log has clock samples spread through the whole
// loading window, not just the two (or, on-device, exactly one) moments a
// transition happened to log one. A clock that's genuinely stuck at 1020
// MHz throughout -- boosted or not -- is a very different finding from one
// that only fails to move right at the very first call.
void log_cpu_clock_periodic(void) {
  log_cpu_clock("periodic sample");
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
