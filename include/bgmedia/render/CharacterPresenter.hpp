#pragma once

#include "bgmedia/choreography/Choreography.hpp"

namespace bgmedia {

// Extension point for renderers and character formats. The engine produces
// CharacterPose values in media time; a presenter decides whether that becomes
// SDL primitives, a sprite sheet, a Live2D rig, a glTF skeleton, or a future
// BeyGram avatar. This interface deliberately lives in the core and carries no
// graphics API types.
class ICharacterPresenter {
public:
    virtual ~ICharacterPresenter() = default;

    // mediaTime is provided for effects that need sub-cue phase; pose already
    // contains the FPS-independent sampling result.
    virtual void present(const CharacterPose& pose, double mediaTime) = 0;
};

} // namespace bgmedia
