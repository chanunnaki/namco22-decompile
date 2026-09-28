#ifndef RR_GXM_H
#define RR_GXM_H

#include <stdbool.h>
#include <stdint.h>
#include <psp2/gxm.h>

bool rr_gxm_init(const char *rom_dir);
void rr_gxm_prepare(bool slave_active);
void rr_gxm_draw(int vw, int vh);
int  rr_gxm_quads(void);
void rr_gxm_swap(void);
void rr_gxm_finish(void);

/* Alias for compatibility with rr_main.c callers */
#define rr_gl_init rr_gxm_init
#define rr_gl_prepare rr_gxm_prepare
#define rr_gl_draw rr_gxm_draw
#define rr_gl_quads rr_gxm_quads
#define rr_gl_context_attributes() ((void)0)
#define rr_gl_open_headless(w, h) (false)
#define rr_gl_write_ppm(p, w, h) (false)

#ifdef __vita__
void vita_log(const char *fmt, ...);
#define VLOG(fmt, ...) vita_log(fmt, ##__VA_ARGS__)
#else
#define VLOG(fmt, ...) ((void)0)
#endif

#endif /* RR_GXM_H */
