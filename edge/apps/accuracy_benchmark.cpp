#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "vastome/accuracy.hpp"
#include "vastome/image_pipeline.hpp"
#include "vastome/synthetic.hpp"

namespace {

struct TestCase {
  std::string name;
  std::string expected;
  vastome::SyntheticOptions options;
};

std::string csv_escape(const std::string& value) {
  std::string escaped = "\"";
  for (const char character : value) {
    if (character == '"') escaped += '"';
    escaped += character;
  }
  return escaped + '"';
}

}  // namespace

int main(const int argc, char** argv) {
  const std::filesystem::path output_directory =
      argc > 1 ? argv[1] : "edge/out/benchmark";
  std::filesystem::create_directories(output_directory);

  const std::vector<TestCase> cases = {
      {"clear", "BRAILLE OPENS BOOKS", {}},
      {"lowercase", "Hello my name is Long", {}},
      {"rotated_left", "READ AT YOUR OWN PACE", {.rotation_degrees = -4.5}},
      {"rotated_right", "ONE CHARACTER AT A TIME", {.rotation_degrees = 5.0}},
      {"uneven_light", "INDEPENDENT READING", {.shadow_strength = 0.58}},
      {"slight_blur", "TACTILE LITERACY", {.blur_sigma = 1.35}},
      {"low_contrast", "PRINT TO BRAILLE", {.contrast = 0.34}},
      {"sensor_noise", "ACCURATE AND PRIVATE", {.noise_sigma = 11.0}},
      {"combined", "VASTOME EDGE READER",
       {.rotation_degrees = 3.0,
        .shadow_strength = 0.42,
        .blur_sigma = 0.8,
        .contrast = 0.72,
        .noise_sigma = 5.0}},
  };

  std::ofstream csv(output_directory / "results.csv");
  csv << "case,expected,recognized,confidence,accuracy,latency_ms,variant,guidance\n";
  vastome::ImagePipeline pipeline;
  double accuracy_sum = 0.0;
  double latency_sum = 0.0;
  std::size_t perfect = 0;

  std::cout << std::left << std::setw(16) << "CASE" << std::setw(10) << "ACC"
            << std::setw(10) << "CONF" << std::setw(12) << "LATENCY"
            << "RECOGNIZED\n";
  for (const TestCase& test : cases) {
    const cv::Mat image = vastome::render_synthetic_line(test.expected, test.options);
    cv::imwrite((output_directory / (test.name + ".png")).string(), image);
    const vastome::OcrResult result = pipeline.recognize_line(image);
    const vastome::AccuracyScore score =
        vastome::character_accuracy(test.expected, result.text);
    accuracy_sum += score.character_accuracy;
    latency_sum += result.elapsed_ms;
    if (score.edits == 0) ++perfect;

    std::cout << std::left << std::setw(16) << test.name << std::setw(10)
              << std::fixed << std::setprecision(1)
              << score.character_accuracy * 100.0 << std::setw(10)
              << result.confidence * 100.0 << std::setw(12) << result.elapsed_ms
              << result.text << '\n';
    csv << test.name << ',' << csv_escape(test.expected) << ','
        << csv_escape(result.text) << ',' << result.confidence << ','
        << score.character_accuracy << ',' << result.elapsed_ms << ','
        << result.preprocessing_variant << ','
        << csv_escape(vastome::ImagePipeline::guidance_message(
               result.quality.guidance))
        << '\n';
  }
  const double count = static_cast<double>(cases.size());
  std::cout << "\nSummary: mean character accuracy=" << std::fixed
            << std::setprecision(1) << (accuracy_sum / count) * 100.0
            << "%, perfect cases=" << perfect << '/' << cases.size()
            << ", mean latency=" << latency_sum / count << " ms\n"
            << "Evidence saved to " << output_directory << '\n';

  return accuracy_sum / count >= 0.90 ? EXIT_SUCCESS : EXIT_FAILURE;
}
