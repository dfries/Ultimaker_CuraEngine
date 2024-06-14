// Copyright (c) 2024 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "PrimeTower/PrimeTower.h"

#include <algorithm>
#include <limits>
#include <numbers>

#include <spdlog/spdlog.h>

#include "Application.h" //To get settings.
#include "ExtruderTrain.h"
#include "LayerPlan.h"
#include "PrimeTower/PrimeTowerInterleaved.h"
#include "PrimeTower/PrimeTowerNormal.h"
#include "Scene.h"
#include "Slice.h"
#include "gcodeExport.h"
#include "infill.h"
#include "raft.h"
#include "sliceDataStorage.h"


namespace cura
{

PrimeTower::PrimeTower()
    : wipe_from_middle_(false)
{
    const Scene& scene = Application::getInstance().current_slice_->scene;
    const Settings& mesh_group_settings = scene.current_mesh_group->settings;
    const coord_t tower_radius = mesh_group_settings.get<coord_t>("prime_tower_size") / 2;
    const coord_t x = mesh_group_settings.get<coord_t>("prime_tower_position_x");
    const coord_t y = mesh_group_settings.get<coord_t>("prime_tower_position_y");
    const coord_t layer_height = mesh_group_settings.get<coord_t>("layer_height");
    const bool base_enabled = mesh_group_settings.get<bool>("prime_tower_brim_enable");
    const coord_t base_extra_radius = scene.settings.get<coord_t>("prime_tower_base_size");
    const coord_t base_height = scene.settings.get<coord_t>("prime_tower_base_height");
    const double base_curve_magnitude = mesh_group_settings.get<double>("prime_tower_base_curve_magnitude");

    middle_ = Point2LL(x - tower_radius, y + tower_radius);
    outer_poly_.push_back(PolygonUtils::makeCircle(middle_, tower_radius, circle_definition_));
    post_wipe_point_ = Point2LL(x - tower_radius, y + tower_radius);

    // Evenly spread out a number of dots along the prime tower's outline. This is done for the complete outline,
    // so use the same start and end segments for this.
    PolygonsPointIndex segment_start = PolygonsPointIndex(&outer_poly_, 0, 0);
    PolygonsPointIndex segment_end = segment_start;

    PolygonUtils::spreadDots(segment_start, segment_end, number_of_prime_tower_start_locations_, prime_tower_start_locations_);

    // Generate the base outline
    if (base_enabled && base_extra_radius > 0 && base_height > 0)
    {
        base_occupied_outline_.init(true);

        for (coord_t z = 0; z < base_height; z += layer_height)
        {
            const double brim_radius_factor = std::pow((1.0 - static_cast<double>(z) / static_cast<double>(base_height)), base_curve_magnitude);
            const coord_t extra_radius = std::llrint(static_cast<double>(base_extra_radius) * brim_radius_factor);
            const coord_t total_radius = tower_radius + extra_radius;
            base_occupied_outline_.emplace_back(std::vector<Polygon>({ PolygonUtils::makeCircle(middle_, total_radius, circle_definition_) }), total_radius);
        }
    }
}

void PrimeTower::generateBase()
{
    const Scene& scene = Application::getInstance().current_slice_->scene;
    const Settings& mesh_group_settings = scene.current_mesh_group->settings;
    const bool base_enabled = mesh_group_settings.get<bool>("prime_tower_brim_enable");
    const coord_t base_extra_radius = scene.settings.get<coord_t>("prime_tower_base_size");
    const coord_t base_height = scene.settings.get<coord_t>("prime_tower_base_height");

    if (base_enabled && base_extra_radius > 0 && base_height > 0)
    {
        base_extrusion_outline_.init(true);

        // Generate the base outside extra annuli for the first extruder of each layer
        auto iterator_extrusion_paths = toolpaths_.begin();
        auto iterator_base_outline = base_occupied_outline_.begin();
        for (; iterator_extrusion_paths != toolpaths_.end() && iterator_base_outline != base_occupied_outline_.end(); ++iterator_extrusion_paths, ++iterator_base_outline)
        {
            std::vector<ExtruderToolPaths>& toolpaths_at_this_layer = iterator_extrusion_paths->second;
            if (! toolpaths_at_this_layer.empty())
            {
                const OccupiedOutline& base_ouline_at_this_layer = *iterator_base_outline;
                ExtruderToolPaths& first_extruder_toolpaths = toolpaths_at_this_layer.front();
                const size_t extruder_nr = first_extruder_toolpaths.extruder_nr;
                const coord_t line_width = scene.extruders[extruder_nr].settings_.get<coord_t>("prime_tower_line_width");

                std::tuple<Shape, coord_t> outset
                    = PolygonUtils::generateCirculatOutset(middle_, first_extruder_toolpaths.outer_radius, base_ouline_at_this_layer.outer_radius, line_width, circle_definition_);
                first_extruder_toolpaths.toolpaths.push_back(std::get<0>(outset));

                base_extrusion_outline_.push_back(std::vector<Polygon>({ PolygonUtils::makeCircle(middle_, std::get<1>(outset), circle_definition_) }));
            }
        }
    }
}

void PrimeTower::generateFirtLayerInset()
{
    // Generate the base inside extra disc for the last extruder of the first layer
    if (! toolpaths_.empty())
    {
        std::vector<ExtruderToolPaths>& toolpaths_first_layer = toolpaths_.begin()->second;
        if (! toolpaths_first_layer.empty())
        {
            ExtruderToolPaths& last_extruder_toolpaths = toolpaths_first_layer.back();
            const Scene& scene = Application::getInstance().current_slice_->scene;
            const size_t extruder_nr = last_extruder_toolpaths.extruder_nr;
            const coord_t line_width = scene.extruders[extruder_nr].settings_.get<coord_t>("prime_tower_line_width");
            Shape pattern = PolygonUtils::generateCircularInset(middle_, last_extruder_toolpaths.inner_radius, line_width, circle_definition_);
            last_extruder_toolpaths.toolpaths.push_back(pattern);
        }
    }
}

std::tuple<Shape, coord_t> PrimeTower::generatePrimeToolpaths(const size_t extruder_nr, const coord_t outer_radius)
{
    const Scene& scene = Application::getInstance().current_slice_->scene;
    const Settings& mesh_group_settings = scene.current_mesh_group->settings;
    const coord_t layer_height = mesh_group_settings.get<coord_t>("layer_height");
    const coord_t line_width = scene.extruders[extruder_nr].settings_.get<coord_t>("prime_tower_line_width");
    const double required_volume = scene.extruders[extruder_nr].settings_.get<double>("prime_tower_min_volume") * 1000000000;
    const Ratio flow = scene.extruders[extruder_nr].settings_.get<Ratio>("prime_tower_flow");
    const coord_t semi_line_width = line_width / 2;

    double current_volume = 0;
    coord_t current_outer_radius = outer_radius - semi_line_width;
    Shape toolpaths;
    while (current_volume < required_volume && current_outer_radius >= semi_line_width)
    {
        Polygon circle = PolygonUtils::makeCircle(middle_, current_outer_radius, circle_definition_);
        toolpaths.push_back(circle);
        current_volume += static_cast<double>(circle.length() * line_width * layer_height) * flow;
        current_outer_radius -= line_width;
    }

    return { toolpaths, current_outer_radius + semi_line_width };
}

Shape PrimeTower::generateSupportToolpaths(const size_t extruder_nr, const coord_t outer_radius, const coord_t inner_radius)
{
    const Scene& scene = Application::getInstance().current_slice_->scene;
    const double max_bridging_distance = static_cast<double>(scene.extruders[extruder_nr].settings_.get<coord_t>("prime_tower_max_bridging_distance"));
    const coord_t line_width = scene.extruders[extruder_nr].settings_.get<coord_t>("prime_tower_line_width");
    const coord_t radius_delta = outer_radius - inner_radius;
    const coord_t semi_line_width = line_width / 2;

    Shape toolpaths;

    // Split annuli according to max bridging distance
    const coord_t nb_annuli = static_cast<coord_t>(std::ceil(static_cast<double>(radius_delta) / max_bridging_distance));
    if (nb_annuli > 0)
    {
        const coord_t actual_radius_step = radius_delta / nb_annuli;

        for (coord_t i = 0; i < nb_annuli; ++i)
        {
            const coord_t annulus_inner_radius = (inner_radius + i * actual_radius_step) + semi_line_width;
            const coord_t annulus_outer_radius = (inner_radius + (i + 1) * actual_radius_step) - semi_line_width;

            const size_t semi_nb_spokes = static_cast<size_t>(std::ceil((std::numbers::pi * static_cast<double>(annulus_outer_radius)) / max_bridging_distance));

            toolpaths.push_back(PolygonUtils::makeWheel(middle_, annulus_inner_radius, annulus_outer_radius, semi_nb_spokes, arc_definition_));
        }
    }

    return toolpaths;
}

void PrimeTower::addToGcode(
    const SliceDataStorage& storage,
    LayerPlan& gcode_layer,
    const std::vector<ExtruderUse>& required_extruder_prime,
    const size_t prev_extruder_nr,
    const size_t new_extruder_nr) const
{
    if (gcode_layer.getPrimeTowerIsPlanned(new_extruder_nr))
    { // don't print the prime tower if it has been printed already with this extruder.
        return;
    }

    const LayerIndex layer_nr = gcode_layer.getLayerNr();
    if (layer_nr > storage.max_print_height_second_to_last_extruder + 1)
    {
        return;
    }

    bool post_wipe = Application::getInstance().current_slice_->scene.extruders[prev_extruder_nr].settings_.get<bool>("prime_tower_wipe_enabled");

    // Do not wipe on the first layer, we will generate non-hollow prime tower there for better bed adhesion.
    if (prev_extruder_nr == new_extruder_nr || layer_nr == 0)
    {
        post_wipe = false;
    }

    auto extruder_iterator = std::find_if(
        required_extruder_prime.begin(),
        required_extruder_prime.end(),
        [new_extruder_nr](const ExtruderUse& extruder_use)
        {
            return extruder_use.extruder_nr == new_extruder_nr;
        });

    if (extruder_iterator == required_extruder_prime.end())
    {
        // Extruder is not used on this layer
        return;
    }

    const Shape* toolpaths = nullptr;
    auto iterator_layer = toolpaths_.find(layer_nr);
    if (iterator_layer != toolpaths_.end())
    {
        const std::vector<ExtruderToolPaths>& toolpaths_at_this_layer = iterator_layer->second;
        auto iterator_extruder = std::find_if(
            toolpaths_at_this_layer.begin(),
            toolpaths_at_this_layer.end(),
            [new_extruder_nr](const ExtruderToolPaths& extruder_toolpaths)
            {
                return extruder_toolpaths.extruder_nr == new_extruder_nr;
            });
        if (iterator_extruder != iterator_layer->second.end())
        {
            toolpaths = &(iterator_extruder->toolpaths);
        }
    }

    if (toolpaths && ! toolpaths->empty())
    {
        gotoStartLocation(gcode_layer, new_extruder_nr);

        const GCodePathConfig& config = gcode_layer.configs_storage_.prime_tower_config_per_extruder[new_extruder_nr];
        gcode_layer.addPolygonsByOptimizer(*toolpaths, config);
    }

    gcode_layer.setPrimeTowerIsPlanned(new_extruder_nr);

    // post-wipe:
    if (post_wipe)
    {
        // Make sure we wipe the old extruder on the prime tower.
        const Settings& previous_settings = Application::getInstance().current_slice_->scene.extruders[prev_extruder_nr].settings_;
        const Point2LL previous_nozzle_offset = Point2LL(previous_settings.get<coord_t>("machine_nozzle_offset_x"), previous_settings.get<coord_t>("machine_nozzle_offset_y"));
        const Settings& new_settings = Application::getInstance().current_slice_->scene.extruders[new_extruder_nr].settings_;
        const Point2LL new_nozzle_offset = Point2LL(new_settings.get<coord_t>("machine_nozzle_offset_x"), new_settings.get<coord_t>("machine_nozzle_offset_y"));
        gcode_layer.addTravel(post_wipe_point_ - previous_nozzle_offset + new_nozzle_offset);
    }
}

const Shape& PrimeTower::getOccupiedOutline(const LayerIndex& layer_nr) const
{
    auto iterator = base_occupied_outline_.iterator_at(layer_nr);
    if (iterator != base_occupied_outline_.end())
    {
        return iterator->outline;
    }
    else
    {
        return outer_poly_;
    }
}

const Shape& PrimeTower::getOccupiedGroundOutline() const
{
    if (! base_extrusion_outline_.empty())
    {
        return base_extrusion_outline_.front();
    }
    else
    {
        return outer_poly_;
    }
}

const Shape& PrimeTower::getExtrusionOutline(const LayerIndex& layer_nr) const
{
    auto iterator = base_extrusion_outline_.iterator_at(layer_nr);
    if (iterator != base_extrusion_outline_.end())
    {
        return *iterator;
    }
    else
    {
        return outer_poly_;
    }
}

void PrimeTower::subtractFromSupport(SliceDataStorage& storage)
{
    for (size_t layer = 0; static_cast<int>(layer) <= storage.max_print_height_second_to_last_extruder + 1 && layer < storage.support.supportLayers.size(); layer++)
    {
        const Shape& outside_polygon = getOccupiedOutline(layer).getOutsidePolygons();
        AABB outside_polygon_boundary_box(outside_polygon);
        SupportLayer& support_layer = storage.support.supportLayers[layer];
        // take the differences of the support infill parts and the prime tower area
        support_layer.excludeAreasFromSupportInfillAreas(outside_polygon, outside_polygon_boundary_box);
    }
}

void PrimeTower::processExtrudersUse(LayerVector<std::vector<ExtruderUse>>& extruders_use, const size_t start_extruder)
{
    polishExtrudersUses(extruders_use, start_extruder);
    toolpaths_ = generateToolPaths(extruders_use);
    generateBase();
    generateFirtLayerInset();
}

PrimeTower* PrimeTower::createPrimeTower(SliceDataStorage& storage)
{
    PrimeTower* prime_tower = nullptr;
    const Scene& scene = Application::getInstance().current_slice_->scene;
    const size_t raft_total_extra_layers = Raft::getTotalExtraLayers();

    if (scene.extruders.size() > 1 && scene.current_mesh_group->settings.get<bool>("prime_tower_enable")
        && scene.current_mesh_group->settings.get<coord_t>("prime_tower_min_volume") > 10 && scene.current_mesh_group->settings.get<coord_t>("prime_tower_size") > 10
        && storage.max_print_height_second_to_last_extruder >= -static_cast<int>(raft_total_extra_layers))
    {
        const Settings& mesh_group_settings = scene.current_mesh_group->settings;
        const PrimeTowerMode method = mesh_group_settings.get<PrimeTowerMode>("prime_tower_mode");

        switch (method)
        {
        case PrimeTowerMode::NORMAL:
            prime_tower = new PrimeTowerNormal();
            break;
        case PrimeTowerMode::INTERLEAVED:
            prime_tower = new PrimeTowerInterleaved();
            break;
        }
    }

    if (prime_tower)
    {
        prime_tower->subtractFromSupport(storage);
    }

    return prime_tower;
}

bool PrimeTower::extruderRequiresPrime(const std::vector<bool>& extruder_is_used_on_this_layer, size_t extruder_nr, size_t last_extruder)
{
    return extruder_is_used_on_this_layer[extruder_nr] && extruder_nr != last_extruder;
}

void PrimeTower::gotoStartLocation(LayerPlan& gcode_layer, const size_t extruder_nr) const
{
    // Layer number may be negative, make it positive (or null) to apply modulo operator
    LayerIndex layer_nr = gcode_layer.getLayerNr();
    if (layer_nr != -Raft::getTotalExtraLayers())
    {
        while (layer_nr < 0)
        {
            layer_nr += number_of_prime_tower_start_locations_;
        }

        size_t current_start_location_idx = ((extruder_nr + 1) * static_cast<size_t>(layer_nr)) % number_of_prime_tower_start_locations_;

        const ClosestPointPolygon wipe_location = prime_tower_start_locations_[current_start_location_idx];
        const ExtruderTrain& train = Application::getInstance().current_slice_->scene.extruders[extruder_nr];
        const coord_t inward_dist = train.settings_.get<coord_t>("machine_nozzle_size") * 3 / 2;
        const coord_t start_dist = train.settings_.get<coord_t>("machine_nozzle_size") * 2;
        const Point2LL prime_end = PolygonUtils::moveInsideDiagonally(wipe_location, inward_dist);
        const Point2LL outward_dir = wipe_location.location_ - prime_end;
        const Point2LL prime_start = wipe_location.location_ + normal(outward_dir, start_dist);

        gcode_layer.addTravel(prime_start);
    }
}

} // namespace cura
