#include "vastome/line_motion.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>
#if __has_include(<opencv2/features.hpp>)
#include <opencv2/features.hpp>
#endif
#include <opencv2/video/tracking.hpp>

namespace vastome {
namespace {

cv::Mat grayscale(const cv::Mat& frame) {
  if (frame.empty()) throw std::invalid_argument("motion frame is empty");
  cv::Mat gray;
  if (frame.channels() == 1) gray = frame.clone();
  else if (frame.channels() == 3) cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
  else if (frame.channels() == 4) cv::cvtColor(frame, gray, cv::COLOR_BGRA2GRAY);
  else throw std::invalid_argument("unsupported motion frame channel count");
  if (gray.depth() != CV_8U) throw std::invalid_argument("motion frame must be 8-bit");
  return gray;
}

double median(std::vector<double> values) {
  const std::size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle),
                   values.end());
  const double upper = values[middle];
  if (values.size() % 2 != 0) return upper;
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle - 1),
                   values.begin() + static_cast<std::ptrdiff_t>(middle));
  return (upper + values[middle - 1]) / 2.0;
}

}  // namespace

LineMotionTracker::LineMotionTracker(const cv::Mat& first_frame)
    : previous_gray_(grayscale(first_frame)) {}

MotionEstimate LineMotionTracker::update(const cv::Mat& frame) {
  cv::Mat current = grayscale(frame);
  if (current.size() != previous_gray_.size()) {
    throw std::invalid_argument("motion frames must have the same dimensions");
  }

  std::vector<cv::Point2f> points;
  cv::goodFeaturesToTrack(previous_gray_, points, 240, 0.01, 4.0);
  MotionEstimate result;
  if (points.size() < 8) {
    previous_gray_ = std::move(current);
    return result;
  }

  std::vector<cv::Point2f> moved, returned;
  std::vector<unsigned char> forward_ok, backward_ok;
  std::vector<float> errors;
  cv::calcOpticalFlowPyrLK(previous_gray_, current, points, moved,
                           forward_ok, errors);
  cv::calcOpticalFlowPyrLK(current, previous_gray_, moved, returned,
                           backward_ok, errors);

  std::vector<double> dx, dy;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (!forward_ok[i] || !backward_ok[i] ||
        cv::norm(points[i] - returned[i]) > 1.5) continue;
    dx.push_back(static_cast<double>(moved[i].x - points[i].x));
    dy.push_back(static_cast<double>(moved[i].y - points[i].y));
  }
  previous_gray_ = std::move(current);
  if (dx.size() < 8) return result;

  const double center_x = median(dx);
  const double center_y = median(dy);
  int inliers = 0;
  for (std::size_t i = 0; i < dx.size(); ++i) {
    if (std::abs(dx[i] - center_x) <= 2.5 &&
        std::abs(dy[i] - center_y) <= 2.5) ++inliers;
  }
  result.tracked_points = inliers;
  result.delta_x = center_x;
  result.delta_y = center_y;
  result.valid = inliers >= 8 &&
                 std::abs(center_y) <= std::max(6.0, std::abs(center_x) * 0.5);
  return result;
}

}  // namespace vastome
