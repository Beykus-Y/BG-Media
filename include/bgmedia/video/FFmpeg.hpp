#pragma once

#include "bgmedia/video/Feed.hpp"

namespace bgmedia::video {

// Optional libav adapter for regular, self-contained MP4 files.
std::unique_ptr<Decoder> openFFmpeg(const Media&, std::stop_token = {});

class FFmpegProvider final : public Provider {
public:
    Prepared prepare(const Media&, std::size_t, std::stop_token) override;
};

}
