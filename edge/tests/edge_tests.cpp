#include <cstdlib>
#include <iostream>
#include <string>

#include "vastome/accuracy.hpp"
#include "vastome/braille.hpp"
#include "vastome/image_pipeline.hpp"
#include "vastome/protocol.hpp"
#include "vastome/simulator.hpp"
#include "vastome/synthetic.hpp"

namespace {

int failures = 0;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

}  // namespace

int main() {
  const vastome::BrailleEncoder encoder;
  const auto cells = encoder.encode("A12 b");
  check(cells.size() == 7, "capital and number indicators are emitted");
  check(cells[0].bit_string() == "000001", "capital indicator is dot 6");
  check(cells[1].bit_string() == "100000", "A is dot 1");
  check(cells[2].bit_string() == "001111", "number sign is dots 3456");
  check(cells[3].bit_string() == "100000", "digit 1 uses letter a");
  check(cells[4].bit_string() == "110000", "digit 2 uses letter b");
  check(cells[5].bits() == 0, "space lowers all dots");

  const vastome::BrailleFrame frame{.sequence = 42, .bits = 0x25, .hold_ms = 300};
  const std::string wire = vastome::serialize_frame(frame);
  std::string error;
  const auto parsed = vastome::parse_frame(wire, &error);
  check(parsed.has_value(), "valid protocol frame parses");
  check(parsed.has_value() && parsed->sequence == frame.sequence &&
            parsed->bits == frame.bits && parsed->hold_ms == frame.hold_ms,
        "protocol frame round trips");
  std::string corrupted = wire;
  corrupted[8] = corrupted[8] == '0' ? '1' : '0';
  check(!vastome::parse_frame(corrupted, &error).has_value(),
        "CRC rejects corruption");

  vastome::VirtualEsp32 device;
  vastome::SimulatedSerialLink link(device, {.drop_every = 2});
  vastome::BrailleStreamer streamer(link, 2);
  const auto stream = streamer.stream(encoder.encode("ab"), 50);
  check(stream.cells_sent == 2 && stream.retries == 1 && stream.failures == 0,
        "streamer retries a dropped packet");

  const auto score = vastome::character_accuracy("Hello  World", "hello world");
  check(score.edits == 0 && score.character_accuracy == 1.0,
        "accuracy normalization ignores case and repeated whitespace");

  vastome::ImagePipeline pipeline;
  const cv::Mat image = vastome::render_synthetic_line("BRAILLE OPENS BOOKS");
  const vastome::OcrResult ocr = pipeline.recognize_line(image);
  const auto ocr_score =
      vastome::character_accuracy("BRAILLE OPENS BOOKS", ocr.text);
  check(ocr_score.character_accuracy >= 0.95,
        "OCR recognizes a clear printed line at >=95% character accuracy");
  check(ocr.elapsed_ms > 0.0, "OCR latency is measured");

  if (failures == 0) {
    std::cout << "All edge tests passed.\n";
    return EXIT_SUCCESS;
  }
  std::cerr << failures << " test(s) failed.\n";
  return EXIT_FAILURE;
}
