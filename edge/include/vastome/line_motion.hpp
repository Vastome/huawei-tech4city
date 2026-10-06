#pragma once

#include <opencv2/core/mat.hpp>

namespace vastome {

struct MotionEstimate {
  double delta_x{};
  double delta_y{};
  int tracked_points{};
  bool valid{};
};

// Tracks the printed line between successive, identically cropped frames.
// An invalid frame preserves the last reliable offset in the caller.
class LineMotionTracker {
 public:
  explicit LineMotionTracker(const cv::Mat& first_frame);
  [[nodiscard]] MotionEstimate update(const cv::Mat& frame);

 private:
  cv::Mat previous_gray_;
};

}  // namespace vastome
