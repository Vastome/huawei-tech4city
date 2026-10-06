#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

namespace vastome {

enum class Guidance {
  ready,
  move_left,
  move_right,
  rotate_left,
  rotate_right,
  too_dark,
  too_bright,
  too_blurry,
  no_text,
};

struct ImageQuality {
  double brightness{};
  double contrast{};
  double blur_variance{};
  double skew_degrees{};
  double horizontal_offset{};
  double ink_ratio{};
  Guidance guidance{Guidance::no_text};
};

struct OcrResult {
  std::string text;
  double confidence{};
  double elapsed_ms{};
  std::string preprocessing_variant;
  ImageQuality quality;
  cv::Mat normalized_line;
};

class ImagePipeline {
 public:
  ImagePipeline();
  ~ImagePipeline();
  ImagePipeline(const ImagePipeline&) = delete;
  ImagePipeline& operator=(const ImagePipeline&) = delete;
  ImagePipeline(ImagePipeline&&) noexcept;
  ImagePipeline& operator=(ImagePipeline&&) noexcept;

  [[nodiscard]] OcrResult recognize_line(const cv::Mat& input);
  [[nodiscard]] std::vector<cv::Mat> preprocess_candidates(
      const cv::Mat& input, ImageQuality* quality = nullptr) const;
  [[nodiscard]] static std::string guidance_message(Guidance guidance);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vastome
