/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Brothers-in-Arms-2-vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"
#include "utils/netlog.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

#define LOG_DIR       DATA_PATH"logs"
#define LOG_INDEX_MIN 1
#define LOG_INDEX_MAX 999

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];
// Buffer C holds the plain (uncolourised) line for the file and UDP sinks.
static char buffer_c[2048];

static SceUID _log_fd     = -1;
static int    _log_index  = -1;
static char   _log_path[128] = "(none)";
static bool   _log_inited = false;

// Monotonic per-line counter. A gap in the sequence tells you the log lost
// lines; the last number tells you exactly how far the run got.
static unsigned int _log_seq = 0;

#ifdef DEBUG_SOLOADER
static int _log_min_level = LT_DEBUG;
#else
static int _log_min_level = LT_INFO;
#endif

static uint64_t _log_t0 = 0;

/** Short tag for the file/UDP sinks, where ANSI colour is just noise. */
static const char * level_tag(int t) {
    switch (t) {
        case LT_DEBUG:   return "DEBUG  ";
        case LT_INFO:    return "INFO   ";
        case LT_WARN:    return "WARNING";
        case LT_ERROR:   return "ERROR  ";
        case LT_FATAL:   return "FATAL  ";
        case LT_SUCCESS: return "SUCCESS";
        case LT_WAIT:    return "WAITING";
        default:         return "?      ";
    }
}

#define LOG_PREFIX "brothersinarms2_"
#define LOG_SUFFIX ".log"

static void log_slot_path(int i, char * path, unsigned int path_size) {
    sceClibSnprintf(path, path_size, LOG_DIR "/" LOG_PREFIX "%03d" LOG_SUFFIX, i);
}

static int datetime_cmp(const SceDateTime * x, const SceDateTime * y) {
    if (x->year != y->year)               return x->year < y->year ? -1 : 1;
    if (x->month != y->month)             return x->month < y->month ? -1 : 1;
    if (x->day != y->day)                 return x->day < y->day ? -1 : 1;
    if (x->hour != y->hour)               return x->hour < y->hour ? -1 : 1;
    if (x->minute != y->minute)           return x->minute < y->minute ? -1 : 1;
    if (x->second != y->second)           return x->second < y->second ? -1 : 1;
    if (x->microsecond != y->microsecond) return x->microsecond < y->microsecond ? -1 : 1;
    return 0;
}

/**
 * NNN of the most recently written logs/brothersinarms2_NNN.log, or 0 when
 * there are none. The next run takes NNN + 1, wrapping 999 -> 001. Going by
 * modification time rather than the highest number is what makes the wrap
 * work (after 999 -> 001, "highest" would stay 999 forever). No index file is
 * kept: the folder only ever holds the .log files themselves.
 */
static int last_log_index(void) {
    SceUID dfd = sceIoDopen(LOG_DIR);
    if (dfd < 0)
        return 0;

    int last = 0;
    SceDateTime last_mtime;
    SceIoDirent entry;
    const unsigned int prefix_len = sizeof(LOG_PREFIX) - 1;

    sceClibMemset(&entry, 0, sizeof(entry));
    while (sceIoDread(dfd, &entry) > 0) {
        const char * name = entry.d_name;
        const char * d = name + prefix_len;
        if (sceClibStrncmp(name, LOG_PREFIX, prefix_len) != 0
            || d[0] < '0' || d[0] > '9' || d[1] < '0' || d[1] > '9'
            || d[2] < '0' || d[2] > '9'
            || sceClibStrncmp(d + 3, LOG_SUFFIX, sizeof(LOG_SUFFIX)) != 0)
            continue;

        int v = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
        if (v < LOG_INDEX_MIN || v > LOG_INDEX_MAX)
            continue;

        if (last == 0 || datetime_cmp(&entry.d_stat.st_mtime, &last_mtime) > 0) {
            last = v;
            last_mtime = entry.d_stat.st_mtime;
        }
    }

    sceIoDclose(dfd);
    return last;
}

/*
 * An unbuffered write survives the process dying, but not a forced power-off
 * (holding POWER on a frozen console): exFAT only commits the new file size
 * on close/sync, so the whole tail since the last commit is lost -- which is
 * exactly what happened to brothersinarms2_001.log. Sync at most every
 * LOG_SYNC_INTERVAL_US, and always for warnings and worse.
 */
#define LOG_SYNC_INTERVAL_US 250000

static SceUInt64 _log_last_sync = 0;

static void log_sync_throttled(bool force) {
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (force || now - _log_last_sync >= LOG_SYNC_INTERVAL_US) {
        sceIoSyncByFd(_log_fd, 0);
        _log_last_sync = now;
    }
}

void log_sync(void) {
    if (_log_fd >= 0)
        sceIoSyncByFd(_log_fd, 0);
}

int log_init(void) {
    if (_log_inited)
        return _log_index;
    _log_inited = true;

    _log_t0 = sceKernelGetProcessTimeWide();

    // sceIoMkdir on an existing directory just fails harmlessly.
    sceIoMkdir(DATA_PATH"logs", 0777);

    // 001, 002, ... 999; after 999 it wraps to 001 and overwrites the
    // oldest numbering (O_TRUNC below).
    int claimed = last_log_index() + 1;
    if (claimed > LOG_INDEX_MAX)
        claimed = LOG_INDEX_MIN;

    char path[128];
    log_slot_path(claimed, path, sizeof(path));

    _log_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (_log_fd < 0) {
        _log_fd = -1;
        _log_index = -1;
        return -1;
    }

    _log_index = claimed;
    sceClibStrncpy(_log_path, path, sizeof(_log_path) - 1);
    _log_path[sizeof(_log_path) - 1] = '\0';

    // Bring the UDP sink up too, if the user asked for it. Doing it here keeps
    // "where do my log lines go" a single decision point.
    netlog_init();

    char header[256];
    int len = sceClibSnprintf(header, sizeof(header),
                              "=== Brothers in Arms 2 Vita loader log #%03d ===\n"
                              "=== udp sink: %s ===\n",
                              claimed, netlog_target());
    if (len > 0) {
        sceIoWrite(_log_fd, header, len);
        netlog_send(header, len);
    }

    return _log_index;
}

void log_shutdown(void) {
    if (_log_fd >= 0) {
        const char * footer = "=== end of log ===\n";
        sceIoWrite(_log_fd, footer, sceClibStrnlen(footer, 64));
        sceIoClose(_log_fd);
        _log_fd = -1;
    }
    netlog_shutdown();
}

void log_set_min_level(int level) {
    _log_min_level = level;
}

int log_current_index(void) {
    return _log_index;
}

const char * log_current_path(void) {
    return _log_path;
}

void _log_print(int t, const char* fmt, ...) {
    // Errors, fatals and the low-volume status levels always get through: they
    // are what a crash report is made of.
    if (t < _log_min_level
        && t != LT_ERROR && t != LT_FATAL
        && t != LT_SUCCESS && t != LT_WAIT)
        return;

    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    sceClibPrintf(buffer_b);

    if (_log_fd >= 0 || netlog_enabled()) {
        // Re-render the message plain, then prefix it with the sequence number
        // and a millisecond timestamp. Both matter when reading a log next to a
        // .psp2dmp: the sequence pins down the last line that made it out, the
        // timestamp shows where the run stalled.
        va_list list2;
        va_start(list2, fmt);
        sceClibVsnprintf(buffer_a, sizeof(buffer_a), fmt, list2);
        va_end(list2);

        // Truncated to 32 bits on purpose: sceClibSnprintf's format support is
        // narrower than libc's and %llu is not reliable there. A u32 of
        // milliseconds still covers ~49 days of uptime.
        unsigned int ms =
            (unsigned int)((sceKernelGetProcessTimeWide() - _log_t0) / 1000);

        int len = sceClibSnprintf(buffer_c, sizeof(buffer_c),
                                  "[%06u][%6u.%03u][%s] %s\n",
                                  ++_log_seq, ms / 1000, ms % 1000,
                                  level_tag(t), buffer_a);
        if (len > 0) {
            if ((unsigned int)len >= sizeof(buffer_c))
                len = (int)sizeof(buffer_c) - 1;
            // Unbuffered write: the tail of the log must survive a data abort.
            if (_log_fd >= 0) {
                sceIoWrite(_log_fd, buffer_c, len);
                log_sync_throttled(t == LT_WARN || t == LT_ERROR || t == LT_FATAL);
            }
            netlog_send(buffer_c, len);
        }
    }

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}
