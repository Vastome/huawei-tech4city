#include "vastome/image_pipeline.hpp"

#include <tesseract/baseapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/geometry/2d.hpp>

namespace vastome {
namespace {

cv::Mat to_grayscale(const cv::Mat& input) {
  if (input.empty()) throw std::invalid_argument("input image is empty");
  cv::Mat gray;
  if (input.channels() == 1) {
    gray = input.clone();
  } else if (input.channels() == 3) {
    cv::cvtColor(input, gray, cv::COLOR_BGR2GRAY);
  } else if (input.channels() == 4) {
    cv::cvtColor(input, gray, cv::COLOR_BGRA2GRAY);
  } else {
    throw std::invalid_argument("unsupported image channel count");
  }
  return gray;
}

cv::Mat fit_for_ocr(const cv::Mat& gray) {
  cv::Mat resized;
  const double scale = gray.rows < 120 ? 120.0 / gray.rows : 1.0;
  cv::resize(gray, resized, {}, scale, scale,
             scale > 1.0 ? cv::INTER_CUBIC : cv::INTER_AREA);
  return resized;
}

double estimate_skew(const cv::Mat& gray) {
  cv::Mat binary;
  cv::threshold(gray, binary, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
  std::vector<cv::Point> foreground;
  cv::findNonZero(binary, foreground);
  if (foreground.size() < 20) return 0.0;
  double angle = cv::minAreaRect(foreground).angle;
  if (angle > 45.0) angle -= 90.0;
  if (angle < -45.0) angle += 90.0;
  return angle;
}

cv::Mat rotate_image(const cv::Mat& input, const double angle) {
  if (std::abs(angle) < 0.15) return input.clone();
  const cv::Point2f center(static_cast<float>(input.cols) / 2.0F,
                           static_cast<float>(input.rows) / 2.0F);
  cv::Mat transform = cv::getRotationMatrix2D(center, angle, 1.0);
  cv::Mat output;
  cv::warpAffine(input, output, transform, input.size(), cv::INTER_CUBIC,
                 cv::BORDER_REPLICATE);
  return output;
}

cv::Mat crop_to_text(const cv::Mat& input) {
  cv::Mat binary;
  cv::threshold(input, binary, 0, 255,
                cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
  std::vector<cv::Point> foreground;
  cv::findNonZero(binary, foreground);
  if (foreground.empty()) return input.clone();
  cv::Rect bounds = cv::boundingRect(foreground);
  const int padding_x = std::max(8, bounds.width / 50);
  const int padding_y = std::max(8, bounds.height / 3);
  bounds.x = std::max(0, bounds.x - padding_x);
  bounds.y = std::max(0, bounds.y - padding_y);
  bounds.width = std::min(input.cols - bounds.x, bounds.width + 2 * padding_x);
  bounds.height = std::min(input.rows - bounds.y, bounds.height + 2 * padding_y);
  return input(bounds).clone();
}

std::string clean_ocr_text(std::string text) {
  std::string output;
  output.reserve(text.size());
  bool pending_space = false;
  for (const char character : text) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (std::isspace(byte) != 0) {
      pending_space = !output.empty();
      continue;
    }
    if (pending_space) {
      output.push_back(' ');
      pending_space = false;
    }
    output.push_back(character);
  }
  return output;
}

ImageQuality assess_quality(const cv::Mat& gray, const double skew) {
  cv::Scalar mean;
  cv::Scalar deviation;
  cv::meanStdDev(gray, mean, deviation);

  cv::Mat laplacian;
  cv::Laplacian(gray, laplacian, CV_64F);
  cv::Scalar laplacian_mean;
  cv::Scalar laplacian_deviation;
  cv::meanStdDev(laplacian, laplacian_mean, laplacian_deviation);

  cv::Mat binary;
  cv::threshold(gray, binary, 0, 255,
                cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
  std::vector<cv::Point> foreground;
  cv::findNonZero(binary, foreground);

  ImageQuality quality;
  quality.brightness = mean[0];
  quality.contrast = deviation[0];
  quality.blur_variance = laplacian_deviation[0] * laplacian_deviation[0];
  quality.skew_degrees = skew;
  quality.ink_ratio = static_cast<double>(cv::countNonZero(binary)) /
                      static_cast<double>(gray.total());

  if (!foreground.empty()) {
    const cv::Rect bounds = cv::boundingRect(foreground);
    const double text_center = bounds.x + bounds.width / 2.0;
    quality.horizontal_offset =
        (text_center - static_cast<double>(gray.cols) / 2.0) /
        (static_cast<double>(gray.cols) / 2.0);
  }

  if (quality.ink_ratio < 0.0015) {
    quality.guidance = Guidance::no_text;
  } else if (quality.brightness < 55.0) {
    quality.guidance = Guidance::too_dark;
  } else if (quality.brightness > 250.0 && quality.contrast < 12.0) {
    quality.guidance = Guidance::too_bright;
  } else if (quality.blur_variance < 28.0) {
    quality.guidance = Guidance::too_blurry;
  } else if (skew > 3.0) {
    quality.guidance = Guidance::rotate_left;
  } else if (skew < -3.0) {
    quality.guidance = Guidance::rotate_right;
  } else if (quality.horizontal_offset > 0.22) {
    quality.guidance = Guidance::move_right;
  } else if (quality.horizontal_offset < -0.22) {
    quality.guidance = Guidance::move_left;
  } else {
    quality.guidance = Guidance::ready;
  }
  return quality;
}

}  // namespace

struct ImagePipeline::Impl {
  tesseract::TessBaseAPI api;

  Impl() {
    if (api.Init(nullptr, "eng", tesseract::OEM_LSTM_ONLY) != 0) {
      throw std::runtime_error("failed to initialize Tesseract English model");
    }
    api.SetPageSegMode(tesseract::PSM_SINGLE_LINE);
    api.SetVariable("preserve_interword_spaces", "1");
    api.SetVariable("user_defined_dpi", "300");
  }

  ~Impl() { api.End(); }
};

ImagePipeline::ImagePipeline() : impl_(std::make_unique<Impl>()) {}
ImagePipeline::~ImagePipeline() = default;
ImagePipeline::ImagePipeline(ImagePipeline&&) noexcept = default;
ImagePipeline& ImagePipeline::operator=(ImagePipeline&&) noexcept = default;

std::vector<cv::Mat> ImagePipeline::preprocess_candidates(
    const cv::Mat& input, ImageQuality* quality) const {
  const cv::Mat gray = fit_for_ocr(to_grayscale(input));
  const double skew = estimate_skew(gray);
  if (quality != nullptr) *quality = assess_quality(gray, skew);

  const cv::Mat deskewed = rotate_image(gray, skew);

  cv::Mat background;
  const int kernel = std::max(31, (std::min(deskewed.rows, deskewed.cols) / 8) | 1);
  cv::GaussianBlur(deskewed, background, cv::Size(kernel, kernel), 0);
  cv::Mat shadow_balanced;
  cv::divide(deskewed, background, shadow_balanced, 255.0);

  cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
  cv::Mat enhanced;
  clahe->apply(shadow_balanced, enhanced);

  cv::Mat otsu;
  cv::threshold(enhanced, otsu, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

  cv::Mat adaptive;
  cv::adaptiveThreshold(enhanced, adaptive, 255,
                        cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 31,
                        11);

  return {crop_to_text(enhanced), crop_to_text(otsu), crop_to_text(adaptive)};
}

OcrResult ImagePipeline::recognize_line(const cv::Mat& input) {
  const auto started = std::chrono::steady_clock::now();
  ImageQuality quality;
  std::vector<cv::Mat> candidates = preprocess_candidates(input, &quality);
  static const std::vector<std::string> names = {"clahe", "otsu", "adaptive"};

  OcrResult best;
  best.confidence = -1.0;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const cv::Mat& candidate = candidates[index];
    impl_->api.SetImage(candidate.data, candidate.cols, candidate.rows,
                        candidate.channels(),
                        static_cast<int>(candidate.step));
    impl_->api.SetSourceResolution(300);
    std::unique_ptr<char[]> raw(impl_->api.GetUTF8Text());
    const std::string text = raw ? clean_ocr_text(raw.get()) : std::string{};
    const double confidence = std::max(0, impl_->api.MeanTextConf()) / 100.0;
    const double selection_score = confidence + (text.empty() ? -1.0 : 0.0);
    if (selection_score > best.confidence) {
      best.text = text;
      best.confidence = selection_score;
      best.preprocessing_variant = names[index];
      best.normalized_line = candidate.clone();
    }
    impl_->api.Clear();
  }
  best.confidence = std::clamp(best.confidence, 0.0, 1.0);
  best.quality = quality;
  const auto finished = std::chrono::steady_clock::now();
  best.elapsed_ms =
      std::chrono::duration<double, std::milli>(finished - started).count();
  return best;
}

std::string ImagePipeline::guidance_message(const Guidance guidance) {
  switch (guidance) {
    case Guidance::ready: return "ready";
    case Guidance::move_left: return "move device left";
    case Guidance::move_right: return "move device right";
    case Guidance::rotate_left: return "rotate device left";
    case Guidance::rotate_right: return "rotate device right";
    case Guidance::too_dark: return "add more light";
    case Guidance::too_bright: return "reduce glare";
    case Guidance::too_blurry: return "hold steady or move closer";
    case Guidance::no_text: return "no printed line detected";
  }
  return "unknown";
}

}  // namespace vastome
