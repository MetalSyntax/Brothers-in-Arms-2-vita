/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

/*
 * appInit() hardcodes the Android locations into g_moudlePath/g_appPath/
 * g_saveDirPath/sound_path (see the pseudo-C of appInit in libbia2.so):
 *   /sdcard/gameloft/games/bia2/                          -- data, saves, sounds
 *   /data/data/com.gameloft.android.GAND.GloftBia2.bia2/  -- sav_path
 * Everything the engine opens goes through fopen()/remove() with those
 * prefixes, so they are rewritten here instead of patching .rodata:
 *   data   -> GAME_PATH (ux0:data/brothersinarms2/BIA2/)
 *   saves  -> SAVE_PATH (ux0:data/brothersinarms2/saves/)
 * The match is case-insensitive: the Android cache ships the folder as
 * "BIA2" while the engine asks for "bia2".
 *
 * On Android the saves live in the same folder as the data (g_saveDirPath ==
 * g_appPath), so they are told apart by name -- the only files the engine
 * writes (SaveBufferToFile / GetSaveFilePath / GetConfigFilePath callers).
 */
static const char * const save_files[] = {
    "BIA2.data",        // Main::SaveBufferToFile / LoadFileToBuffer
    "BIA2_bak.data",
    "profile.sav",      // SaveProfile / LoadProfile (GLXPlayer)
    "serverConfig.sav", // GLXPlayerSereverConfig
    "oconf.bar",        // GetConfigFilePath
};

static bool is_save_file(const char * name) {
    for (size_t i = 0; i < sizeof(save_files) / sizeof(*save_files); i++)
        if (strcasecmp(name, save_files[i]) == 0)
            return true;
    return false;
}

static const char * match_prefix(const char * path, const char * prefix) {
    size_t len = strlen(prefix);
    if (strncasecmp(path, prefix, len) != 0 || (path[len] != '/' && path[len] != '\0'))
        return NULL;
    const char * rest = path + len;
    while (*rest == '/')
        rest++;
    return rest;
}

const char * vita_path(const char * path, char * buf, size_t buf_size, bool for_write) {
    if (!path)
        return path;

    const char * rest;

    if ((rest = match_prefix(path, "/sdcard/gameloft/games/bia2"))) {
        if (is_save_file(rest)) {
            snprintf(buf, buf_size, "%s%s", SAVE_PATH, rest);
            // First run: no save of our own yet -- read the one shipped in
            // the Android cache instead (it is what Android would have read).
            // Writes always land in SAVE_PATH.
            if (!for_write && !file_exists(buf))
                snprintf(buf, buf_size, "%s%s", GAME_PATH, rest);
        } else {
            snprintf(buf, buf_size, "%s%s", GAME_PATH, rest);
        }
        return buf;
    }

    if ((rest = match_prefix(path, "/data/data/com.gameloft.android.GAND.GloftBia2.bia2"))) {
        snprintf(buf, buf_size, "%s%s", SAVE_PATH, rest);
        return buf;
    }

    return path;
}

static bool mode_writes(const char * mode) {
    return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}

int remove_soloader(const char * path) {
    char buf[PATH_MAX];
    path = vita_path(path, buf, sizeof(buf), true);
    int ret = remove(path);
    l_debug("remove(%s): %i", path, ret);
    return ret;
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    char path_buf[PATH_MAX];
    filename = vita_path(filename, path_buf, sizeof(path_buf), mode_writes(mode));

    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    char path_buf[PATH_MAX];
    path = vita_path(path, path_buf, sizeof(path_buf),
                     (oflag & (BIONIC_O_WRONLY | BIONIC_O_RDWR | BIONIC_O_CREAT)) != 0);

    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    char path_buf[PATH_MAX];
    path = vita_path(path, path_buf, sizeof(path_buf), false);

    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    DIR* ret = opendir(_pathname);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
