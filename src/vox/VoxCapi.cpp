#include "bgmedia/vox/vox_capi.h"

#include "bgmedia/vox/VoxEnhance.hpp"

#include <cstring>
#include <vector>

extern "C" {

int bgm_vox_enhance_s16(const int16_t* in, size_t frames, uint32_t sample_rate,
                        uint32_t channels, int mode, int16_t* out,
                        const volatile int* cancel_flag) {
    using namespace bgmedia::vox;
    try {
        if (in == nullptr || frames == 0 || channels == 0) {
            return static_cast<int>(VoxStatus::FallbackInvalidInput);
        }
        if (frames > static_cast<size_t>(-1) / channels / sizeof(int16_t)) {
            return static_cast<int>(VoxStatus::FallbackTooLong);
        }
        const size_t total = frames * channels;

        VoxEnhanceConfig config;
        switch (mode) {
        case BGM_VOX_MODE_OFF: config.mode = VoxEnhanceMode::Off; break;
        case BGM_VOX_MODE_NATURAL: config.mode = VoxEnhanceMode::Natural; break;
        case BGM_VOX_MODE_STRONG: config.mode = VoxEnhanceMode::StrongExperimental; break;
        default: config.mode = VoxEnhanceMode::Off; break;
        }
        const bool badMode = mode < BGM_VOX_MODE_OFF || mode > BGM_VOX_MODE_STRONG;

        VoxCancelToken token(cancel_flag);
        VoxEnhanceResult result =
            enhanceVoice(in, frames, VoxPcmFormat{sample_rate, channels}, config, &token);
        int status = static_cast<int>(result.report.status);
        if (badMode) {
            status = static_cast<int>(VoxStatus::FallbackInvalidInput);
        }
        if (out != nullptr) {
            if (status == static_cast<int>(VoxStatus::Processed) && result.pcm.size() == total) {
                floatToInt16(result.pcm.data(), total, out);
            } else if (out != in) {
                std::memcpy(out, in, total * sizeof(int16_t));
            }
        }
        return status;
    } catch (...) {
        if (in != nullptr && out != nullptr && out != in && frames != 0 && channels != 0) {
            std::memcpy(out, in, frames * channels * sizeof(int16_t));
        }
        return static_cast<int>(bgmedia::vox::VoxStatus::FallbackInternalError);
    }
}

const char* bgm_vox_status_name(int status) {
    using namespace bgmedia::vox;
    if (status < 0 || status > static_cast<int>(VoxStatus::FallbackInvalidInput)) {
        return "Unknown";
    }
    return voxStatusName(static_cast<VoxStatus>(status));
}

} // extern "C"
