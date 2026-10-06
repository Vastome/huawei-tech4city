#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include <opencv2/imgcodecs.hpp>

#include "vastome/accuracy.hpp"
#include "vastome/braille.hpp"
#include "vastome/image_pipeline.hpp"
#include "vastome/simulator.hpp"
#include "vastome/synthetic.hpp"

namespace {

struct Arguments {
  std::string image_path;
  std::string synthetic_text;
  std::string expected_text;
  std::uint16_t hold_ms{350};
  std::size_t drop_every{};
  std::size_t corrupt_every{};
  bool quiet_cells{false};
  bool trace{false};
};

void print_usage() {
  std::cout
      << "Usage:\n"
      << "  vastome_edge_sim --image LINE.png [--expected TEXT]\n"
      << "  vastome_edge_sim --synthetic TEXT [--expected TEXT]\n"
      << "Options: --hold-ms N --drop-every N --corrupt-every N --quiet-cells --trace\n";
}

Arguments parse_arguments(const int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string flag(argv[index]);
    const auto value = [&]() -> std::string {
      if (index + 1 >= argc) throw std::runtime_error("missing value for " + flag);
      return argv[++index];
    };
    if (flag == "--image") arguments.image_path = value();
    else if (flag == "--synthetic") arguments.synthetic_text = value();
    else if (flag == "--expected") arguments.expected_text = value();
    else if (flag == "--hold-ms")
      arguments.hold_ms = static_cast<std::uint16_t>(std::stoul(value()));
    else if (flag == "--drop-every")
      arguments.drop_every = std::stoul(value());
    else if (flag == "--corrupt-every")
      arguments.corrupt_every = std::stoul(value());
    else if (flag == "--quiet-cells") arguments.quiet_cells = true;
    else if (flag == "--trace") arguments.trace = true;
    else if (flag == "--help") {
      print_usage();
      std::exit(EXIT_SUCCESS);
    } else {
      throw std::runtime_error("unknown argument: " + flag);
    }
  }
  if (arguments.image_path.empty() == arguments.synthetic_text.empty()) {
    throw std::runtime_error("choose exactly one of --image or --synthetic");
  }
  if (arguments.expected_text.empty() && !arguments.synthetic_text.empty()) {
    arguments.expected_text = arguments.synthetic_text;
  }
  return arguments;
}

}  // namespace

int main(const int argc, char** argv) {
  try {
    const Arguments arguments = parse_arguments(argc, argv);
    cv::Mat image = arguments.image_path.empty()
                        ? vastome::render_synthetic_line(arguments.synthetic_text)
                        : cv::imread(arguments.image_path, cv::IMREAD_COLOR);
    if (image.empty()) throw std::runtime_error("could not read input image");

    if (arguments.trace) {
      std::cout << "[RASPBERRY PI SIM] 1. Captured line image\n";
    }
    vastome::ImagePipeline pipeline;
    const vastome::OcrResult ocr = pipeline.recognize_line(image);
    if (arguments.trace) {
      std::cout << "[RASPBERRY PI SIM] 2. Preprocessed image and completed OCR\n";
    }
    std::cout << std::fixed << std::setprecision(1)
              << "OCR text:       " << ocr.text << '\n'
              << "OCR confidence: " << ocr.confidence * 100.0 << "%\n"
              << "OCR latency:    " << ocr.elapsed_ms << " ms\n"
              << "Best variant:   " << ocr.preprocessing_variant << '\n'
              << "Guidance:       "
              << vastome::ImagePipeline::guidance_message(ocr.quality.guidance)
              << '\n'
              << "Quality:        brightness=" << ocr.quality.brightness
              << " contrast=" << ocr.quality.contrast
              << " blur=" << ocr.quality.blur_variance
              << " skew=" << ocr.quality.skew_degrees << " deg\n";

    if (!arguments.expected_text.empty()) {
      const vastome::AccuracyScore score =
          vastome::character_accuracy(arguments.expected_text, ocr.text);
      std::cout << "Character accuracy: " << score.character_accuracy * 100.0
                << "% (" << score.edits << " edit(s) / "
                << score.reference_characters << ")\n";
    }

    const vastome::BrailleEncoder encoder;
    const std::vector<vastome::BrailleCell> cells = encoder.encode(ocr.text);
    if (arguments.trace) {
      std::cout << "[RASPBERRY PI SIM] 3. Encoded " << cells.size()
                << " Braille cells\n";
    }
    vastome::VirtualEsp32 device;
    vastome::SimulatedSerialLink link(
        device, {.drop_every = arguments.drop_every,
                 .corrupt_every = arguments.corrupt_every});
    vastome::BrailleStreamer streamer(link);
    const vastome::StreamStats stats = streamer.stream(
        cells, arguments.hold_ms,
        arguments.quiet_cells
            ? vastome::BrailleStreamer::CellCallback{}
            : [](const vastome::BrailleCell& cell,
                 const vastome::VirtualBrailleCell& rendered) {
                std::cout << "\n'" << cell.source << "' ["
                          << cell.bit_string() << "]\n"
                          << rendered.render() << '\n';
              });
    std::cout << "\nStream: " << stats.cells_sent << " delivered, "
              << stats.retries << " retries, " << stats.failures
              << " failures\n";
    if (arguments.trace) {
      const auto clean_line = [](std::string value) {
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
          value.pop_back();
        }
        return value;
      };
      std::cout << "\n========== SIMULATED BOARD TRACE ==========\n";
      for (const vastome::LinkEvent& event : link.events()) {
        std::cout << "[PI -> UART] TX #" << event.transmission << ": "
                  << clean_line(event.request) << '\n';
        if (event.dropped) {
          std::cout << "[UART LINK] Packet deliberately dropped; Pi will retry\n";
          continue;
        }
        if (event.corrupted) {
          std::cout << "[UART LINK] Packet deliberately corrupted\n";
        }
        std::cout << "[ESP32 SIM] RX validated; reply: "
                  << clean_line(event.response) << '\n'
                  << "[ESP32 GPIO] Six actuator outputs:\n"
                  << event.device_state << '\n';
      }
      std::cout << "===========================================\n";
    }
    return stats.failures == 0 && !ocr.text.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    print_usage();
    return EXIT_FAILURE;
  }
}
