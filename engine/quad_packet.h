/* CPU/GPU packet layout shared by geometry workers and the uploader. */
#ifndef QUAD_PACKET_H
#define QUAD_PACKET_H
typedef struct {
    float x, y;
    float u, v, w;
    float r, g, b, a;
    float bank, palette, divisor, count;
    float fr, fg, fb, keep;
    float textured;
} eng_batch_vertex;
#endif
