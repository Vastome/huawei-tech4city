#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "vastome/braille.hpp"
#include "vastome/image_pipeline.hpp"
#include "vastome/line_motion.hpp"
#include "vastome/reading_cursor.hpp"
#include "vastome/simulator.hpp"
#include "vastome/synthetic.hpp"

namespace {

struct Arguments {
  std::string image_path;
  std::string synthetic_text;
  std::string synthetic_motion_text;
  std::string video_path;
  std::optional<int> camera_index;
  std::optional<cv::Rect> roi;
  std::filesystem::path trace_path;
  std::filesystem::path output_path{"edge/out/position"};
  std::uint16_t hold_ms{350};
  std::size_t max_frames{300};
};

Arguments parse_arguments(int argc, char** argv) {
  Arguments args;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + flag);
      return argv[++i];
    };
    if (flag == "--image") args.image_path = value();
    else if (flag == "--synthetic") args.synthetic_text = value();
    else if (flag == "--synthetic-motion") args.synthetic_motion_text = value();
    else if (flag == "--video") args.video_path = value();
    else if (flag == "--camera") {
      const std::string raw = value();
      std::size_t consumed = 0;
      const int index = std::stoi(raw, &consumed);
      if (consumed != raw.size() || index < 0) {
        throw std::runtime_error("--camera must be a nonnegative index");
      }
      args.camera_index = index;
    }
    else if (flag == "--roi") {
      const std::string raw = value();
      int x = 0, y = 0, width = 0, height = 0;
      char trailing = '\0';
      if (std::sscanf(raw.c_str(), "%d,%d,%d,%d%c", &x, &y, &width,
                      &height, &trailing) != 4 ||
          x < 0 || y < 0 || width <= 0 || height <= 0) {
        throw std::runtime_error("--roi must be x,y,width,height with positive size");
      }
      args.roi = cv::Rect(x, y, width, height);
    }
    else if (flag == "--trace") args.trace_path = value();
    else if (flag == "--output") args.output_path = value();
    else if (flag == "--max-frames") {
      const std::string raw = value();
      std::size_t consumed = 0;
      const unsigned long count = std::stoul(raw, &consumed);
      if (consumed != raw.size() || count < 2 || count > 100000) {
        throw std::runtime_error("--max-frames must be between 2 and 100000");
      }
      args.max_frames = count;
    }
    else if (flag == "--hold-ms") {
      const unsigned long parsed = std::stoul(value());
      if (parsed == 0 || parsed > 65535) {
        throw std::runtime_error("--hold-ms must be between 1 and 65535");
      }
      args.hold_ms = static_cast<std::uint16_t>(parsed);
    } else if (flag == "--help") {
      std::cout << "Usage: vastome_position_sim (--synthetic TEXT | "
                   "--synthetic-motion TEXT | --image LINE.png | "
                   "--video LINE.mp4 | --camera INDEX) [--roi x,y,w,h] "
                   "[--trace offsets.tsv] [--max-frames N] "
                   "[--output DIR] [--hold-ms N]\n"
                   "Video/camera: first frame OCR, then optical-flow motion tracking. "
                   "Keep one line inside the crop.\n"
                   "Trace: one image offset in pixels per line, or time_ms<TAB>offset_px.\n";
      std::exit(0);
    } else throw std::runtime_error("unknown argument: " + flag);
  }
  const int input_count = static_cast<int>(!args.image_path.empty()) +
                          static_cast<int>(!args.synthetic_text.empty()) +
                          static_cast<int>(!args.synthetic_motion_text.empty()) +
                          static_cast<int>(!args.video_path.empty()) +
                          static_cast<int>(args.camera_index.has_value());
  if (input_count != 1) {
    throw std::runtime_error("choose exactly one of --image, --synthetic, --synthetic-motion, --video or --camera");
  }
  if (!args.trace_path.empty() &&
      (!args.video_path.empty() || args.camera_index.has_value() ||
       !args.synthetic_motion_text.empty())) {
    throw std::runtime_error("--trace cannot be combined with motion capture modes");
  }
  if (args.roi.has_value() && args.video_path.empty() && !args.camera_index.has_value()) {
    throw std::runtime_error("--roi requires --video or --camera");
  }
  return args;
}

cv::Mat crop_frame(const cv::Mat& frame, const std::optional<cv::Rect>& roi) {
  if (frame.empty()) throw std::runtime_error("camera/video yielded an empty frame");
  if (!roi.has_value()) return frame.clone();
  if (roi->width > frame.cols || roi->height > frame.rows ||
      roi->x > frame.cols - roi->width || roi->y > frame.rows - roi->height) {
    throw std::runtime_error("--roi extends beyond the camera/video frame");
  }
  return frame(*roi).clone();
}

std::vector<double> read_trace(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open trace: " + path.string());
  std::vector<double> offsets;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto separator = line.find('\t');
    const std::string value = separator == std::string::npos
                                  ? line : line.substr(separator + 1);
    std::size_t consumed = 0;
    const double offset = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(offset)) {
      throw std::runtime_error("invalid trace offset: " + line);
    }
    offsets.push_back(offset);
  }
  if (offsets.size() < 2) {
    throw std::runtime_error("trace needs at least two positions");
  }
  return offsets;
}

std::vector<double> default_trace(const std::vector<double>& centers,
                                  double cursor_x, double hysteresis) {
  std::vector<double> offsets;
  offsets.push_back(cursor_x - centers.front() + hysteresis + 12.0);
  for (double center : centers) {
    const double crossed = cursor_x - center - hysteresis - 2.0;
    offsets.push_back(crossed);
    offsets.push_back(crossed);       // pause
    offsets.push_back(crossed + 2.0); // small reverse jitter
    offsets.push_back(crossed);
  }
  const std::size_t stop = centers.size() > 3 ? centers.size() - 3 : 0;
  for (std::size_t i = centers.size(); i > stop; --i) {
    offsets.push_back(cursor_x - centers[i - 1] + hysteresis + 2.0);
  }
  return offsets;
}

std::string json_escape(const std::string& value) {
  std::string output = "\"";
  for (unsigned char c : value) {
    if (c == '<') { output += "\\u003c"; continue; }
    if (c == '>') { output += "\\u003e"; continue; }
    if (c == '&') { output += "\\u0026"; continue; }
    if (c == '"' || c == '\\') output.push_back('\\');
    if (c < 32) {
      if (c == '\n') output += "\\n";
      else if (c == '\r') output += "\\r";
      else if (c == '\t') output += "\\t";
      else output += ' ';
    } else output.push_back(static_cast<char>(c));
  }
  return output + '"';
}

std::string csv_escape(const std::string& value) {
  std::string output = "\"";
  for (char c : value) {
    if (c == '"') output.push_back('"');
    output.push_back(c);
  }
  return output + '"';
}

void write_cell_json(std::ostream& output, const vastome::BrailleCell& cell) {
  output << "{\"bits\":" << json_escape(cell.bit_string())
         << ",\"unicode\":" << json_escape(cell.unicode())
         << ",\"indicator\":" << (cell.indicator ? "true" : "false")
         << '}';
}

std::string render_viewer(const std::filesystem::path& template_path,
                          const vastome::OcrResult& ocr,
                          const std::vector<double>& offsets,
                          const std::vector<std::vector<vastome::ReadingEvent>>& steps,
                          double frame_interval_ms) {
  std::ifstream input(template_path);
  if (!input) throw std::runtime_error("cannot open viewer template: " + template_path.string());
  std::string html((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  std::ostringstream data;
  data << "{\"text\":" << json_escape(ocr.text)
       << ",\"confidence\":" << ocr.confidence
       << ",\"imageWidth\":" << ocr.normalized_line.cols
       << ",\"imageHeight\":" << ocr.normalized_line.rows
       << ",\"cursorX\":550,\"frameIntervalMs\":" << frame_interval_ms
       << ",\"boxes\":[";
  for (std::size_t i = 0; i < ocr.character_boxes.size(); ++i) {
    if (i) data << ',';
    const auto& box = ocr.character_boxes[i];
    data << "{\"x\":" << box.x << ",\"y\":" << box.y
         << ",\"width\":" << box.width << ",\"height\":" << box.height
         << ",\"character\":" << json_escape(std::string(1, ocr.text[i])) << '}';
  }
  data << "],\"offsets\":[";
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    if (i) data << ',';
    data << offsets[i];
  }
  data << "],\"ocrLatencyMs\":" << ocr.elapsed_ms
       << ",\"encoded\":[";
  std::vector<std::vector<vastome::BrailleCell>> encoded(ocr.text.size());
  const vastome::BrailleEncoder encoder;
  for (const auto& entry : encoder.encode_mapped(ocr.text)) {
    encoded[entry.source_index].push_back(entry.cell);
  }
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (i) data << ',';
    data << '[';
    for (std::size_t cell = 0; cell < encoded[i].size(); ++cell) {
      if (cell) data << ',';
      write_cell_json(data, encoded[i][cell]);
    }
    data << ']';
  }
  data << "],\"steps\":[";
  for (std::size_t step = 0; step < steps.size(); ++step) {
    if (step) data << ',';
    data << '[';
    for (std::size_t i = 0; i < steps[step].size(); ++i) {
      if (i) data << ',';
      const auto& event = steps[step][i];
      data << "{\"index\":" << event.character_index
           << ",\"character\":" << json_escape(std::string(1, event.character))
           << ",\"direction\":"
           << json_escape(event.direction == vastome::ReadingDirection::forward
                              ? "forward" : "reverse") << ",\"cells\":[";
      for (std::size_t cell = 0; cell < event.cells.size(); ++cell) {
        if (cell) data << ',';
        write_cell_json(data, event.cells[cell]);
      }
      data << "]}";
    }
    data << ']';
  }
  data << "]}";
  const std::string marker = "__VASTOME_DATA__";
  const std::size_t at = html.find(marker);
  if (at == std::string::npos) throw std::runtime_error("viewer template data marker is missing");
  html.replace(at, marker.size(), data.str());
  return html;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Arguments args = parse_arguments(argc, argv);
    const bool synthetic_motion = !args.synthetic_motion_text.empty();
    const bool capture_mode = !args.video_path.empty() || args.camera_index.has_value() ||
                              synthetic_motion;
    cv::VideoCapture capture;
    double frame_interval_ms = 0.0;
    cv::Mat input;
    if (synthetic_motion) {
      input = vastome::render_synthetic_line(args.synthetic_motion_text);
      frame_interval_ms = 50.0;
    } else if (capture_mode) {
      const bool opened = args.camera_index.has_value()
          ? capture.open(*args.camera_index) : capture.open(args.video_path);
      if (!opened) throw std::runtime_error("could not open camera/video source");
      const double fps = capture.get(cv::CAP_PROP_FPS);
      frame_interval_ms = std::isfinite(fps) && fps >= 1.0 && fps <= 120.0
          ? 1000.0 / fps : 33.3;
      cv::Mat first_frame;
      if (!capture.read(first_frame)) throw std::runtime_error("camera/video has no frames");
      input = crop_frame(first_frame, args.roi);
    } else {
      input = args.image_path.empty()
          ? vastome::render_synthetic_line(args.synthetic_text)
          : cv::imread(args.image_path, cv::IMREAD_COLOR);
    }
    if (input.empty()) throw std::runtime_error("could not read input image");
    vastome::ImagePipeline pipeline;
    const vastome::OcrResult ocr = pipeline.recognize_line(input);
    if (ocr.text.empty() || ocr.character_boxes.size() != ocr.text.size()) {
      throw std::runtime_error("OCR found no positioned printed characters");
    }
    std::vector<double> centers;
    for (const auto& box : ocr.character_boxes) {
      const double center = box.x + box.width / 2.0;
      centers.push_back(centers.empty() ? center
                    : std::max(center, centers.back() + 0.01));
    }
    const double cursor_x = 550.0;
    const double hysteresis = 6.0;
    vastome::ReadingCursor reader(ocr.text, centers, cursor_x, hysteresis);

    std::filesystem::create_directories(args.output_path);
    if (!cv::imwrite((args.output_path / "line.png").string(), ocr.normalized_line)) {
      throw std::runtime_error("could not write OCR line image");
    }
    std::ofstream csv(args.output_path / "events.csv");
    if (!csv) throw std::runtime_error("could not write events.csv");
    csv << "step,offset_px,direction,char_index,character,braille_bits,cells_delivered,retries,failures\n";
    vastome::VirtualEsp32 device;
    vastome::SimulatedSerialLink link(device);
    vastome::BrailleStreamer streamer(link);
    std::vector<double> offsets;
    std::vector<std::vector<vastome::ReadingEvent>> steps;
    std::size_t event_count = 0;
    std::size_t failure_count = 0;
    const auto record_position = [&](const double offset) {
      const std::size_t step = offsets.size();
      offsets.push_back(offset);
      const auto events = reader.update(offset);
      for (const auto& event : events) {
        ++event_count;
        const auto stats = streamer.stream(event.cells, args.hold_ms);
        failure_count += stats.failures;
        std::string bits;
        for (const auto& cell : event.cells) {
          if (!bits.empty()) bits += ' ';
          bits += cell.bit_string();
        }
        const std::string direction = event.direction == vastome::ReadingDirection::forward
                                          ? "forward" : "reverse";
        csv << step << ',' << offset << ',' << direction << ','
            << event.character_index << ',' << csv_escape(std::string(1, event.character))
            << ',' << csv_escape(bits) << ',' << stats.cells_sent << ','
            << stats.retries << ',' << stats.failures << '\n';
        if (capture_mode) {
          std::cout << direction << "  " << event.character << "  ";
          for (const auto& cell : event.cells) std::cout << cell.unicode();
          std::cout << "  (frame " << step << ")\n" << std::flush;
        }
      }
      steps.push_back(events);
    };
    std::size_t lost_frames = 0;
    if (capture_mode) {
      const double scale = std::max(1.0, 120.0 / static_cast<double>(input.rows));
      double offset = cursor_x - centers.front() + hysteresis + 12.0;
      vastome::LineMotionTracker tracker(input);
      record_position(offset);
      std::cout << "OCR: " << ocr.text << " (" << ocr.confidence * 100.0
                << "% confidence). Tracking one line for up to "
                << args.max_frames << " frames...\n" << std::flush;
      const auto track_frame = [&](const cv::Mat& frame) {
        const auto motion = tracker.update(frame);
        if (motion.valid) offset += motion.delta_x * scale;
        else ++lost_frames;
        record_position(offset);
      };
      if (synthetic_motion) {
        const auto leg = static_cast<std::size_t>(std::ceil(
            (centers.back() - centers.front() + 60.0) / 8.0));
        const std::size_t frames = std::min(args.max_frames - 1, 2 * leg);
        for (std::size_t frame_index = 1; frame_index <= frames; ++frame_index) {
          const double shift = -8.0 * static_cast<double>(
              std::min(frame_index, 2 * leg - frame_index));
          cv::Mat transform = cv::Mat::eye(2, 3, CV_64F);
          transform.at<double>(0, 2) = shift;
          cv::Mat frame;
          cv::warpAffine(input, frame, transform, input.size(), cv::INTER_LINEAR,
                         cv::BORDER_CONSTANT, cv::Scalar(245));
          track_frame(frame);
        }
      } else {
        for (std::size_t frame_index = 1;
             frame_index < args.max_frames; ++frame_index) {
          cv::Mat frame;
          if (!capture.read(frame)) break;
          track_frame(crop_frame(frame, args.roi));
        }
      }
    } else {
      const auto replay = args.trace_path.empty()
          ? default_trace(centers, cursor_x, hysteresis)
          : read_trace(args.trace_path);
      for (const double offset : replay) record_position(offset);
    }
    if (offsets.size() < 2) {
      throw std::runtime_error("camera/video needs at least two readable frames");
    }
    std::filesystem::path template_path =
        std::filesystem::absolute(argv[0]).parent_path() / "position_viewer.html";
    if (!std::filesystem::exists(template_path)) {
      template_path = "edge/assets/position_viewer.html";
    }
    std::ofstream html(args.output_path / "index.html");
    if (!html) throw std::runtime_error("could not write index.html");
    html << render_viewer(template_path, ocr, offsets, steps, frame_interval_ms);
    if (!html || !csv) throw std::runtime_error("could not finish writing demo files");
    std::cout << "OCR: " << ocr.text << " (" << ocr.confidence * 100.0
              << "% confidence)\n"
              << "Trace: " << offsets.size() << " positions, " << event_count
              << " character crossings, " << lost_frames << " untracked frames, "
              << failure_count << " delivery failures\n"
              << "Open " << (args.output_path / "index.html") << '\n';
    return failure_count == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
