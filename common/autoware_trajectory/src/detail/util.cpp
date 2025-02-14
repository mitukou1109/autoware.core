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

#include "autoware/trajectory/detail/helpers.hpp"

#include <algorithm>
#include <vector>

namespace autoware::trajectory::detail
{
inline namespace helpers
{
std::vector<double> fill_bases(const std::vector<double> & x, const size_t & min_points)
{
  const auto original_size = x.size();

  if (original_size >= min_points) {
    return x;
  }

  const auto points_to_add = min_points - original_size;
  const auto num_gaps = original_size - 1;

  std::vector<size_t> points_per_gap(num_gaps, points_to_add / num_gaps);
  std::fill_n(points_per_gap.begin(), points_to_add % num_gaps, points_per_gap[0] + 1);

  std::vector<double> result;
  result.reserve(min_points);

  for (size_t i = 0; i < original_size - 1; ++i) {
    result.push_back(x[i]);

    const double start = x[i];
    const double end = x[i + 1];
    const double step = (end - start) / static_cast<int16_t>(points_per_gap[i] + 1);

    for (size_t j = 0; j < points_per_gap[i]; ++j) {
      result.push_back(start + static_cast<int16_t>(j + 1) * step);
    }
  }

  result.push_back(x[original_size - 1]);

  return result;
}

std::vector<double> crop_bases(
  const std::vector<double> & x, const double & start, const double & end)
{
  std::vector<double> result;

  // Add start point if it's not already in x
  if (std::find(x.begin(), x.end(), start) == x.end()) {
    result.push_back(start);
  }

  // Copy all points within the range [start, end]
  std::copy_if(x.begin(), x.end(), std::back_inserter(result), [start, end](double i) {
    return i >= start && i <= end;
  });

  // Add end point if it's not already in x
  if (std::find(x.begin(), x.end(), end) == x.end()) {
    result.push_back(end);
  }

  return result;
}
}  // namespace helpers
}  // namespace autoware::trajectory::detail
