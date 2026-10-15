#pragma once

// Fitting Godot's stereo into a plugin port of another width. Shared by the CLAP and VST3 layers.

#include <cstdint>
#include <cstring>

namespace gaph {

// AUDIO THREAD. Stereo planar source -> a port of any width: mono gets (L+R)/2, stereo a copy,
// wider ports L and R in the first two channels and silence in the rest. Copies rather than
// lending the host's buffers out, because a plugin may write into its inputs ("in place").
inline void copy_stereo_into_port(uint32_t p_channels, float *const *p_dst, const float *const *p_src, uint32_t p_frames) {
	const size_t bytes = sizeof(float) * p_frames;
	if (p_channels == 1) {
		for (uint32_t f = 0; f < p_frames; f++) {
			p_dst[0][f] = 0.5f * (p_src[0][f] + p_src[1][f]);
		}
		return;
	}
	for (uint32_t c = 0; c < p_channels; c++) {
		if (c < 2) {
			std::memcpy(p_dst[c], p_src[c], bytes);
		} else {
			std::memset(p_dst[c], 0, bytes);
		}
	}
}

} // namespace gaph
