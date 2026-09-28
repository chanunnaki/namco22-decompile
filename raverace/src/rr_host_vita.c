/*
 * rr_host_vita.c -- PS Vita platform host for Rave Racer.
 * Handles Vita controls (buttons + analog), GXM display presentation, and SDL2 audio.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <SDL.h>

#include "rr_hw.h"
#include "rr_input.h"
#include "rr_sound.h"
#include "rr_gxm.h"

#ifdef __vita__
#include <stdarg.h>
unsigned int sceUserMainThreadStackSizeInBytes = 2 * 1024 * 1024;
unsigned int sceLibcHeapSize = 128 * 1024 * 1024;
__attribute__((used)) static const char vita_elf_pad[8192] = { 1 };

static FILE *s_vlog = NULL;
static SceUID s_vlog_mutex = -1;
void vita_log(const char *fmt, ...) {
    /* First called by main before workers exist. Serialize the shared FILE,
     * including its flush: Vita libc does not protect this stream for us. */
    if (s_vlog_mutex < 0) s_vlog_mutex = sceKernelCreateMutex("rr_log", 0, 0, NULL);
    if (s_vlog_mutex < 0 || sceKernelLockMutex(s_vlog_mutex, 1, NULL) < 0) return;
    if (!s_vlog) s_vlog = fopen("ux0:/data/raverace_boot.log", "a");
    if (s_vlog) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(s_vlog, fmt, ap);
        va_end(ap);
        fflush(s_vlog);
    }
    sceKernelUnlockMutex(s_vlog_mutex, 1);
}
#endif

static bool running = true;
static bool audio_ok = false;

/* Settings stubs */
void rr_host_set_winmode(int m) { (void)m; }
void rr_host_set_scale(int k) { (void)k; }
void rr_host_set_res(int w, int h) { (void)w; (void)h; }
void rr_host_set_wide(int on) { (void)on; }
void rr_host_set_draw(int level) { (void)level; }
const char *rr_host_draw_name(int level) { (void)level; return "default"; }
void rr_host_set_aspect(int a) { (void)a; }
void rr_host_set_scaling(int s) { (void)s; }
void rr_host_set_volume(int percent) { (void)percent; }
void rr_host_set_freeplay(bool on) { rr_hw_set_freeplay(on); }
void rr_host_toggle_record(void) {}
int  rr_host_res_count(void) { return 1; }
void rr_host_res_get(int i, int *w, int *h) { (void)i; *w = 960; *h = 544; }
void rr_host_render_size(int *w, int *h) { *w = 960; *h = 544; }
int  rr_host_win_w(int k) { (void)k; return 960; }
int  rr_host_win_h(int k) { (void)k; return 544; }

bool rr_host_paused(void) { return false; }
int  rr_host_joytest(void) { return 0; }

bool rr_host_open(int scale) {
    (void)scale;

    vita_log("[BUILD] frame-pipeline-atlas-3\n");

    // Maximize Vita performance profile (444 MHz CPU, 222 MHz GPU)
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    vita_log("[CLOCK] CPU=%d bus=%d GPU=%d crossbar=%d MHz\n", scePowerGetArmClockFrequency(),
             scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());

    // Enable analog sticks on Vita
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    // Initialize SDL2 for audio subsystem
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "[VITA_HOST] SDL_Init audio warning: %s\n", SDL_GetError());
    }

    if (rr_audio_output_open()) {
        rr_audio_set_volume(100);
        audio_ok = true;
    }

    running = true;
    return true;
}

void rr_host_close(void) {
    running = false;
    rr_gxm_finish();
    SDL_Quit();
}

static inline void set_bit(uint16_t bit, int down) {
    if (down) g_hw.inputs &= (uint16_t)~bit; else g_hw.inputs |= bit;
}

static void ramp(uint16_t *v, int toward, int lo, int hi, int step) {
    int x = *v;
    if (x < toward) { x += step; if (x > toward) x = toward; }
    else if (x > toward) { x -= step; if (x < toward) x = toward; }
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    *v = (uint16_t)x;
}

bool rr_host_frame(void) {
    if (!running) return false;

    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);

    /* Map buttons (active low bits in g_hw.inputs) */
    set_bit(0x1000, (pad.buttons & SCE_CTRL_SELECT) || (pad.buttons & SCE_CTRL_START));   /* Select / Start = Coin 1 */
    set_bit(0x0400, (pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) == (SCE_CTRL_SELECT | SCE_CTRL_START)); /* Test */
    set_bit(0x0001, (pad.buttons & SCE_CTRL_CIRCLE) || (pad.buttons & SCE_CTRL_L1));  /* Circle/L1 = Shift Down */
    set_bit(0x0002, (pad.buttons & SCE_CTRL_TRIANGLE) || (pad.buttons & SCE_CTRL_R1)); /* Triangle/R1 = Shift Up */
    set_bit(0x0040, pad.buttons & SCE_CTRL_DOWN);     /* D-Pad Down = View Change */

    /* Steering: analog stick or D-pad */
    int deadzone = 20;
    int lx = (int)pad.lx - 128;
    if (abs(lx) < deadzone) lx = 0;
    else lx = (lx > 0 ? lx - deadzone : lx + deadzone) * 32767 / (128 - deadzone);

    int dir = ((pad.buttons & SCE_CTRL_RIGHT) ? 1 : 0) - ((pad.buttons & SCE_CTRL_LEFT) ? 1 : 0);
    if (lx != 0) {
        g_hw.steer = (uint16_t)(0x800 + lx * 0x580 / 32767);
    } else {
        ramp(&g_hw.steer, 0x800 + dir * 0x580, 0x280, 0xD80, dir ? 40 : 25);
    }

    /* Gas: Cross (X) or R-Trigger */
    int gas_pressed = (pad.buttons & SCE_CTRL_CROSS) || (pad.buttons & SCE_CTRL_RTRIGGER);
    ramp(&g_hw.gas, gas_pressed ? 0x610 : 0, 0, 0x610, 160);

    /* Brake: Square or L-Trigger */
    int brake_pressed = (pad.buttons & SCE_CTRL_SQUARE) || (pad.buttons & SCE_CTRL_LTRIGGER);
    ramp(&g_hw.brake, brake_pressed ? 0x610 : 0, 0, 0x610, 160);

    /* Draw and present GXM frame */
    rr_gxm_draw(960, 544);
    rr_gxm_swap();

    return running;
}
