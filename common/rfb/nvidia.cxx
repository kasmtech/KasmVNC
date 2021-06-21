/*
 * H.264/HEVC hardware encoding using nvidia nvenc
 * Copyright (c) 2016 Timo Rothenpieler <timo@rothenpieler.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <rfb/LogWriter.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvidia.h"

using namespace rfb;

static LogWriter vlog("nvidia");

#define FFNV_LOG_FUNC(logctx, msg, ...) vlog.info((msg), __VA_ARGS__)
#define FFNV_DEBUG_LOG_FUNC(logctx, msg, ...)

#include "dynlink_loader.h"

#define NUM_SURF 4

typedef struct NvencSurface
{
	NV_ENC_INPUT_PTR input_surface;
	int reg_idx;
	int width;
	int height;
	int pitch;

	NV_ENC_OUTPUT_PTR output_surface;
	NV_ENC_BUFFER_FORMAT format;
} NvencSurface;

typedef struct NvencDynLoadFunctions
{
	CudaFunctions *cuda_dl;
	NvencFunctions *nvenc_dl;

	void *nvenc_ctx;
	NV_ENCODE_API_FUNCTION_LIST nvenc_funcs;

	NV_ENC_INITIALIZE_PARAMS init_enc_parms;
	NV_ENC_CONFIG enc_cfg;
	CUdevice cu_dev;
	CUcontext cu_ctx;

	NvencSurface surf[NUM_SURF];
	uint8_t cursurf;
} NvencDynLoadFunctions;

static NvencDynLoadFunctions nvenc;

/*
Recommended settings for streaming
Low-Latency High Quality preset
Rate control mode = Two-pass CBR
Very low VBV buffer size (Single frame)
No B Frames
Infinite GOP length
Adaptive Quantization enabled
*/

static int loadfuncs() {
	int ret;
	NVENCSTATUS err;
	uint32_t nvenc_max_ver;

	ret = cuda_load_functions(&nvenc.cuda_dl);
	if (ret < 0)
		return ret;

	ret = nvenc_load_functions(&nvenc.nvenc_dl);
	if (ret < 0)
		return ret;

	err = nvenc.nvenc_dl->NvEncodeAPIGetMaxSupportedVersion(&nvenc_max_ver);
	if (err != NV_ENC_SUCCESS)
		return -1;

	vlog.info("Loaded nvenc version %u.%u", nvenc_max_ver >> 4, nvenc_max_ver & 0xf);

	if ((NVENCAPI_MAJOR_VERSION << 4 | NVENCAPI_MINOR_VERSION) > nvenc_max_ver) {
		vlog.error("Your Nvidia driver is too old. Nvenc %u.%u required",
			NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
		return -1;
	}

	nvenc.nvenc_funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;

	err = nvenc.nvenc_dl->NvEncodeAPICreateInstance(&nvenc.nvenc_funcs);
	if (err != NV_ENC_SUCCESS)
		return -1;
	return 0;
}

static int nvenc_check_cap(NV_ENC_CAPS cap) {
	NV_ENC_CAPS_PARAM params;
	memset(&params, 0, sizeof(NV_ENC_CAPS_PARAM));

	params.version = NV_ENC_CAPS_PARAM_VER;
	params.capsToQuery = cap;

	int ret, val = 0;

	ret = nvenc.nvenc_funcs.nvEncGetEncodeCaps(nvenc.nvenc_ctx,
						nvenc.init_enc_parms.encodeGUID,
						&params, &val);
	if (ret == NV_ENC_SUCCESS)
		return val;
	return 0;
}

static int setupdevice() {
	int ret;

	nvenc.init_enc_parms.encodeGUID = NV_ENC_CODEC_H264_GUID;
	nvenc.init_enc_parms.presetGUID = NV_ENC_PRESET_P7_GUID;

	ret = nvenc.cuda_dl->cuInit(0);
	if (ret < 0)
		return ret;

	ret = nvenc.cuda_dl->cuDeviceGet(&nvenc.cu_dev, 0);
	if (ret < 0)
		return ret;

	ret = nvenc.cuda_dl->cuCtxCreate(&nvenc.cu_ctx, CU_CTX_SCHED_BLOCKING_SYNC,
					nvenc.cu_dev);
	if (ret < 0)
		return ret;

	CUcontext dummy;
	nvenc.cuda_dl->cuCtxPopCurrent(&dummy);

	// cuda stream is NULL to use the default

	// open session
	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params;
	memset(&params, 0, sizeof(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS));
	NVENCSTATUS err;

	params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
	params.apiVersion = NVENCAPI_VERSION;
	params.device = nvenc.cu_ctx;
	params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;

	err = nvenc.nvenc_funcs.nvEncOpenEncodeSessionEx(&params, &nvenc.nvenc_ctx);
	if (err != NV_ENC_SUCCESS)
		return -1;

	// check caps
	const int maxw = nvenc_check_cap(NV_ENC_CAPS_WIDTH_MAX);
	const int maxh = nvenc_check_cap(NV_ENC_CAPS_HEIGHT_MAX);
	const int minw = nvenc_check_cap(NV_ENC_CAPS_WIDTH_MIN);
	const int minh = nvenc_check_cap(NV_ENC_CAPS_HEIGHT_MIN);

	vlog.info("Max enc resolution %ux%u, min %ux%u\n", maxw, maxh, minw, minh);

	return 0;
}

static int setupenc(const unsigned w, const unsigned h, const unsigned kbps,
			const unsigned fps) {
	NVENCSTATUS err;

	nvenc.enc_cfg.version = NV_ENC_CONFIG_VER;
	nvenc.init_enc_parms.version = NV_ENC_INITIALIZE_PARAMS_VER;
	nvenc.init_enc_parms.darWidth =
	nvenc.init_enc_parms.encodeWidth = w;
	nvenc.init_enc_parms.darHeight =
	nvenc.init_enc_parms.encodeHeight = h;

	nvenc.init_enc_parms.frameRateNum = fps;
	nvenc.init_enc_parms.frameRateDen = 1;

	nvenc.init_enc_parms.encodeConfig = &nvenc.enc_cfg;
	nvenc.init_enc_parms.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;

	NV_ENC_PRESET_CONFIG preset_cfg;
	memset(&preset_cfg, 0, sizeof(NV_ENC_PRESET_CONFIG));

	preset_cfg.version = NV_ENC_PRESET_CONFIG_VER;
	preset_cfg.presetCfg.version = NV_ENC_CONFIG_VER;

	err = nvenc.nvenc_funcs.nvEncGetEncodePresetConfigEx(nvenc.nvenc_ctx,
			nvenc.init_enc_parms.encodeGUID,
			nvenc.init_enc_parms.presetGUID,
			nvenc.init_enc_parms.tuningInfo,
			&preset_cfg);
	if (err != NV_ENC_SUCCESS)
		return -1;

	memcpy(&nvenc.enc_cfg, &preset_cfg.presetCfg, sizeof(nvenc.enc_cfg));

	nvenc.enc_cfg.version = NV_ENC_CONFIG_VER;

	nvenc.init_enc_parms.enableEncodeAsync = 0;
	nvenc.init_enc_parms.enablePTD = 1;

	nvenc.enc_cfg.frameIntervalP = 0;
	nvenc.enc_cfg.gopLength = 1;

	// use 4 surfaces

	// setup rate control
	nvenc.enc_cfg.rcParams.multiPass = NV_ENC_TWO_PASS_FULL_RESOLUTION;
	nvenc.enc_cfg.rcParams.averageBitRate = kbps * 1024;
	nvenc.enc_cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
	nvenc.enc_cfg.rcParams.lowDelayKeyFrameScale = 1;

	nvenc.enc_cfg.rcParams.enableAQ = 1;
	nvenc.enc_cfg.rcParams.aqStrength = 4; // 1 - 15, 0 would be auto

	nvenc.enc_cfg.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

	// setup_codec_config
	nvenc.enc_cfg.encodeCodecConfig.h264Config.h264VUIParameters.videoFullRangeFlag = 1;
	nvenc.enc_cfg.encodeCodecConfig.h264Config.outputBufferingPeriodSEI = 1;
	nvenc.enc_cfg.encodeCodecConfig.h264Config.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_ENABLE;
	nvenc.enc_cfg.encodeCodecConfig.h264Config.fmoMode = NV_ENC_H264_FMO_DISABLE;
	nvenc.enc_cfg.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;

	nvenc.cuda_dl->cuCtxPushCurrent(nvenc.cu_ctx);

	err = nvenc.nvenc_funcs.nvEncInitializeEncoder(nvenc.nvenc_ctx,
			&nvenc.init_enc_parms);
	if (err != NV_ENC_SUCCESS)
		return -1;

	// custream?

	CUcontext dummy;
	nvenc.cuda_dl->cuCtxPopCurrent(&dummy);

	return 0;
}

static int setupsurf(const unsigned w, const unsigned h) {

	nvenc.cuda_dl->cuCtxPushCurrent(nvenc.cu_ctx);

	int i;
	for (i = 0; i < NUM_SURF; i++) {
		NVENCSTATUS err;
		NV_ENC_CREATE_BITSTREAM_BUFFER allocOut;
		memset(&allocOut, 0, sizeof(NV_ENC_CREATE_BITSTREAM_BUFFER));
		allocOut.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

		NV_ENC_CREATE_INPUT_BUFFER allocSurf;
		memset(&allocSurf, 0, sizeof(NV_ENC_CREATE_INPUT_BUFFER));

		nvenc.surf[i].format = NV_ENC_BUFFER_FORMAT_ABGR; // doesn't have RGBA!
		allocSurf.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
		allocSurf.width = w;
		allocSurf.height = h;
		allocSurf.bufferFmt = nvenc.surf[i].format;

		err = nvenc.nvenc_funcs.nvEncCreateInputBuffer(nvenc.nvenc_ctx, &allocSurf);
		if (err != NV_ENC_SUCCESS)
			return -1;

		nvenc.surf[i].input_surface = allocSurf.inputBuffer;
		nvenc.surf[i].width = allocSurf.width;
		nvenc.surf[i].height = allocSurf.height;

		// output
		err = nvenc.nvenc_funcs.nvEncCreateBitstreamBuffer(nvenc.nvenc_ctx, &allocOut);
		if (err != NV_ENC_SUCCESS)
			return -1;

		nvenc.surf[i].output_surface = allocOut.bitstreamBuffer;
	}

	CUcontext dummy;
	nvenc.cuda_dl->cuCtxPopCurrent(&dummy);

	return 0;
}

int nvenc_frame(const uint8_t *data, unsigned pts, uint8_t *out, uint32_t &outlen) {
	NVENCSTATUS err;

	NV_ENC_PIC_PARAMS params;
	memset(&params, 0, sizeof(NV_ENC_PIC_PARAMS));
	params.version = NV_ENC_PIC_PARAMS_VER;
	params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEINTRA | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

	nvenc.cuda_dl->cuCtxPushCurrent(nvenc.cu_ctx);

	NV_ENC_LOCK_INPUT_BUFFER lockBufferParams;
	memset(&lockBufferParams, 0, sizeof(NV_ENC_LOCK_INPUT_BUFFER));
	lockBufferParams.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
	lockBufferParams.inputBuffer = nvenc.surf[nvenc.cursurf].input_surface;

	err = nvenc.nvenc_funcs.nvEncLockInputBuffer(nvenc.nvenc_ctx, &lockBufferParams);
	if (err != NV_ENC_SUCCESS)
		return -1;

	nvenc.surf[nvenc.cursurf].pitch = lockBufferParams.pitch;
	vlog.info("pitch %u\n", lockBufferParams.pitch);

	// copy frame
	unsigned y;
	uint8_t *dst = (uint8_t *) lockBufferParams.bufferDataPtr;
	const unsigned linelen = nvenc.surf[nvenc.cursurf].width * 4;
	for (y = 0; y < (unsigned) nvenc.surf[nvenc.cursurf].height; y++) {
		memcpy(dst, data, linelen);
		data += linelen;
		dst += lockBufferParams.pitch;
	}

	err = nvenc.nvenc_funcs.nvEncUnlockInputBuffer(nvenc.nvenc_ctx,
				nvenc.surf[nvenc.cursurf].input_surface);
	if (err != NV_ENC_SUCCESS)
		return -1;

	CUcontext dummy;
	nvenc.cuda_dl->cuCtxPopCurrent(&dummy);

	params.inputBuffer = nvenc.surf[nvenc.cursurf].input_surface;
	params.bufferFmt = nvenc.surf[nvenc.cursurf].format;
	params.inputWidth = nvenc.surf[nvenc.cursurf].width;
	params.inputHeight = nvenc.surf[nvenc.cursurf].height;
	params.inputPitch = nvenc.surf[nvenc.cursurf].pitch;
	params.outputBitstream = nvenc.surf[nvenc.cursurf].output_surface;
	params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	params.inputTimeStamp = pts;

	nvenc.cuda_dl->cuCtxPushCurrent(nvenc.cu_ctx);

	err = nvenc.nvenc_funcs.nvEncEncodePicture(nvenc.nvenc_ctx, &params);

	nvenc.cuda_dl->cuCtxPopCurrent(&dummy);

	if (err != NV_ENC_SUCCESS)
		return -1;


	nvenc.cuda_dl->cuCtxPushCurrent(nvenc.cu_ctx);

	// Get output
	NV_ENC_LOCK_BITSTREAM lock_params;
	memset(&lock_params, 0, sizeof(NV_ENC_LOCK_BITSTREAM));

	lock_params.version = NV_ENC_LOCK_BITSTREAM_VER;
	lock_params.doNotWait = 0;
	lock_params.outputBitstream = nvenc.surf[nvenc.cursurf].output_surface;
	// lock_params.sliceOffsets = slice_offsets; TODO?

	err = nvenc.nvenc_funcs.nvEncLockBitstream(nvenc.nvenc_ctx, &lock_params);
	if (err != NV_ENC_SUCCESS)
		return -1;

	memcpy(out, lock_params.bitstreamBufferPtr, lock_params.bitstreamSizeInBytes);
	outlen = lock_params.bitstreamSizeInBytes;

	err = nvenc.nvenc_funcs.nvEncUnlockBitstream(nvenc.nvenc_ctx,
				nvenc.surf[nvenc.cursurf].output_surface);
	if (err != NV_ENC_SUCCESS)
		return -1;

	nvenc.cuda_dl->cuCtxPopCurrent(&dummy);

	vlog.info("Pic type %x, idr %x i %x\n", lock_params.pictureType, NV_ENC_PIC_TYPE_IDR,
		NV_ENC_PIC_TYPE_I);

	return 0;
}

static void unload() {
	NV_ENC_PIC_PARAMS params;
	memset(&params, 0, sizeof(NV_ENC_PIC_PARAMS));
	params.version = NV_ENC_PIC_PARAMS_VER;
	params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

	nvenc.cuda_dl->cuCtxPushCurrent(nvenc.cu_ctx);

	nvenc.nvenc_funcs.nvEncEncodePicture(nvenc.nvenc_ctx, &params);

	int i;
	for (i = 0; i < NUM_SURF; i++) {
		nvenc.nvenc_funcs.nvEncDestroyInputBuffer(nvenc.nvenc_ctx,
					nvenc.surf[i].input_surface);
		nvenc.nvenc_funcs.nvEncDestroyBitstreamBuffer(nvenc.nvenc_ctx,
					nvenc.surf[i].output_surface);
	}

	nvenc.nvenc_funcs.nvEncDestroyEncoder(nvenc.nvenc_ctx);

	CUcontext dummy;
	nvenc.cuda_dl->cuCtxPopCurrent(&dummy);

	nvenc.cuda_dl->cuCtxDestroy(nvenc.cu_ctx);

	nvenc_free_functions(&nvenc.nvenc_dl);
	cuda_free_functions(&nvenc.cuda_dl);
}
/*
int main() {

	unsigned w = 256, h = 256, kbps = 400, fps = 15;

	memset(&nvenc, 0, sizeof(NvencDynLoadFunctions));
	if (loadfuncs() < 0)
		return 1;
	if (setupdevice() < 0)
		return 1;
	if (setupenc(w, h, kbps, fps) < 0)
		return 1;
	if (setupsurf(w, h) < 0)
		return 1;

	unload();

	return 0;
}
*/

int nvidia_init(const unsigned w, const unsigned h, const unsigned kbps,
		const unsigned fps) {

	memset(&nvenc, 0, sizeof(NvencDynLoadFunctions));
	if (loadfuncs() < 0)
		return 1;
	if (setupdevice() < 0)
		return 1;
	if (setupenc(w, h, kbps, fps) < 0)
		return 1;
	if (setupsurf(w, h) < 0)
		return 1;

	return 0;
}
