#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include <opencv2/imgproc.hpp>

#include "vastome/accuracy.hpp"
#include "vastome/braille.hpp"
#include "vastome/image_pipeline.hpp"
#include "vastome/line_motion.hpp"
#include "vastome/protocol.hpp"
#include "vastome/reading_cursor.hpp"
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
  const auto mapped = encoder.encode_mapped("A12");
  check(mapped.size() == 5 && mapped[0].source_index == 0 &&
            mapped[1].source_index == 0 && mapped[2].source_index == 1 &&
            mapped[3].source_index == 1 && mapped[4].source_index == 2,
        "Braille indicators remain attached to their source character");

  vastome::ReadingCursor cursor("A12", {10.0, 30.0, 50.0}, 100.0, 4.0);
  check(cursor.update(100.0).empty(), "initial position emits nothing");
  auto crossings = cursor.update(86.0);
  check(crossings.size() == 1 && crossings[0].character_index == 0 &&
            crossings[0].cells.size() == 2,
        "forward crossing emits capital indicator and character once");
  check(cursor.update(86.0).empty() && cursor.update(90.0).empty() &&
            cursor.update(86.0).empty(),
        "pause and small jitter do not re-emit a character");
  crossings = cursor.update(40.0);
  check(crossings.size() == 2 && crossings[0].character_index == 1 &&
            crossings[1].character_index == 2 &&
            crossings[0].cells.size() == 2 && crossings[1].cells.size() == 1,
        "a skipped position emits all crossed characters in order");
  crossings = cursor.update(76.0);
  check(crossings.size() == 2 &&
            crossings[0].character_index == 2 &&
            crossings[1].character_index == 1 &&
            crossings[0].direction == vastome::ReadingDirection::reverse,
        "reverse reading emits crossed characters in reverse order");
  crossings = cursor.update(95.0);
  check(crossings.size() == 1 && crossings[0].character_index == 0,
        "reverse reading reaches the first character");

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
  check(ocr.character_boxes.size() == ocr.text.size(),
        "OCR returns one position per recognized character");

  const auto shift = [](const cv::Mat& source, double pixels) {
    cv::Mat moved;
    cv::Mat transform = cv::Mat::eye(2, 3, CV_64F);
    transform.at<double>(0, 2) = pixels;
    cv::warpAffine(source, moved, transform, source.size(), cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar(245));
    return moved;
  };
  vastome::LineMotionTracker tracker(image);
  const auto left = tracker.update(shift(image, -12.0));
  check(left.valid && std::abs(left.delta_x + 12.0) < 1.0,
        "optical flow measures forward reading displacement");
  const auto still = tracker.update(shift(image, -12.0));
  check(still.valid && std::abs(still.delta_x) < 0.5,
        "stationary page produces no displacement");
  const auto right = tracker.update(shift(image, -4.0));
  check(right.valid && std::abs(right.delta_x - 8.0) < 1.0,
        "optical flow measures reverse reading displacement");
  const auto lost = tracker.update(cv::Mat(image.size(), image.type(), cv::Scalar(245)));
  check(!lost.valid, "feature loss cannot emit a guessed displacement");

  if (failures == 0) {
    std::cout << "All edge tests passed.\n";
    return EXIT_SUCCESS;
  }
  std::cerr << failures << " test(s) failed.\n";
  return EXIT_FAILURE;
}
