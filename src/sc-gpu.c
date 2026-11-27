/*
 * Grouwdi-OBS-FrameGen - NVIDIA optical flow on OBS's Direct3D 11 device. See sc-gpu.h.
 *
 * CUDA declarations: FFmpeg nv-codec-headers (third_party/ffnvcodec), the same
 * ones OBS's own NVENC plugin uses. Optical flow: NVIDIA Optical Flow API 2.0
 * headers (third_party/nvof). Nothing is linked at build time; both DLLs are
 * part of the NVIDIA display driver and loaded with LoadLibrary.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifdef _WIN32

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <ffnvcodec/dynlink_cuda.h>
/* nvOpticalFlowCommon.h declares "struct NV_OF_ROI_RECT" but then uses
 * "NV_OF_ROI_RECT *" without the struct keyword, which only compiles as C++.
 * This forward typedef makes it valid C without modifying NVIDIA's header. */
typedef struct NV_OF_ROI_RECT NV_OF_ROI_RECT;
#include <nvOpticalFlowCuda.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc-gpu.h"

/* ---------------- runtime-loaded CUDA driver functions ---------------- */

struct cuda_fns {
	tcuInit *cuInit;
	tcuD3D11GetDevice *cuD3D11GetDevice;
	tcuCtxCreate_v2 *cuCtxCreate;
	tcuCtxDestroy_v2 *cuCtxDestroy;
	tcuCtxPushCurrent_v2 *cuCtxPushCurrent;
	tcuCtxPopCurrent_v2 *cuCtxPopCurrent;
	tcuStreamCreate *cuStreamCreate;
	tcuStreamDestroy_v2 *cuStreamDestroy;
	tcuStreamSynchronize *cuStreamSynchronize;
	tcuGraphicsD3D11RegisterResource *cuGraphicsD3D11RegisterResource;
	tcuGraphicsUnregisterResource *cuGraphicsUnregisterResource;
	tcuGraphicsMapResources *cuGraphicsMapResources;
	tcuGraphicsUnmapResources *cuGraphicsUnmapResources;
	tcuGraphicsSubResourceGetMappedArray *cuGraphicsSubResourceGetMappedArray;
	tcuMemcpy2DAsync_v2 *cuMemcpy2DAsync;
	tcuMemHostAlloc *cuMemHostAlloc;
	tcuMemFreeHost *cuMemFreeHost;
	tcuGetErrorString *cuGetErrorString;
};

#define CUDA_SYM(field, name) {offsetof(struct cuda_fns, field), name}
static const struct {
	size_t offset;
	const char *name;
} cuda_syms[] = {
	/* export names as loaded by FFmpeg's dynlink_loader.h / OBS obs-nvenc */
	CUDA_SYM(cuInit, "cuInit"),
	CUDA_SYM(cuD3D11GetDevice, "cuD3D11GetDevice"),
	CUDA_SYM(cuCtxCreate, "cuCtxCreate_v2"),
	CUDA_SYM(cuCtxDestroy, "cuCtxDestroy_v2"),
	CUDA_SYM(cuCtxPushCurrent, "cuCtxPushCurrent_v2"),
	CUDA_SYM(cuCtxPopCurrent, "cuCtxPopCurrent_v2"),
	CUDA_SYM(cuStreamCreate, "cuStreamCreate"),
	CUDA_SYM(cuStreamDestroy, "cuStreamDestroy_v2"),
	CUDA_SYM(cuStreamSynchronize, "cuStreamSynchronize"),
	CUDA_SYM(cuGraphicsD3D11RegisterResource, "cuGraphicsD3D11RegisterResource"),
	CUDA_SYM(cuGraphicsUnregisterResource, "cuGraphicsUnregisterResource"),
	CUDA_SYM(cuGraphicsMapResources, "cuGraphicsMapResources"),
	CUDA_SYM(cuGraphicsUnmapResources, "cuGraphicsUnmapResources"),
	CUDA_SYM(cuGraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray"),
	CUDA_SYM(cuMemcpy2DAsync, "cuMemcpy2DAsync_v2"),
	CUDA_SYM(cuMemHostAlloc, "cuMemHostAlloc"),
	CUDA_SYM(cuMemFreeHost, "cuMemFreeHost"),
	CUDA_SYM(cuGetErrorString, "cuGetErrorString"),
};

typedef NV_OF_STATUS(NVOFAPI *PFN_NvOFAPICreateInstanceCuda)(uint32_t apiVer, NV_OF_CUDA_API_FUNCTION_LIST *list);

struct sc_gpu {
	HMODULE cuda_dll, nvof_dll;
	struct cuda_fns cu;
	NV_OF_CUDA_API_FUNCTION_LIST of;

	CUcontext ctx;
	CUstream stream;
	NvOFHandle hof;

	CUgraphicsResource res[2];            /* 0 = gray, 1 = thumb */
	NvOFGPUBufferHandle in[2];            /* two input slots, CUarray */
	NvOFGPUBufferHandle out_fwd, out_bwd; /* CUdeviceptr */
	NV_OF_CUDA_BUFFER_STRIDE_INFO out_stride;

	uint32_t fw, fh, ow, oh;
	int cur; /* input slot written by the next submit */
	int ref; /* input slot holding the reference frame, -1 if none */

	/* pinned host memory for results */
	uint8_t *h_thumb, *h_fwd, *h_bwd, *h_gray[2];

	/* what the last submit queued */
	bool submitted, sub_flow, sub_gray;
	int sub_cur, sub_ref;

	char err[256];
};

static void set_err(struct sc_gpu *g, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g->err, sizeof(g->err), fmt, ap);
	va_end(ap);
}

static bool cu_ok(struct sc_gpu *g, CUresult r, const char *what)
{
	if (r == CUDA_SUCCESS)
		return true;
	const char *s = NULL;
	if (g->cu.cuGetErrorString)
		g->cu.cuGetErrorString(r, &s);
	set_err(g, "CUDA %s failed: %d %s", what, (int)r, s ? s : "");
	return false;
}

static bool of_ok(struct sc_gpu *g, NV_OF_STATUS st, const char *what)
{
	if (st == NV_OF_SUCCESS)
		return true;
	char msg[160] = {0};
	uint32_t sz = sizeof(msg);
	if (g->hof && g->of.nvOFGetLastError)
		g->of.nvOFGetLastError(g->hof, msg, &sz);
	set_err(g, "Optical flow %s failed: status %d %s", what, (int)st, msg);
	return false;
}

#define SC_VENDOR_NVIDIA 0x10DE

/* Vendor of the adapter OBS renders on; fills name (UTF-8). 0 if unknown. */
static unsigned adapter_vendor(ID3D11Device *dev, char *name, size_t name_len)
{
	IDXGIDevice *dxgi = NULL;
	IDXGIAdapter *adapter = NULL;
	DXGI_ADAPTER_DESC desc;
	unsigned vendor = 0;
	if (name_len)
		name[0] = 0;
	if (FAILED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi)))
		return 0;
	if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)) && SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
		vendor = desc.VendorId;
		if (name_len)
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, (int)name_len, NULL, NULL);
	}
	if (adapter)
		IDXGIAdapter_Release(adapter);
	IDXGIDevice_Release(dxgi);
	return vendor;
}

static bool load_libs(struct sc_gpu *g)
{
	g->cuda_dll = LoadLibraryW(L"nvcuda.dll");
	if (!g->cuda_dll) {
		set_err(g, "nvcuda.dll not found (NVIDIA driver with CUDA support required)");
		return false;
	}
	for (size_t i = 0; i < sizeof(cuda_syms) / sizeof(cuda_syms[0]); i++) {
		FARPROC p = GetProcAddress(g->cuda_dll, cuda_syms[i].name);
		if (!p) {
			set_err(g, "nvcuda.dll is missing %s (update the NVIDIA driver)", cuda_syms[i].name);
			return false;
		}
		memcpy((uint8_t *)&g->cu + cuda_syms[i].offset, &p, sizeof(p));
	}

	g->nvof_dll = LoadLibraryW(L"nvofapi64.dll");
	if (!g->nvof_dll) {
		set_err(g, "nvofapi64.dll not found (NVIDIA driver with Optical Flow support required)");
		return false;
	}
	PFN_NvOFAPICreateInstanceCuda create =
		(PFN_NvOFAPICreateInstanceCuda)(void *)GetProcAddress(g->nvof_dll, "NvOFAPICreateInstanceCuda");
	if (!create) {
		set_err(g, "nvofapi64.dll is missing NvOFAPICreateInstanceCuda");
		return false;
	}
	memset(&g->of, 0, sizeof(g->of));
	return of_ok(g, create(NV_OF_API_VERSION, &g->of), "API instance");
}

static bool create_context(struct sc_gpu *g, ID3D11Device *dev)
{
	IDXGIDevice *dxgi = NULL;
	IDXGIAdapter *adapter = NULL;
	CUdevice cudev;
	bool ok = false;

	if (!cu_ok(g, g->cu.cuInit(0), "init"))
		return false;
	if (FAILED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi))) {
		set_err(g, "could not get the DXGI device from OBS's Direct3D 11 device");
		return false;
	}
	if (FAILED(IDXGIDevice_GetAdapter(dxgi, &adapter))) {
		set_err(g, "could not get OBS's graphics adapter");
		goto done;
	}
	if (g->cu.cuD3D11GetDevice(&cudev, adapter) != CUDA_SUCCESS) {
		set_err(g, "OBS is not running on an NVIDIA GPU (Settings > Advanced > Renderer / adapter)");
		goto done;
	}
	if (!cu_ok(g, g->cu.cuCtxCreate(&g->ctx, CU_CTX_SCHED_BLOCKING_SYNC, cudev), "context create"))
		goto done;
	/* cuCtxCreate makes it current; we push/pop around every use instead */
	CUcontext dummy;
	g->cu.cuCtxPopCurrent(&dummy);
	ok = true;
done:
	if (adapter)
		IDXGIAdapter_Release(adapter);
	if (dxgi)
		IDXGIDevice_Release(dxgi);
	return ok;
}

/* Two-stage caps query (size with NULL first, then values); 0 if unavailable. */
static uint32_t cap_scalar(struct sc_gpu *g, NV_OF_CAPS which)
{
	uint32_t size = 0, vals[8] = {0};
	if (g->of.nvOFGetCaps(g->hof, which, NULL, &size) != NV_OF_SUCCESS || size == 0 || size > 8)
		return 0;
	if (g->of.nvOFGetCaps(g->hof, which, vals, &size) != NV_OF_SUCCESS)
		return 0;
	return vals[0];
}

static bool init_flow(struct sc_gpu *g, enum sc_gpu_quality q)
{
	if (!of_ok(g, g->of.nvCreateOpticalFlowCuda(g->ctx, &g->hof), "session create"))
		return false;
	if (!of_ok(g, g->of.nvOFSetIOCudaStreams(g->hof, g->stream, g->stream), "set streams"))
		return false;

	/* capabilities: grid size 4 and our resolution */
	uint32_t caps[8] = {0}, size = 0;
	if (g->of.nvOFGetCaps(g->hof, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, NULL, &size) == NV_OF_SUCCESS &&
	    size > 0 && size <= 8 &&
	    g->of.nvOFGetCaps(g->hof, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, caps, &size) == NV_OF_SUCCESS) {
		bool has4 = false;
		for (uint32_t i = 0; i < size; i++)
			has4 |= caps[i] == NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
		if (!has4) {
			set_err(g, "this GPU's optical flow engine does not support 4x4 output");
			return false;
		}
	}
	uint32_t wmin = cap_scalar(g, NV_OF_CAPS_WIDTH_MIN), hmin = cap_scalar(g, NV_OF_CAPS_HEIGHT_MIN);
	uint32_t wmax = cap_scalar(g, NV_OF_CAPS_WIDTH_MAX), hmax = cap_scalar(g, NV_OF_CAPS_HEIGHT_MAX);
	if ((wmin && g->fw < wmin) || (hmin && g->fh < hmin) || (wmax && g->fw > wmax) || (hmax && g->fh > hmax)) {
		set_err(g, "flow size %ux%u outside supported %ux%u..%ux%u", g->fw, g->fh, wmin, hmin, wmax, hmax);
		return false;
	}

	NV_OF_INIT_PARAMS ip;
	memset(&ip, 0, sizeof(ip));
	ip.width = g->fw;
	ip.height = g->fh;
	ip.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
	ip.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
	ip.mode = NV_OF_MODE_OPTICALFLOW;
	ip.perfLevel = q == SC_GPU_FAST ? NV_OF_PERF_LEVEL_FAST
					: (q == SC_GPU_SLOW ? NV_OF_PERF_LEVEL_SLOW : NV_OF_PERF_LEVEL_MEDIUM);
	ip.enableExternalHints = NV_OF_FALSE;
	ip.enableOutputCost = NV_OF_FALSE;
	ip.hPrivData = NULL;
	ip.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
	ip.enableRoi = NV_OF_FALSE;
	if (!of_ok(g, g->of.nvOFInit(g->hof, &ip), "init"))
		return false;

	NV_OF_BUFFER_DESCRIPTOR d;
	memset(&d, 0, sizeof(d));
	d.width = g->fw;
	d.height = g->fh;
	d.bufferUsage = NV_OF_BUFFER_USAGE_INPUT;
	d.bufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
	for (int i = 0; i < 2; i++)
		if (!of_ok(g, g->of.nvOFCreateGPUBufferCuda(g->hof, &d, NV_OF_CUDA_BUFFER_TYPE_CUARRAY, &g->in[i]),
			   "input buffer"))
			return false;

	g->ow = (g->fw + 3) / 4;
	g->oh = (g->fh + 3) / 4;
	d.width = g->ow;
	d.height = g->oh;
	d.bufferUsage = NV_OF_BUFFER_USAGE_OUTPUT;
	d.bufferFormat = NV_OF_BUFFER_FORMAT_SHORT2;
	if (!of_ok(g, g->of.nvOFCreateGPUBufferCuda(g->hof, &d, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &g->out_fwd),
		   "output buffer") ||
	    !of_ok(g, g->of.nvOFCreateGPUBufferCuda(g->hof, &d, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &g->out_bwd),
		   "output buffer"))
		return false;
	if (!of_ok(g, g->of.nvOFGPUBufferGetStrideInfo(g->out_fwd, &g->out_stride), "stride info"))
		return false;
	if (g->out_stride.strideInfo[0].strideXInBytes < g->ow * 4) {
		set_err(g, "unexpected flow buffer pitch %u", g->out_stride.strideInfo[0].strideXInBytes);
		return false;
	}
	return true;
}

static bool alloc_host(struct sc_gpu *g)
{
	size_t flow = (size_t)g->ow * 4 * g->oh, gray = (size_t)g->fw * g->fh;
	void *p;
	if (!cu_ok(g, g->cu.cuMemHostAlloc(&p, SC_THUMB_W * SC_THUMB_H, 0), "host alloc"))
		return false;
	g->h_thumb = p;
	if (!cu_ok(g, g->cu.cuMemHostAlloc(&p, flow, 0), "host alloc"))
		return false;
	g->h_fwd = p;
	if (!cu_ok(g, g->cu.cuMemHostAlloc(&p, flow, 0), "host alloc"))
		return false;
	g->h_bwd = p;
	for (int i = 0; i < 2; i++) {
		if (!cu_ok(g, g->cu.cuMemHostAlloc(&p, gray, 0), "host alloc"))
			return false;
		g->h_gray[i] = p;
	}
	return true;
}

struct sc_gpu *sc_gpu_create(void *d3d11_device, void *gray_tex, void *thumb_tex, uint32_t flow_w, uint32_t flow_h,
			     enum sc_gpu_quality q, char *err, size_t err_len, enum sc_gpu_fail *why)
{
	enum sc_gpu_fail fail = SC_GPU_FAIL_OTHER;
	if (why)
		*why = SC_GPU_FAIL_OTHER;

	char adapter_name[128];
	unsigned vendor = adapter_vendor((ID3D11Device *)d3d11_device, adapter_name, sizeof(adapter_name));
	if (vendor && vendor != SC_VENDOR_NVIDIA) {
		snprintf(err, err_len, "%s", adapter_name[0] ? adapter_name : "a non-NVIDIA GPU");
		if (why)
			*why = SC_GPU_FAIL_NOT_NVIDIA;
		return NULL;
	}

	struct sc_gpu *g = calloc(1, sizeof(*g));
	bool ok = false, pushed = false;
	if (!g) {
		snprintf(err, err_len, "out of memory");
		return NULL;
	}
	g->fw = flow_w;
	g->fh = flow_h;
	g->ref = -1;

	if (!load_libs(g)) {
		/* nvcuda.dll / nvofapi64.dll missing or too old on an NVIDIA GPU: driver problem */
		fail = SC_GPU_FAIL_NO_OF;
		goto done;
	}
	if (!create_context(g, (ID3D11Device *)d3d11_device))
		goto done;
	if (!cu_ok(g, g->cu.cuCtxPushCurrent(g->ctx), "push context"))
		goto done;
	pushed = true;
	if (!cu_ok(g, g->cu.cuStreamCreate(&g->stream, CU_STREAM_NON_BLOCKING), "stream create"))
		goto done;
	if (!cu_ok(g, g->cu.cuGraphicsD3D11RegisterResource(&g->res[0], gray_tex, CU_GRAPHICS_REGISTER_FLAGS_NONE),
		   "register gray texture") ||
	    !cu_ok(g, g->cu.cuGraphicsD3D11RegisterResource(&g->res[1], thumb_tex, CU_GRAPHICS_REGISTER_FLAGS_NONE),
		   "register thumbnail texture"))
		goto done;
	if (!init_flow(g, q)) {
		/* the optical flow session itself could not be set up: no OF engine (pre-Turing) or old driver */
		fail = SC_GPU_FAIL_NO_OF;
		goto done;
	}
	if (!alloc_host(g))
		goto done;
	ok = true;
done:
	if (pushed) {
		CUcontext dummy;
		g->cu.cuCtxPopCurrent(&dummy);
	}
	if (!ok) {
		snprintf(err, err_len, "%s", g->err[0] ? g->err : "unknown error");
		if (why)
			*why = fail;
		sc_gpu_destroy(g);
		return NULL;
	}
	return g;
}

void sc_gpu_destroy(struct sc_gpu *g)
{
	if (!g)
		return;
	if (g->ctx && g->cu.cuCtxPushCurrent && g->cu.cuCtxPushCurrent(g->ctx) == CUDA_SUCCESS) {
		if (g->stream)
			g->cu.cuStreamSynchronize(g->stream);
		if (g->hof) {
			if (g->out_fwd)
				g->of.nvOFDestroyGPUBufferCuda(g->out_fwd);
			if (g->out_bwd)
				g->of.nvOFDestroyGPUBufferCuda(g->out_bwd);
			for (int i = 0; i < 2; i++)
				if (g->in[i])
					g->of.nvOFDestroyGPUBufferCuda(g->in[i]);
			g->of.nvOFDestroy(g->hof);
		}
		for (int i = 0; i < 2; i++)
			if (g->res[i])
				g->cu.cuGraphicsUnregisterResource(g->res[i]);
		if (g->h_thumb)
			g->cu.cuMemFreeHost(g->h_thumb);
		if (g->h_fwd)
			g->cu.cuMemFreeHost(g->h_fwd);
		if (g->h_bwd)
			g->cu.cuMemFreeHost(g->h_bwd);
		for (int i = 0; i < 2; i++)
			if (g->h_gray[i])
				g->cu.cuMemFreeHost(g->h_gray[i]);
		if (g->stream)
			g->cu.cuStreamDestroy(g->stream);
		CUcontext dummy;
		g->cu.cuCtxPopCurrent(&dummy);
		g->cu.cuCtxDestroy(g->ctx);
	}
	if (g->nvof_dll)
		FreeLibrary(g->nvof_dll);
	if (g->cuda_dll)
		FreeLibrary(g->cuda_dll);
	free(g);
}

static bool copy2d(struct sc_gpu *g, CUDA_MEMCPY2D *c, const char *what)
{
	return cu_ok(g, g->cu.cuMemcpy2DAsync(c, g->stream), what);
}

bool sc_gpu_submit(struct sc_gpu *g, bool want_gray)
{
	CUcontext dummy;
	CUarray gray_arr = NULL, thumb_arr = NULL;
	bool ok = false, mapped = false;

	g->submitted = false;
	if (!cu_ok(g, g->cu.cuCtxPushCurrent(g->ctx), "push context"))
		return false;

	/* waits on the GPU (not the CPU) for OBS's rendering into the two targets */
	if (!cu_ok(g, g->cu.cuGraphicsMapResources(2, g->res, g->stream), "map"))
		goto done;
	mapped = true;
	if (!cu_ok(g, g->cu.cuGraphicsSubResourceGetMappedArray(&gray_arr, g->res[0], 0, 0), "get gray array") ||
	    !cu_ok(g, g->cu.cuGraphicsSubResourceGetMappedArray(&thumb_arr, g->res[1], 0, 0), "get thumb array"))
		goto done;

	CUDA_MEMCPY2D c;
	memset(&c, 0, sizeof(c));
	c.srcMemoryType = CU_MEMORYTYPE_ARRAY;
	c.srcArray = gray_arr;
	c.dstMemoryType = CU_MEMORYTYPE_ARRAY;
	c.dstArray = g->of.nvOFGPUBufferGetCUarray(g->in[g->cur]);
	c.WidthInBytes = g->fw;
	c.Height = g->fh;
	if (!copy2d(g, &c, "copy gray"))
		goto done;

	memset(&c, 0, sizeof(c));
	c.srcMemoryType = CU_MEMORYTYPE_ARRAY;
	c.srcArray = thumb_arr;
	c.dstMemoryType = CU_MEMORYTYPE_HOST;
	c.dstHost = g->h_thumb;
	c.dstPitch = SC_THUMB_W;
	c.WidthInBytes = SC_THUMB_W;
	c.Height = SC_THUMB_H;
	if (!copy2d(g, &c, "copy thumbnail"))
		goto done;

	if (!cu_ok(g, g->cu.cuGraphicsUnmapResources(2, g->res, g->stream), "unmap"))
		goto done;
	mapped = false;

	/* every frame: this frame's gray image to the CPU, for exact repeat detection */
	memset(&c, 0, sizeof(c));
	c.srcMemoryType = CU_MEMORYTYPE_ARRAY;
	c.srcArray = g->of.nvOFGPUBufferGetCUarray(g->in[g->cur]);
	c.dstMemoryType = CU_MEMORYTYPE_HOST;
	c.dstHost = g->h_gray[1];
	c.dstPitch = g->fw;
	c.WidthInBytes = g->fw;
	c.Height = g->fh;
	if (!copy2d(g, &c, "copy gray to host"))
		goto done;

	g->sub_flow = g->ref >= 0;
	g->sub_gray = want_gray && g->sub_flow;
	g->sub_cur = g->cur;
	g->sub_ref = g->ref;

	if (g->sub_flow) {
		NV_OF_EXECUTE_INPUT_PARAMS in;
		NV_OF_EXECUTE_OUTPUT_PARAMS out;
		memset(&in, 0, sizeof(in));
		memset(&out, 0, sizeof(out));
		/* pairs alternate direction, so the driver's temporal hints would mislead it */
		in.disableTemporalHints = NV_OF_TRUE;

		in.inputFrame = g->in[g->ref];
		in.referenceFrame = g->in[g->cur];
		out.outputBuffer = g->out_fwd;
		if (!of_ok(g, g->of.nvOFExecute(g->hof, &in, &out), "execute (forward)"))
			goto done;
		in.inputFrame = g->in[g->cur];
		in.referenceFrame = g->in[g->ref];
		out.outputBuffer = g->out_bwd;
		if (!of_ok(g, g->of.nvOFExecute(g->hof, &in, &out), "execute (backward)"))
			goto done;

		const size_t pitch = g->out_stride.strideInfo[0].strideXInBytes;
		NvOFGPUBufferHandle outs[2] = {g->out_fwd, g->out_bwd};
		uint8_t *dsts[2] = {g->h_fwd, g->h_bwd};
		for (int i = 0; i < 2; i++) {
			memset(&c, 0, sizeof(c));
			c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
			c.srcDevice = g->of.nvOFGPUBufferGetCUdeviceptr(outs[i]);
			c.srcPitch = pitch;
			c.dstMemoryType = CU_MEMORYTYPE_HOST;
			c.dstHost = dsts[i];
			c.dstPitch = (size_t)g->ow * 4;
			c.WidthInBytes = (size_t)g->ow * 4;
			c.Height = g->oh;
			if (!copy2d(g, &c, "copy flow"))
				goto done;
		}
		if (g->sub_gray) { /* calibration also needs the reference frame's gray */
			memset(&c, 0, sizeof(c));
			c.srcMemoryType = CU_MEMORYTYPE_ARRAY;
			c.srcArray = g->of.nvOFGPUBufferGetCUarray(g->in[g->ref]);
			c.dstMemoryType = CU_MEMORYTYPE_HOST;
			c.dstHost = g->h_gray[0];
			c.dstPitch = g->fw;
			c.WidthInBytes = g->fw;
			c.Height = g->fh;
			if (!copy2d(g, &c, "copy reference gray to host"))
				goto done;
		}
	}
	g->submitted = true;
	ok = true;
done:
	if (mapped)
		g->cu.cuGraphicsUnmapResources(2, g->res, g->stream);
	g->cu.cuCtxPopCurrent(&dummy);
	return ok;
}

bool sc_gpu_collect(struct sc_gpu *g, struct sc_gpu_result *r)
{
	CUcontext dummy;
	LARGE_INTEGER f, t0, t1;
	memset(r, 0, sizeof(*r));
	if (!g->submitted)
		return false;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	if (!cu_ok(g, g->cu.cuCtxPushCurrent(g->ctx), "push context"))
		return false;
	bool ok = cu_ok(g, g->cu.cuStreamSynchronize(g->stream), "synchronize");
	g->cu.cuCtxPopCurrent(&dummy);
	QueryPerformanceCounter(&t1);
	g->submitted = false;
	if (!ok)
		return false;

	r->wait_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
	r->thumb = g->h_thumb;
	r->has_flow = g->sub_flow;
	r->fwd = g->h_fwd;
	r->bwd = g->h_bwd;
	r->flow_pitch = (size_t)g->ow * 4;
	r->ow = g->ow;
	r->oh = g->oh;
	r->has_gray = g->sub_gray;
	r->gray_prev = g->h_gray[0];
	r->gray_cur = g->h_gray[1];
	return true;
}

void sc_gpu_commit(struct sc_gpu *g, bool was_new)
{
	if (was_new) {
		/* the frame just collected becomes the reference; write the other slot next */
		g->ref = g->sub_cur;
		g->cur = 1 - g->sub_cur;
	} else {
		/* repeated frame: keep the reference, overwrite the same slot next time */
		g->cur = g->sub_cur;
	}
}

void sc_gpu_drop_reference(struct sc_gpu *g)
{
	g->ref = -1;
	g->cur = 0;
}

const char *sc_gpu_error(const struct sc_gpu *g)
{
	return g->err;
}

#endif /* _WIN32 */
