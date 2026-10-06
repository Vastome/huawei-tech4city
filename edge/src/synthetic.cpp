#include "vastome/synthetic.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <opencv2/imgproc.hpp>
#include <opencv2/geometry/2d.hpp>

namespace vastome {

cv::Mat render_synthetic_line(std::string_view text,
                              const SyntheticOptions& options) {
  constexpr int width = 1600;
  constexpr int height = 260;
  constexpr int margin = 55;
  cv::Mat image(height, width, CV_8UC1, cv::Scalar(245));

  const std::string printable(text);
  const int font = cv::FONT_HERSHEY_SIMPLEX;
  int baseline = 0;
  const cv::Size unit = cv::getTextSize(printable, font, 1.0, 3, &baseline);
  const double scale_x = static_cast<double>(width - 2 * margin) /
                         std::max(1, unit.width);
  const double scale_y = static_cast<double>(height - 2 * margin) /
                         std::max(1, unit.height);
  const double scale = std::clamp(std::min(scale_x, scale_y), 0.55, 2.2);
  const int thickness = std::max(2, static_cast<int>(std::round(scale * 2.0)));
  const cv::Size size =
      cv::getTextSize(printable, font, scale, thickness, &baseline);
  const cv::Point origin((width - size.width) / 2,
                         (height + size.height) / 2 - baseline);
  cv::putText(image, printable, origin, font, scale, cv::Scalar(18), thickness,
              cv::LINE_AA);

  if (std::abs(options.rotation_degrees) > 0.01) {
    const cv::Point2f center(static_cast<float>(width) / 2.0F,
                             static_cast<float>(height) / 2.0F);
    const cv::Mat transform =
        cv::getRotationMatrix2D(center, options.rotation_degrees, 1.0);
    cv::warpAffine(image, image, transform, image.size(), cv::INTER_CUBIC,
                   cv::BORDER_CONSTANT, cv::Scalar(245));
  }

  if (options.shadow_strength > 0.0) {
    const double strength = std::clamp(options.shadow_strength, 0.0, 0.85);
    for (int row = 0; row < image.rows; ++row) {
      auto* pixels = image.ptr<unsigned char>(row);
      for (int column = 0; column < image.cols; ++column) {
        const double position = static_cast<double>(column) /
                                static_cast<double>(image.cols - 1);
        const double lighting = 1.0 - strength * (1.0 - position);
        pixels[column] = static_cast<unsigned char>(
            std::clamp(std::round(pixels[column] * lighting), 0.0, 255.0));
      }
    }
  }

  if (std::abs(options.contrast - 1.0) > 0.001) {
    image.convertTo(image, CV_8U, options.contrast,
                    128.0 * (1.0 - options.contrast));
  }
  if (options.blur_sigma > 0.0) {
    cv::GaussianBlur(image, image, cv::Size(), options.blur_sigma);
  }
  if (options.noise_sigma > 0.0) {
    cv::Mat noise(image.size(), CV_16SC1);
    cv::RNG generator(0x56415354U);
    generator.fill(noise, cv::RNG::NORMAL, 0.0, options.noise_sigma);
    cv::Mat signed_image;
    image.convertTo(signed_image, CV_16SC1);
    signed_image += noise;
    signed_image.convertTo(image, CV_8UC1);
  }
  return image;
}

}  // namespace vastome
