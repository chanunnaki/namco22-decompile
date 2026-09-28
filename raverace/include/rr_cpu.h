#ifndef RR_CPU_H
#define RR_CPU_H
#include <stdint.h>

/* Native frame controller. The original translated CPU remains the oracle. */
extern int rr_cpu_native;
extern uint64_t rr_cpu_wait_instructions;
void rr_cpu_init(void);
uint32_t rr_cpu_frame_loop(void);
void rr_cpu_wait_frame(void);
void rr_cpu_wait_service(void);
#endif
