#include "libslic3r/AlignmentSnap.hpp"
#include <cmath>
#include <algorithm>

namespace Slic3r { namespace AlignmentSnap {

namespace {
    enum MoverKey { KeyLo = 0, KeyHi = 1, KeyCenter = 2 };
    struct Cand { double correction; double dist_px; };
    inline double lo (const BoundingBoxf& b, int a){ return a == 0 ? b.min.x() : b.min.y(); }
    inline double hi (const BoundingBoxf& b, int a){ return a == 0 ? b.max.x() : b.max.y(); }
    inline double ctr(const BoundingBoxf& b, int a){ return a == 0 ? b.center().x() : b.center().y(); }

    // Do the two boxes overlap along the axis perpendicular to `axis`?
    inline bool overlaps_perp(const BoundingBoxf& m, const BoundingBoxf& t, int axis) {
        int p = axis ^ 1;
        return lo(m, p) < hi(t, p) && lo(t, p) < hi(m, p);
    }

    // Are two boxes aligned along the axis perpendicular to `axis` (share an edge or center)?
    inline bool perp_aligned(const BoundingBoxf& x, const BoundingBoxf& y, int axis, double tol) {
        int p = axis ^ 1;
        return std::abs(lo(x, p)  - lo(y, p))  <= tol
            || std::abs(ctr(x, p) - ctr(y, p)) <= tol
            || std::abs(hi(x, p)  - hi(y, p))  <= tol;
    }
}

SnapResult compute_snap(const BoundingBoxf& mover_start,
                        const Vec2d& raw_delta,
                        const std::vector<Neighbor>& targets,
                        const SnapSettings& s,
                        double px_per_mm,
                        SnapState& state)
{
    SnapResult r;
    r.corrected_delta = raw_delta;
    if (!s.enabled || targets.empty() || px_per_mm <= 0.0)
        return r;

    const double thresh_mm = s.sensitivity_px / px_per_mm;

    // Mover box at the raw (unsnapped) position.
    BoundingBoxf mover = mover_start;
    mover.min += raw_delta;
    mover.max += raw_delta;

    // X and Y are resolved independently.
    for (int a = 0; a < 2; ++a) {
        const double mover_lo = lo(mover, a);
        const double mover_hi = hi(mover, a);
        const double mover_c  = ctr(mover, a);

        // If this axis was already engaged, honour the wider break-away window first (hysteresis).
        const double keys[3] = { mover_lo, mover_hi, mover_c };
        if (state.engaged[a]) {
            // Stay snapped on the same mover key (edge or center) that engaged, so a narrow
            // object can't hop from its center to an edge while the snap is held.
            const double bestc = state.coord[a] - keys[state.key[a]];
            if (std::abs(bestc) * px_per_mm <= s.sensitivity_px + s.strength_px) {
                if (a == 0) r.corrected_delta.x() += bestc; else r.corrected_delta.y() += bestc;
                r.engaged[a] = true;
                continue;
            }
            state.engaged[a]      = false; // drifted past the margin -> fresh search
            state.from_spacing[a] = false;
            state.badge_cache[a].clear();
        }

        Cand   best { 0.0, s.sensitivity_px + 1.0 };
        bool   found = false;
        double engaged_line = 0.0;
        int    engaged_key  = KeyLo;
        bool   gap_won      = false;   // the winning candidate is a fixed-gap snap
        SpacingBadge gap_badge {};
        for (const Neighbor& n : targets) {
            auto consider = [&](int mover_key, double target_key) {
                const double correction = target_key - keys[mover_key]; // mm to add on this axis
                const double dpx        = std::abs(correction) * px_per_mm;
                if (dpx <= s.sensitivity_px && dpx < best.dist_px) {
                    best = { correction, dpx };
                    found = true;
                    engaged_line = target_key;
                    engaged_key  = mover_key;
                    gap_won      = false;
                    return true;
                }
                return false;
            };
            if (s.edge_align) {
                consider(KeyLo, lo(n.bbox, a)); // min <-> min
                consider(KeyHi, hi(n.bbox, a)); // max <-> max
            }
            if (s.center_align) {
                consider(KeyCenter, ctr(n.bbox, a)); // center <-> center
            }
            if (s.contact && overlaps_perp(mover, n.bbox, a)) {
                consider(KeyHi, lo(n.bbox, a)); // mover max touches target min
                consider(KeyLo, hi(n.bbox, a)); // mover min touches target max
            }
            if (s.gap_snap && s.gap_mm > 0.0 && overlaps_perp(mover, n.bbox, a)) {
                // Badge spans the gap, at the middle of the edges' shared perpendicular range.
                const int    p    = a ^ 1;
                const double perp = 0.5 * (std::max(lo(mover, p), lo(n.bbox, p)) + std::min(hi(mover, p), hi(n.bbox, p)));
                auto pt = [&](double along) { return a == 0 ? Vec2d(along, perp) : Vec2d(perp, along); };
                const double before = lo(n.bbox, a) - s.gap_mm; // mover sits before the target
                const double after  = hi(n.bbox, a) + s.gap_mm; // mover sits after the target
                if (consider(KeyHi, before)) { gap_won = true; gap_badge = { pt(before), pt(lo(n.bbox, a)), s.gap_mm }; }
                if (consider(KeyLo, after))  { gap_won = true; gap_badge = { pt(hi(n.bbox, a)), pt(after), s.gap_mm }; }
            }
        }
        if (found) {
            if (a == 0) r.corrected_delta.x() += best.correction; else r.corrected_delta.y() += best.correction;
            r.engaged[a]          = true;
            state.engaged[a]      = true;
            state.coord[a]        = engaged_line;
            state.key[a]          = engaged_key;
            // Only a fixed-gap snap shows a badge; edge/center/contact snaps don't.
            state.from_spacing[a] = gap_won;
            state.badge_cache[a].clear();
            if (gap_won)
                state.badge_cache[a].push_back(gap_badge);
        }
    }

    // Spacing propagation: continue an existing evenly spaced, aligned row at either end.
    if (s.spacing_propagation) {
        std::vector<const Neighbor*> row;
        row.reserve(targets.size());
        for (const Neighbor& n : targets) row.push_back(&n);

        for (int a = 0; a < 2; ++a) {
            if (r.engaged[a]) continue; // don't override a hard alignment

            // Mover at its current position (the other axis may already be snapped this frame).
            BoundingBoxf cur = mover_start;
            cur.min += r.corrected_delta;
            cur.max += r.corrected_delta;
            const double half    = 0.5 * (hi(cur, a) - lo(cur, a));
            const double mover_c = ctr(cur, a);

            // A slot is usable only if the mover placed there would not overlap another object.
            auto slot_free = [&](double slot) {
                for (const Neighbor& n : targets)
                    if (overlaps_perp(cur, n.bbox, a) && lo(n.bbox, a) < slot + half - EPSILON && slot - half + EPSILON < hi(n.bbox, a))
                        return false;
                return true;
            };

            std::sort(row.begin(), row.end(), [&](const Neighbor* p, const Neighbor* q) {
                return ctr(p->bbox, a) < ctr(q->bbox, a);
            });

            double best_dpx  = s.sensitivity_px + 1.0;
            double best_slot = 0.0;
            std::vector<SpacingBadge> best_badges;
            for (size_t i = 0; i + 1 < row.size(); ++i) {
                const BoundingBoxf& b0 = row[i]->bbox;
                const BoundingBoxf& b1 = row[i + 1]->bbox;
                if (!perp_aligned(b0, b1, a, thresh_mm)) continue;
                // The mover must sit in the row's band, not in some other row far away.
                if (!overlaps_perp(cur, b0, a) && !overlaps_perp(cur, b1, a)) continue;
                const double c0 = ctr(b0, a), c1 = ctr(b1, a);
                const double spacing = c1 - c0;
                if (spacing <= thresh_mm) continue;

                const double perp = ctr(b0, a ^ 1);
                auto pt = [&](double along) { return a == 0 ? Vec2d(along, perp) : Vec2d(perp, along); };
                const double slots[2] = { c1 + spacing, c0 - spacing }; // after the pair, before it
                for (int k = 0; k < 2; ++k) {
                    const double dpx = std::abs(slots[k] - mover_c) * px_per_mm;
                    if (dpx > s.sensitivity_px || dpx >= best_dpx || !slot_free(slots[k])) continue;
                    best_dpx  = dpx;
                    best_slot = slots[k];
                    // Badges show the existing gap, then the new one.
                    if (k == 0)
                        best_badges = { SpacingBadge{ pt(c0), pt(c1), spacing }, SpacingBadge{ pt(c1), pt(slots[k]), spacing } };
                    else
                        best_badges = { SpacingBadge{ pt(slots[k]), pt(c0), spacing }, SpacingBadge{ pt(c0), pt(c1), spacing } };
                }
            }

            if (best_dpx <= s.sensitivity_px) {
                const double correction = best_slot - mover_c;
                if (a == 0) r.corrected_delta.x() += correction; else r.corrected_delta.y() += correction;
                r.engaged[a]          = true;
                state.engaged[a]      = true;
                state.coord[a]        = best_slot;
                state.key[a]          = KeyCenter;
                state.from_spacing[a] = true;
                // Cached so the badges persist every frame the row snap is held.
                state.badge_cache[a]  = std::move(best_badges);
            }
        }
    }

    // Guides: ghost boxes for nearby neighbors + alignment lines for engaged axes.
    BoundingBoxf mv = mover_start;
    mv.min += r.corrected_delta;
    mv.max += r.corrected_delta;

    const double reveal_mm = 3.0 * thresh_mm;
    for (const Neighbor& n : targets) {
        const double dx = std::max({ 0.0, n.bbox.min.x() - mv.max.x(), mv.min.x() - n.bbox.max.x() });
        const double dy = std::max({ 0.0, n.bbox.min.y() - mv.max.y(), mv.min.y() - n.bbox.max.y() });
        if (std::sqrt(dx * dx + dy * dy) <= reveal_mm)
            r.ghosts.push_back(GhostBox{ n.bbox });
    }

    for (int a = 0; a < 2; ++a) {
        if (!r.engaged[a]) continue;
        const int p = a ^ 1;
        double span_lo = (p == 0 ? mv.min.x() : mv.min.y());
        double span_hi = (p == 0 ? mv.max.x() : mv.max.y());
        for (const Neighbor& n : targets) {
            span_lo = std::min(span_lo, (p == 0 ? n.bbox.min.x() : n.bbox.min.y()));
            span_hi = std::max(span_hi, (p == 0 ? n.bbox.max.x() : n.bbox.max.y()));
        }
        r.lines.push_back(GuideLine{ a == 0 ? Axis::X : Axis::Y, state.coord[a], span_lo, span_hi });

        // Row-spacing badges persist from the cache for as long as the row snap is held.
        if (state.from_spacing[a])
            r.badges.insert(r.badges.end(), state.badge_cache[a].begin(), state.badge_cache[a].end());
    }

    return r;
}

TargetSet list_targets(const BoundingBoxf& mover,
                       const std::vector<Neighbor>& targets,
                       const SnapSettings& s,
                       size_t max_neighbors)
{
    TargetSet out;
    if (!s.enabled || targets.empty())
        return out;

    // Nearest neighbors by footprint distance.
    auto dist2 = [&](const BoundingBoxf& b) {
        const double dx = std::max({ 0.0, lo(b, 0) - hi(mover, 0), lo(mover, 0) - hi(b, 0) });
        const double dy = std::max({ 0.0, lo(b, 1) - hi(mover, 1), lo(mover, 1) - hi(b, 1) });
        return dx * dx + dy * dy;
    };
    std::vector<const Neighbor*> near;
    for (const Neighbor& n : targets) near.push_back(&n);
    std::sort(near.begin(), near.end(), [&](const Neighbor* p, const Neighbor* q) { return dist2(p->bbox) < dist2(q->bbox); });
    if (near.size() > max_neighbors)
        near.resize(max_neighbors);
    auto is_near = [&](const Neighbor* n) { return std::find(near.begin(), near.end(), n) != near.end(); };

    // Rails: one per distinct line, spanning the neighbors that share it and the object.
    auto add_rail = [&](int a, double coord, const BoundingBoxf& n) {
        const int    p  = a ^ 1;
        const double s0 = std::min(lo(n, p), lo(mover, p));
        const double s1 = std::max(hi(n, p), hi(mover, p));
        for (GuideLine& g : out.rails)
            if (static_cast<int>(g.axis) == a && std::abs(g.coord - coord) < EPSILON) {
                g.span_lo = std::min(g.span_lo, s0);
                g.span_hi = std::max(g.span_hi, s1);
                return;
            }
        out.rails.push_back(GuideLine{ a == 0 ? Axis::X : Axis::Y, coord, s0, s1 });
    };
    for (const Neighbor* n : near)
        for (int a = 0; a < 2; ++a) {
            if (s.edge_align) { add_rail(a, lo(n->bbox, a), n->bbox); add_rail(a, hi(n->bbox, a), n->bbox); }
            if (s.center_align) add_rail(a, ctr(n->bbox, a), n->bbox);
        }

    // Spots: the object's footprint centered at (cx, cy), kept only if it is a real move
    // and doesn't overlap another object (touching is fine).
    const Vec2d size = mover.size();
    auto add_spot = [&](double cx, double cy, SpotKind kind, int id) {
        const BoundingBoxf f(Vec2d(cx - 0.5 * size.x(), cy - 0.5 * size.y()), Vec2d(cx + 0.5 * size.x(), cy + 0.5 * size.y()));
        if ((f.min - mover.min).norm() < EPSILON)
            return;
        for (const Neighbor& t : targets)
            if (lo(f, 0) < hi(t.bbox, 0) - EPSILON && lo(t.bbox, 0) < hi(f, 0) - EPSILON &&
                lo(f, 1) < hi(t.bbox, 1) - EPSILON && lo(t.bbox, 1) < hi(f, 1) - EPSILON)
                return;
        for (const TargetSpot& sp : out.spots)
            if ((sp.footprint.min - f.min).norm() < EPSILON)
                return;
        out.spots.push_back(TargetSpot{ f, kind, id });
    };
    // Place along axis a at `along`, centered on `perp` across it.
    auto add_at = [&](int a, double along, double perp, SpotKind kind, int id) {
        if (a == 0) add_spot(along, perp, kind, id); else add_spot(perp, along, kind, id);
    };

    for (const Neighbor* n : near)
        for (int a = 0; a < 2; ++a) {
            const double half = 0.5 * (a == 0 ? size.x() : size.y());
            const double perp = ctr(n->bbox, a ^ 1);
            if (s.contact) {
                add_at(a, lo(n->bbox, a) - half, perp, SpotKind::Contact, n->id);
                add_at(a, hi(n->bbox, a) + half, perp, SpotKind::Contact, n->id);
            }
            if (s.gap_snap && s.gap_mm > 0.0) {
                add_at(a, lo(n->bbox, a) - s.gap_mm - half, perp, SpotKind::Gap, n->id);
                add_at(a, hi(n->bbox, a) + s.gap_mm + half, perp, SpotKind::Gap, n->id);
            }
        }

    // Row slots at either end of each evenly spaced pair that includes a near neighbor.
    if (s.spacing_propagation) {
        const double tol = 0.5; // mm: how closely a pair must line up to count as a row
        std::vector<const Neighbor*> row;
        for (const Neighbor& n : targets) row.push_back(&n);
        for (int a = 0; a < 2; ++a) {
            std::sort(row.begin(), row.end(), [&](const Neighbor* p, const Neighbor* q) { return ctr(p->bbox, a) < ctr(q->bbox, a); });
            for (size_t i = 0; i + 1 < row.size(); ++i) {
                const Neighbor* n0 = row[i];
                const Neighbor* n1 = row[i + 1];
                if (!is_near(n0) && !is_near(n1)) continue;
                if (!perp_aligned(n0->bbox, n1->bbox, a, tol)) continue;
                const double c0 = ctr(n0->bbox, a), c1 = ctr(n1->bbox, a);
                const double spacing = c1 - c0;
                if (spacing <= tol) continue;
                const double perp = ctr(n0->bbox, a ^ 1);
                add_at(a, c1 + spacing, perp, SpotKind::Row, n1->id);
                add_at(a, c0 - spacing, perp, SpotKind::Row, n0->id);
            }
        }
    }
    return out;
}

}} // namespace Slic3r::AlignmentSnap
