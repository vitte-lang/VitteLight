// SPDX-License-Identifier: GPL-3.0-or-later
//
// fswatch.c — File system watcher (C17)
// Namespace: "fswatch"
//
// Build (Linux):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c fswatch.c
//
// Scope:
//   - Linux: inotify-based, directory watching (non-recursive).
//   - Other OS: stubs return ENOSYS.
//
// API (single TU; include this file or make a header as needed):
//   typedef struct FSW FSW;
//   enum fsw_event_kind { FSW_CREATE=1, FSW_DELETE=2, FSW_MODIFY=4, FSW_MOVE=8, FSW_OVERFLOW=16 };
//   typedef struct { char path[4096]; unsigned kind; } fsw_event;
//   FSW* fsw_open(void);
//   // mask is a bitmask of fsw_event_kind hints; pass 0 for all typical events.
//   // Returns watch id (>=0) or -1 on error.
//   int  fsw_add(FSW* w, const char* dirpath, unsigned mask);
//   // Remove a watch by id (value returned by fsw_add). Returns 0 or -1.
//   int  fsw_remove(FSW* w, int watch_id);
//   // Poll events into `out` up to `max_events`. timeout_ms < 0 blocks, =0 non-blocking.
//   // Returns number of events ≥0, or -1 on error.
//   int  fsw_poll(FSW* w, fsw_event* out, int max_events, int timeout_ms);
//   void fsw_close(FSW* w);
//
// Notes:
//   - Non-recursive: add a watch per subdirectory if needed.
//   - Emits full path: "<watched_dir>/<name>" or "<watched_dir>" if name empty.
//   - MOVE is best-effort; pair matching by cookie is not exposed, we emit FSW_MOVE.
//   - Overflow is mapped to FSW_OVERFLOW.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(__linux__)
  #include <sys/inotify.h>
  #include <sys/poll.h>
  #include <unistd.h>
  #include <limits.h>
  #ifndef PATH_MAX
  #define PATH_MAX 4096
  #endif
#else
  // Stubs for non-Linux
  #include <limits.h>
  #ifndef PATH_MAX
  #define PATH_MAX 4096
  #endif
#endif

typedef struct FSW FSW;

enum fsw_event_kind { FSW_CREATE=1u, FSW_DELETE=2u, FSW_MODIFY=4u, FSW_MOVE=8u, FSW_OVERFLOW=16u };

typedef struct {
    char     path[PATH_MAX];
    unsigned kind;
} fsw_event;

#if defined(__linux__)

// -------- Linux implementation (inotify) --------

typedef struct {
    int   wd;
    char* dir;   // watched directory, absolute or as passed
    unsigned mask; // user mask (fsw bits) for info; not used by inotify
} watch_entry;

struct FSW {
    int fd;
    watch_entry* arr;
    int n;
    int cap;
};

// Map fsw mask -> inotify mask set
static uint32_t in_mask_from_fsw(unsigned m) {
    uint32_t im = 0;
    if (m == 0) {
        // default full set
        im = IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF | IN_ATTRIB | IN_CLOSE_WRITE;
    } else {
        if (m & FSW_CREATE) im |= IN_CREATE | IN_MOVED_TO;
        if (m & FSW_DELETE) im |= IN_DELETE | IN_DELETE_SELF;
        if (m & FSW_MODIFY) im |= IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE;
        if (m & FSW_MOVE)   im |= IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF;
    }
    // Always watch for overflow and ignored
    im |= IN_Q_OVERFLOW | IN_IGNORED;
    // Directory-specific optimization
    im |= IN_ONLYDIR;
    return im;
}

static unsigned fsw_from_in(uint32_t im) {
    unsigned k = 0;
    if (im & (IN_CREATE|IN_MOVED_TO)) k |= FSW_CREATE;
    if (im & (IN_DELETE|IN_DELETE_SELF)) k |= FSW_DELETE;
    if (im & (IN_MODIFY|IN_ATTRIB|IN_CLOSE_WRITE)) k |= FSW_MODIFY;
    if (im & (IN_MOVED_FROM|IN_MOVED_TO|IN_MOVE_SELF)) k |= FSW_MOVE;
    if (im & IN_Q_OVERFLOW) k |= FSW_OVERFLOW;
    return k;
}

static int ensure_cap(FSW* w) {
    if (w->n < w->cap) return 0;
    int ncap = w->cap ? w->cap * 2 : 8;
    watch_entry* na = (watch_entry*)realloc(w->arr, (size_t)ncap * sizeof(*na));
    if (!na) return -1;
    w->arr = na;
    w->cap = ncap;
    return 0;
}

static int idx_from_wd(FSW* w, int wd) {
    for (int i = 0; i < w->n; ++i) if (w->arr[i].wd == wd) return i;
    return -1;
}

FSW* fsw_open(void) {
    FSW* w = (FSW*)calloc(1, sizeof(*w));
    if (!w) return NULL;
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) { free(w); return NULL; }
    w->fd = fd;
    return w;
}

int fsw_add(FSW* w, const char* dirpath, unsigned mask) {
    if (!w || !dirpath) { errno = EINVAL; return -1; }
    uint32_t inmask = in_mask_from_fsw(mask);
    int wd = inotify_add_watch(w->fd, dirpath, inmask);
    if (wd < 0) return -1;

    if (ensure_cap(w) != 0) { (void)inotify_rm_watch(w->fd, wd); return -1; }
    w->arr[w->n].wd = wd;
    w->arr[w->n].mask = mask;
    w->arr[w->n].dir = strdup(dirpath);
    if (!w->arr[w->n].dir) { (void)inotify_rm_watch(w->fd, wd); return -1; }
    w->n += 1;
    return wd;
}

int fsw_remove(FSW* w, int watch_id) {
    if (!w) { errno = EINVAL; return -1; }
    int i = idx_from_wd(w, watch_id);
    if (i < 0) { errno = ENOENT; return -1; }
    (void)inotify_rm_watch(w->fd, watch_id);
    free(w->arr[i].dir);
    w->arr[i] = w->arr[w->n - 1];
    w->n -= 1;
    return 0;
}

static void join_path(char out[PATH_MAX], const char* dir, const char* name) {
    size_t ld = strlen(dir);
    int need_slash = (ld > 0 && dir[ld-1] != '/');
    if (name && *name) {
        snprintf(out, PATH_MAX, "%s%s%s", dir, need_slash ? "/" : "", name);
    } else {
        snprintf(out, PATH_MAX, "%s", dir);
    }
}

int fsw_poll(FSW* w, fsw_event* out, int max_events, int timeout_ms) {
    if (!w || !out || max_events <= 0) { errno = EINVAL; return -1; }

    struct pollfd pfd = { .fd = w->fd, .events = POLLIN, .revents = 0 };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0; // timeout

    // inotify event buffer
    // Kernel allows read of multiple events; use a reasonably large buffer.
    char buf[64 * 1024];
    ssize_t n = read(w->fd, buf, sizeof buf);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }

    int emitted = 0;
    for (char* p = buf; p < buf + n; ) {
        if ((size_t)(buf + n - p) < sizeof(struct inotify_event)) break;
        struct inotify_event* ev = (struct inotify_event*)p;
        p += sizeof(struct inotify_event) + ev->len;

        int idx = idx_from_wd(w, ev->wd);
        const char* dir = (idx >= 0) ? w->arr[idx].dir : "";
        char full[PATH_MAX];
        join_path(full, dir, ev->len ? ev->name : "");

        unsigned kind = fsw_from_in(ev->mask);
        if (kind == 0) continue;

        // emit one compact event per inotify event
        if (emitted < max_events) {
            strncpy(out[emitted].path, full, sizeof(out[emitted].path) - 1);
            out[emitted].path[sizeof(out[emitted].path)-1] = '\0';
            out[emitted].kind = kind;
            emitted++;
        } else {
            // drop extras; signal overflow on last slot if possible
            out[max_events - 1].kind |= FSW_OVERFLOW;
            break;
        }
    }
    return emitted;
}

void fsw_close(FSW* w) {
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    for (int i = 0; i < w->n; ++i) free(w->arr[i].dir);
    free(w->arr);
    free(w);
}

#else

// -------- Stubs for non-Linux OS --------

struct FSW { int _dummy; };

FSW* fsw_open(void) { errno = ENOSYS; return NULL; }
int  fsw_add(FSW* w, const char* dirpath, unsigned mask) { (void)w; (void)dirpath; (void)mask; errno = ENOSYS; return -1; }
int  fsw_remove(FSW* w, int watch_id) { (void)w; (void)watch_id; errno = ENOSYS; return -1; }
int  fsw_poll(FSW* w, fsw_event* out, int max_events, int timeout_ms) { (void)w; (void)out; (void)max_events; (void)timeout_ms; errno = ENOSYS; return -1; }
void fsw_close(FSW* w) { (void)w; }

#endif

// -------- Optional demo --------
#ifdef FSW_DEMO
#include <stdio.h>
int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s DIR\n", argv[0]); return 2; }
    FSW* w = fsw_open();
    if (!w) { perror("fsw_open"); return 1; }
    int wid = fsw_add(w, argv[1], 0);
    if (wid < 0) { perror("fsw_add"); fsw_close(w); return 1; }
    printf("Watching %s (wd=%d). Ctrl-C to exit.\n", argv[1], wid);

    fsw_event evs[32];
    for (;;) {
        int n = fsw_poll(w, evs, 32, 1000);
        if (n < 0) { perror("fsw_poll"); break; }
        for (int i = 0; i < n; ++i) {
            unsigned k = evs[i].kind;
            printf("[%s] %s\n",
                   (k & FSW_OVERFLOW) ? "OVERFLOW" :
                   (k & FSW_CREATE) ? "CREATE" :
                   (k & FSW_DELETE) ? "DELETE" :
                   (k & FSW_MOVE)   ? "MOVE"   :
                   (k & FSW_MODIFY) ? "MODIFY" : "EVENT",
                   evs[i].path);
        }
    }
    fsw_close(w);
    return 0;
}
#endif