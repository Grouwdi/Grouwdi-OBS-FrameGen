/*
 * Grouwdi-OBS-FrameGen - NVIDIA optical flow on OBS's Direct3D 11 device (Windows).
 *
 * Loads nvcuda.dll and nvofapi64.dll at runtime (both ship with the NVIDIA
 * display driver), shares two OBS render targets with CUDA, and computes
 * forward + backward optical flow on the GPU's optical-flow hardware.
 *
 * Usage per OBS tick (graphics thread):
 *   sc_gpu_collect()  - wait for / read the results submitted last tick
 *   sc_gpu_commit()   - tell it whether that frame was new (becomes the reference)
 *   sc_gpu_submit()   - queue work for the frame rendered this tick
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC_THUMB_W 128
#define SC_THUMB_H 72

enum sc_gpu_quality { SC_GPU_FAST = 0, SC_GPU_MEDIUM = 1, SC_GPU_SLOW = 2 };

struct sc_gpu;

struct sc_gpu_result {
	const uint8_t *thumb;     /* SC_THUMB_W x SC_THUMB_H, pitch SC_THUMB_W */
	bool has_flow;            /* false if there was no reference frame yet */
	const uint8_t *fwd;       /* raw flow, input = previous new frame, reference = this frame */
	const uint8_t *bwd;       /* raw flow, input = this frame, reference = previous new frame */
	size_t flow_pitch;        /* bytes per row of fwd/bwd */
	uint32_t ow, oh;          /* flow grid size */
	const uint8_t *gray_cur;  /* this frame's gray image, flow_w x flow_h, pitch flow_w (always) */
	bool has_gray;            /* gray_prev valid (only when requested in submit) */
	const uint8_t *gray_prev; /* reference frame's gray image */
	double wait_ms;           /* time spent waiting for the GPU in collect */
};

/* Why sc_gpu_create failed, so the filter can tell the user in plain words. */
enum sc_gpu_fail {
	SC_GPU_FAIL_OTHER = 0,
	SC_GPU_FAIL_NOT_NVIDIA = 1, /* OBS renders on a non-NVIDIA GPU */
	SC_GPU_FAIL_NO_OF = 2,      /* NVIDIA GPU, but no usable optical flow engine or driver too old */
};

/*
 * d3d11_device: ID3D11Device* from gs_get_device_obj().
 * gray_tex: ID3D11Texture2D* of a GS_R8 render target, flow_w x flow_h.
 * thumb_tex: ID3D11Texture2D* of a GS_R8 render target, SC_THUMB_W x SC_THUMB_H.
 * On failure returns NULL, writes a readable reason into err and the kind of
 * failure into *why (may be NULL). For SC_GPU_FAIL_NOT_NVIDIA, err holds the
 * adapter's name.
 */
struct sc_gpu *sc_gpu_create(void *d3d11_device, void *gray_tex, void *thumb_tex, uint32_t flow_w, uint32_t flow_h,
			     enum sc_gpu_quality q, char *err, size_t err_len, enum sc_gpu_fail *why);
void sc_gpu_destroy(struct sc_gpu *g);

/* Queue copies of the two render targets and, if a reference exists, both flows. */
bool sc_gpu_submit(struct sc_gpu *g, bool want_gray);

/* Wait for the last submit and expose its results (valid until the next submit). */
bool sc_gpu_collect(struct sc_gpu *g, struct sc_gpu_result *r);

/* After collect: was that frame new content? New frames become the next reference. */
void sc_gpu_commit(struct sc_gpu *g, bool was_new);

/* Forget the reference frame (after a reset). */
void sc_gpu_drop_reference(struct sc_gpu *g);

const char *sc_gpu_error(const struct sc_gpu *g);

#ifdef __cplusplus
}
#endif
