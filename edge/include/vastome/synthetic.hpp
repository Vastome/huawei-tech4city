#pragma once

#include <string_view>

#include <opencv2/core/mat.hpp>

namespace vastome {

struct SyntheticOptions {
  double rotation_degrees{};
  double shadow_strength{};
  double blur_sigma{};
  double contrast{1.0};
  double noise_sigma{};
};

[[nodiscard]] cv::Mat render_synthetic_line(
    std::string_view text, const SyntheticOptions& options = {});

}  // namespace vastome
