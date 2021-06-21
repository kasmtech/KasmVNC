#ifndef KASM_NVIDIA_H
#define KASM_NVIDIA_H

#include <stdint.h>

int nvidia_init(const unsigned w, const unsigned h, const unsigned kbps,
		const unsigned fps);
int nvenc_frame(const uint8_t *data, unsigned pts, uint8_t *out, uint32_t &outlen);

#endif
