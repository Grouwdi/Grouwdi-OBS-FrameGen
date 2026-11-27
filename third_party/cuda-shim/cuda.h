/* Shim so nvOpticalFlowCuda.h (#include <cuda.h>) uses FFmpeg nv-codec-headers
 * declarations; CUDA is loaded at runtime from nvcuda.dll, no CUDA SDK needed. */
#pragma once
#include <ffnvcodec/dynlink_cuda.h>
