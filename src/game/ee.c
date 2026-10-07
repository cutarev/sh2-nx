/* Silent Hill 2 Enhancements (github.com/elishacloud/Silent-Hill-2-Enhancements, zlib license) ported
 * to the recompiled v1.0 executable. EE patches the game at run time from a d3d8.dll; here the code
 * patches are applied to the exe before recompilation (tools/patch_exe.py) and EE's hooks become the
 * functions below. A `sub_X` defined here replaces the generated one; `sub_X_gen` is the original,
 * which the recompiler emits under that name (regen.sh passes --exclude-manual for this file).
 *
 * Addresses are v1.0's, resolved from EE's search patterns by tools/eeaddr.py and tools/eepattern.py. */
#include "../runtime/rt.h"
#include <math.h>
#include <SDL2/SDL.h>
#include <sys/stat.h>
#include <ctype.h>
#include "../runtime/host.h"
#include "ee_sfx.h"

/* ---- game state (EE Patches/Common.cpp, Patches.h) ---- */
#define EE_ROOM_ID            0x01FB7DACu
#define EE_EVENT_INDEX        0x00932178u
#define EE_MENU_EVENT         0x00932174u
#define EE_TRANSITION_STATE   0x01F7D490u
#define EE_VIBRATION_LEVEL    0x01DBBFF9u  /* 0 off, 1 soft, 2 normal, 3 hard */
#define EE_RUMBLE_FLAG        0x00940EB6u
#define EE_GAMEPAD_TYPE       0x00941148u
enum { EVENT_IN_GAME = 4, R_NONE = 0, FADE_NONE = 0 };

/* ---- input (EE Patches/ControllerTweaks.cpp) ---- */
#define JOY_STATE        0x00940F20u  /* the game's DIJOYSTATE2 */
#define USING_GAMEPAD    0x01FB806Au
#define MOVE_DIRECTION   0x01FB7CE8u
#define STRAFING_LEFT    0x01FB7F26u
#define STRAFING_RIGHT   0x01FB7F27u

/* EE settings, as the Enhanced Edition ships them (d3d8.ini). */
static const int DPadMovementFix = 1;           /* DPAD_MOVEMENT_MODE: the d-pad moves like the stick */
static const int RestoreSearchCamMovement = 2;  /* right stick (Z / Rz) drives the search camera */

static int is_moving_around(void) {
    return MEM8(USING_GAMEPAD) || MEM8(STRAFING_LEFT) || MEM8(STRAFING_RIGHT);
}

/* ProcessDInputData(GamePadState *state): the game turning the polled DIJOYSTATE2 into its pad
 * state. EE's hook runs first: d-pad movement, then the right stick into state->m_rightStick. */
extern void sub_00458060_gen(void);
void sub_00458060(void) {
    uint32_t state = MEM32(g_esp + 4);
    uint32_t pov = MEM32(JOY_STATE + 32);
    if (DPadMovementFix && (pov & 0xFFFF) != 0xFFFF) {
        double a = (double)pov * M_PI / (180.0 * 100.0);
        MEM32(JOY_STATE + 0) = (uint32_t)(int32_t)(sin(a) * 32767.0);   /* lX */
        MEM32(JOY_STATE + 4) = (uint32_t)(int32_t)(cos(a) * -32767.0);  /* lY */
    }
    if (RestoreSearchCamMovement == 2) {
        *(int16_t *)GPTR(state + 0xC) = (int16_t)(int32_t)MEM32(JOY_STATE + 8);   /* lZ */
        *(int16_t *)GPTR(state + 0xE) = (int16_t)(int32_t)MEM32(JOY_STATE + 20);  /* lRz */
    }
    sub_00458060_gen();
}

/* UsingSearchCamera, as EE's UsingSearchCamera_SeparateAnalogs at the nine walking call sites EE
 * patches: walking and looking around at the same time. The two other callers keep the original. */
extern void sub_00534660_gen(void);
void sub_00534660(void) {
    uint32_t ret = MEM32(g_esp);
    if (ret != 0x0051EC65u && ret != 0x00547FFFu && is_moving_around()) {
        g_eax = 0;
        g_esp += 4;
        return;
    }
    sub_00534660_gen();
}

/* Cave 0x401460, called from 0x535CBD: EE's StartSearchCamera_Hook (TRUE: do not start it). */
void sub_00401460(void) {
    uint32_t dir = MEM32(MOVE_DIRECTION);
    g_eax = is_moving_around() ? 0 : (dir != 0 && dir != 3);
    g_esp += 4;
}

/* Cave 0x4015D0, called from 0x4697B5 (`mov eax, 10h` before the event index is stored): EE's
 * PauseScreenASM, leaving Options from the pause screen straight back to gameplay. */
void sub_004015D0(void) {
    guest_call(0x0047EEE0u, 0, NULL, 0);  /* fixes doubled text */
    guest_call(0x0046AB10u, 0, NULL, 0);  /* the call used when returning to gameplay */
    guest_call(0x00401032u, 0, NULL, 0);  /* fixes incorrect text */
    g_eax = 2;
    g_esp += 4;
}

/* ---- TexPatch (EE Patches/TexPatch.cpp, EnableTexAddrHack) ----
 * Texture files are read into load buffers sized for the original game; the EE HD textures are much
 * larger. tools/patch_exe.py points the game at these guest buffers instead. */
#define TEX_BUF1 0x04000000u
#define TEX_BUF2 0x06B00000u
#define TEX_BUF3 0x09600000u
#define TITLE_TEXPATH 0x007BA838u
#define TITLE_PATHS   0x0EC00000u   /* guest strings, committed in ee_init */
#define TEX_SIZE 44760576u   /* 2 x 22,380,640 bytes, rounded to 4 KB; buffer 3 is twice that */

/* ---- SfxPatch (EE Patches/SfxPatch.cpp, EnableSFXAddrHack) ----
 * The sound effect bank (data/sound/sddata.bin) is read into a static buffer sized for the 17 MB
 * original; the audio pack's is 88 MB. tools/patch_exe.py points the game at this guest buffer, and
 * ee_sfx_init re-indexes the effect table at 0x8A67DC to the RIFF positions of the file in use. */
#define SFX_BUF 0xD0000000u

static void ee_sfx_init(void) {
    char host[512];
    struct stat st;
    uint32_t size = 0;
    const char *names[] = {"data/sound/sddata.bin", "data/sound/sddatn.bin"};
    for (int i = 0; i < 2; i++)
        if (stat(rt_path(names[i], host, sizeof host), &st) == 0 && (uint32_t)st.st_size > size) size = st.st_size;
    if (!size) return;
    host_arena_commit(SFX_BUF, (size + 0x2FFFu) & ~0xFFFu);
    rt_path(names[0], host, sizeof host);
    if (strncmp(host, "sh2e/", 5)) return;  /* the original bank: the table already matches */
    FILE *f = fopen(host, "rb");
    if (!f) return;
    enum { BLOCK = 1 << 20 };
    static uint8_t chunk[BLOCK + 5];
    uint32_t found[EE_SFX_COUNT], n = 0, x = 0;
    while (x + 5 < size && n < EE_SFX_COUNT) {
        size_t want = size - x < BLOCK + 5 ? size - x : BLOCK + 5;
        fseek(f, x, SEEK_SET);
        want = fread(chunk, 1, want, f);
        if (want < 5) break;
        const uint8_t *p = memmem(chunk, want, "RIFF", 4);
        if (p) {
            found[n++] = x + (uint32_t)(p - chunk);
            x += (uint32_t)(p - chunk) + 5;
        } else
            x += BLOCK;
    }
    fclose(f);
    if (n != EE_SFX_COUNT) {
        rt_log("ee: sfx: found %u of %u RIFF banks in %s, table left alone", n, EE_SFX_COUNT, host);
        return;
    }
    for (int i = 0; i < 700; i++) MEM32(EE_SFX_TABLE + 4 * i) = found[ee_sfx_map[i]];
    rt_log("ee: sfx table remapped to %s (%u bytes)", host, size);
}

/* The file read at 0x449FD7 (0x4494A0) gets the load buffer as its second argument; EE reads the same
 * value from ebp at 0x44A00D, but ebp is a local of the lifted caller, so it is remembered here. */
static uint32_t tex_load_buffer;

/* FullscreenImages core (EE GetTextureOnLoad / SetImageScaling): when a texture EE lists by its
 * original size is loaded, the sprite code's scale (patched to read these) becomes new / original
 * size, so coordinates in original texels still land on the same part of an HD image. */
#include "ee_textures.h"
#define EE_TEX_SCALE_X 0x0EC01000u
#define EE_TEX_SCALE_Y 0x0EC01004u
#define TEX_NAME_PTR   0x009335ACu   /* char * of the texture being loaded */
/* Called for every file the game opens for reading (EE OnFileLoadTex): a texture EE lists by its
 * original size sets the scale from the header of the file actually opened (sh2e or data). */
void ee_file_opened(const char *guest, FILE *f) {
    char name[256];
    size_t n = 0;
    for (; guest[n] && n < sizeof name - 1; n++) name[n] = guest[n] == '\\' ? '/' : (char)tolower((unsigned char)guest[n]);
    name[n] = 0;
    const char *d = strstr(name, "data/");  /* drop "./", a drive or the game folder in front */
    if (!d) return;
    memmove(name, d, strlen(d) + 1);
    static int logged;
    if (strstr(name, "/pic/") && logged++ < 30) rt_log("ee: open %s", name);
    for (size_t i = 0; i < sizeof ee_texture_list / sizeof *ee_texture_list; i++) {
        const EeTexSize *t = &ee_texture_list[i];
        if (!t->ref || strcmp(t->name, name)) continue;
        uint8_t hdr[128];
        long at = ftell(f);
        size_t got = fread(hdr, 1, sizeof hdr, f);
        fseek(f, at, SEEK_SET);
        if (got != sizeof hdr || (hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24) != 0x19990901u) return;
        uint32_t w = 0, h = 0;
        static const int off[3] = {20, 24, 56};  /* EE GetTextureRes: first non-zero size */
        for (int k = 0; k < 3 && !(w && h); k++) {
            w = hdr[off[k]] | hdr[off[k] + 1] << 8;
            h = hdr[off[k] + 2] | hdr[off[k] + 3] << 8;
        }
        if (!w || !h) return;
        float sx = (float)w / t->x, sy = (float)h / t->y;
        if (t->map || !strncmp(name, "data/menu/mc/savebg", 19) || !strncmp(name, "data/pic/etc/start00", 20))
            sx = sy;  /* same scale on both axes for these screens */
        rt_log("ee: %s %ux%u -> texture scale %.3f %.3f", name, w, h, sx, sy);
        memcpy(GPTR(EE_TEX_SCALE_X), &sx, 4);
        memcpy(GPTR(EE_TEX_SCALE_Y), &sy, 4);
        return;
    }
}

extern void sub_004494A0_gen(void);
void sub_004494A0(void) {
    tex_load_buffer = MEM32(g_esp + 8);
    sub_004494A0_gen();
}

/* Cave 0x401970, called from 0x44A00D in place of the texture loader 0x448810: EE's TexBufferASM,
 * which clears the buffer the texture was read into once the loader is done with it. */
void sub_00401970(void) {
    rt_resolve(0x00448810u)();  /* same stack and return address: it returns for us */
    uint32_t at = tex_load_buffer;
    if (at == TEX_BUF1 || at == TEX_BUF2) memset(GPTR(at), 0, TEX_SIZE);
    else if (at == TEX_BUF3) memset(GPTR(at), 0, 2 * TEX_SIZE);
}


/* ---- WidescreenFix (tools/ws_patches.py) ----
 * The game's 2D is laid out in a 4:3 area (the width variable at 0xA33480 holds its width) and these
 * guest variables, moved into the exe's operands by ws_patches.py, carry the wide screen's offsets.
 * `resolution=WxH` in sh2e.ini (default 1280x720; d3d8.c must list it). At 4:3, e.g. 960x720, every
 * value is the original. */
#define WS_BASE   0x0EC01100u
enum { V_W, V_H, V_FW, V_FH, V_HUD, V_W43, V_TEXT, V_F_05, V_F05, V_N640, V_N480, V_BORDER0, V_BORDER1 };
#define WS_VAR(i) (WS_BASE + 4 * (i))
#define RES_TABLE 0x0089E884u
#define WIDTH_VAR 0x00A33480u
#define ASPECT_VAR 0x01F5EEBCu

static void ws_setf(uint32_t va, float v) { memcpy(GPTR(va), &v, 4); }
static float ws_getf(uint32_t va) { float v; memcpy(&v, GPTR(va), 4); return v; }
static float ws_text(void) { return ws_getf(WS_VAR(V_TEXT)); }

extern uint32_t ee_res_w, ee_res_h;  /* d3d8.c */
static void ee_widescreen_init(void) {
    uint32_t w = 1280, h = 720;
    FILE *f = fopen("sh2e.ini", "r");
    char line[128];
    while (f && fgets(line, sizeof line, f)) {
        unsigned a, b;
        if (sscanf(line, " resolution = %ux%u", &a, &b) == 2 && a >= 320 && b >= 240 && a <= 1920 && b <= 1440) w = a, h = b;
    }
    if (f) fclose(f);
    ee_res_w = w;
    ee_res_h = h;
    float fw = w, fh = h, aspect = fw / fh, w43 = (int32_t)(fh * (4.0f / 3.0f));
    MEM32(WS_VAR(V_W)) = w;
    MEM32(WS_VAR(V_H)) = h;
    ws_setf(WS_VAR(V_FW), fw);
    ws_setf(WS_VAR(V_FH), fh);
    ws_setf(WS_VAR(V_HUD), 0.5f / ((4.0f / 3.0f) / aspect));
    MEM32(WS_VAR(V_W43)) = (uint32_t)w43;
    float toff = (fw - fh * (4.0f / 3.0f)) / 2.0f;
    ws_setf(WS_VAR(V_TEXT), toff);
    /* image position: half-width offset of the 4:3 area (WidescreenFix pattern_19/20) */
    float off = (toff / (2.0f / (aspect / (4.0f / 3.0f)))) / fw;
    ws_setf(WS_VAR(V_F_05), -0.5f + off);
    ws_setf(WS_VAR(V_F05), 0.5f + off);
    /* cutscene letterbox bars: the game's (0.5, 0.5) at 4:3, hidden (0, 1) on a wide screen (EE: DisableCutsceneBorders = Auto) */
    bool wide = aspect > 1.5f;
    ws_setf(WS_VAR(V_BORDER0), wide ? 0.0f : 0.5f);
    ws_setf(WS_VAR(V_BORDER1), wide ? 1.0f : 0.5f);
    for (int i = 0; i < 22; i++) MEM32(WS_VAR(64 + i)) = w;  /* the 2D width sites (ws_patches.py PIC_INDICES) */
    for (int i = 0; i < 16; i++) MEM32(WS_VAR(128 + i)) = w;  /* the V_W sites */
    MEM32(WS_VAR(V_N640)) = 640;
    MEM32(WS_VAR(V_N480)) = 480;
    for (int i = 0; i < 6; i++) {  /* every mode is the same screen */
        MEM32(RES_TABLE + 8 * i) = w;
        MEM32(RES_TABLE + 8 * i + 4) = h;
    }
    MEM32(WIDTH_VAR) = (uint32_t)w43;
    ws_setf(ASPECT_VAR, aspect);
    /* cores=1 in sh2e.ini confines the guest threads to one core, the PC's workaround for CRI's looping
     * music; its cause, lost PulseEvents (kernel32.c), is fixed, so they spread over three by default. */
    unsigned cores = 3;
    /* debugging: wsN=value in sh2e.ini overrides variable N (a float when it has a '.', else an integer) */
    f = fopen("sh2e.ini", "r");
    while (f && fgets(line, sizeof line, f)) {
        unsigned n;
        char val[32];
        if (sscanf(line, " cores = %u", &n) == 1) cores = n;
        if (sscanf(line, " trace = %u", &n) == 1) rt_trace = n;  /* debugging: log every bridged call */
        if (sscanf(line, " ws%u = %31s", &n, val) == 2 && n < 160) {
            if (strchr(val, '.')) ws_setf(WS_VAR(n), (float)atof(val));
            else MEM32(WS_VAR(n)) = (uint32_t)atoi(val);
            rt_log("ee: ws%u = %s", n, val);
        }
    }
    if (f) fclose(f);
    host_use_single_core(cores >= 3 ? -1 : 0);
    rt_log("ee: guest threads on %s", cores >= 3 ? "three cores" : "one core");
    rt_log("ee: widescreen %ux%u (4:3 area %d, text offset %.1f)", w, h, (int)w43, toff);
}

/* Text position hooks (cave i at 0x4011C0 + 8 i, each replacing a few instructions of the game's text
 * drawing, which they redo before shifting the x87 values by the text offset). The caves run after a
 * `call`, so the original esp is g_esp + 4. */
#define ST(i) g_fp_stack[(g_fp_top + (i)) & 7]
static uint32_t ws_esp(void) { return g_esp + 4; }
static void ws_addf(uint32_t va, float d) { ws_setf(va, ws_getf(va) + d); }
static void ws_st0_add(void) { ST(0) = (float)(ST(0) + ws_text()); }

void sub_004011C0(void) {  /* mov [esp+4], edx; movzx edx, word [eax+0xA]; ST(2) += offset */
    MEM32(ws_esp() + 4) = g_edx;
    g_edx = MEM16(g_eax + 0xA);
    ST(2) = (float)(ST(2) + ws_text());
    g_esp += 4;
}
void sub_004011C8(void) {  /* fstp [0x803900]: the speech line's x positions */
    float t = (float)rt_fpop(), d = ws_text();
    ws_addf(0x8038B0, d);
    ws_addf(0x8038E0, d);
    ws_setf(0x803900, t + d);
    ws_addf(0x8038C0, d);
    ws_addf(0x8038D0, d);
    ws_addf(0x8038F0, d);
    g_esp += 4;
}
void sub_004011D0(void) {  /* mov [esp+0xA8], edx */
    MEM32(ws_esp() + 0xA8) = g_edx;
    ws_st0_add();
    ws_addf(ws_esp() + 0x6C + 4, ws_text());
    g_esp += 4;
}
void sub_004011D8(void) {  /* fst [esp+0x90] */
    ws_st0_add();
    ws_setf(ws_esp() + 0x90, (float)ST(0));
    g_esp += 4;
}
void sub_004011E0(void) {  /* fild [height] */
    ws_addf(ws_esp() + 0x30, ws_text());
    rt_fpush((double)(int32_t)MEM32(WS_VAR(V_H)));
    g_esp += 4;
}
void sub_004011E8(void) {  /* fst [esp+0x58]; fxch st(1) */
    ws_st0_add();
    ws_setf(ws_esp() + 0x58, (float)ST(0));
    double t = ST(0); ST(0) = ST(1); ST(1) = t;
    g_esp += 4;
}
void sub_004011F0(void) {  /* fild [height] */
    ws_st0_add();
    ws_addf(ws_esp() + 0x2C, ws_text());
    rt_fpush((double)(int32_t)MEM32(WS_VAR(V_H)));
    g_esp += 4;
}
void sub_004011F8(void) {  /* fst [esp+0x40]; fld st(1) */
    ws_st0_add();
    ws_setf(ws_esp() + 0x40, (float)ST(0));
    rt_fpush(ST(1));
    g_esp += 4;
}
void sub_00401200(void) {  /* mov eax, [0xA32894] */
    g_eax = MEM32(0xA32894u);
    ws_addf(ws_esp() + 0x7C, ws_text());
    ws_addf(ws_esp() + 0x90, ws_text());
    g_esp += 4;
}

/* Called once after sh2pc.exe is loaded. */
void ee_init(void) {
    host_arena_commit(TEX_BUF1, TEX_BUF3 + 2 * TEX_SIZE - TEX_BUF1);
    host_arena_commit(TITLE_PATHS, 0x10000);
    ee_sfx_init();
    ee_widescreen_init();
    float one = 1.0f;
    memcpy(GPTR(0x0EC01000u), &one, 4);  /* texture scale X/Y (FullscreenImages) */
    memcpy(GPTR(0x0EC01004u), &one, 4);
    /* The game writes SET DX_CONFIG_SAFE_MODE 1 at start and removes it on a clean exit; anything else
     * means "it crashed", and the next start falls back to 640x480 safe graphics. On the Switch the
     * normal way out is HOME, so the flag is dropped before the game reads it. */
    FILE *f = fopen("settings.ini", "rb");
    if (f) {
        char buf[4096], out[4096];
        size_t n = fread(buf, 1, sizeof buf - 1, f), o = 0;
        fclose(f);
        buf[n] = 0;
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
            if (!strstr(line, "DX_CONFIG_SAFE_MODE")) o += snprintf(out + o, sizeof out - o, "%s\n", line);
        if ((f = fopen("settings.ini", "wb"))) { fwrite(out, 1, o, f); fclose(f); }
    }
    rt_log("ee: texture buffers at %08X/%08X/%08X", TEX_BUF1, TEX_BUF2, TEX_BUF3);
    /* Start00Scaling, data half (the code half is in tools/patch_exe.py): the save screen background
     * position and width, whose stores are NOPed, and the title logo highlight, for 2732x2048 art. */
    *(uint16_t *)GPTR(0x0093C940u) = 5464;                 /* save screen pos X: 4096 * 2732/2048 */
    *(uint16_t *)GPTR(0x0093C93Cu) = 61440 - (5464 - 4096); /* save screen width */
    *(uint16_t *)GPTR(0x0088BC4Au) = 126;                  /* title logo highlight Y */
}

/* ---- MainMenu (EE Patches/MainMenu.cpp, PatchMainMenuTitlePerLang) ----
 * Cave 0x401D60, called from 0x496F30 in place of `xor esi, esi; or eax, -1`: points the title menu
 * texture (the TexPath at 0x7BA838) at start01<lang>.tex when that file exists, then reloads the
 * messages as EE's MainMenuTitleASM does. */
void sub_00401D60(void) {
    static const char langs[] = "jefgis";  /* the game's language index (0x1DBC006) */
    uint8_t lang = MEM8(0x01DBC006u);
    char guest[64], host[512];
    struct stat st;
    snprintf(guest, sizeof guest, "data/pic/etc/start01%c.tex", lang < 6 ? langs[lang] : 'e');
    if (stat(rt_path(guest, host, sizeof host), &st) == 0) {
        strcpy(GPTR(TITLE_PATHS), guest);
        MEM32(TITLE_TEXPATH) = TITLE_PATHS;
        rt_log("ee: title menu %s", host);
    }
    uint32_t zero = 0;
    guest_call(0x004457C0u, 1, &zero, 0);  /* LoadMes(0) */
    g_esi = 0;
    g_eax = 0xFFFFFFFFu;
    g_esp += 4;
}

/* DrawCursor (called once, from 0x476128): the knife mouse pointer. The Switch has no mouse, so it is
 * never drawn (EE InputTweaks' DrawCursor_Hook with HideMouseCursor). */
extern void sub_0045A6D0_gen(void);
void sub_0045A6D0(void) {
    g_esp += 4;
}

/* ---- vibration (EE Wrappers/dinput8/IDirectInputEffect.cpp, RestoreVibration) ---- */

/* CreateDirectInputGamepad: a pad that created an effect is driven as a rumble pad (type 2). */
extern void sub_00458760_gen(void);
void sub_00458760(void) {
    sub_00458760_gen();
    if (MEM32(EE_GAMEPAD_TYPE) != 0) MEM32(EE_GAMEPAD_TYPE) = 2;
}

/* Strength follows the game's own Vibration option. Called by the DirectInput effect bridge. */
uint32_t ee_rumble_gain(uint32_t magnitude) {
    uint8_t level = MEM8(EE_VIBRATION_LEVEL);
    if (level >= 3) return magnitude ? 10000 : 0;
    return magnitude ? 10000u * level / 3u : 0;
}

void dinput_rumble_stop(void);


/* ---- save debugging: the return value of the save module's functions, logged when it changes (sh2.log) ---- */
static void save_log(uint32_t fn, uint32_t eax, uint32_t edx, uint32_t from) {
    static struct { uint32_t fn, eax, edx; } last[32];
    int i = 0;
    while (i < 32 && last[i].fn && last[i].fn != fn) i++;
    if (i == 32) return;
    if (last[i].fn == fn && last[i].eax == eax && last[i].edx == edx) return;
    last[i].fn = fn; last[i].eax = eax; last[i].edx = edx;
    rt_log("save: fn %08X -> eax=%08X edx=%08X (from %08X)", fn, eax, edx, from);
}

extern void sub_0044D2B0_gen(void);
void sub_0044D2B0(void) {
    uint32_t from = MEM32(g_esp);
    /* the 0xC00-byte sh2pc.sys buffer as the validator sees it */
    const uint8_t *b = GPTR(0x00933608u);
    uint32_t h = 2166136261u;
    for (int i = 0; i < 0xC00; i++) h = (h ^ b[i]) * 16777619u;
    rt_log("save: sys buffer fnv=%08X head %02X%02X%02X%02X%02X%02X%02X%02X tail %02X%02X%02X%02X%02X%02X%02X%02X hdr@1c=%08X", h,
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[0xBF8], b[0xBF9], b[0xBFA], b[0xBFB], b[0xBFC], b[0xBFD], b[0xBFE], b[0xBFF],
           *(const uint32_t *)(b + 0x1C));
    int lead = 0, nz = 0;
    while (lead < 0xC00 && !b[lead]) lead++;
    for (int i = 0; i < 0xC00; i++) nz += b[i] != 0;
    rt_log("save: sys buffer leading zeros %d, nonzero bytes %d, bytes 0x40..: %02X%02X%02X%02X%02X%02X%02X%02X", lead, nz, b[0x40], b[0x41], b[0x42], b[0x43], b[0x44], b[0x45], b[0x46], b[0x47]);
    sub_0044D2B0_gen();
    save_log(0x0044D2B0, g_eax, g_edx, from);
}
extern void sub_0044D520_gen(void);
void sub_0044D520(void) { uint32_t from = MEM32(g_esp); sub_0044D520_gen(); save_log(0x0044D520, g_eax, g_edx, from); }
extern void sub_0044DE20_gen(void);
void sub_0044DE20(void) { uint32_t from = MEM32(g_esp); sub_0044DE20_gen(); save_log(0x0044DE20, g_eax, g_edx, from); }
extern void sub_0044EDA0_gen(void);
void sub_0044EDA0(void) { uint32_t from = MEM32(g_esp); sub_0044EDA0_gen(); save_log(0x0044EDA0, g_eax, g_edx, from); }
extern void sub_0044F270_gen(void);
void sub_0044F270(void) { uint32_t from = MEM32(g_esp); sub_0044F270_gen(); save_log(0x0044F270, g_eax, g_edx, from); }
extern void sub_004512B0_gen(void);
void sub_004512B0(void) { uint32_t from = MEM32(g_esp); sub_004512B0_gen(); save_log(0x004512B0, g_eax, g_edx, from); }
extern void sub_00450E70_gen(void);
void sub_00450E70(void) { uint32_t from = MEM32(g_esp); sub_00450E70_gen(); save_log(0x00450E70, g_eax, g_edx, from); }
extern void sub_00450AB0_gen(void);
void sub_00450AB0(void) { uint32_t from = MEM32(g_esp); sub_00450AB0_gen(); save_log(0x00450AB0, g_eax, g_edx, from); }
extern void sub_00451850_gen(void);
void sub_00451850(void) { uint32_t from = MEM32(g_esp); sub_00451850_gen(); save_log(0x00451850, g_eax, g_edx, from); }
extern void sub_00456530_gen(void);
void sub_00456530(void) { uint32_t from = MEM32(g_esp); sub_00456530_gen(); save_log(0x00456530, g_eax, g_edx, from); }
extern void sub_004565F0_gen(void);
void sub_004565F0(void) { uint32_t from = MEM32(g_esp); sub_004565F0_gen(); save_log(0x004565F0, g_eax, g_edx, from); }
extern void sub_00456C00_gen(void);
void sub_00456C00(void) { uint32_t from = MEM32(g_esp); sub_00456C00_gen(); save_log(0x00456C00, g_eax, g_edx, from); }
extern void sub_00456CE0_gen(void);
void sub_00456CE0(void) { uint32_t from = MEM32(g_esp); sub_00456CE0_gen(); save_log(0x00456CE0, g_eax, g_edx, from); }
extern void sub_00456EE0_gen(void);
void sub_00456EE0(void) { uint32_t from = MEM32(g_esp); sub_00456EE0_gen(); save_log(0x00456EE0, g_eax, g_edx, from); }
extern void sub_004570B0_gen(void);
void sub_004570B0(void) { uint32_t from = MEM32(g_esp); sub_004570B0_gen(); save_log(0x004570B0, g_eax, g_edx, from); }
extern void sub_00455660_gen(void);
void sub_00455660(void) { uint32_t from = MEM32(g_esp); sub_00455660_gen(); save_log(0x00455660, g_eax, g_edx, from); }
extern void sub_004567B0_gen(void);
void sub_004567B0(void) { uint32_t from = MEM32(g_esp); sub_004567B0_gen(); save_log(0x004567B0, g_eax, g_edx, from); }

/* recomp_lookup_manual: every call the recompiler routed through the manual table lands here. */
recomp_func_t ee_lookup_manual(uint32_t va) {
    switch (va) {
    case 0x00458060u: return sub_00458060;
    case 0x00534660u: return sub_00534660;
    case 0x00458760u: return sub_00458760;
    case 0x0045A6D0u: return sub_0045A6D0;
    case 0x00401970u: return sub_00401970;
    case 0x004494A0u: return sub_004494A0;
    case 0x00401D60u: return sub_00401D60;
    case 0x00401460u: return sub_00401460;
    case 0x004015D0u: return sub_004015D0;
    case 0x0044D2B0u: return sub_0044D2B0;
    case 0x0044D520u: return sub_0044D520;
    case 0x0044DE20u: return sub_0044DE20;
    case 0x0044EDA0u: return sub_0044EDA0;
    case 0x0044F270u: return sub_0044F270;
    case 0x004512B0u: return sub_004512B0;
    case 0x00450E70u: return sub_00450E70;
    case 0x00450AB0u: return sub_00450AB0;
    case 0x00451850u: return sub_00451850;
    case 0x00456530u: return sub_00456530;
    case 0x004565F0u: return sub_004565F0;
    case 0x00456C00u: return sub_00456C00;
    case 0x00456CE0u: return sub_00456CE0;
    case 0x00456EE0u: return sub_00456EE0;
    case 0x004570B0u: return sub_004570B0;
    case 0x00455660u: return sub_00455660;
    case 0x004567B0u: return sub_004567B0;
    case 0x004011C0u: return sub_004011C0;
    case 0x004011C8u: return sub_004011C8;
    case 0x004011D0u: return sub_004011D0;
    case 0x004011D8u: return sub_004011D8;
    case 0x004011E0u: return sub_004011E0;
    case 0x004011E8u: return sub_004011E8;
    case 0x004011F0u: return sub_004011F0;
    case 0x004011F8u: return sub_004011F8;
    case 0x00401200u: return sub_00401200;
    }
    return NULL;
}

/* Once per presented frame: no rumble outside gameplay, in a fade or between rooms (EE's
 * RunInfiniteRumbleFix). */
void ee_frame(void) {
    static int blocked;
    int block = MEM8(EE_EVENT_INDEX) != EVENT_IN_GAME || MEM32(EE_ROOM_ID) == R_NONE ||
                MEM32(EE_TRANSITION_STATE) != FADE_NONE;
    if (block && !blocked) {
        MEM8(EE_RUMBLE_FLAG) = 0;
        if (MEM32(EE_TRANSITION_STATE) != FADE_NONE) dinput_rumble_stop();
    } else if (!block && blocked)
        MEM8(EE_RUMBLE_FLAG) = 1;
    blocked = block;

    /* SaveBGImage (EE RunSaveBGImage): back at the main menu, the chapter is the main scenario. */
    static uint8_t last_menu;
    uint8_t menu = MEM8(EE_MENU_EVENT);
    if (last_menu != 7 && menu == 7) MEM8(0x01DBC00Cu) = 0;  /* MENU_MAIN_MENU, CHAPTER_MAIN_SCENARIO */
    last_menu = menu;
}
