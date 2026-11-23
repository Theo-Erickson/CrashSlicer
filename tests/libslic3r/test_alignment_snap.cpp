#include <catch2/catch_all.hpp>
#include "libslic3r/AlignmentSnap.hpp"

using namespace Slic3r;
using namespace Slic3r::AlignmentSnap;
using Catch::Matchers::WithinAbs;

static BoundingBoxf box(double x0, double y0, double x1, double y1) {
    return BoundingBoxf(Vec2d(x0, y0), Vec2d(x1, y1));
}

TEST_CASE("no targets returns raw delta unchanged", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true;
    SnapState st;
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(3.0, 4.0), {}, s, 1.0, st);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(3.0, 1e-6));
    REQUIRE_THAT(r.corrected_delta.y(), WithinAbs(4.0, 1e-6));
}

// ---- Task 2: edge alignment ------------------------------------------------

TEST_CASE("left edges align across a gap", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.center_align = false; s.contact = false; s.spacing_propagation = false;
    SnapState st;
    // target left edge at x=0; mover left edge would land at x=2 (2mm away) -> snap to 0.
    BoundingBoxf mover = box(0,0,10,10);
    std::vector<Neighbor> t = { { box(0,40,10,50), 1 } };
    SnapResult r = compute_snap(mover, Vec2d(2.0, 0.0), t, s, 1.0, st);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(0.0, 1e-6)); // pulled back to align left edges
    REQUIRE(r.engaged[0]);
}

TEST_CASE("right edges align across a gap", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.center_align = false; s.contact = false; s.spacing_propagation = false;
    SnapState st;
    // mover right edge starts at 10; target right edge at 13; raw delta +2 -> 12, within 5 of 13 -> snap.
    BoundingBoxf mover = box(0,0,10,10);
    std::vector<Neighbor> t = { { box(3,40,13,50), 1 } };
    SnapResult r = compute_snap(mover, Vec2d(2.0, 0.0), t, s, 1.0, st);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(3.0, 1e-6)); // right edge -> 13
    REQUIRE(r.engaged[0]);
}

TEST_CASE("edge beyond sensitivity does not snap", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.center_align = false; s.contact = false; s.spacing_propagation = false;
    SnapState st;
    BoundingBoxf mover = box(0,0,10,10);
    std::vector<Neighbor> t = { { box(0,40,10,50), 1 } };
    SnapResult r = compute_snap(mover, Vec2d(20.0, 0.0), t, s, 1.0, st);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(20.0, 1e-6));
    REQUIRE_FALSE(r.engaged[0]);
}

// ---- Task 3: center alignment ----------------------------------------------

TEST_CASE("centers align when centerlines are close", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.contact = false; s.spacing_propagation = false;
    SnapState st;
    // mover center x = 5; target (width 20) center x = 6; raw 0 -> snap center to 6 (correction +1)
    BoundingBoxf mover = box(0,0,10,10);
    std::vector<Neighbor> t = { { box(-4,40,16,50), 1 } }; // center x = 6
    SnapResult r = compute_snap(mover, Vec2d(0.0, 0.0), t, s, 1.0, st);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(1.0, 1e-6));
    REQUIRE(r.engaged[0]);
}

// ---- Task 4: contact snapping with perpendicular-overlap gating ------------

TEST_CASE("contact snaps edges to touch when overlapping perpendicular", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.spacing_propagation = false;
    SnapState st;
    // target occupies x[13..23], y[0..10]; mover x[0..10] y[0..10]; raw +2 -> right edge 12, target left 13 -> touch.
    BoundingBoxf mover = box(0,0,10,10);
    std::vector<Neighbor> t = { { box(13,0,23,10), 1 } };
    SnapResult r = compute_snap(mover, Vec2d(2.0, 0.0), t, s, 1.0, st);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(3.0, 1e-6)); // mover right edge -> 13
    REQUIRE(r.engaged[0]);
}

TEST_CASE("contact does NOT snap without perpendicular overlap", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.spacing_propagation = false;
    SnapState st;
    // same X proximity but target is far in Y (no vertical overlap) -> no contact snap.
    BoundingBoxf mover = box(0,0,10,10);
    std::vector<Neighbor> t = { { box(13,80,23,90), 1 } };
    SnapResult r = compute_snap(mover, Vec2d(2.0, 0.0), t, s, 1.0, st);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(2.0, 1e-6));
    REQUIRE_FALSE(r.engaged[0]);
}

// ---- Task 5: independent-axis resolution + disabled passthrough ------------

TEST_CASE("x and y snap independently in one call", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.center_align = false; s.contact = false; s.spacing_propagation = false;
    SnapState st;
    BoundingBoxf mover = box(0,0,10,10);
    // one neighbor aligns left edge in X (x0=0), another aligns bottom edge in Y (y0=0)
    std::vector<Neighbor> t = { { box(0,100,10,110), 1 }, { box(100,0,110,10), 2 } };
    SnapResult r = compute_snap(mover, Vec2d(2.0, 3.0), t, s, 1.0, st);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(r.corrected_delta.y(), WithinAbs(0.0, 1e-6));
    REQUIRE(r.engaged[0]);
    REQUIRE(r.engaged[1]);
}

TEST_CASE("disabled master returns raw delta", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = false;
    SnapState st;
    std::vector<Neighbor> t = { { box(0,0,10,10), 1 } };
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(1.0, 1.0), t, s, 1.0, st);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(r.corrected_delta.y(), WithinAbs(1.0, 1e-6));
}

// ---- Task 6: hysteresis (break-away strength) ------------------------------

TEST_CASE("stays engaged within break-away margin", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0; s.strength_px = 5.0;
    s.center_align = false; s.contact = false; s.spacing_propagation = false;
    SnapState st;
    BoundingBoxf mover = box(0,0,10,10);
    std::vector<Neighbor> t = { { box(0,40,10,50), 1 } }; // left edge target x=0
    // first frame: within sensitivity -> engage and snap to 0
    SnapResult r1 = compute_snap(mover, Vec2d(2.0,0.0), t, s, 1.0, st);
    REQUIRE(r1.engaged[0]);
    REQUIRE(st.engaged[0]);
    // second frame: raw delta 8 -> 8mm away (> sensitivity 5) but < sensitivity+strength (10) -> stays snapped
    SnapResult r2 = compute_snap(mover, Vec2d(8.0,0.0), t, s, 1.0, st);
    REQUIRE(r2.engaged[0]);
    REQUIRE_THAT(r2.corrected_delta.x(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("breaks away beyond margin", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0; s.strength_px = 5.0;
    s.center_align = false; s.contact = false; s.spacing_propagation = false;
    SnapState st;
    BoundingBoxf mover = box(0,0,10,10);
    std::vector<Neighbor> t = { { box(0,40,10,50), 1 } };
    compute_snap(mover, Vec2d(2.0,0.0), t, s, 1.0, st);       // engage
    SnapResult r = compute_snap(mover, Vec2d(12.0,0.0), t, s, 1.0, st); // 12 > 10 margin -> release
    REQUIRE_FALSE(r.engaged[0]);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(12.0, 1e-6));
    REQUIRE_FALSE(st.engaged[0]);
}

// ---- Task 7: guides (alignment lines + ghost boxes) ------------------------

TEST_CASE("emits an alignment line for an engaged axis", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.center_align = false; s.contact = false; s.spacing_propagation = false;
    SnapState st;
    std::vector<Neighbor> t = { { box(0,40,10,50), 1 } };
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(2.0,0.0), t, s, 1.0, st);
    REQUIRE(r.lines.size() >= 1);
    REQUIRE(static_cast<int>(r.lines[0].axis) == static_cast<int>(AlignmentSnap::Axis::X));
    REQUIRE_THAT(r.lines[0].coord, WithinAbs(0.0, 1e-6)); // the aligned x line
}

TEST_CASE("emits ghost boxes for nearby targets", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    SnapState st;
    // near target (within reveal radius, should ghost) and a far one (should not)
    std::vector<Neighbor> t = { { box(0,15,10,25), 1 }, { box(900,900,910,910), 2 } };
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(2.0,0.0), t, s, 1.0, st);
    REQUIRE(r.ghosts.size() == 1);
}

// ---- Task 8: spacing propagation (row extend) ------------------------------

TEST_CASE("row spacing propagates to the next slot", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.contact = false;
    s.spacing_propagation = true;
    SnapState st;
    // Two aligned boxes: centers at x=5 and x=25 (spacing 20), same y-band. Next slot center = 45.
    std::vector<Neighbor> t = { { box(0,0,10,10), 1 }, { box(20,0,30,10), 2 } };
    // mover width 10 -> center offset 5; start center 5, raw +38 -> center 43, snaps to 45.
    BoundingBoxf mover = box(0,0,10,10);
    SnapResult r = compute_snap(mover, Vec2d(38.0, 0.0), t, s, 1.0, st);
    REQUIRE(r.engaged[0]);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(40.0, 1e-6)); // center -> 45
    REQUIRE(r.badges.size() >= 1);
}

TEST_CASE("row badges persist while the snap is held (hysteresis)", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0; s.strength_px = 6.0;
    s.edge_align = false; s.center_align = false; s.contact = false;
    s.spacing_propagation = true;
    SnapState st;
    std::vector<Neighbor> t = { { box(0,0,10,10), 1 }, { box(20,0,30,10), 2 } };
    // frame 1: center 43 -> snaps to slot 45, badges emitted
    SnapResult r1 = compute_snap(box(0,0,10,10), Vec2d(38.0, 0.0), t, s, 1.0, st);
    REQUIRE(r1.engaged[0]);
    REQUIRE(r1.badges.size() >= 1);
    // frame 2: center drifts to 46, still within break-away -> badges MUST still be present
    SnapResult r2 = compute_snap(box(0,0,10,10), Vec2d(41.0, 0.0), t, s, 1.0, st);
    REQUIRE(r2.engaged[0]);
    REQUIRE(r2.badges.size() >= 1);
}

TEST_CASE("row propagation requires perpendicular alignment", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.contact = false;
    s.spacing_propagation = true;
    SnapState st;
    // Same spacing but staggered in Y (not a row) -> no propagation.
    std::vector<Neighbor> t = { { box(0,0,10,10), 1 }, { box(20,50,30,60), 2 } };
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(38.0, 0.0), t, s, 1.0, st);
    REQUIRE_FALSE(r.engaged[0]);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(38.0, 1e-6));
}

// ---- Review fixes -----------------------------------------------------------

TEST_CASE("held center snap keeps the center, not the nearest edge", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0; s.strength_px = 6.0;
    s.edge_align = false; s.contact = false; s.spacing_propagation = false;
    SnapState st;
    // Narrow mover (width 2, center 1); target center at x=50.
    std::vector<Neighbor> t = { { box(45,40,55,50), 1 } };
    SnapResult r1 = compute_snap(box(0,0,2,2), Vec2d(48.0, 0.0), t, s, 1.0, st); // center 49 -> 50
    REQUIRE(r1.engaged[0]);
    REQUIRE_THAT(r1.corrected_delta.x(), WithinAbs(49.0, 1e-6));
    // Drift to center 53: the min edge (52) is now closer to 50, but the center must stay locked.
    SnapResult r2 = compute_snap(box(0,0,2,2), Vec2d(52.0, 0.0), t, s, 1.0, st);
    REQUIRE(r2.engaged[0]);
    REQUIRE_THAT(r2.corrected_delta.x(), WithinAbs(49.0, 1e-6));
}

TEST_CASE("row spacing propagates before the first object too", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.contact = false;
    SnapState st;
    // Row centers at 45 and 65 (spacing 20): the slot before the row is center 25.
    std::vector<Neighbor> t = { { box(40,0,50,10), 1 }, { box(60,0,70,10), 2 } };
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(22.0, 0.0), t, s, 1.0, st); // center 27
    REQUIRE(r.engaged[0]);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(20.0, 1e-6)); // center -> 25
    REQUIRE(r.badges.size() == 2);
}

TEST_CASE("row propagation ignores a row the mover is not in", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.contact = false;
    SnapState st;
    // The row sits at y 0..10; the mover is dragged far away at y 100..110.
    std::vector<Neighbor> t = { { box(0,0,10,10), 1 }, { box(20,0,30,10), 2 } };
    SnapResult r = compute_snap(box(0,100,10,110), Vec2d(38.0, 0.0), t, s, 1.0, st);
    REQUIRE_FALSE(r.engaged[0]);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(38.0, 1e-6));
}

TEST_CASE("row propagation skips a slot that is already occupied", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.contact = false;
    SnapState st;
    // Three evenly spaced objects (centers 5, 25, 45): the first pair's next slot (45) is taken.
    std::vector<Neighbor> t = { { box(0,0,10,10), 1 }, { box(20,0,30,10), 2 }, { box(40,0,50,10), 3 } };
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(41.0, 0.0), t, s, 1.0, st); // center 46
    REQUIRE_FALSE(r.engaged[0]);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(41.0, 1e-6));
}

// ---- Fixed gap -------------------------------------------------------------

TEST_CASE("fixed gap snaps facing edges the set distance apart", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.contact = false; s.spacing_propagation = false;
    s.gap_snap = true; s.gap_mm = 5.0;
    SnapState st;
    // Target x[30..40]; mover x[0..10] dragged +17 -> right edge 27, gap slot at 25 -> correction -2.
    std::vector<Neighbor> t = { { box(30,0,40,10), 1 } };
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(17.0, 0.0), t, s, 1.0, st);
    REQUIRE(r.engaged[0]);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(15.0, 1e-6)); // right edge -> 25
    REQUIRE(r.badges.size() == 1);
    REQUIRE_THAT(r.badges[0].value_mm, WithinAbs(5.0, 1e-6));
}

TEST_CASE("fixed gap works on the far side of the target", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.contact = false; s.spacing_propagation = false;
    s.gap_snap = true; s.gap_mm = 3.0;
    SnapState st;
    // Target x[0..10]; mover x[0..10] dragged +14 -> left edge 14, gap slot at 13 -> correction -1.
    std::vector<Neighbor> t = { { box(0,0,10,10), 1 } };
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(14.0, 0.0), t, s, 1.0, st);
    REQUIRE(r.engaged[0]);
    REQUIRE_THAT(r.corrected_delta.x(), WithinAbs(13.0, 1e-6));
}

TEST_CASE("fixed gap needs the objects to face each other", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.contact = false; s.spacing_propagation = false;
    s.gap_snap = true; s.gap_mm = 5.0;
    SnapState st;
    std::vector<Neighbor> t = { { box(30,80,40,90), 1 } }; // far away in Y
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(17.0, 0.0), t, s, 1.0, st);
    REQUIRE_FALSE(r.engaged[0]);
}

TEST_CASE("fixed gap is off by default", "[AlignmentSnap]") {
    SnapSettings s; s.enabled = true; s.sensitivity_px = 5.0;
    s.edge_align = false; s.center_align = false; s.contact = false; s.spacing_propagation = false;
    SnapState st;
    std::vector<Neighbor> t = { { box(30,0,40,10), 1 } };
    SnapResult r = compute_snap(box(0,0,10,10), Vec2d(17.0, 0.0), t, s, 1.0, st);
    REQUIRE_FALSE(r.engaged[0]);
}
