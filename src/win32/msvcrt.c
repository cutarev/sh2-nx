/* MSVCR70 / MSVCP70: the C runtime the game imports, implemented over guest memory. */
#include "../runtime/rt.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>

/* ---- printf family: format string and arguments live in guest memory ---- */

int rt_format(char *out, size_t n, const char *fmt, uint32_t args) {
    size_t len = 0;
#define EMIT(...) do { int _k = snprintf(out + len, len < n ? n - len : 0, __VA_ARGS__); if (_k > 0) len += _k; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { if (len + 1 < n) out[len] = *p; len++; continue; }
        char spec[32] = "%";
        int s = 1, wide64 = 0, isshort = 0;
        for (p++; *p && strchr("-+ #0", *p); p++) spec[s++] = *p;
        for (; *p == '*' || isdigit((unsigned char)*p) || *p == '.'; p++) {
            if (*p == '*') { s += snprintf(spec + s, sizeof spec - s, "%d", (int32_t)MEM32(args)); args += 4; }
            else spec[s++] = *p;
        }
        if (*p == 'l' && p[1] == 'l') { wide64 = 1; p += 2; }
        else if (!strncmp(p, "I64", 3)) { wide64 = 1; p += 3; }
        else if (*p == 'h') { isshort = 1; p++; }
        else if (*p == 'l' || *p == 'L' || *p == 'I') p++;
        char c = *p;
        if (!c) break;
        if (c == '%') { if (len + 1 < n) out[len] = '%'; len++; continue; }
        if (strchr("diouxXc", c)) {
            if (wide64) { spec[s++] = 'l'; spec[s++] = 'l'; }
            spec[s++] = c;
            spec[s] = 0;
            uint64_t v = MEM32(args) | (wide64 ? (uint64_t)MEM32(args + 4) << 32 : 0);
            args += wide64 ? 8 : 4;
            if (isshort) v = (c == 'd' || c == 'i') ? (uint64_t)(int16_t)v : (uint16_t)v;
            else if (!wide64 && (c == 'd' || c == 'i')) v = (uint64_t)(int64_t)(int32_t)v;
            if (wide64) EMIT(spec, (long long)v); else EMIT(spec, (int)v);
        } else if (strchr("eEfgGaA", c)) {
            double d;
            memcpy(&d, GPTR(args), 8);
            args += 8;
            spec[s++] = c;
            spec[s] = 0;
            EMIT(spec, d);
        } else if (c == 's' || c == 'S') {
            uint32_t sv = MEM32(args);
            args += 4;
            spec[s++] = 's';
            spec[s] = 0;
            EMIT(spec, sv ? GSTR(sv) : "(null)");
        } else if (c == 'p') {
            EMIT("%08X", MEM32(args));
            args += 4;
        } else if (c == 'n') {
            MEM32(MEM32(args)) = (uint32_t)len;
            args += 4;
        }
    }
    if (n) out[len < n ? len : n - 1] = 0;
    return (int)len;
#undef EMIT
}

static int gvsprintf(uint32_t buf, uint32_t fmt, uint32_t args) {
    char tmp[8192];
    int k = rt_format(tmp, sizeof tmp, GSTR(fmt), args);
    strcpy(GPTR(buf), tmp);
    return k;
}
CDECL(c_sprintf, "sprintf") { return gvsprintf(ARG(0), ARG(1), g_esp + 12); }
CDECL(c_vsprintf, "vsprintf") { return gvsprintf(ARG(0), ARG(1), ARG(2)); }
CDECL(c_printf, "printf") {
    char tmp[8192];
    int k = rt_format(tmp, sizeof tmp, GSTR(ARG(0)), g_esp + 8);
    fputs(tmp, stdout);
    return k;
}
CDECL(c_puts, "puts") { return puts(GSTR(ARG(0))); }

/* ---- FILE: a guest FILE* is a guest copy of MSVC's struct _iobuf. _file (+16) holds our index;
 * _flag (+12) is kept current because feof/ferror are macros that the game reads inline. ---- */
static FILE *files[256];
static FILE *gfile(uint32_t va) {
    uint32_t i = va ? MEM32(va + 16) : 0;
    if (i >= 256 || !files[i]) rt_fatal("bad FILE* %08X", va);
    return files[i];
}
static uint32_t sync_flags(uint32_t va, uint32_t r) {
    FILE *f = gfile(va);
    MEM32(va + 12) = 0x3 | (feof(f) ? 0x10 : 0) | (ferror(f) ? 0x20 : 0);  /* _IOREAD|_IOWRT, _IOEOF, _IOERR */
    return r;
}
void ee_file_opened(const char *guest, FILE *f);  /* src/game/ee.c */
CDECL(c_fopen, "fopen") {
    char path[512], mode[8];
    rt_path(GSTR(ARG(0)), path, sizeof path);
    snprintf(mode, sizeof mode, "%s", GSTR(ARG(1)));
    for (char *m = mode; *m; m++) if (*m == 't') *m = 'b';  /* no text mode translation, same as the files on disk */
    FILE *f = fopen(path, mode);
    if (rt_trace) rt_log("  fopen %s (%s) -> %s", GSTR(ARG(0)), path, f ? "ok" : "FAILED");
    if (!f) return 0;
    if (!strchr(mode, 'w') && !strchr(mode, 'a') && !strchr(mode, '+')) ee_file_opened(GSTR(ARG(0)), f);
    for (uint32_t i = 1; i < 256; i++)
        if (!files[i]) {
            files[i] = f;
            uint32_t va = galloc(32);
            MEM32(va + 16) = i;
            return sync_flags(va, va);
        }
    rt_fatal("too many open files");
}
CDECL(c_fclose, "fclose") {
    fclose(gfile(ARG(0)));
    files[MEM32(ARG(0) + 16)] = NULL;
    gfree(ARG(0));
    return 0;
}
CDECL(c_fread, "fread") { return sync_flags(ARG(3), fread(GPTR(ARG(0)), ARG(1), ARG(2), gfile(ARG(3)))); }
CDECL(c_fwrite, "fwrite") { return sync_flags(ARG(3), fwrite(GPTR(ARG(0)), ARG(1), ARG(2), gfile(ARG(3)))); }
CDECL(c_fseek, "fseek") { return sync_flags(ARG(0), fseek(gfile(ARG(0)), (int32_t)ARG(1), ARG(2))); }
CDECL(c_ftell, "ftell") { return ftell(gfile(ARG(0))); }
CDECL(c_fgets, "fgets") { return sync_flags(ARG(2), fgets(GPTR(ARG(0)), ARG(1), gfile(ARG(2))) ? ARG(0) : 0); }
CDECL(c_fputs, "fputs") { return sync_flags(ARG(1), fputs(GSTR(ARG(0)), gfile(ARG(1)))); }
CDECL(c_fprintf, "fprintf") {
    char tmp[8192];
    int k = rt_format(tmp, sizeof tmp, GSTR(ARG(1)), g_esp + 12);
    fputs(tmp, gfile(ARG(0)));
    return k;
}

/* ---- strings ---- */
CDECL(c_strncpy, "strncpy") { strncpy(GPTR(ARG(0)), GSTR(ARG(1)), ARG(2)); return ARG(0); }
CDECL(c_strncat, "strncat") { strncat(GPTR(ARG(0)), GSTR(ARG(1)), ARG(2)); return ARG(0); }
CDECL(c_strncmp, "strncmp") { return strncmp(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2)); }
CDECL(c_stricmp, "_stricmp") { return strcasecmp(GSTR(ARG(0)), GSTR(ARG(1))); }
CDECL(c_strnicmp, "_strnicmp") { return strncasecmp(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2)); }
CDECL(c_strstr, "strstr") { return GVA(strstr(GPTR(ARG(0)), GSTR(ARG(1)))); }
CDECL(c_strtok, "strtok") { return GVA(strtok(ARG(0) ? GPTR(ARG(0)) : NULL, GSTR(ARG(1)))); }
CDECL(c_strtoul, "strtoul") {
    char *end;
    uint32_t v = strtoul(GSTR(ARG(0)), &end, ARG(2));
    if (ARG(1)) MEM32(ARG(1)) = GVA(end);
    return v;
}
CDECL(c_wcslen, "wcslen") { uint32_t n = 0; while (((uint16_t *)GPTR(ARG(0)))[n]) n++; return n; }
CDECL(c_isspace, "isspace") { return isspace((int)ARG(0)) != 0; }
CDECL(c_isdigit, "isdigit") { return isdigit((int)ARG(0)) != 0; }
CDECL(c_iswascii, "iswascii") { return (ARG(0) & 0xFFFF) < 0x80; }

/* qsort calls the guest comparator: int __cdecl cmp(const void *, const void *). */
static __thread uint32_t qsort_cmp;
static int qsort_tramp(const void *a, const void *b) {
    uint32_t args[2] = {GVA(a), GVA(b)};
    return (int32_t)guest_call(qsort_cmp, 2, args, 0);
}
CDECL(c_qsort, "qsort") {
    uint32_t saved = qsort_cmp;
    qsort_cmp = ARG(3);
    qsort(GPTR(ARG(0)), ARG(1), ARG(2), qsort_tramp);
    qsort_cmp = saved;
    return 0;
}
CDECL(c_srand, "srand") { srand(ARG(0)); return 0; }

/* ---- memory ---- */
CDECL(c_malloc, "malloc") { return galloc(ARG(0)); }
CDECL(c_calloc, "calloc") { return galloc(ARG(0) * ARG(1)); }
CDECL(c_free, "free") { gfree(ARG(0)); return 0; }
CDECL(c_new, "??2@YAPAXI@Z") { return galloc(ARG(0)); }
CDECL(c_new_array, "??_U@YAPAXI@Z") { return galloc(ARG(0)); }
CDECL(c_delete, "??3@YAXPAX@Z") { gfree(ARG(0)); return 0; }
CDECL(c_delete_array, "??_V@YAXPAX@Z") { gfree(ARG(0)); return 0; }
CDECL(c_aligned_malloc, "_aligned_malloc") {
    uint32_t align = ARG(1) < 4 ? 4 : ARG(1), raw = galloc(ARG(0) + align + 4);
    uint32_t p = (raw + 4 + align - 1) & ~(align - 1);
    MEM32(p - 4) = raw;
    return p;
}
CDECL(c_aligned_free, "_aligned_free") { if (ARG(0)) gfree(MEM32(ARG(0) - 4)); return 0; }

/* ---- math: MSVC returns doubles in ST(0); the _CI helpers also take their arguments there ---- */
static double darg(int i) { double d; memcpy(&d, GPTR(g_esp + 4 + 4 * i), 8); return d; }
CDECL(c_floor, "floor") { rt_fpush(floor(darg(0))); return 0; }
CDECL(c_ceil, "ceil") { rt_fpush(ceil(darg(0))); return 0; }
CDECL(c_finite, "_finite") { return isfinite(darg(0)); }
CDECL(c_CIpow, "_CIpow") { double y = rt_fpop(), x = rt_fpop(); rt_fpush(pow(x, y)); return 0; }
CDECL(c_CIfmod, "_CIfmod") { double y = rt_fpop(), x = rt_fpop(); rt_fpush(fmod(x, y)); return 0; }
CDECL(c_CIasin, "_CIasin") { rt_fpush(asin(rt_fpop())); return 0; }
CDECL(c_CIacos, "_CIacos") { rt_fpush(acos(rt_fpop())); return 0; }
/* _ftol: ST(0) truncated to a 64-bit integer in EDX:EAX, popped. */
CDECL(c_ftol, "_ftol") {
    int64_t v = (int64_t)rt_fpop();
    g_edx = (uint32_t)((uint64_t)v >> 32);
    return (uint32_t)v;
}
/* _controlfp(new, mask) in the CRT's portable bits; precision and rounding reach the x87 model. */
static uint32_t cw_portable = 0x0009001F;
CDECL(c_controlfp, "_controlfp") {
    cw_portable = (cw_portable & ~ARG(1)) | (ARG(0) & ARG(1));
    static const uint16_t pc[4] = {0x300, 0x200, 0x000, 0x300};  /* _PC_64, _PC_53, _PC_24 -> x87 PC bits */
    g_fp_control_word = (g_fp_control_word & ~0xF00) | pc[(cw_portable >> 16) & 3] | ((cw_portable & 0x300) << 2);
    return cw_portable;
}

/* ---- startup and exit ---- */
static uint32_t fmode_va, commode_va, onexit[64], n_onexit;
CDECL(c_set_app_type, "__set_app_type") { return 0; }
CDECL(c_setusermatherr, "__setusermatherr") { return 0; }
CDECL(c_p_fmode, "__p__fmode") { if (!fmode_va) fmode_va = galloc(4); return fmode_va; }
CDECL(c_p_commode, "__p__commode") { if (!commode_va) commode_va = galloc(4); return commode_va; }
CDECL(c_getmainargs, "__getmainargs") {
    uint32_t argv = galloc(8), envp = galloc(4);
    MEM32(argv) = gstrdup("sh2pc.exe");
    MEM32(ARG(0)) = 1;
    MEM32(ARG(1)) = argv;
    MEM32(ARG(2)) = envp;
    return 0;
}
/* Runs the static constructors in [begin, end). */
CDECL(c_initterm, "_initterm") {
    for (uint32_t p = ARG(0), end = ARG(1); p < end; p += 4)
        if (MEM32(p)) guest_call(MEM32(p), 0, NULL, 0);
    return 0;
}
CDECL(c_onexit, "_onexit") { if (n_onexit < 64) onexit[n_onexit++] = ARG(0); return ARG(0); }
CDECL(c_dllonexit, "__dllonexit") { if (n_onexit < 64) onexit[n_onexit++] = ARG(0); return ARG(0); }
static void run_onexit(void) { while (n_onexit) guest_call(onexit[--n_onexit], 0, NULL, 0); }
CDECL(c_exit, "exit") { rt_log("exit(%d)", (int)ARG(0)); run_onexit(); exit(ARG(0)); }
CDECL(c__exit, "_exit") { rt_log("_exit(%d)", (int)ARG(0)); _Exit(ARG(0)); }
CDECL(c_cexit, "_cexit") { run_onexit(); return 0; }
CDECL(c_c_exit, "_c_exit") { return 0; }
CDECL(c_amsg_exit, "_amsg_exit") { rt_fatal("_amsg_exit(%u)", ARG(0)); }
CDECL(c_terminate, "?terminate@@YAXXZ") { rt_fatal("terminate()"); }
CDECL(c_security, "__security_error_handler") { rt_fatal("__security_error_handler"); }

/* ---- exceptions: SEH and C++ throws only happen on error paths, so they stop the port loudly ---- */
CDECL(c_except_handler3, "_except_handler3") { rt_fatal("_except_handler3 called: a structured exception was raised"); }
CDECL(c_cxxframe, "__CxxFrameHandler") { rt_fatal("__CxxFrameHandler called"); }
CDECL(c_xcptfilter, "_XcptFilter") { rt_fatal("_XcptFilter: unhandled exception"); }
CDECL(c_cxxthrow, "_CxxThrowException") { rt_fatal("C++ exception thrown (object %08X, info %08X)", ARG(0), ARG(1)); }
/* The lifter pairs each call to _setjmp3 with a native setjmp (regen.sh passes --setjmp); this fills the
 * guest jmp_buf the way MSVC's does, and longjmp jumps back through the native one. */
CDECL(c_setjmp3, "_setjmp3") {
    uint32_t b = ARG(0);
    MEM32(b) = g_seh_ebp;
    MEM32(b + 4) = g_ebx;
    MEM32(b + 8) = g_edi;
    MEM32(b + 12) = g_esi;
    MEM32(b + 16) = g_esp;          /* at the return address, as setjmp's own esp */
    MEM32(b + 20) = MEM32(g_esp);
    MEM32(b + 24) = MEM32(g_fs_base);
    MEM32(b + 28) = 0xFFFFFFFFu;
    MEM32(b + 32) = 0x56433230u;    /* "VC20" */
    return 0;
}
CDECL(c_longjmp, "longjmp") {
    if (rt_trace)  /* libpng reports its error through a callback that longjmps: show the message */
        for (uint32_t a = g_esp; a < g_esp + 256; a += 4) {
            uint32_t v = MEM32(a);
            if (v >= 0x629000 && v < 0x799000 && strlen(GSTR(v)) > 3 && strlen(GSTR(v)) < 80)
                rt_log("  longjmp context string: %s", GSTR(v));
        }
    recomp_guest_longjmp(ARG(0), ARG(1));
    rt_fatal("longjmp(%08X) without a native setjmp", ARG(0));
}

/* std::exception (thiscall: this in ECX). Layout: vtable, what, doFree. */
WINAPI(exc_ctor, "??0exception@@QAE@XZ", 0) {
    MEM32(g_ecx + 4) = 0;
    MEM32(g_ecx + 8) = 0;
    return g_ecx;
}
WINAPI(exc_copy, "??0exception@@QAE@ABV0@@Z", 1) {
    MEM32(g_ecx + 4) = MEM32(ARG(0) + 4);
    MEM32(g_ecx + 8) = 0;
    return g_ecx;
}
WINAPI(exc_dtor, "??1exception@@UAE@XZ", 0) { return 0; }
WINAPI(typeinfo_dtor, "??1type_info@@UAE@XZ", 0) { return 0; }

/* MSVCP70 std::string (VC7 layout: allocator pad, 16-byte buffer or pointer, size, capacity). */
static uint32_t str_ptr(uint32_t s) { return MEM32(s + 24) < 16 ? s + 4 : MEM32(s + 4); }
static void str_set(uint32_t s, const char *src) {
    uint32_t n = strlen(src);
    if (n < 16) { MEM32(s + 24) = 15; strcpy(GPTR(s + 4), src); }
    else { MEM32(s + 24) = n; MEM32(s + 4) = gstrdup(src); }
    MEM32(s + 20) = n;
}
#define STR "?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@"
WINAPI(str_ctor, "??0" STR "QAE@PBD@Z", 1) { str_set(g_ecx, GSTR(ARG(0))); return g_ecx; }
WINAPI(str_copy, "??0" STR "QAE@ABV01@@Z", 1) { str_set(g_ecx, GSTR(str_ptr(ARG(0)))); return g_ecx; }
WINAPI(str_dtor, "??1" STR "QAE@XZ", 0) {
    if (MEM32(g_ecx + 24) >= 16) gfree(MEM32(g_ecx + 4));
    return 0;
}
CDECL(str_less, "??Mstd@@YA_NABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@0@Z") {
    return strcmp(GSTR(str_ptr(ARG(0))), GSTR(str_ptr(ARG(1)))) < 0;
}
