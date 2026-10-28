#include "framework.hpp"

#include "support/TriangleTest.hpp"

namespace {

using namespace bgmedia;
using math::Vec3;
using probe::triangleContact;

Vec3 P(float x, float y, float z) { return {x, y, z}; }

// Horizontal unit triangle in the plane z = 0 (x, y >= 0, x + y <= 1).
const Vec3 kFloor[3] = {P(0, 0, 0), P(1, 0, 0), P(0, 1, 0)};

} // namespace

TEST_CASE(triangle.crossing_pair_reports_depth_through_the_face) {
    // A steep triangle pushes through the floor: two vertices lie 3 cm below it, one 2 cm above.
    const Vec3 wall[3] = {P(0.2f, 0.2f, -0.03f), P(0.6f, 0.2f, -0.03f), P(0.4f, 0.4f, 0.02f)};
    const auto ab = triangleContact(wall, kFloor);
    CHECK(ab.intersects);
    CHECK_NEAR(ab.depth, 0.02, 1e-5);
    const auto ba = triangleContact(kFloor, wall);
    CHECK(ba.intersects);
    CHECK_NEAR(ba.depth, 0.02, 1e-5);
}

TEST_CASE(triangle.separated_pairs_have_no_contact) {
    const Vec3 above[3] = {P(0, 0, 0.05f), P(1, 0, 0.05f), P(0, 1, 0.05f)};
    const auto parallel = triangleContact(above, kFloor);
    CHECK(!parallel.intersects);
    CHECK_NEAR(parallel.depth, 0.0, 1e-9);

    // Its plane crosses the floor, but the crossing lies outside the floor triangle.
    const Vec3 beside[3] = {P(2, 0, -0.1f), P(3, 0, -0.1f), P(2, 0, 0.1f)};
    const auto crossing = triangleContact(beside, kFloor);
    CHECK(!crossing.intersects);
    CHECK_NEAR(crossing.depth, 0.0, 1e-9);
}

TEST_CASE(triangle.coplanar_touching_along_an_edge_is_contact_without_depth) {
    const Vec3 neighbour[3] = {P(1, 0, 0), P(1, 1, 0), P(0, 1, 0)};  // shares the hypotenuse
    const auto touch = triangleContact(kFloor, neighbour);
    CHECK(touch.intersects);
    CHECK_NEAR(touch.depth, 0.0, 1e-9);
}

TEST_CASE(triangle.coplanar_separated_and_overlapping_cases) {
    const Vec3 apart[3] = {P(2, 0, 0), P(3, 0, 0), P(2, 1, 0)};
    CHECK(!triangleContact(kFloor, apart).intersects);

    const Vec3 inside[3] = {P(0.2f, 0.2f, 0), P(0.8f, 0.2f, 0), P(0.2f, 0.8f, 0)};
    const auto overlap = triangleContact(kFloor, inside);
    CHECK(overlap.intersects);
    CHECK_NEAR(overlap.depth, 0.0, 1e-9);
}

TEST_CASE(triangle.shared_edge_fold_is_contact_without_depth) {
    // The second triangle folds up from the floor's edge (0,0,0)-(1,0,0) at 90 degrees.
    const Vec3 fold[3] = {P(0, 0, 0), P(1, 0, 0), P(0.5f, 0, 0.5f)};
    const auto contact = triangleContact(kFloor, fold);
    CHECK(contact.intersects);
    CHECK_NEAR(contact.depth, 0.0, 1e-9);
    const auto reverse = triangleContact(fold, kFloor);
    CHECK(reverse.intersects);
    CHECK_NEAR(reverse.depth, 0.0, 1e-9);
}

TEST_CASE(triangle.vertex_touch_is_contact_without_depth) {
    const Vec3 lifted[3] = {P(0.2f, 0.2f, 0), P(0.5f, 0.2f, 0.1f), P(0.2f, 0.5f, 0.1f)};
    const auto contact = triangleContact(lifted, kFloor);
    CHECK(contact.intersects);
    CHECK_NEAR(contact.depth, 0.0, 1e-9);
}

TEST_CASE(triangle.crossing_planes_meeting_at_one_vertex_have_no_depth) {
    // Both triangles straddle the other's plane but share only the origin.
    const Vec3 a[3] = {P(0, 0, 0), P(0.01f, 0.01f, 0), P(0.01f, -0.01f, 0)};
    const Vec3 b[3] = {P(0, 0, 0), P(-0.01f, 0, 0.01f), P(-0.01f, 0, -0.01f)};
    const auto ab = triangleContact(a, b);
    CHECK(ab.intersects);
    CHECK_NEAR(ab.depth, 0.0, 1e-9);
    const auto ba = triangleContact(b, a);
    CHECK(ba.intersects);
    CHECK_NEAR(ba.depth, 0.0, 1e-9);
}

TEST_CASE(triangle.near_miss_within_epsilon_counts_as_touching) {
    const float epsilon = 1.0e-4f;
    const Vec3 near[3] = {P(0.1f, 0.1f, 0.5e-4f), P(0.5f, 0.1f, 0.5e-4f), P(0.1f, 0.5f, 0.5e-4f)};
    const auto within = triangleContact(near, kFloor, epsilon);
    CHECK(within.intersects);
    CHECK_NEAR(within.depth, 0.0, 1e-9);

    const Vec3 miss[3] = {P(0.1f, 0.1f, 2.0e-4f), P(0.5f, 0.1f, 2.0e-4f), P(0.1f, 0.5f, 2.0e-4f)};
    CHECK(!triangleContact(miss, kFloor, epsilon).intersects);
}

TEST_CASE(triangle.degenerate_triangles_never_intersect) {
    // Three collinear vertices: a line segment piercing the floor is not a surface contact.
    const Vec3 line[3] = {P(0.2f, 0.2f, -0.1f), P(0.2f, 0.2f, 0.0f), P(0.2f, 0.2f, 0.1f)};
    const auto piercing = triangleContact(line, kFloor);
    CHECK(!piercing.intersects);
    CHECK_NEAR(piercing.depth, 0.0, 1e-9);

    const Vec3 repeated[3] = {P(0.2f, 0.2f, 0.0f), P(0.2f, 0.2f, 0.0f), P(0.5f, 0.2f, 0.0f)};
    CHECK(!triangleContact(kFloor, repeated).intersects);
    CHECK(!triangleContact(repeated, kFloor).intersects);
}
