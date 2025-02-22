#include "SeamPlacer.hpp"

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/EdgeGrid.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/SVG.hpp"
#include "libslic3r/Layer.hpp"

namespace Slic3r {

// This penalty is added to all points inside custom blockers (subtracted from pts inside enforcers).
static constexpr float ENFORCER_BLOCKER_PENALTY = 100;

// In case there are custom enforcers/blockers, the loop polygon shall always have
// sides smaller than this (so it isn't limited to original resolution).
static constexpr float MINIMAL_POLYGON_SIDE = scale_(0.2f);

// When spAligned is active and there is a support enforcer,
// add this penalty to its center.
static constexpr float ENFORCER_CENTER_PENALTY = -10.f;




static float extrudate_overlap_penalty(float nozzle_r, float weight_zero, float overlap_distance)
{
    // The extrudate is not fully supported by the lower layer. Fit a polynomial penalty curve.
    // Solved by sympy package:
/*
from sympy import *
(x,a,b,c,d,r,z)=symbols('x a b c d r z')
p = a + b*x + c*x*x + d*x*x*x
p2 = p.subs(solve([p.subs(x, -r), p.diff(x).subs(x, -r), p.diff(x,x).subs(x, -r), p.subs(x, 0)-z], [a, b, c, d]))
from sympy.plotting import plot
plot(p2.subs(r,0.2).subs(z,1.), (x, -1, 3), adaptive=False, nb_of_points=400)
*/
    if (overlap_distance < - nozzle_r) {
        // The extrudate is fully supported by the lower layer. This is the ideal case, therefore zero penalty.
        return 0.f;
    } else {
        float x  = overlap_distance / nozzle_r;
        float x2 = x * x;
        float x3 = x2 * x;
        return weight_zero * (1.f + 3.f * x + 3.f * x2 + x3);
    }
}



// Return a value in <0, 1> of a cubic B-spline kernel centered around zero.
// The B-spline is re-scaled so it has value 1 at zero.
// 0 -> 1 ; ~0.465 -> 0.75 ; ~0.72 -> 0.5 ; 1 -> 0.25 ; ~1.23 -> 0.125 ; 2+ -> 0
static inline float bspline_kernel(float x)
{
    x = std::abs(x);
    if (x < 1.f) {
        return 1.f - (3.f / 2.f) * x * x + (3.f / 4.f) * x * x * x;
    }
    else if (x < 2.f) {
        x -= 1.f;
        float x2 = x * x;
        float x3 = x2 * x;
        return (1.f / 4.f) - (3.f / 4.f) * x + (3.f / 4.f) * x2 - (1.f / 4.f) * x3;
    }
    else
        return 0;
}



static Points::const_iterator project_point_to_polygon_and_insert(Polygon &polygon, const Point &pt, double eps)
{
    assert(polygon.points.size() >= 2);
    if (polygon.points.size() <= 1)
    if (polygon.points.size() == 1)
        return polygon.points.begin();

    Point  pt_min;
    double d_min = std::numeric_limits<double>::max();
    size_t i_min = size_t(-1);

    for (size_t i = 0; i < polygon.points.size(); ++ i) {
        size_t j = i + 1;
        if (j == polygon.points.size())
            j = 0;
        const Point &p1 = polygon.points[i];
        const Point &p2 = polygon.points[j];
        const Slic3r::Point v_seg = p2 - p1;
        const Slic3r::Point v_pt  = pt - p1;
        const int64_t l2_seg = int64_t(v_seg(0)) * int64_t(v_seg(0)) + int64_t(v_seg(1)) * int64_t(v_seg(1));
        int64_t t_pt = int64_t(v_seg(0)) * int64_t(v_pt(0)) + int64_t(v_seg(1)) * int64_t(v_pt(1));
        if (t_pt < 0) {
            // Closest to p1.
            double dabs = sqrt(int64_t(v_pt(0)) * int64_t(v_pt(0)) + int64_t(v_pt(1)) * int64_t(v_pt(1)));
            if (dabs < d_min) {
                d_min  = dabs;
                i_min  = i;
                pt_min = p1;
            }
        }
        else if (t_pt > l2_seg) {
            // Closest to p2. Then p2 is the starting point of another segment, which shall be discovered in the next step.
            continue;
        } else {
            // Closest to the segment.
            assert(t_pt >= 0 && t_pt <= l2_seg);
            int64_t d_seg = int64_t(v_seg(1)) * int64_t(v_pt(0)) - int64_t(v_seg(0)) * int64_t(v_pt(1));
            double d = double(d_seg) / sqrt(double(l2_seg));
            double dabs = std::abs(d);
            if (dabs < d_min) {
                d_min  = dabs;
                i_min  = i;
                // Evaluate the foot point.
                pt_min = p1;
                double linv = double(d_seg) / double(l2_seg);
                pt_min(0) = pt(0) - coord_t(floor(double(v_seg(1)) * linv + 0.5));
                pt_min(1) = pt(1) + coord_t(floor(double(v_seg(0)) * linv + 0.5));
                assert(Line(p1, p2).distance_to(pt_min) < scale_(1e-5));
            }
        }
    }

    assert(i_min != size_t(-1));
    if ((pt_min - polygon.points[i_min]).cast<double>().norm() > eps) {
        // Insert a new point on the segment i_min, i_min+1.
        return polygon.points.insert(polygon.points.begin() + (i_min + 1), pt_min);
    }
    return polygon.points.begin() + i_min;
}



static std::vector<float> polygon_angles_at_vertices(const Polygon &polygon, const std::vector<float> &lengths, float min_arm_length)
{
    assert(polygon.points.size() + 1 == lengths.size());
    if (min_arm_length > 0.25f * lengths.back())
        min_arm_length = 0.25f * lengths.back();

    // Find the initial prev / next point span.
    size_t idx_prev = polygon.points.size();
    size_t idx_curr = 0;
    size_t idx_next = 1;
    while (idx_prev > idx_curr && lengths.back() - lengths[idx_prev] < min_arm_length)
        -- idx_prev;
    while (idx_next < idx_prev && lengths[idx_next] < min_arm_length)
        ++ idx_next;

    std::vector<float> angles(polygon.points.size(), 0.f);
    for (; idx_curr < polygon.points.size(); ++ idx_curr) {
        // Move idx_prev up until the distance between idx_prev and idx_curr is lower than min_arm_length.
        if (idx_prev >= idx_curr) {
            while (idx_prev < polygon.points.size() && lengths.back() - lengths[idx_prev] + lengths[idx_curr] > min_arm_length)
                ++ idx_prev;
            if (idx_prev == polygon.points.size())
                idx_prev = 0;
        }
        while (idx_prev < idx_curr && lengths[idx_curr] - lengths[idx_prev] > min_arm_length)
            ++ idx_prev;
        // Move idx_prev one step back.
        if (idx_prev == 0)
            idx_prev = polygon.points.size() - 1;
        else
            -- idx_prev;
        // Move idx_next up until the distance between idx_curr and idx_next is greater than min_arm_length.
        if (idx_curr <= idx_next) {
            while (idx_next < polygon.points.size() && lengths[idx_next] - lengths[idx_curr] < min_arm_length)
                ++ idx_next;
            if (idx_next == polygon.points.size())
                idx_next = 0;
        }
        while (idx_next < idx_curr && lengths.back() - lengths[idx_curr] + lengths[idx_next] < min_arm_length)
            ++ idx_next;
        // Calculate angle between idx_prev, idx_curr, idx_next.
        const Point &p0 = polygon.points[idx_prev];
        const Point &p1 = polygon.points[idx_curr];
        const Point &p2 = polygon.points[idx_next];
        const Point  v1 = p1 - p0;
        const Point  v2 = p2 - p1;
        int64_t dot   = int64_t(v1(0))*int64_t(v2(0)) + int64_t(v1(1))*int64_t(v2(1));
        int64_t cross = int64_t(v1(0))*int64_t(v2(1)) - int64_t(v1(1))*int64_t(v2(0));
        float angle = float(atan2(double(cross), double(dot)));
        angles[idx_curr] = angle;
    }

    return angles;
}



void SeamPlacer::init(const Print& print)
{
    m_enforcers.clear();
    m_blockers.clear();
    m_seam_history.clear();
    m_po_list.clear();

   const std::vector<double>& nozzle_dmrs = print.config().nozzle_diameter.values;
   float max_nozzle_dmr = *std::max_element(nozzle_dmrs.begin(), nozzle_dmrs.end());


    std::vector<ExPolygons> temp_enf;
    std::vector<ExPolygons> temp_blk;

    for (const PrintObject* po : print.objects()) {
        temp_enf.clear();
        temp_blk.clear();
        po->project_and_append_custom_facets(true, EnforcerBlockerType::ENFORCER, temp_enf);
        po->project_and_append_custom_facets(true, EnforcerBlockerType::BLOCKER, temp_blk);

        // Offset the triangles out slightly.
        for (auto* custom_per_object : {&temp_enf, &temp_blk}) {
            float offset = max_nozzle_dmr - po->config().first_layer_size_compensation;
            for (ExPolygons& explgs : *custom_per_object) {
                explgs = Slic3r::offset_ex(explgs, scale_(offset));
                offset = max_nozzle_dmr;
            }
        }

//     FIXME: Offsetting should be done somehow cheaper, but following does not work
//        for (auto* custom_per_object : {&temp_enf, &temp_blk}) {
//            for (ExPolygons& plgs : *custom_per_object) {
//                for (ExPolygon& plg : plgs) {
//                    auto out = Slic3r::offset_ex(plg, scale_(max_nozzle_dmr));
//                    plg = out.empty() ? ExPolygon() : out.front();
//                    assert(out.empty() || out.size() == 1);
//                }
//            }
//        }



        // Remember this PrintObject and initialize a store of enforcers and blockers for it.
        m_po_list.push_back(po);
        size_t po_idx = m_po_list.size() - 1;
        m_enforcers.emplace_back(std::vector<CustomTrianglesPerLayer>(temp_enf.size()));
        m_blockers.emplace_back(std::vector<CustomTrianglesPerLayer>(temp_blk.size()));

        // A helper class to store data to build the AABB tree from.
        class CustomTriangleRef {
        public:
            CustomTriangleRef(size_t idx,
                              Point&& centroid,
                              BoundingBox&& bb)
                : m_idx{idx}, m_centroid{centroid},
                  m_bbox{AlignedBoxType(bb.min, bb.max)}
            {}
            size_t idx() const              { return m_idx;      }
            const Point& centroid() const   { return m_centroid; }
            const TreeType::BoundingBox& bbox() const { return m_bbox; }

        private:
            size_t m_idx;
            Point m_centroid;
            AlignedBoxType m_bbox;
        };

        // A lambda to extract the ExPolygons and save them into the member AABB tree.
        // Will be called for enforcers and blockers separately.
        auto add_custom = [](std::vector<ExPolygons>& src, std::vector<CustomTrianglesPerLayer>& dest) {
            // Go layer by layer, and append all the ExPolygons into the AABB tree.
            size_t layer_idx = 0;
            for (ExPolygons& expolys_on_layer : src) {
                CustomTrianglesPerLayer& layer_data = dest[layer_idx];
                std::vector<CustomTriangleRef> triangles_data;
                layer_data.polys.reserve(expolys_on_layer.size());
                triangles_data.reserve(expolys_on_layer.size());

                for (ExPolygon& expoly : expolys_on_layer) {
                    if (expoly.empty())
                        continue;
                    layer_data.polys.emplace_back(std::move(expoly));
                    triangles_data.emplace_back(layer_data.polys.size() - 1,
                                                layer_data.polys.back().centroid(),
                                                layer_data.polys.back().bounding_box());
                }
                // All polygons are saved, build the AABB tree for them.
                layer_data.tree.build(std::move(triangles_data));
                ++layer_idx;
            }
        };

        add_custom(temp_enf, m_enforcers.at(po_idx));
        add_custom(temp_blk, m_blockers.at(po_idx));
    }

    this->external_perimeters_first = print.default_region_config().external_perimeters_first;
}



Point SeamPlacer::get_seam(const Layer& layer, SeamPosition seam_position,
               const ExtrusionLoop& loop, Point last_pos, coordf_t nozzle_dmr,
               const PrintObject* po, const uint16_t print_object_instance_idx,
               bool was_clockwise, const EdgeGrid::Grid* lower_layer_edge_grid)
{
    Polygon polygon = loop.polygon();
    BoundingBox polygon_bb = polygon.bounding_box();
    const coord_t  nozzle_r   = coord_t(scale_(0.5 * nozzle_dmr) + 0.5);
    float last_pos_weight = 1.f;
    float angle_weight = 1.f;

    size_t po_idx = std::find(m_po_list.begin(), m_po_list.end(), po) - m_po_list.begin();

    // Find current layer in respective PrintObject. Cache the result so the
    // lookup is only done once per layer, not for each loop.
    const Layer* layer_po = nullptr;
    if (po == m_last_po && layer.print_z == m_last_print_z)
        layer_po = m_last_layer_po;
    else {
        layer_po = po->get_layer_at_printz(layer.print_z);
        m_last_po = po;
        m_last_print_z = layer.print_z;
        m_last_layer_po = layer_po;
    }
    if (! layer_po)
        return last_pos;

    // Index of this layer in the respective PrintObject.
    size_t layer_idx = layer_po->id() - po->layers().front()->id(); // raft layers

    assert(layer_idx < po->layer_count());

    if (this->is_custom_seam_on_layer(layer_idx, po_idx)) {
        // Seam enf/blockers can begin and end in between the original vertices.
        // Let add extra points in between and update the leghths.
        polygon.densify(MINIMAL_POLYGON_SIDE);
    }

    bool has_seam_custom = false;
    if(print_object_instance_idx < po->instances().size())
        for (ModelVolume* v : po->model_object()->volumes)
            if (v->is_seam_position()) {
                has_seam_custom = true;
                break;
            }
    if (has_seam_custom) {
        // Look for all lambda-seam-modifiers below current z, choose the highest one
        ModelVolume* v_lambda_seam = nullptr;
        Vec3d lambda_pos;
        double lambda_dist;
        double lambda_radius;
        //get model_instance (like from po->model_object()->instances, but we don't have the index for that array)
        const ModelInstance* model_instance = po->instances()[print_object_instance_idx].model_instance;
        for (ModelVolume* v : po->model_object()->volumes) {
            if (v->is_seam_position()) {
                //xy in object coordinates, z in plater coordinates
                Vec3d test_lambda_pos = model_instance->transform_vector(v->get_offset(), true);

                Point xy_lambda(scale_(test_lambda_pos.x()), scale_(test_lambda_pos.y()));
                Point nearest = polygon.point_projection(xy_lambda);
                Vec3d polygon_3dpoint{ unscaled(nearest.x()), unscaled(nearest.y()), (double)layer.print_z };
                double test_lambda_dist = (polygon_3dpoint - test_lambda_pos).norm();
                double sphere_radius = po->model_object()->instance_bounding_box(0, true).size().x() / 2;
                //if (test_lambda_dist > sphere_radius)
                //    continue;

                //use this one if the first or nearer (in z)
                if (v_lambda_seam == nullptr || lambda_dist > test_lambda_dist) {
                    v_lambda_seam = v;
                    lambda_pos = test_lambda_pos;
                    lambda_radius = sphere_radius;
                    lambda_dist = test_lambda_dist;
                }
            }
        }

        if (v_lambda_seam != nullptr) {
            lambda_pos = model_instance->transform_vector(v_lambda_seam->get_offset(), true);
            // Found, get the center point and apply rotation and scaling of Model instance. Continues to spAligned if not found or Weight set to Zero.
            last_pos = Point::new_scale(lambda_pos.x(), lambda_pos.y());
            // Weight is set by user and stored in the radius of the sphere
            last_pos_weight = std::max(0.0, std::round(100 * (lambda_radius)));
            if (last_pos_weight > 0.0)
                seam_position = spCustom;
        }
    }

    if (seam_position != spRandom) {
        // Retrieve the last start position for this object.

        double travel_cost = 1;
        if (seam_position == spAligned) {
            // Seam is aligned to the seam at the preceding layer.
            if (po != nullptr) {
                std::optional<Point> pos = m_seam_history.get_last_seam(m_po_list[po_idx], layer_idx, polygon_bb);
                if (pos.has_value()) {
                    last_pos = *pos;
                }
                // TODO: check why i put it out of the if
                last_pos_weight = is_custom_enforcer_on_layer(layer_idx, po_idx) ? 0.f : 1.f;
            }
        }else if (seam_position == spRear) {
            // Object is centered around (0,0) in its current coordinate system.
            last_pos.x() = 0;
            last_pos.y() += coord_t(3. * po->bounding_box().radius());
            last_pos_weight = 5.f;
            travel_cost = 0;
        }else if (seam_position == spNearest) {
            last_pos_weight = 25.f;
            travel_cost = 0;
            angle_weight = 0;
        }else if (seam_position == spCost) {
            // last_pos already contains current nozzle position
            // set base last_pos_weight to the same value as penaltyFlatSurface
            last_pos_weight = 5.f;
            if (po != nullptr) {
                last_pos_weight = po->config().seam_travel_cost.get_abs_value(last_pos_weight);
                angle_weight = po->config().seam_angle_cost.get_abs_value(angle_weight);
                travel_cost = po->config().seam_travel_cost.get_abs_value(1);
            }
        }



        // Insert a projection of last_pos into the polygon.
        size_t last_pos_proj_idx;
        {
            Points::const_iterator it = project_point_to_polygon_and_insert(polygon, last_pos, 0.1 * nozzle_r );
            last_pos_proj_idx = it - polygon.points.begin();
        }
        Point last_pos_proj = polygon.points[last_pos_proj_idx];

        // Parametrize the polygon by its length.
        std::vector<float> lengths = polygon.parameter_by_length();

        //find the max dist the seam can be
        float dist_max = 0.1f * lengths.back();// 5.f * nozzle_dmr
        if (po != nullptr && travel_cost >= 1) {
            last_pos_weight *= 2;
            dist_max = 0;
            for (size_t i = 0; i < polygon.points.size(); ++i) {
                dist_max = std::max(dist_max, (float)polygon.points[i].distance_to(last_pos_proj));
            }
        }

        // For each polygon point, store a penalty.
        // First calculate the angles, store them as penalties. The angles are caluculated over a minimum arm length of nozzle_r.
        std::vector<float> penalties = polygon_angles_at_vertices(polygon, lengths,
            this->is_custom_seam_on_layer(layer_idx, po_idx) ? std::min(MINIMAL_POLYGON_SIDE / 2.f, float(nozzle_r)) : float(nozzle_r));
        // No penalty for reflex points, slight penalty for convex points, high penalty for flat surfaces.
        const float penaltyConvexVertex = 1.f;
        const float penaltyFlatSurface  = 5.f;
        const float penaltyOverhangHalf = 10.f;
        // Penalty for visible seams.
       for (size_t i = 0; i < polygon.points.size(); ++ i) {
            float ccwAngle = penalties[i];
            if (was_clockwise)
                ccwAngle = - ccwAngle;
            float penalty = 0;
            //if (ccwAngle < -float(0.6 * PI))
            //    penalty = 0.f;
            //else if (ccwAngle > float(0.6 * PI))
            //    
            //    penalty = penaltyConvexVertex;
            //else 
            if (ccwAngle < 0.f) {
                // We love Sharp reflex vertex (high negative ccwAngle). It hides the seam perfectly.
                // Interpolate penalty between maximum and zero.
                penalty = penaltyFlatSurface * bspline_kernel(ccwAngle);
            } else  if (ccwAngle > float(0.67 * PI)) {
                //penalize too sharp convex angle, it's best to be nearer to ~100°
                penalty = penaltyConvexVertex + (penaltyFlatSurface - penaltyConvexVertex) * bspline_kernel( (PI - ccwAngle) * 1.5);
            } else {
                // Interpolate penalty between maximum and the penalty for a convex vertex.
                penalty = penaltyConvexVertex + (penaltyFlatSurface - penaltyConvexVertex) * bspline_kernel(ccwAngle);
            }
            penalty *= angle_weight;
            if (po != nullptr && travel_cost >= 1) {
                penalty += last_pos_weight * polygon.points[i].distance_to(last_pos_proj) / dist_max;
            } else {
                // Give a negative penalty for points close to the last point or the prefered seam location.
                float dist_to_last_pos_proj = (i < last_pos_proj_idx) ?
                    std::min(lengths[last_pos_proj_idx] - lengths[i], lengths.back() - lengths[last_pos_proj_idx] + lengths[i]) :
                    std::min(lengths[i] - lengths[last_pos_proj_idx], lengths.back() - lengths[i] + lengths[last_pos_proj_idx]);
                penalty -= last_pos_weight * bspline_kernel(dist_to_last_pos_proj / dist_max);
            }
            penalties[i] = std::max(0.f, penalty);
        }

        // Penalty for overhangs.
        if (lower_layer_edge_grid) {
            // Use the edge grid distance field structure over the lower layer to calculate overhangs.
            coord_t nozzle_r = coord_t(std::floor(scale_(0.5 * nozzle_dmr) + 0.5));
            coord_t search_r = coord_t(std::floor(scale_(0.8 * nozzle_dmr) + 0.5));
            for (size_t i = 0; i < polygon.points.size(); ++ i) {
                const Point &p = polygon.points[i];
                coordf_t dist;
                // Signed distance is positive outside the object, negative inside the object.
                // The point is considered at an overhang, if it is more than nozzle radius
                // outside of the lower layer contour.
                [[maybe_unused]] bool found = lower_layer_edge_grid->signed_distance(p, search_r, dist);
                // If the approximate Signed Distance Field was initialized over lower_layer_edge_grid,
                // then the signed distnace shall always be known.
                assert(found);
                penalties[i] += extrudate_overlap_penalty(float(nozzle_r), penaltyOverhangHalf, float(dist));
            }
        }

        // Custom seam. Huge (negative) constant penalty is applied inside
        // blockers (enforcers) to rule out points that should not win.
        std::vector<float> penalties_with_custom_seam = penalties;
        this->apply_custom_seam(polygon, po_idx, penalties_with_custom_seam, lengths, layer_idx, seam_position);

        // Find a point with a minimum penalty.
        size_t idx_min = std::min_element(penalties_with_custom_seam.begin(), penalties_with_custom_seam.end()) - penalties_with_custom_seam.begin();

        if (seam_position != spAligned || ! is_custom_enforcer_on_layer(layer_idx, po_idx)) {
            // Very likely the weight of idx_min is very close to the weight of last_pos_proj_idx.
            // In that case use last_pos_proj_idx instead.
            float penalty_aligned  = penalties[last_pos_proj_idx];
            float penalty_min      = penalties[idx_min];
            float penalty_diff_abs = std::abs(penalties_with_custom_seam[idx_min] - penalties_with_custom_seam[last_pos_proj_idx]);
            float penalty_max      = std::max(std::abs(penalties[idx_min]), std::abs(penalties[last_pos_proj_idx]));
            float penalty_diff_rel = (penalty_max == 0.f) ? 0.f : penalty_diff_abs / penalty_max;
            // printf("Align seams, penalty aligned: %f, min: %f, diff abs: %f, diff rel: %f\n", penalty_aligned, penalty_min, penalty_diff_abs, penalty_diff_rel);
            if (std::abs(penalty_diff_rel) < 0.05 && penalty_diff_abs < 3) {
                // Penalty of the aligned point is very close to the minimum penalty.
                // Align the seams as accurately as possible.
                idx_min = last_pos_proj_idx;
            }
        }

        if (loop.role() == erExternalPerimeter)
            m_seam_history.add_seam(po, polygon.points[idx_min], polygon_bb);


        // Export the contour into a SVG file.
        #if 0
        {
            static int iRun = 0;
            SVG svg(debug_out_path("GCode_extrude_loop-%d.svg", iRun ++));
            if (m_layer->lower_layer != NULL)
                svg.draw(m_layer->lower_layer->slices);
            for (size_t i = 0; i < loop.paths.size(); ++ i)
                svg.draw(loop.paths[i].as_polyline(), "red");
            Polylines polylines;
            for (size_t i = 0; i < loop.paths.size(); ++ i)
                polylines.push_back(loop.paths[i].as_polyline());
            Slic3r::Polygons polygons;
            coordf_t nozzle_dmr = EXTRUDER_CONFIG(nozzle_diameter);
            coord_t delta = scale_(0.5*nozzle_dmr);
            Slic3r::offset(polylines, &polygons, delta);
//            for (size_t i = 0; i < polygons.size(); ++ i) svg.draw((Polyline)polygons[i], "blue");
            svg.draw(last_pos, "green", 3);
            svg.draw(polygon.points[idx_min], "yellow", 3);
            svg.Close();
        }
        #endif
        return polygon.points[idx_min];

    } else { // spRandom
        if ( (loop.loop_role() & ExtrusionLoopRole::elrInternal) != 0 && loop.role() != erExternalPerimeter) {
            // This loop does not contain any other loop. Set a random position.
            // The other loops will get a seam close to the random point chosen
            // on the innermost contour.
            last_pos = this->get_random_seam(layer_idx, polygon, po_idx);
        } else if (loop.role() == erExternalPerimeter) {
            bool saw_custom = false;
            if (is_custom_seam_on_layer(layer_idx, po_idx)) {
                // There is a possibility that the loop will be influenced by custom
                // seam enforcer/blocker. In this case do not inherit the seam
                // from internal loops (which may conflict with the custom selection
                // and generate another random one.
                Point candidate = this->get_random_seam(layer_idx, polygon, po_idx, &saw_custom);
                if (saw_custom)
                    last_pos = candidate;
            }
            if(!saw_custom)
                if (external_perimeters_first || (loop.loop_role() & ExtrusionLoopRole::elrFirstLoop) != 0) {
                    // this is if external_perimeters_first
                    // this is if only space for one externalperimeter.
                    //in these case, there isn't a seam from the inner loops, so we had to creat our on
                    last_pos = this->get_random_seam(layer_idx, polygon, po_idx);
                }
        } else if (loop.role() == erThinWall) {
            //thin wall loop is like an external perimeter, but without anything near it.
            last_pos = this->get_random_seam(layer_idx, polygon, po_idx);
        }
        return last_pos;
    }
}


Point SeamPlacer::get_random_seam(size_t layer_idx, const Polygon& polygon, size_t po_idx,
                                  bool* saw_custom) const
{
    // Parametrize the polygon by its length.
    const std::vector<float> lengths = polygon.parameter_by_length();

    // Which of the points are inside enforcers/blockers?
    std::vector<size_t> enforcers_idxs;
    std::vector<size_t> blockers_idxs;
    this->get_enforcers_and_blockers(layer_idx, polygon, po_idx, enforcers_idxs, blockers_idxs);

    bool has_enforcers = ! enforcers_idxs.empty();
    bool has_blockers = ! blockers_idxs.empty();
    if (saw_custom)
        *saw_custom = has_enforcers || has_blockers;

    assert(std::is_sorted(enforcers_idxs.begin(), enforcers_idxs.end()));
    assert(std::is_sorted(blockers_idxs.begin(), blockers_idxs.end()));
    std::vector<float> edges;

    // Lambda to calculate lengths of all edges of interest. Last parameter
    // decides whether to measure edges inside or outside idxs.
    // Negative number = not an edge of interest.
    auto get_valid_length = [&lengths](const std::vector<size_t>& idxs,
                                       std::vector<float>& edges,
                                       bool measure_inside_edges) -> float
    {
        // First mark edges we are interested in by assigning a positive number.
        edges.assign(lengths.size()-1, measure_inside_edges ? -1.f : 1.f);
        for (size_t i=0; i<idxs.size(); ++i) {
            size_t this_pt_idx = idxs[i];
            // Two concurrent indices in the list -> the edge between them is the enforcer/blocker.
            bool inside_edge = ((i != idxs.size()-1 && idxs[i+1] == this_pt_idx + 1)
                             || (i == idxs.size()-1 && idxs.back() == lengths.size()-2 && idxs[0] == 0));
            if (inside_edge)
                edges[this_pt_idx] = measure_inside_edges ? 1.f : -1.f;
        }
        // Now measure them.
        float running_total = 0.f;
        for (size_t i=0; i<edges.size(); ++i) {
            if (edges[i] > 0.f) {
                edges[i] = lengths[i+1] - lengths[i];
                running_total += edges[i];
            }
        }
        return running_total;
    };

    // Find all seam candidate edges and their lengths.
    float valid_length = 0.f;
    if (has_enforcers)
        valid_length = get_valid_length(enforcers_idxs, edges, true);

    if (! has_enforcers || valid_length == 0.f) {
        // Second condition covers case with isolated enf points. Given how the painted
        // triangles are projected, this should not happen. Stay on the safe side though.
        if (has_blockers)
            valid_length = get_valid_length(blockers_idxs, edges, false);
        if (valid_length == 0.f) // No blockers or everything blocked - use the whole polygon.
            valid_length = lengths.back();
    }
    assert(valid_length != 0.f);
    // Now generate a random length and find the respective edge.
    float rand_len = valid_length * (rand()/float(RAND_MAX));
    size_t pt_idx = 0; // Index of the edge where to put the seam.
    if (valid_length == lengths.back()) {
        // Whole polygon is used for placing the seam.
        auto it = std::lower_bound(lengths.begin(), lengths.end(), rand_len);
        pt_idx = it == lengths.begin() ? 0 : (it-lengths.begin()-1); // this takes care of a corner case where rand() returns 0
    } else {
        float running = 0.f;
        for (size_t i=0; i<edges.size(); ++i) {
            running += edges[i] > 0.f ? edges[i] : 0.f;
            if (running >= rand_len) {
                pt_idx = i;
                break;
            }
        }
    }

    if (! has_enforcers && ! has_blockers) {
        // The polygon may be too coarse, calculate the point exactly.
        assert(valid_length == lengths.back());
        bool last_seg = pt_idx == polygon.points.size()-1;
        size_t next_idx = last_seg ? 0 : pt_idx+1;
        const Point& prev = polygon.points[pt_idx];
        const Point& next = polygon.points[next_idx];
        assert(next_idx == 0 || pt_idx+1 == next_idx);
        coordf_t diff_x = next.x() - prev.x();
        coordf_t diff_y = next.y() - prev.y();
        coordf_t dist = lengths[last_seg ? pt_idx+1 : next_idx] - lengths[pt_idx];
        return Point(prev.x() + (rand_len - lengths[pt_idx]) * (diff_x/dist),
                     prev.y() + (rand_len - lengths[pt_idx]) * (diff_y/dist));

    } else {
        // The polygon should be dense enough.
        return polygon.points[pt_idx];
    }
}








void SeamPlacer::get_enforcers_and_blockers(size_t layer_id,
                             const Polygon& polygon,
                             size_t po_idx,
                             std::vector<size_t>& enforcers_idxs,
                             std::vector<size_t>& blockers_idxs) const
{
    enforcers_idxs.clear();
    blockers_idxs.clear();

    auto is_inside = [](const Point& pt,
                        const CustomTrianglesPerLayer& custom_data) -> bool {
        assert(! custom_data.polys.empty());
        // Now ask the AABB tree which polygons we should check and check them.
        std::vector<size_t> candidates;
        AABBTreeIndirect::get_candidate_idxs(custom_data.tree, pt, candidates);
        if (! candidates.empty())
            for (size_t idx : candidates)
                if (custom_data.polys[idx].contains(pt))
            return true;
        return false;
    };

    if (! m_enforcers[po_idx].empty()) {
        const CustomTrianglesPerLayer& enforcers = m_enforcers[po_idx][layer_id];
        if (! enforcers.polys.empty()) {
    for (size_t i=0; i<polygon.points.size(); ++i) {
                if (is_inside(polygon.points[i], enforcers))
                    enforcers_idxs.emplace_back(i);
            }
        }
    }

    if (! m_blockers[po_idx].empty()) {
        const CustomTrianglesPerLayer& blockers = m_blockers[po_idx][layer_id];
        if (! blockers.polys.empty()) {
            for (size_t i=0; i<polygon.points.size(); ++i) {
                if (is_inside(polygon.points[i], blockers))
                    blockers_idxs.emplace_back(i);
            }
        }
    }

}


// Go through the polygon, identify points inside support enforcers and return
// indices of points in the middle of each enforcer (measured along the contour).
static std::vector<size_t> find_enforcer_centers(const Polygon& polygon,
                                                 const std::vector<float>& lengths,
                                                 const std::vector<size_t>& enforcers_idxs)
{
    std::vector<size_t> out;
    assert(polygon.points.size()+1 == lengths.size());
    assert(std::is_sorted(enforcers_idxs.begin(), enforcers_idxs.end()));
    if (polygon.size() < 2 || enforcers_idxs.empty())
        return out;

    auto get_center_idx = [&polygon, &lengths](size_t start_idx, size_t end_idx) -> size_t {
        assert(end_idx >= start_idx);
        if (start_idx == end_idx)
            return start_idx;
        float t_c = lengths[start_idx] + 0.5f * (lengths[end_idx] - lengths[start_idx]);
        auto it = std::lower_bound(lengths.begin() + start_idx, lengths.begin() + end_idx, t_c);
        int ret = it - lengths.begin();
        return ret;
    };

    int last_enforcer_start_idx = enforcers_idxs.front();
    bool first_pt_in_list = enforcers_idxs.front() != 0;
    bool last_pt_in_list = enforcers_idxs.back() == polygon.points.size() - 1;
    bool wrap_around = last_pt_in_list && first_pt_in_list;

    for (size_t i=0; i<enforcers_idxs.size(); ++i) {
        if (i != enforcers_idxs.size() - 1) {
            if (enforcers_idxs[i+1] != enforcers_idxs[i] + 1) {
                // i is last point of current enforcer
                out.push_back(get_center_idx(last_enforcer_start_idx, enforcers_idxs[i]));
                last_enforcer_start_idx = enforcers_idxs[i+1];
            }
        } else {
            if (! wrap_around) {
                // we can safely use the last enforcer point.
                out.push_back(get_center_idx(last_enforcer_start_idx, enforcers_idxs[i]));
            }
        }
    }

    if (wrap_around) {
        // Update first center already found.
        if (out.empty()) {
            // Probably an enforcer around the whole contour. Return nothing.
            return out;
        }

        // find last point of the enforcer at the beginning:
        size_t idx = 0;
        while (enforcers_idxs[idx]+1 == enforcers_idxs[idx+1])
            ++idx;

        float t_s = lengths[last_enforcer_start_idx];
        float t_e = lengths[idx];
        float half_dist = 0.5f * (t_e + lengths.back() - t_s);
        float t_c = (half_dist > t_e) ? t_s + half_dist : t_e - half_dist;

        auto it = std::lower_bound(lengths.begin(), lengths.end(), t_c);
        out[0] = it - lengths.begin();
        if (out[0] == lengths.size() - 1)
            --out[0];
        assert(out[0] < lengths.size() - 1);
    }
    return out;
}



void SeamPlacer::apply_custom_seam(const Polygon& polygon, size_t po_idx,
                                   std::vector<float>& penalties,
                                   const std::vector<float>& lengths,
                                   int layer_id, SeamPosition seam_position) const
{
    if (! is_custom_seam_on_layer(layer_id, po_idx))
        return;

    std::vector<size_t> enforcers_idxs;
    std::vector<size_t> blockers_idxs;
    this->get_enforcers_and_blockers(layer_id, polygon, po_idx, enforcers_idxs, blockers_idxs);

    for (size_t i : enforcers_idxs) {
        assert(i < penalties.size());
        penalties[i] -= float(ENFORCER_BLOCKER_PENALTY);
    }
    for (size_t i : blockers_idxs) {
        assert(i < penalties.size());
        penalties[i] += float(ENFORCER_BLOCKER_PENALTY);
    }
    if (seam_position == spAligned) {
        std::vector<size_t> enf_centers = find_enforcer_centers(polygon, lengths, enforcers_idxs);
        for (size_t idx : enf_centers) {
            assert(idx < penalties.size());
            penalties[idx] += ENFORCER_CENTER_PENALTY;
        }
    }

////////////////////////
//    std::ostringstream os;
//    os << std::setw(3) << std::setfill('0') << layer_id;
//    int a = scale_(30.);
//    SVG svg("custom_seam" + os.str() + ".svg", BoundingBox(Point(-a, -a), Point(a, a)));
//    //if (! m_enforcers[po_idx].empty())
//    //    svg.draw(m_enforcers[po_idx][layer_id].polys, "blue");
//    //if (! m_blockers[po_idx].empty())
//    //    svg.draw(m_blockers[po_idx][layer_id].polys, "red");



//    size_t min_idx = std::min_element(penalties.begin(), penalties.end()) - penalties.begin();

//    //svg.draw(polygon.points[idx_min], "red", 6e5);
//    for (size_t i=0; i<polygon.points.size(); ++i) {
//        std::string fill;
//        coord_t size = 0;
//        if (min_idx == i) {
//            fill = "yellow";
//            size = 5e5;
//        } else
//            fill = (std::find(enforcers_idxs.begin(), enforcers_idxs.end(), i) != enforcers_idxs.end() ? "green" : "black");
//        if (i != 0)
//            svg.draw(polygon.points[i], fill, size);
//        else
//            svg.draw(polygon.points[i], "red", 5e5);
//    }
//////////////////////

}



std::optional<Point> SeamHistory::get_last_seam(const PrintObject* po, coord_t layer_z, const BoundingBox& island_bb)
{
    assert(layer_z >= m_layer_z);
    if (layer_z > m_layer_z) {
        // Get seam was called for different layer than last time.
        m_data_last_layer = m_data_this_layer;
        m_data_this_layer.clear();
        m_layer_z = layer_z;
    }



    std::optional<Point> out;

    auto seams_it = m_data_last_layer.find(po);
    if (seams_it == m_data_last_layer.end())
        return out;

    const std::vector<SeamPoint>& seam_data_po = seams_it->second;

    // Find a bounding-box on the last layer that is close to one we see now.
    double min_score = std::numeric_limits<double>::max();
    for (const SeamPoint& sp : seam_data_po) {
        const BoundingBox& bb = sp.m_island_bb;

        if (! bb.overlap(island_bb)) {
            // This bb does not even overlap. It is likely unrelated.
            continue;
        }

        double score = std::pow(bb.min(0) - island_bb.min(0), 2.)
                     + std::pow(bb.min(1) - island_bb.min(1), 2.)
                     + std::pow(bb.max(0) - island_bb.max(0), 2.)
                     + std::pow(bb.max(1) - island_bb.max(1), 2.);

        if (score < min_score) {
            min_score = score;
            out = sp.m_pos;
        }
    }

    return out;
}



void SeamHistory::add_seam(const PrintObject* po, const Point& pos, const BoundingBox& island_bb)
{
    m_data_this_layer[po].push_back({pos, island_bb});;
}



void SeamHistory::clear()
{
    m_layer_z = 0;
    m_data_last_layer.clear();
    m_data_this_layer.clear();
}


}
