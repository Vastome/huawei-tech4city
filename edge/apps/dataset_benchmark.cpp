#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <opencv2/imgcodecs.hpp>

#include "vastome/accuracy.hpp"
#include "vastome/image_pipeline.hpp"

namespace {

std::string csv_escape(const std::string& value) {
  std::string output = "\"";
  for (const char character : value) {
    if (character == '"') output.push_back('"');
    output.push_back(character);
  }
  return output + '"';
}

void usage() {
  std::cerr << "Usage: vastome_dataset_benchmark MANIFEST.tsv OUTPUT.csv\n"
            << "Each non-comment line: relative/or/absolute/image.jpg<TAB>expected text\n";
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc != 3) {
    usage();
    return EXIT_FAILURE;
  }
  const std::filesystem::path manifest_path = argv[1];
  std::ifstream manifest(manifest_path);
  if (!manifest) {
    std::cerr << "Could not open manifest: " << manifest_path << '\n';
    return EXIT_FAILURE;
  }
  std::ofstream report(argv[2]);
  if (!report) {
    std::cerr << "Could not create report: " << argv[2] << '\n';
    return EXIT_FAILURE;
  }
  report << "image,expected,recognized,confidence,accuracy,edits,latency_ms,variant,guidance\n";

  vastome::ImagePipeline pipeline;
  std::size_t samples = 0;
  std::size_t unreadable = 0;
  std::size_t perfect = 0;
  double accuracy_sum = 0.0;
  double latency_sum = 0.0;
  std::string line;
  while (std::getline(manifest, line)) {
    if (line.empty() || line.front() == '#') continue;
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
      std::cerr << "Skipping malformed manifest row: " << line << '\n';
      ++unreadable;
      continue;
    }
    std::filesystem::path image_path = line.substr(0, tab);
    if (image_path.is_relative()) image_path = manifest_path.parent_path() / image_path;
    const std::string expected = line.substr(tab + 1);
    const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
    if (image.empty()) {
      std::cerr << "Could not read: " << image_path << '\n';
      ++unreadable;
      continue;
    }
    const vastome::OcrResult result = pipeline.recognize_line(image);
    const vastome::AccuracyScore score =
        vastome::character_accuracy(expected, result.text);
    ++samples;
    if (score.edits == 0) ++perfect;
    accuracy_sum += score.character_accuracy;
    latency_sum += result.elapsed_ms;
    report << csv_escape(image_path.string()) << ',' << csv_escape(expected)
           << ',' << csv_escape(result.text) << ',' << result.confidence << ','
           << score.character_accuracy << ',' << score.edits << ','
           << result.elapsed_ms << ',' << result.preprocessing_variant << ','
           << csv_escape(vastome::ImagePipeline::guidance_message(
                  result.quality.guidance))
           << '\n';
    std::cout << std::setw(4) << samples << "  " << std::fixed
              << std::setprecision(1) << score.character_accuracy * 100.0
              << "%  " << image_path.filename().string() << " -> "
              << result.text << '\n';
  }
  if (samples == 0) {
    std::cerr << "No readable samples were evaluated.\n";
    return EXIT_FAILURE;
  }
  std::cout << "\nReal dataset summary: samples=" << samples
            << ", perfect=" << perfect << '/' << samples
            << ", mean character accuracy=" << std::fixed
            << std::setprecision(1)
            << accuracy_sum / static_cast<double>(samples) * 100.0
            << "%, mean latency="
            << latency_sum / static_cast<double>(samples) << " ms"
            << ", unreadable rows=" << unreadable << '\n';
  return unreadable == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
