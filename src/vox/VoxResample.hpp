#pragma once

// Internal band-limited mono resampler for BG Vox Transcribe.

#include "VoxDsp.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bgmedia::vox::detail {

// Kaiser-windowed-sinc polyphase resampler (about 80 dB stop-band). The
// low-pass sits at 0.95 of the LOWER of the two Nyquist frequencies with a
// transition band of 5 percent on either side of that, so content that would
// alias (above the output Nyquist when down-sampling) is removed first and
// everything up to 0.9 of the output Nyquist passes within a fraction of a dB.
// DC gain is exactly 1. The filter is symmetric: no group delay, output sample
// n sits at input time n * inRate / outRate. Output length is
// floor(frames * outRate / inRate), at least 1 for non-empty input.
// Returns false when `keep` returned false (cancelled); `out` is then
// unspecified. Throws std::bad_alloc like any vector.
bool resampleMono(const float* in, std::size_t frames, std::uint32_t inRate,
                  std::uint32_t outRate, std::vector<float>& out, const KeepGoing& keep);

} // namespace bgmedia::vox::detail
