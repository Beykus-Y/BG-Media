#include "framework.hpp"

#include "bgmedia/vrm/VrmModel.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::filesystem::path fixturePath() {
    return std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "minimal.vrm";
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

std::filesystem::path realModelPath() {
    if (const char* env = std::getenv("BGMEDIA_VRM_TEST_MODEL")) {
        return std::filesystem::path(env);
    }
#ifdef BGMEDIA_SOURCE_DIR
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "models" / "Model1.vrm";
#else
    return {};
#endif
}

} // namespace

TEST_CASE(vrm.minimal_fixture_import) {
    using namespace bgmedia::vrm;
    const LoadResult loaded = VrmLoader::load(fixturePath());
    CHECK(loaded.ok());
    if (!loaded.ok()) {
        std::cout << "\n            " << loaded.error;
        return;
    }
    const VrmModel& model = *loaded.model;
    CHECK(model.nodes.size() == 22);
    CHECK(model.meshes.size() == 1);
    CHECK(model.meshes[0].primitives.size() == 1);
    CHECK(model.meshes[0].primitives[0].vertices.size() == 3);
    CHECK(model.meshes[0].primitives[0].indices.size() == 3);
    CHECK(model.diagnostics.triangleCount == 1);
    CHECK(model.materials.size() == 1);
    CHECK(model.materials[0].shader == "VRM/MToon");
    CHECK(model.materials[0].toon);
    CHECK(model.humanoidEntries.size() == 21);
    CHECK(model.completeHumanoid());
    CHECK(model.hasBone(HumanoidBone::Hips));
    CHECK(model.hasBone(HumanoidBone::Head));
    CHECK(model.hasBone(HumanoidBone::LeftFoot));
    CHECK(model.bounds.valid);
    CHECK_NEAR(model.height, 1.7, 0.02);
    // The fixture's eyes are on -Z, so the generic detector must not assume +Z.
    CHECK(model.forward.z < -0.9f);
    CHECK(model.boneBindPosition(HumanoidBone::Head).y > 1.2f);
}

TEST_CASE(vrm.rejects_malformed_containers) {
    using namespace bgmedia::vrm;
    const std::vector<std::uint8_t> bytes = readFile(fixturePath());
    CHECK(!bytes.empty());
    if (bytes.empty()) {
        return;
    }
    const std::size_t cutPoints[] = {0, 4, 11, 12, 16, 24, 64, 256,
                                     bytes.size() / 2, bytes.size() - 1};
    for (std::size_t size : cutPoints) {
        const LoadResult loaded =
            VrmLoader::loadFromMemory(std::span<const std::uint8_t>(bytes.data(), size),
                                      "truncated.vrm");
        CHECK(!loaded.ok());
        CHECK(!loaded.error.empty());
    }
    std::vector<std::uint8_t> badMagic = bytes;
    badMagic[0] = 'x';
    CHECK(!VrmLoader::loadFromMemory(badMagic, "bad-magic.vrm").ok());

    std::vector<std::uint8_t> badVersion = bytes;
    badVersion[4] = 3;
    CHECK(!VrmLoader::loadFromMemory(badVersion, "bad-version.vrm").ok());

    std::vector<std::uint8_t> badLength = bytes;
    badLength[8] ^= 0x40;
    CHECK(!VrmLoader::loadFromMemory(badLength, "bad-length.vrm").ok());

    std::vector<std::uint8_t> noise(4096);
    for (std::size_t i = 0; i < noise.size(); ++i) {
        noise[i] = static_cast<std::uint8_t>((i * 131u + 17u) & 0xffu);
    }
    CHECK(!VrmLoader::loadFromMemory(noise, "noise.vrm").ok());
}

TEST_CASE(vrm.random_mutations_never_crash) {
    using namespace bgmedia::vrm;
    const std::vector<std::uint8_t> bytes = readFile(fixturePath());
    CHECK(!bytes.empty());
    if (bytes.empty()) {
        return;
    }
    std::uint32_t state = 0x9E3779B9u;
    int accepted = 0;
    for (int iteration = 0; iteration < 150; ++iteration) {
        std::vector<std::uint8_t> mutated = bytes;
        for (int mutation = 0; mutation < 4; ++mutation) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const std::size_t at = state % mutated.size();
            mutated[at] ^= static_cast<std::uint8_t>((state >> 8) & 0xffu);
        }
        const LoadResult loaded =
            VrmLoader::loadFromMemory(mutated, "mutated.vrm");
        if (loaded.ok()) {
            ++accepted;
            const VrmModel& model = *loaded.model;
            CHECK(model.bounds.valid);
            CHECK(std::isfinite(model.height));
            CHECK(model.height > 0.0f);
            for (const auto& node : model.nodes) {
                CHECK(node.local.finite());
            }
        } else {
            CHECK(!loaded.error.empty());
        }
    }
    std::cout << "\n            mutation corpus: " << accepted
              << " accepted, " << (150 - accepted) << " rejected";
}

TEST_CASE(vrm.model1_if_available) {
    using namespace bgmedia::vrm;
    const std::filesystem::path path = realModelPath();
    if (path.empty() || !std::filesystem::exists(path)) {
        return; // Private model is intentionally not part of the repository.
    }
    const LoadResult loaded = VrmLoader::load(path);
    CHECK(loaded.ok());
    if (!loaded.ok()) {
        std::cout << "\n            " << loaded.error;
        return;
    }
    const VrmModel& model = *loaded.model;
    std::cout << "\n            Model1: nodes=" << model.nodes.size()
              << " meshes=" << model.meshes.size()
              << " humanoid=" << model.humanoidEntries.size()
              << " bones=" << model.diagnostics.humanoidCoreCount
              << " tris=" << model.diagnostics.triangleCount
              << " materials=" << model.materials.size()
              << " textures=" << model.textures.size()
              << " decoded=" << model.diagnostics.decodedTextureCount
              << " skins=" << model.skins.size()
              << " height=" << model.height
              << " forward=(" << model.forward.x << ',' << model.forward.y << ','
              << model.forward.z << ')'
              << " load=" << model.diagnostics.loadMilliseconds << "ms";
    CHECK(model.nodes.size() == 175);
    CHECK(model.meshes.size() == 3);
    CHECK(model.skins.size() == 3);
    CHECK(model.materials.size() == 15);
    CHECK(model.textures.size() == 27);
    CHECK(model.humanoidEntries.size() == 54);
    CHECK(model.completeHumanoid());
    CHECK(model.diagnostics.triangleCount > 30000);
    CHECK(model.diagnostics.triangleCount < 40000);
    CHECK(model.diagnostics.decodedTextureCount >= 15);
    CHECK(model.textures[0].decoded);
    CHECK(model.textures[0].width > 0);
    CHECK(model.textures[0].height > 0);
    CHECK(!model.textures[0].rgba.empty());
    CHECK(model.height > 1.4f && model.height < 1.8f);
    CHECK(model.forward.z < -0.8f);
    CHECK(model.materials[0].toon);
    CHECK(model.materials[0].shader == "VRM/MToon");
    CHECK(model.skins[0].joints.size() == 171);
    CHECK(model.skins[0].inverseBindMatrices.size() == 171);
}
