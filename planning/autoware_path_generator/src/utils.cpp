// Copyright 2024 TIER IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "autoware/path_generator/utils.hpp"

#include <autoware/motion_utils/constants.hpp>
#include <autoware/motion_utils/resample/resample.hpp>
#include <autoware/motion_utils/trajectory/trajectory.hpp>
#include <autoware/universe_utils/geometry/geometry.hpp>
#include <autoware/universe_utils/math/unit_conversion.hpp>
#include <autoware_lanelet2_extension/utility/message_conversion.hpp>
#include <autoware_lanelet2_extension/utility/utilities.hpp>

#include <lanelet2_core/geometry/Lanelet.h>

#include <utility>
#include <vector>

namespace autoware::path_generator
{
namespace utils
{
namespace
{
template <typename T>
bool exists(const std::vector<T> & vec, const T & item)
{
  return std::find(vec.begin(), vec.end(), item) != vec.end();
}
}  // namespace

std::optional<lanelet::ConstLanelets> get_lanelets_within_route(
  const lanelet::ConstLanelet & lanelet, const PlannerData & planner_data,
  const geometry_msgs::msg::Pose & current_pose, const double backward_distance,
  const double forward_distance)
{
  if (!exists(planner_data.route_lanelets, lanelet)) {
    return std::nullopt;
  }

  const auto arc_coordinates = lanelet::utils::getArcCoordinates({lanelet}, current_pose);
  const auto lanelet_length = lanelet::utils::getLaneletLength2d(lanelet);

  const auto backward_lanelets = get_lanelets_within_route_up_to(
    lanelet, planner_data, backward_distance - arc_coordinates.length);
  if (!backward_lanelets) {
    return std::nullopt;
  }

  const auto forward_lanelets = get_lanelets_within_route_after(
    lanelet, planner_data, forward_distance - (lanelet_length - arc_coordinates.length));
  if (!forward_lanelets) {
    return std::nullopt;
  }

  lanelet::ConstLanelets lanelets(std::move(*backward_lanelets));
  lanelets.push_back(lanelet);
  std::move(forward_lanelets->begin(), forward_lanelets->end(), std::back_inserter(lanelets));

  return lanelets;
}

std::optional<lanelet::ConstLanelets> get_lanelets_within_route_up_to(
  const lanelet::ConstLanelet & lanelet, const PlannerData & planner_data, const double distance)
{
  if (!exists(planner_data.route_lanelets, lanelet)) {
    return std::nullopt;
  }

  lanelet::ConstLanelets lanelets{};
  auto current_lanelet = lanelet;
  auto length = 0.;

  while (rclcpp::ok() && length < distance) {
    const auto prev_lanelet = get_previous_lanelet_within_route(current_lanelet, planner_data);
    if (!prev_lanelet) {
      break;
    }

    lanelets.push_back(*prev_lanelet);
    current_lanelet = *prev_lanelet;
    length += lanelet::utils::getLaneletLength2d(*prev_lanelet);
  }

  std::reverse(lanelets.begin(), lanelets.end());
  return lanelets;
}

std::optional<lanelet::ConstLanelets> get_lanelets_within_route_after(
  const lanelet::ConstLanelet & lanelet, const PlannerData & planner_data, const double distance)
{
  if (!exists(planner_data.route_lanelets, lanelet)) {
    return std::nullopt;
  }

  lanelet::ConstLanelets lanelets{};
  auto current_lanelet = lanelet;
  auto length = 0.;

  while (rclcpp::ok() && length < distance) {
    const auto next_lanelet = get_next_lanelet_within_route(current_lanelet, planner_data);
    if (!next_lanelet) {
      break;
    }

    lanelets.push_back(*next_lanelet);
    current_lanelet = *next_lanelet;
    length += lanelet::utils::getLaneletLength2d(*next_lanelet);
  }

  return lanelets;
}

std::optional<lanelet::ConstLanelet> get_previous_lanelet_within_route(
  const lanelet::ConstLanelet & lanelet, const PlannerData & planner_data)
{
  if (exists(planner_data.start_lanelets, lanelet)) {
    return std::nullopt;
  }

  const auto prev_lanelets = planner_data.routing_graph_ptr->previous(lanelet);
  if (prev_lanelets.empty()) {
    return std::nullopt;
  }

  const auto prev_lanelet_itr = std::find_if(
    prev_lanelets.cbegin(), prev_lanelets.cend(),
    [&](const lanelet::ConstLanelet & l) { return exists(planner_data.route_lanelets, l); });
  if (prev_lanelet_itr == prev_lanelets.cend()) {
    return std::nullopt;
  }
  return *prev_lanelet_itr;
}

std::optional<lanelet::ConstLanelet> get_next_lanelet_within_route(
  const lanelet::ConstLanelet & lanelet, const PlannerData & planner_data)
{
  if (planner_data.preferred_lanelets.empty()) {
    return std::nullopt;
  }

  if (exists(planner_data.goal_lanelets, lanelet)) {
    return std::nullopt;
  }

  const auto next_lanelets = planner_data.routing_graph_ptr->following(lanelet);
  if (
    next_lanelets.empty() ||
    next_lanelets.front().id() == planner_data.preferred_lanelets.front().id()) {
    return std::nullopt;
  }

  const auto next_lanelet_itr = std::find_if(
    next_lanelets.cbegin(), next_lanelets.cend(),
    [&](const lanelet::ConstLanelet & l) { return exists(planner_data.route_lanelets, l); });
  if (next_lanelet_itr == next_lanelets.cend()) {
    return std::nullopt;
  }
  return *next_lanelet_itr;
}

std::vector<std::pair<lanelet::ConstPoints3d, std::pair<double, double>>> get_waypoint_groups(
  const lanelet::LaneletSequence & lanelet_sequence, const lanelet::LaneletMap & lanelet_map,
  const double group_separation_threshold, const double interval_margin_ratio)
{
  std::vector<std::pair<lanelet::ConstPoints3d, std::pair<double, double>>> waypoint_groups{};

  const auto get_interval_bound =
    [&](const lanelet::ConstPoint3d & point, const double lateral_distance_factor) {
      const auto arc_coordinates = lanelet::geometry::toArcCoordinates(
        lanelet_sequence.centerline2d(), lanelet::utils::to2D(point));
      return arc_coordinates.length + lateral_distance_factor * std::abs(arc_coordinates.distance);
    };

  for (const auto & lanelet : lanelet_sequence) {
    if (!lanelet.hasAttribute("waypoints")) {
      continue;
    }

    const auto waypoints_id = lanelet.attribute("waypoints").asId().value();
    const auto & waypoints = lanelet_map.lineStringLayer.get(waypoints_id);

    if (
      waypoint_groups.empty() ||
      lanelet::geometry::distance2d(waypoint_groups.back().first.back(), waypoints.front()) >
        group_separation_threshold) {
      waypoint_groups.emplace_back().second.first =
        get_interval_bound(waypoints.front(), -interval_margin_ratio);
    }
    waypoint_groups.back().second.second =
      get_interval_bound(waypoints.back(), interval_margin_ratio);

    waypoint_groups.back().first.insert(
      waypoint_groups.back().first.end(), waypoints.begin(), waypoints.end());
  }

  return waypoint_groups;
}

std::vector<geometry_msgs::msg::Point> get_path_bound(
  const lanelet::CompoundLineString2d & lanelet_bound,
  const lanelet::CompoundLineString2d & lanelet_centerline, const double s_start,
  const double s_end)
{
  const auto path_start_point =
    lanelet::geometry::interpolatedPointAtDistance(lanelet_centerline, s_start);
  const auto path_end_point =
    lanelet::geometry::interpolatedPointAtDistance(lanelet_centerline, s_end);

  auto s_bound_start =
    lanelet::geometry::toArcCoordinates(
      lanelet::utils::to2D(lanelet_bound.lineStrings().front()), path_start_point)
      .length;
  auto s_bound_end = lanelet::geometry::toArcCoordinates(lanelet_bound, path_end_point).length;

  std::vector<geometry_msgs::msg::Point> path_bound{};
  auto s = 0.;

  for (auto it = lanelet_bound.begin(); it != std::prev(lanelet_bound.end()); ++it) {
    s += lanelet::geometry::distance2d(*it, *std::next(it));
    if (s < s_bound_start) {
      continue;
    }

    if (path_bound.empty()) {
      const auto interpolated_point =
        lanelet::geometry::interpolatedPointAtDistance(lanelet_bound, s_bound_start);
      path_bound.push_back(
        lanelet::utils::conversion::toGeomMsgPt(lanelet::utils::to3D(interpolated_point)));
    } else {
      path_bound.push_back(lanelet::utils::conversion::toGeomMsgPt(*it));
    }

    if (s >= s_bound_end) {
      const auto interpolated_point =
        lanelet::geometry::interpolatedPointAtDistance(lanelet_bound, s_bound_end);
      path_bound.push_back(
        lanelet::utils::conversion::toGeomMsgPt(lanelet::utils::to3D(interpolated_point)));
      break;
    }
  }

  return path_bound;
}

TurnIndicatorsCommand get_turn_signal(
  const PathWithLaneId & path, const PlannerData & planner_data,
  const geometry_msgs::msg::Pose & current_pose, const double current_vel,
  const double search_distance, const double search_time, const double resampling_interval,
  const double angle_threshold_deg, const double base_link_to_front)
{
  TurnIndicatorsCommand turn_signal;
  turn_signal.command = TurnIndicatorsCommand::NO_COMMAND;

  const lanelet::BasicPoint2d current_point{current_pose.position.x, current_pose.position.y};
  const auto base_search_distance = search_distance + current_vel * search_time;

  std::vector<lanelet::Id> searched_lanelet_ids = {};
  std::optional<double> arc_length_from_vehicle_front_to_lanelet_start = std::nullopt;

  for (const auto & point : path.points) {
    for (const auto & lane_id : point.lane_ids) {
      if (exists(searched_lanelet_ids, lane_id)) {
        continue;
      }
      searched_lanelet_ids.push_back(lane_id);

      const auto lanelet = planner_data.lanelet_map_ptr->laneletLayer.get(lane_id);
      if (!get_next_lanelet_within_route(lanelet, planner_data)) {
        continue;
      }

      if (
        !arc_length_from_vehicle_front_to_lanelet_start &&
        !lanelet::geometry::inside(lanelet, current_point)) {
        continue;
      }

      if (lanelet.hasAttribute("turn_direction")) {
        const auto is_turn_signal_required = [&]() {
          if (arc_length_from_vehicle_front_to_lanelet_start) {  // ego is in front of lanelet
            return arc_length_from_vehicle_front_to_lanelet_start <=
                   lanelet.attributeOr("turn_signal_distance", base_search_distance);
          }

          // ego is already inside lanelet
          const auto required_end_point =
            get_turn_signal_required_end_point(lanelet, resampling_interval, angle_threshold_deg);
          return lanelet::geometry::toArcCoordinates(lanelet.centerline2d(), current_point)
                   .length <=
                 lanelet::geometry::toArcCoordinates(lanelet.centerline2d(), required_end_point)
                   .length;
        }();

        if (is_turn_signal_required) {
          turn_signal.command =
            turn_signal_command_map.at(lanelet.attribute("turn_direction").value());
          return turn_signal;
        }
      }

      const auto lanelet_length = lanelet::utils::getLaneletLength2d(lanelet);
      if (arc_length_from_vehicle_front_to_lanelet_start) {
        *arc_length_from_vehicle_front_to_lanelet_start += lanelet_length;
      } else {
        arc_length_from_vehicle_front_to_lanelet_start =
          lanelet_length -
          lanelet::geometry::toArcCoordinates(lanelet.centerline2d(), current_point).length -
          base_link_to_front;
      }
      break;
    }
  }

  return turn_signal;
}

lanelet::ConstPoint2d get_turn_signal_required_end_point(
  const lanelet::ConstLanelet & lanelet, const double resampling_interval,
  const double angle_threshold_deg)
{
  std::vector<geometry_msgs::msg::Pose> centerline(lanelet.centerline().size());
  for (size_t i = 0; i < lanelet.centerline().size(); ++i) {
    centerline.at(i).position = lanelet::utils::conversion::toGeomMsgPt(lanelet.centerline()[i]);
  }
  autoware::motion_utils::insertOrientation(centerline, true);

  // Create resampling intervals
  const auto lanelet_length = autoware::motion_utils::calcArcLength(centerline);
  std::vector<double> resampled_arclength;
  for (double s = 0.0; s < lanelet_length; s += resampling_interval) {
    resampled_arclength.push_back(s);
  }

  // Insert terminal point
  if (lanelet_length - resampled_arclength.back() < autoware::motion_utils::overlap_threshold) {
    resampled_arclength.back() = lanelet_length;
  } else {
    resampled_arclength.push_back(lanelet_length);
  }

  const auto resampled_centerline =
    autoware::motion_utils::resamplePoseVector(centerline, resampled_arclength);
  const double terminal_yaw = tf2::getYaw(resampled_centerline.back().orientation);

  auto required_end_point = resampled_centerline.back().position;
  for (size_t i = 0; i < resampled_centerline.size(); ++i) {
    const auto yaw = tf2::getYaw(resampled_centerline.at(i).orientation);
    const auto yaw_diff = autoware::universe_utils::normalizeRadian(yaw - terminal_yaw);
    if (std::fabs(yaw_diff) < autoware::universe_utils::deg2rad(angle_threshold_deg)) {
      required_end_point = resampled_centerline.at(i).position;
      break;
    }
  }

  return lanelet::utils::conversion::toLaneletPoint(required_end_point);
}
}  // namespace utils
}  // namespace autoware::path_generator
