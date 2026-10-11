/* KERNEL32 and WINMM: files, threads, sync objects, time, resources. */
#include "../runtime/rt.h"
#include "../runtime/host.h"
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <ctype.h>

#define INVALID_HANDLE 0xFFFFFFFFu
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_NO_MORE_FILES 18
#define WAIT_TIMEOUT 0x102

static __thread uint32_t last_error;
void rt_set_last_error(uint32_t e) { last_error = e; }

/* ---- paths ---- */

/* Finds name in dir ignoring case. Linux file systems are case sensitive, Windows (and the Switch SD card) not. */
static int match_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(*dir ? dir : ".");
    struct dirent *e;
    int found = 0;
    while (d && (e = readdir(d)))
        if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s", e->d_name); found = 1; break; }
    if (d) closedir(d);
    return found;
}

/* Enhanced Edition mod folder (EE Common/FileSystemHooks.cpp): data/X is read from sh2e/X when that
 * file exists. Each top-level folder is switched on separately in sh2e.ini ("pic=1", "movie=1", ...),
 * because some EE packs only work with EE patches the port does not have yet. */
static int sh2e_enabled(const char *rel) {  /* rel: path after "data/" */
    static char on[32][16];
    static int n_on = -1;
    if (n_on < 0) {
        n_on = 0;
        FILE *f = fopen("sh2e.ini", "r");
        char line[128], key[16];
        int v;
        while (f && fgets(line, sizeof line, f))
            if (sscanf(line, " %15[^= ] = %d", key, &v) == 2 && v && n_on < 32) snprintf(on[n_on++], 16, "%s", key);
        if (f) fclose(f);
        if (n_on) rt_log("sh2e: %d mod folders enabled", n_on);
    }
    /* The title and save screens always take the Enhanced Edition's pictures: tools/patch_exe.py lays
     * them out for that 2732x2048 art, and the original pictures come out misplaced. */
    if (!strncasecmp(rel, "pic/etc/start0", 14) || !strncasecmp(rel, "menu/mc/savebg", 14)) return 1;
    size_t cl = strcspn(rel, "/");
    for (int i = 0; i < n_on; i++)
        if (strlen(on[i]) == cl && !strncasecmp(on[i], rel, cl)) return 1;
    return 0;
}

static const char *rt_path_base(const char *guest, char *out, size_t n);

const char *rt_path(const char *guest, char *out, size_t n) {
    const char *r = rt_path_base(guest, out, n);
    if (!strncasecmp(r, "data/", 5) && sh2e_enabled(r + 5)) {
        char alt[512], res[512];
        struct stat st;
        snprintf(alt, sizeof alt, "sh2e/%s", r + 5);
        rt_path_base(alt, res, sizeof res);
        if (stat(res, &st) == 0 && S_ISREG(st.st_mode)) {
            if (rt_trace) rt_log("  sh2e: %s", res);
            snprintf(out, n, "%s", res);
        }
    }
    return out;
}

static const char *rt_path_base(const char *guest, char *out, size_t n) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", guest);
    for (char *c = tmp; *c; c++) if (*c == '\\') *c = '/';
    const char *p = tmp;
    char cd[512];
    if ((p[0] == 'd' || p[0] == 'D') && p[1] == ':' && !strncasecmp(p + 2, "/movie/", 7)) {
        snprintf(cd, sizeof cd, "data%s", p + 2);  /* the virtual CD (GetDriveTypeA): its Movie folder is data/movie */
        p = cd;
    }
    if (p[0] && p[1] == ':') p += 2;               /* any drive is the game folder */
    if (!strncasecmp(p, "/SH2/", 5)) p += 5;       /* the folder GetFullPathNameA reports */
    while (*p == '/') p++;
    if (!strncmp(p, "./", 2)) p += 2;
    struct stat st;
    if (*p && stat(p, &st) == 0) {  /* exact spelling exists: no directory scans */
        snprintf(out, n, "%s", p);
        return out;
    }
    out[0] = 0;
    size_t len = 0;
    for (const char *s = p; *s;) {
        size_t cl = strcspn(s, "/");
        char comp[256], real[256];
        snprintf(comp, sizeof comp, "%.*s", (int)cl, s);
        if (!match_ci(out, comp, real, sizeof real)) snprintf(real, sizeof real, "%s", comp);
        len += snprintf(out + len, n - len, "%s%s", len ? "/" : "", real);
        s += cl;
        while (*s == '/') s++;
    }
    if (!len) snprintf(out, n, ".");
    return out;
}

/* ---- kernel objects ---- */
enum { H_FILE = 1, H_EVENT, H_MUTEX, H_THREAD, H_FIND, H_MAPPING, H_TIMER };
typedef struct Obj {
    int type;
    FILE *f;
    int signaled, manual, count;       /* events, mutexes (count = recursion), threads (signaled = done) */
    int waiters, pulsed;               /* threads in wait_one; how many of them the last PulseEvent frees */
    uint32_t pulse;                    /* PulseEvent count, so a wait knows a pulse came after it began */
    HostThread *owner, *thread;        /* mutexes: who holds it; threads: the host thread */
    uint32_t exit_code, tid, suspended;
    int started;
    DIR *dir;                          /* FindFirstFile */
    char pattern[256], dirpath[512];
    int dots;                          /* "." and ".." listed so far */
    uint32_t map_file;                 /* CreateFileMapping */
} Obj;

/* ponytail: one lock and one condition variable for every waitable object. Fine for a game's
 * handful of threads; per-object conditions if wakeups ever show up in a profile. */
static pthread_mutex_t obj_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t obj_cv = PTHREAD_COND_INITIALIZER;
static Obj *objs[4096];

static uint32_t new_handle(int type) {
    pthread_mutex_lock(&obj_mx);
    for (int i = 1; i < 4096; i++)
        if (!objs[i]) {
            objs[i] = calloc(1, sizeof(Obj));
            objs[i]->type = type;
            pthread_mutex_unlock(&obj_mx);
            return 0x1000 + i * 4;
        }
    rt_fatal("out of handles");
}

static __thread uint32_t self_handle;  /* GetCurrentThread's pseudo handle resolves here */

static Obj *obj(uint32_t h) {
    if (h == 0xFFFFFFFEu) h = self_handle;
    uint32_t i = (h - 0x1000) / 4;
    return h >= 0x1000 && i < 4096 ? objs[i] : NULL;
}

static int close_handle(uint32_t h) {
    Obj *o = obj(h);
    if (!o) return 0;
    if (o->f) fclose(o->f);
    if (o->dir) closedir(o->dir);
    if (o->type != H_THREAD) {  /* a running thread still uses its object */
        pthread_mutex_lock(&obj_mx);
        objs[(h - 0x1000) / 4] = NULL;
        pthread_mutex_unlock(&obj_mx);
        free(o);
    }
    return 1;
}
WINAPI(CloseHandle, "CloseHandle", 1) { return close_handle(ARG(0)); }

/* ---- sync ---- */

static int ready(Obj *o) {
    if (o->type == H_MUTEX) return !o->count || o->owner == host_thread_self();
    return o->signaled;
}

static void acquire(Obj *o) {
    if (o->type == H_MUTEX) { o->owner = host_thread_self(); o->count++; }
    else if (o->type == H_EVENT && !o->manual) o->signaled = 0;
}

static uint32_t wait_one(uint32_t h, uint32_t ms) {
    Obj *o = obj(h);
    if (!o) return 0xFFFFFFFFu;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&obj_mx);
    uint32_t pulse = o->pulse;
    o->waiters++;
    while (!ready(o) && !(o->pulse != pulse && o->pulsed)) {
        if (ms == 0 || (ms != 0xFFFFFFFFu && pthread_cond_timedwait(&obj_cv, &obj_mx, &ts) == ETIMEDOUT)) {
            o->waiters--;
            pthread_mutex_unlock(&obj_mx);
            return WAIT_TIMEOUT;
        }
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&obj_cv, &obj_mx);
    }
    if (ready(o)) acquire(o);
    else o->pulsed--;
    o->waiters--;
    pthread_mutex_unlock(&obj_mx);
    return 0;
}

static void signal_obj(Obj *o, int state) {
    pthread_mutex_lock(&obj_mx);
    o->signaled = state;
    pthread_cond_broadcast(&obj_cv);
    pthread_mutex_unlock(&obj_mx);
}

void rt_signal_event(uint32_t h) { Obj *o = obj(h); if (o) signal_obj(o, 1); }

WINAPI(WaitForSingleObject, "WaitForSingleObject", 2) { return wait_one(ARG(0), ARG(1)); }

WINAPI(CreateEventA, "CreateEventA", 4) {
    uint32_t h = new_handle(H_EVENT);
    obj(h)->manual = ARG(1);
    obj(h)->signaled = ARG(2);
    return h;
}
WINAPI(SetEvent, "SetEvent", 1) { Obj *o = obj(ARG(0)); if (o) signal_obj(o, 1); return o != NULL; }
/* Frees the threads waiting right now (one, for an auto-reset event) and leaves the event reset.
 * Setting and resetting it instead loses the pulse whenever the reset wins the race with the woken
 * waiter: CRI's ADX server thread waits on one, pulsed by a 17 ms timer, and missing pulses left the
 * music silent, or looping its stream buffer. */
WINAPI(PulseEvent, "PulseEvent", 1) {
    Obj *o = obj(ARG(0));
    if (!o) return 0;
    pthread_mutex_lock(&obj_mx);
    if (o->waiters) {
        o->pulse++;
        o->pulsed = o->manual ? o->waiters : 1;
        pthread_cond_broadcast(&obj_cv);
    }
    o->signaled = 0;
    pthread_mutex_unlock(&obj_mx);
    return 1;
}

WINAPI(CreateMutexA, "CreateMutexA", 3) {
    uint32_t h = new_handle(H_MUTEX);
    if (ARG(1)) { obj(h)->owner = host_thread_self(); obj(h)->count = 1; }
    return h;
}
WINAPI(ReleaseMutex, "ReleaseMutex", 1) {
    Obj *o = obj(ARG(0));
    if (!o || !o->count) return 0;
    pthread_mutex_lock(&obj_mx);
    o->count--;
    pthread_cond_broadcast(&obj_cv);
    pthread_mutex_unlock(&obj_mx);
    return 1;
}

/* CRITICAL_SECTION lives in guest memory; its DebugInfo field holds our recursive mutex index + 1. */
static pthread_mutex_t cs_mx[2048];
static uint32_t n_cs;
static pthread_mutex_t *cs_get(uint32_t cs) {
    if (!MEM32(cs)) {
        pthread_mutex_lock(&obj_mx);
        if (!MEM32(cs)) {
            if (n_cs == 2048) rt_fatal("too many critical sections");
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&cs_mx[n_cs], &a);
            MEM32(cs) = ++n_cs;
        }
        pthread_mutex_unlock(&obj_mx);
    }
    return &cs_mx[MEM32(cs) - 1];
}
WINAPI(InitializeCriticalSection, "InitializeCriticalSection", 1) { MEM32(ARG(0)) = 0; cs_get(ARG(0)); return 0; }
WINAPI(DeleteCriticalSection, "DeleteCriticalSection", 1) { return 0; }
WINAPI(EnterCriticalSection, "EnterCriticalSection", 1) { pthread_mutex_lock(cs_get(ARG(0))); return 0; }
WINAPI(LeaveCriticalSection, "LeaveCriticalSection", 1) { pthread_mutex_unlock(cs_get(ARG(0))); return 0; }

WINAPI(InterlockedExchange, "InterlockedExchange", 2) {
    return __atomic_exchange_n((uint32_t *)GPTR(ARG(0)), ARG(1), __ATOMIC_SEQ_CST);
}

/* ---- threads ---- */

static uint32_t next_tid = 0x100;

static void setup_guest_thread(uint32_t stack_size, uint32_t tid) {
    uint32_t stack = galloc(stack_size), tib = galloc(0x1000);
    MEM32(tib + 0x00) = 0xFFFFFFFFu;            /* SEH chain end */
    MEM32(tib + 0x04) = stack + stack_size;     /* stack base */
    MEM32(tib + 0x08) = stack;                  /* stack limit */
    MEM32(tib + 0x18) = tib;                    /* self */
    MEM32(tib + 0x20) = 0x1234;                 /* process id */
    MEM32(tib + 0x24) = tid;
    g_fs_base = tib;
    g_esp = stack + stack_size - 64;
    g_fp_top = 0;
    g_fp_control_word = 0x027F;
}

static void thread_started(Obj *o) {
    o->thread = host_thread_self();
    o->started = 1;
}

#ifndef __SWITCH__
/* SH2_DUMP_AFTER=<seconds>: every guest thread prints where it is, for hangs (Linux only). */
static void on_dump(int sig) {
    (void)sig;
    fprintf(stderr, "=== thread %u (handle %X)\n", MEM32(g_fs_base + 0x24), self_handle);
    rt_dump_thread();
}
static void dump_watchdog(void *arg) {
    sleep((unsigned)(uintptr_t)arg);
    for (int i = 1; i < 4096; i++)
        if (objs[i] && objs[i]->type == H_THREAD && objs[i]->started && !objs[i]->signaled) {
            host_thread_signal(objs[i]->thread, SIGUSR1);
            usleep(300000);
        }
}
#endif

/* Called on the thread that runs the game's entry point. */
void rt_thread_init_main(uint32_t stack_size) {
#ifndef __SWITCH__
    signal(SIGUSR1, on_dump);
    if (getenv("SH2_DUMP_AFTER"))
        host_thread_start(dump_watchdog, (void *)(uintptr_t)atoi(getenv("SH2_DUMP_AFTER")), 1u << 16);
#endif
    setup_guest_thread(stack_size, next_tid++);
    self_handle = new_handle(H_THREAD);
    obj(self_handle)->tid = MEM32(g_fs_base + 0x24);
    thread_started(obj(self_handle));
}

typedef struct { void (*fn)(void *); void *arg; uint32_t stack_size, handle; } Spawn;

static void spawn_main(void *p) {
    Spawn s = *(Spawn *)p;
    free(p);
    Obj *o = obj(s.handle);
    self_handle = s.handle;
    setup_guest_thread(s.stack_size, o->tid);
    pthread_mutex_lock(&obj_mx);
    while (o->suspended) pthread_cond_wait(&obj_cv, &obj_mx);  /* CREATE_SUSPENDED */
    thread_started(o);
    pthread_mutex_unlock(&obj_mx);
    s.fn(s.arg);
    signal_obj(o, 1);
}

static uint32_t spawn(void (*fn)(void *), void *arg, uint32_t stack_size, int suspended) {
    uint32_t h = new_handle(H_THREAD);
    Obj *o = obj(h);
    o->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_SEQ_CST);
    o->suspended = suspended;
    o->exit_code = 0x103;  /* STILL_ACTIVE */
    Spawn *s = malloc(sizeof *s);
    *s = (Spawn){fn, arg, stack_size ? stack_size : 1u << 20, h};
    host_thread_start(spawn_main, s, 8u << 20);  /* host stack: lifted code recurses as deep as the guest */
    return h;
}

void rt_spawn(void (*fn)(void *), void *arg, uint32_t stack_size) { spawn(fn, arg, stack_size, 0); }

typedef struct { uint32_t start, param; int stdcall; } GuestStart;
static void guest_thread(void *p) {
    GuestStart g = *(GuestStart *)p;
    free(p);
    uint32_t code = guest_call(g.start, 1, &g.param, g.stdcall);
    obj(self_handle)->exit_code = code;
}

WINAPI(CreateThread, "CreateThread", 6) {
    GuestStart *g = malloc(sizeof *g);
    *g = (GuestStart){ARG(2), ARG(3), 1};
    uint32_t h = spawn(guest_thread, g, ARG(1), (ARG(4) & 4) != 0);  /* CREATE_SUSPENDED */
    if (ARG(5)) MEM32(ARG(5)) = obj(h)->tid;
    return h;
}

/* MSVCR70 _beginthread(start, stack, arg): cdecl start routine. Lives here next to CreateThread. */
CDECL(_beginthread, "_beginthread") {
    GuestStart *g = malloc(sizeof *g);
    *g = (GuestStart){ARG(0), ARG(2), 0};
    return spawn(guest_thread, g, ARG(1), 0);
}

WINAPI(ExitThread, "ExitThread", 1) {
    obj(self_handle)->exit_code = ARG(0);
    signal_obj(obj(self_handle), 1);
    host_thread_exit();
}
WINAPI(GetExitCodeThread, "GetExitCodeThread", 2) {
    Obj *o = obj(ARG(0));
    if (!o) return 0;
    MEM32(ARG(1)) = o->signaled ? o->exit_code : 0x103;
    return 1;
}
WINAPI(ResumeThread, "ResumeThread", 1) {
    Obj *o = obj(ARG(0));
    if (!o) return 0xFFFFFFFFu;
    pthread_mutex_lock(&obj_mx);
    uint32_t prev = o->suspended;
    int wake = prev && --o->suspended == 0 && o->started;
    pthread_cond_broadcast(&obj_cv);  /* threads created suspended wait on the condition */
    pthread_mutex_unlock(&obj_mx);
    if (wake) host_thread_resume(o->thread);
    return prev;
}
WINAPI(SuspendThread, "SuspendThread", 1) {
    Obj *o = obj(ARG(0));
    if (!o) return 0xFFFFFFFFu;
    pthread_mutex_lock(&obj_mx);
    uint32_t prev = o->suspended++;
    int park = !prev && o->started && !o->signaled;
    pthread_mutex_unlock(&obj_mx);
    if (park) host_thread_pause(o->thread);
    return prev;
}
WINAPI(GetCurrentThread, "GetCurrentThread", 0) { return 0xFFFFFFFEu; }
WINAPI(GetCurrentThreadId, "GetCurrentThreadId", 0) { return MEM32(g_fs_base + 0x24); }
WINAPI(GetCurrentProcessId, "GetCurrentProcessId", 0) { return 0x1234; }
WINAPI(SetThreadPriority, "SetThreadPriority", 2) { return 1; }
WINAPI(GetThreadPriority, "GetThreadPriority", 1) { return 0; }
WINAPI(SetThreadPriorityBoost, "SetThreadPriorityBoost", 2) { return 1; }
/* Sleep(0) sleeps 0.1 ms rather than yielding: the game's frame limiters (0x4F6CE0 at
 * 30 Hz, 0x518AD0 at 60 Hz) spin on it until a QueryPerformanceCounter deadline, which otherwise
 * keeps two cores busy doing nothing. */
WINAPI(Sleep, "Sleep", 1) {
    usleep(ARG(0) ? ARG(0) * 1000u : 100);
    return 0;
}

/* ---- time ---- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
WINAPI(GetTickCount, "GetTickCount", 0) { return (uint32_t)(now_ns() / 1000000); }
WINAPI(timeGetTime, "timeGetTime", 0) { return (uint32_t)(now_ns() / 1000000); }
WINAPI(timeBeginPeriod, "timeBeginPeriod", 1) { return 0; }
WINAPI(timeEndPeriod, "timeEndPeriod", 1) { return 0; }
WINAPI(QueryPerformanceFrequency, "QueryPerformanceFrequency", 1) {
    MEM32(ARG(0)) = 1000000000u;
    MEM32(ARG(0) + 4) = 0;
    return 1;
}
WINAPI(QueryPerformanceCounter, "QueryPerformanceCounter", 1) {
    uint64_t t = now_ns();
    MEM32(ARG(0)) = (uint32_t)t;
    MEM32(ARG(0) + 4) = (uint32_t)(t >> 32);
    return 1;
}

/* FILETIME: 100 ns ticks since 1601. SYSTEMTIME: 8 WORDs. */
static uint64_t filetime_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ull + ts.tv_nsec / 100 + 116444736000000000ull;
}
/* timegm, which newlib lacks: days from 1970-01-01 for a proleptic Gregorian date. */
static time_t utc_seconds(const struct tm *t) {
    int y = t->tm_year + 1900 - (t->tm_mon < 2), era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int doy = (153 * (t->tm_mon + (t->tm_mon < 2 ? 10 : -2)) + 2) / 5 + t->tm_mday - 1;
    long days = era * 146097L + (yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    return days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static void put_systemtime(uint32_t st, time_t t, uint32_t ms) {
    struct tm tm;
    gmtime_r(&t, &tm);
    uint16_t w[8] = {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_wday, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms};
    memcpy(GPTR(st), w, 16);
}
WINAPI(GetSystemTimeAsFileTime, "GetSystemTimeAsFileTime", 1) {
    uint64_t t = filetime_now();
    MEM32(ARG(0)) = (uint32_t)t;
    MEM32(ARG(0) + 4) = (uint32_t)(t >> 32);
    return 0;
}
WINAPI(GetLocalTime, "GetLocalTime", 1) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    put_systemtime(ARG(0), utc_seconds(&tm), ts.tv_nsec / 1000000);
    return 0;
}
WINAPI(FileTimeToSystemTime, "FileTimeToSystemTime", 2) {
    uint64_t t = MEM32(ARG(0)) | (uint64_t)MEM32(ARG(0) + 4) << 32;
    put_systemtime(ARG(1), (time_t)((t - 116444736000000000ull) / 10000000), (t / 10000) % 1000);
    return 1;
}
/* The game stamps a save with the local time (GetLocalTime) and compares it with the file's time after this
 * conversion, so the console's UTC offset has to be applied here. */
WINAPI(FileTimeToLocalFileTime, "FileTimeToLocalFileTime", 2) {
    uint64_t t = MEM32(ARG(0)) | (uint64_t)MEM32(ARG(0) + 4) << 32;
    time_t utc = (time_t)((t - 116444736000000000ull) / 10000000);
    struct tm tm;
    localtime_r(&utc, &tm);
    t += (uint64_t)((int64_t)(utc_seconds(&tm) - utc) * 10000000);
    MEM32(ARG(1)) = (uint32_t)t;
    MEM32(ARG(1) + 4) = (uint32_t)(t >> 32);
    return 1;
}
WINAPI(SystemTimeToFileTime, "SystemTimeToFileTime", 2) {
    uint16_t w[8];
    memcpy(w, GPTR(ARG(0)), 16);
    struct tm tm = {.tm_year = w[0] - 1900, .tm_mon = w[1] - 1, .tm_mday = w[3],
                    .tm_hour = w[4], .tm_min = w[5], .tm_sec = w[6]};
    uint64_t t = (uint64_t)utc_seconds(&tm) * 10000000ull + w[7] * 10000ull + 116444736000000000ull;
    MEM32(ARG(1)) = (uint32_t)t;
    MEM32(ARG(1) + 4) = (uint32_t)(t >> 32);
    return 1;
}

/* timeSetEvent: callbacks void CALLBACK proc(id, msg, user, dw1, dw2), all run by one timer thread
 * (the game re-arms a one-shot timer every frame). */
typedef struct { uint32_t delay, proc, user, periodic; uint64_t due; int live; } MmTimer;
static MmTimer mm_timers[64];
static pthread_mutex_t mm_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mm_cv = PTHREAD_COND_INITIALIZER;
static int mm_thread_up;

static void mm_timer_thread(void *unused) {
    (void)unused;
    pthread_mutex_lock(&mm_mx);
    for (;;) {
        uint64_t now = now_ns(), next = now + 100000000ull;
        for (uint32_t i = 1; i < 64; i++) {
            MmTimer *t = &mm_timers[i];
            if (!t->live) continue;
            if (t->due <= now) {
                uint32_t a[5] = {i, 0, t->user, 0, 0}, proc = t->proc;
                if (t->periodic) t->due += t->delay * 1000000ull; else t->live = 0;
                pthread_mutex_unlock(&mm_mx);
                guest_call(proc, 5, a, 1);
                pthread_mutex_lock(&mm_mx);
                now = now_ns();
            }
            if (t->live && t->due < next) next = t->due;
        }
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t wait = next > now ? next - now : 0, abs_ns = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec + wait;
        ts.tv_sec = abs_ns / 1000000000ull;
        ts.tv_nsec = abs_ns % 1000000000ull;
        pthread_cond_timedwait(&mm_cv, &mm_mx, &ts);
    }
}
WINAPI(timeSetEvent, "timeSetEvent", 5) {
    pthread_mutex_lock(&mm_mx);
    if (!mm_thread_up) {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&mm_cv, &ca);
        mm_thread_up = 1;
        rt_spawn(mm_timer_thread, NULL, 256u << 10);
    }
    uint32_t id = 0;
    for (uint32_t i = 1; i < 64 && !id; i++)
        if (!mm_timers[i].live) {
            uint32_t d = ARG(0) ? ARG(0) : 1;
            mm_timers[i] = (MmTimer){d, ARG(2), ARG(3), ARG(4) & 1, now_ns() + d * 1000000ull, 1};
            id = i;
        }
    pthread_cond_signal(&mm_cv);
    pthread_mutex_unlock(&mm_mx);
    return id;
}
WINAPI(timeKillEvent, "timeKillEvent", 1) {
    pthread_mutex_lock(&mm_mx);
    uint32_t ok = ARG(0) && ARG(0) < 64 && mm_timers[ARG(0)].live;
    if (ok) mm_timers[ARG(0)].live = 0;
    pthread_mutex_unlock(&mm_mx);
    return ok ? 0 : 97;  /* MMSYSERR_INVALPARAM */
}

/* ---- files ---- */

void ee_file_opened(const char *guest, FILE *f);  /* src/game/ee.c */

/* Save folder activity is always logged (sh2.log), so a damaged-save report can be read afterwards. */
static int has_save(const char *s) {
    for (; *s; s++)
        if (!strncasecmp(s, "save", 4)) return 1;
    return 0;
}

static uint32_t open_file(const char *guest, uint32_t access, uint32_t disposition) {
    char path[512];
    rt_path(guest, path, sizeof path);
    int write = (access & 0x40000000u) != 0;  /* GENERIC_WRITE */
    struct stat st;
    int exists = stat(path, &st) == 0;
    const char *mode = write ? "r+b" : "rb";
    switch (disposition) {
    case 1: if (exists) { last_error = 80; return INVALID_HANDLE; } mode = "w+b"; break;  /* CREATE_NEW */
    case 2: mode = "w+b"; break;                                                           /* CREATE_ALWAYS */
    case 4: if (!exists) mode = "w+b"; break;                                              /* OPEN_ALWAYS */
    case 5: mode = "w+b"; break;                                                           /* TRUNCATE_EXISTING */
    }
    if (exists && S_ISDIR(st.st_mode)) { last_error = 5; return INVALID_HANDLE; }
    FILE *f = fopen(path, mode);
    if (rt_trace) rt_log("  open %s -> %s %s", guest, path, f ? "ok" : "FAILED");
    if (has_save(guest)) rt_log("save: open %s access %08X disposition %u -> %s", guest, access, disposition, f ? "ok" : "FAILED");
    if (!f && !write) rt_log("open: %s not found (%s)", guest, path);  /* the game's "Files are missing" */
    if (!f) { last_error = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE; }
    if (!write) ee_file_opened(guest, f);
    uint32_t h = new_handle(H_FILE);
    obj(h)->f = f;
    return h;
}
WINAPI(CreateFileA, "CreateFileA", 7) { return open_file(GSTR(ARG(0)), ARG(1), ARG(4)); }
WINAPI(CreateFileW, "CreateFileW", 7) {
    char s[512];
    size_t i = 0;
    for (uint16_t *w = GPTR(ARG(0)); w[i] && i < sizeof s - 1; i++) s[i] = (char)w[i];
    s[i] = 0;
    return open_file(s, ARG(1), ARG(4));
}

/* OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent. Reads complete before returning. */
static uint32_t file_io(int write) {
    Obj *o = obj(ARG(0));
    uint32_t ov = ARG(4);
    if (!o || !o->f) { last_error = 6; return 0; }
    if (ov) fseeko(o->f, MEM32(ov + 8) | (off_t)MEM32(ov + 12) << 32, SEEK_SET);
    off_t at = ftello(o->f);
    size_t n;
    if (write) n = fwrite(GPTR(ARG(1)), 1, ARG(2), o->f);
    else {
        /* Never ask fread for more than the file holds: at the end of the file the C library zeroed the front of
         * the destination buffer, and the game reads a save a second time at EOF (expecting 0 bytes and its
         * buffer untouched), which turned every save into "damaged folders". */
        fseeko(o->f, 0, SEEK_END);
        off_t end = ftello(o->f);
        fseeko(o->f, at, SEEK_SET);
        size_t want = ARG(2);
        if (at >= end) want = 0;
        else if ((uint64_t)(end - at) < want) want = (size_t)(end - at);
        n = want ? fread(GPTR(ARG(1)), 1, want, o->f) : 0;
    }
    if (ARG(2) >= 0x400 && ARG(2) <= 0x4800 && ARG(2) % 0x400 == 0)  /* save files */
        rt_log("save: %s handle %X buf %08X size %u at %lld -> %u%s", write ? "WriteFile" : "ReadFile", ARG(0), ARG(1), ARG(2), (long long)at, (unsigned)n, ov ? " overlapped" : "");
    if (!write && ARG(1) == 0x00933608u && ARG(2) == 0xC00) {
        const uint8_t *b = GPTR(0x00933608u);
        int lead = 0;
        while (lead < 0xC00 && !b[lead]) lead++;
        rt_log("save:   after read: first bytes %02X%02X%02X%02X, leading zeros %d", b[0], b[1], b[2], b[3], lead);
    }
    if (write) fflush(o->f);
    if (ARG(3)) MEM32(ARG(3)) = (uint32_t)n;
    if (ov) {
        MEM32(ov) = 0;
        MEM32(ov + 4) = (uint32_t)n;
        if (obj(MEM32(ov + 16))) signal_obj(obj(MEM32(ov + 16)), 1);
    }
    return 1;
}
WINAPI(ReadFile, "ReadFile", 5) { return file_io(0); }
WINAPI(WriteFile, "WriteFile", 5) { return file_io(1); }
WINAPI(GetOverlappedResult, "GetOverlappedResult", 4) {
    MEM32(ARG(2)) = MEM32(ARG(1) + 4);
    return 1;
}
WINAPI(SetFilePointer, "SetFilePointer", 4) {
    Obj *o = obj(ARG(0));
    if (!o || !o->f) return 0xFFFFFFFFu;
    off_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (off_t)(ARG(1) | (uint64_t)MEM32(ARG(2)) << 32);
    fseeko(o->f, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    off_t pos = ftello(o->f);
    if (ARG(2)) MEM32(ARG(2)) = (uint32_t)(pos >> 32);
    return (uint32_t)pos;
}
WINAPI(GetFileSize, "GetFileSize", 2) {
    Obj *o = obj(ARG(0));
    if (!o || !o->f) return 0xFFFFFFFFu;
    struct stat st;
    fstat(fileno(o->f), &st);
    if (ARG(1)) MEM32(ARG(1)) = (uint32_t)(st.st_size >> 32);
    return (uint32_t)st.st_size;
}

static uint32_t path_op(int (*op)(const char *)) {
    char p[512];
    return op(rt_path(GSTR(ARG(0)), p, sizeof p)) == 0;
}
static int mkdir_755(const char *p) { return mkdir(p, 0755); }
WINAPI(DeleteFileA, "DeleteFileA", 1) { return path_op(unlink); }
WINAPI(RemoveDirectoryA, "RemoveDirectoryA", 1) { return path_op(rmdir); }
WINAPI(CreateDirectoryA, "CreateDirectoryA", 2) { return path_op(mkdir_755); }
WINAPI(SetFileAttributesA, "SetFileAttributesA", 2) { return 1; }
WINAPI(CopyFileA, "CopyFileA", 3) {
    char a[512], b[512];
    FILE *in = fopen(rt_path(GSTR(ARG(0)), a, sizeof a), "rb"), *out = NULL;
    struct stat st;
    if (in && !(ARG(2) && stat(rt_path(GSTR(ARG(1)), b, sizeof b), &st) == 0))
        out = fopen(rt_path(GSTR(ARG(1)), b, sizeof b), "wb");
    char buf[65536];
    size_t n;
    while (in && out && (n = fread(buf, 1, sizeof buf, in))) fwrite(buf, 1, n, out);
    if (in) fclose(in);
    if (out) fclose(out);
    return out != NULL;
}
WINAPI(GetFullPathNameA, "GetFullPathNameA", 4) {
    const char *in = GSTR(ARG(0));
    char full[512];
    if (in[0] && in[1] == ':') snprintf(full, sizeof full, "%s", in);
    else snprintf(full, sizeof full, "C:\\SH2\\%s", in + (in[0] == '\\'));
    uint32_t n = strlen(full);
    if (n + 1 > ARG(1)) return n + 1;
    strcpy(GPTR(ARG(2)), full);
    if (ARG(3)) {
        char *slash = strrchr(GPTR(ARG(2)), '\\');
        MEM32(ARG(3)) = slash ? GVA(slash + 1) : ARG(2);
    }
    return n;
}
/* D: is the game's CD (DRIVE_CDROM): the disc check opens D:\\Movie\\Open.bik, which rt_path finds in
 * data/movie. Every other drive is fixed. */
WINAPI(GetDriveTypeA, "GetDriveTypeA", 1) {
    const char *d = ARG(0) ? GSTR(ARG(0)) : "C";
    return (d[0] == 'd' || d[0] == 'D') ? 5 : 3;
}
WINAPI(GetDiskFreeSpaceExA, "GetDiskFreeSpaceExA", 4) {
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) { MEM32(ARG(i)) = 0x40000000u; MEM32(ARG(i) + 4) = 0; }
    return 1;
}

/* Windows wildcards (* and ?), ignoring case. */
static int wildmatch(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') return wildmatch(p + 1, s) || (*s && wildmatch(p, s + 1));
    return *s && (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) && wildmatch(p + 1, s + 1);
}

/* WIN32_FIND_DATAA: attributes @0, size @28/@32, cFileName @44. */
static void find_entry(uint32_t fd, const char *name, int dir, uint64_t size, time_t mtime) {
    memset(GPTR(fd), 0, 320);
    MEM32(fd) = dir ? 0x10 : 0x80;
    uint64_t ft = ((uint64_t)mtime + 11644473600ull) * 10000000ull;  /* creation, access, write: the file's mtime */
    for (int i = 0; i < 3; i++) {
        MEM32(fd + 4 + 8 * i) = (uint32_t)ft;
        MEM32(fd + 8 + 8 * i) = (uint32_t)(ft >> 32);
    }
    MEM32(fd + 28) = (uint32_t)(size >> 32);
    MEM32(fd + 32) = (uint32_t)size;
    snprintf((char *)GPTR(fd + 44), 260, "%s", name);
}

/* Windows lists "." and ".." first in any folder, and the save code counts them: a save folder is
 * whole when it holds 5 entries. Horizon's readdir has neither, so they are made up here, the same
 * on both hosts. */
static int find_fill(Obj *o, uint32_t fd) {
    while (o->dir && o->dots < 2) {
        const char *dot = o->dots++ ? ".." : ".";
        if (wildmatch(o->pattern, dot)) { find_entry(fd, dot, 1, 0, time(NULL)); return 1; }
    }
    struct dirent *e;
    while (o->dir && (e = readdir(o->dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !wildmatch(o->pattern, e->d_name)) continue;
        char p[1024];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", o->dirpath, e->d_name);
        if (stat(rt_path(p, p, sizeof p), &st)) continue;  /* the size of what opening it reads (EE music) */
        find_entry(fd, e->d_name, S_ISDIR(st.st_mode), (uint64_t)st.st_size, st.st_mtime);
        if (has_save(o->dirpath)) rt_log("save: find %s [%s] -> %s%s %lld", o->dirpath, o->pattern, e->d_name, S_ISDIR(st.st_mode) ? "/" : "", (long long)st.st_size);
        if (rt_trace) rt_log("  find %s [%s] -> %s%s %lld mtime %lld", o->dirpath, o->pattern, e->d_name, S_ISDIR(st.st_mode) ? "/" : "", (long long)st.st_size, (long long)st.st_mtime);
        return 1;
    }
    last_error = ERROR_NO_MORE_FILES;
    return 0;
}
WINAPI(FindFirstFileA, "FindFirstFileA", 2) {
    char guest[512], *slash;
    snprintf(guest, sizeof guest, "%s", GSTR(ARG(0)));
    for (char *c = guest; *c; c++) if (*c == '\\') *c = '/';
    uint32_t h = new_handle(H_FIND);
    Obj *o = obj(h);
    if ((slash = strrchr(guest, '/'))) {
        *slash = 0;
        rt_path(guest, o->dirpath, sizeof o->dirpath);
        snprintf(o->pattern, sizeof o->pattern, "%s", slash + 1);
    } else {
        strcpy(o->dirpath, ".");
        snprintf(o->pattern, sizeof o->pattern, "%s", guest);
    }
    if (!strcmp(o->pattern, "*.*")) strcpy(o->pattern, "*");
    o->dir = opendir(o->dirpath);
    if (rt_trace) rt_log("  FindFirstFile %s [%s] %s", o->dirpath, o->pattern, o->dir ? "" : "(no such folder)");
    if (find_fill(o, ARG(1))) return h;
    close_handle(h);
    last_error = ERROR_FILE_NOT_FOUND;
    return INVALID_HANDLE;
}
WINAPI(FindNextFileA, "FindNextFileA", 2) { Obj *o = obj(ARG(0)); return o && find_fill(o, ARG(1)); }
WINAPI(FindClose, "FindClose", 1) { return close_handle(ARG(0)); }

/* File mappings are copies: MapViewOfFile reads the file into guest memory. */
WINAPI(CreateFileMappingA, "CreateFileMappingA", 6) {
    uint32_t h = new_handle(H_MAPPING);
    obj(h)->map_file = ARG(0);
    return h;
}
WINAPI(MapViewOfFile, "MapViewOfFile", 5) {
    Obj *m = obj(ARG(0)), *f = m ? obj(m->map_file) : NULL;
    if (!f || !f->f) return 0;
    struct stat st;
    fstat(fileno(f->f), &st);
    off_t off = ARG(3) | (off_t)ARG(2) << 32;
    uint32_t n = ARG(4) ? ARG(4) : (uint32_t)(st.st_size - off);
    uint32_t va = galloc(n);
    fseeko(f->f, off, SEEK_SET);
    if (fread(GPTR(va), 1, n, f->f) != n) rt_log("MapViewOfFile: short read");
    return va;
}
WINAPI(UnmapViewOfFile, "UnmapViewOfFile", 1) { gfree(ARG(0)); return 1; }

/* ---- memory ---- */
WINAPI(HeapCreate, "HeapCreate", 3) { return 0x00F00000u; }
WINAPI(HeapDestroy, "HeapDestroy", 1) { return 1; }
WINAPI(HeapAlloc, "HeapAlloc", 3) { return galloc(ARG(2)); }
WINAPI(HeapFree, "HeapFree", 3) { gfree(ARG(2)); return 1; }
WINAPI(GlobalMemoryStatus, "GlobalMemoryStatus", 1) {
    uint32_t m = ARG(0), v[8] = {32, 10, 512u << 20, 384u << 20, 1024u << 20, 900u << 20, 2047u << 20, 1800u << 20};
    memcpy(GPTR(m), v, sizeof v);
    return 0;
}

/* ---- process, modules, resources ---- */
WINAPI(GetLastError, "GetLastError", 0) { return last_error; }
WINAPI(ExitProcess, "ExitProcess", 1) { rt_log("ExitProcess(%u)", ARG(0)); exit(ARG(0)); }
WINAPI(OutputDebugStringA, "OutputDebugStringA", 1) { rt_log("debug: %s", GSTR(ARG(0))); return 0; }
WINAPI(IsProcessorFeaturePresent, "IsProcessorFeaturePresent", 1) { return 0; }
WINAPI(GetStartupInfoA, "GetStartupInfoA", 1) {
    memset(GPTR(ARG(0)), 0, 68);
    MEM32(ARG(0)) = 68;
    return 0;
}
WINAPI(GetVersionExA, "GetVersionExA", 1) {
    uint32_t v = ARG(0);
    MEM32(v + 4) = 5;      /* Windows XP 5.1.2600 */
    MEM32(v + 8) = 1;
    MEM32(v + 12) = 2600;
    MEM32(v + 16) = 2;     /* VER_PLATFORM_WIN32_NT */
    *(char *)GPTR(v + 20) = 0;
    return 1;
}
WINAPI(GetModuleHandleA, "GetModuleHandleA", 1) { return ARG(0) ? 0x0F000000u : 0x00400000u; }
WINAPI(LoadLibraryA, "LoadLibraryA", 1) {
    if (rt_trace) rt_log("  LoadLibraryA(%s)", GSTR(ARG(0)));
    return 0x0F000000u;
}
WINAPI(GetProcAddress, "GetProcAddress", 2) {
    if (ARG(1) < 0x10000) return 0;
    extern const Bridge __start_sh2_bridges[], __stop_sh2_bridges[];
    for (const Bridge *b = __start_sh2_bridges; b < __stop_sh2_bridges; b++)
        if (!strcmp(b->name, GSTR(ARG(1)))) return rt_host_fn(b->name, b->fn);
    rt_log("GetProcAddress(%s): not bridged", GSTR(ARG(1)));
    return 0;
}

/* Resource directory walk: type -> name -> first language. Names are UTF-16, compared ignoring case. */
static int res_ch(uint32_t s, int i, int wide) { return wide ? *(uint16_t *)GPTR(s + 2 * i) : *(uint8_t *)GPTR(s + i); }
static int res_id_matches(uint32_t entry, uint32_t rsrc, uint32_t want, int wide) {
    uint32_t id = MEM32(entry);
    if (want < 0x10000) return !(id & 0x80000000u) && id == want;
    if (!(id & 0x80000000u)) return 0;
    uint32_t s = rsrc + (id & 0x7FFFFFFF);
    uint16_t len = *(uint16_t *)GPTR(s);
    for (uint32_t i = 0; i <= len; i++) {
        int a = i < len ? *(uint16_t *)GPTR(s + 2 + 2 * i) : 0, b = res_ch(want, i, wide);
        if ((a < 128 ? tolower(a) : a) != (b < 128 ? tolower(b) : b)) return 0;
    }
    return 1;
}
static uint32_t res_find(uint32_t dir, uint32_t rsrc, uint32_t want, int wide) {
    if (want >= 0x10000 && want != 0xFFFFFFFFu && res_ch(want, 0, wide) == '#') {  /* "#123" names an id */
        uint32_t v = 0;
        for (int i = 1, c; (c = res_ch(want, i, wide)) >= '0' && c <= '9'; i++) v = v * 10 + c - '0';
        want = v;
    }
    uint32_t n = *(uint16_t *)GPTR(dir + 12) + *(uint16_t *)GPTR(dir + 14);
    for (uint32_t i = 0, e = dir + 16; i < n; i++, e += 8)
        if (want == 0xFFFFFFFFu || res_id_matches(e, rsrc, want, wide)) return MEM32(e + 4);
    return 0;
}
static uint32_t find_resource(uint32_t name, uint32_t type, int wide) {
    uint32_t rsrc = 0x400000 + MEM32(0x400000 + MEM32(0x40003C) + 24 + 96 + 16), sub;
    if (!(sub = res_find(rsrc, rsrc, type, wide)) || !(sub & 0x80000000u)) return 0;
    if (!(sub = res_find(rsrc + (sub & 0x7FFFFFFF), rsrc, name, wide)) || !(sub & 0x80000000u)) return 0;
    if (!(sub = res_find(rsrc + (sub & 0x7FFFFFFF), rsrc, 0xFFFFFFFFu, wide)) || (sub & 0x80000000u)) return 0;
    return rsrc + sub;  /* IMAGE_RESOURCE_DATA_ENTRY: OffsetToData (RVA), Size */
}
WINAPI(FindResourceA, "FindResourceA", 3) { return find_resource(ARG(1), ARG(2), 0); }
WINAPI(FindResourceW, "FindResourceW", 3) { return find_resource(ARG(1), ARG(2), 1); }
WINAPI(LoadResource, "LoadResource", 2) { return ARG(1) ? 0x400000 + MEM32(ARG(1)) : 0; }
WINAPI(LockResource, "LockResource", 1) { return ARG(0); }
WINAPI(SizeofResource, "SizeofResource", 2) { return ARG(1) ? MEM32(ARG(1) + 4) : 0; }

/* ---- strings and locale ---- */
WINAPI(lstrlenA, "lstrlenA", 1) { return ARG(0) ? strlen(GSTR(ARG(0))) : 0; }
WINAPI(lstrcpyA, "lstrcpyA", 2) { strcpy(GPTR(ARG(0)), GSTR(ARG(1))); return ARG(0); }
WINAPI(lstrcatA, "lstrcatA", 2) { strcat(GPTR(ARG(0)), GSTR(ARG(1))); return ARG(0); }
WINAPI(lstrcmpiA, "lstrcmpiA", 2) { return strcasecmp(GSTR(ARG(0)), GSTR(ARG(1))); }
WINAPI(GetACP, "GetACP", 0) { return 1252; }
WINAPI(GetThreadLocale, "GetThreadLocale", 0) { return 0x409; }
WINAPI(GetLocaleInfoA, "GetLocaleInfoA", 4) {
    const char *v = (ARG(1) & 0xFFF) == 0x1004 ? "1252" : (ARG(1) & 0xFFF) == 0x0B ? "437" : "";
    if (!ARG(3)) return strlen(v) + 1;
    snprintf(GPTR(ARG(2)), ARG(3), "%s", v);
    return strlen(v) + 1;
}
/* Code pages as Latin-1: byte value == code point. */
WINAPI(MultiByteToWideChar, "MultiByteToWideChar", 6) {
    const uint8_t *s = GPTR(ARG(2));
    int n = (int32_t)ARG(3) < 0 ? (int)strlen((const char *)s) + 1 : (int)ARG(3);
    if (!ARG(5)) return n;
    if (n > (int)ARG(5)) n = ARG(5);
    for (int i = 0; i < n; i++) ((uint16_t *)GPTR(ARG(4)))[i] = s[i];
    return n;
}
WINAPI(WideCharToMultiByte, "WideCharToMultiByte", 8) {
    const uint16_t *s = GPTR(ARG(2));
    int n = (int)ARG(3);
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    if (!ARG(5)) return n;
    if (n > (int)ARG(5)) n = ARG(5);
    for (int i = 0; i < n; i++) ((uint8_t *)GPTR(ARG(4)))[i] = s[i] < 256 ? s[i] : '?';
    return n;
}
