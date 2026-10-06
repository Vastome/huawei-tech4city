#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "vastome/braille.hpp"
#include "vastome/image_pipeline.hpp"
#include "vastome/reading_cursor.hpp"
#include "vastome/simulator.hpp"
#include "vastome/synthetic.hpp"

namespace {

struct Arguments {
  std::string image_path;
  std::string synthetic_text;
  std::filesystem::path trace_path;
  std::filesystem::path output_path{"edge/out/position"};
  std::uint16_t hold_ms{350};
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
    else if (flag == "--trace") args.trace_path = value();
    else if (flag == "--output") args.output_path = value();
    else if (flag == "--hold-ms") {
      const unsigned long parsed = std::stoul(value());
      if (parsed == 0 || parsed > 65535) {
        throw std::runtime_error("--hold-ms must be between 1 and 65535");
      }
      args.hold_ms = static_cast<std::uint16_t>(parsed);
    } else if (flag == "--help") {
      std::cout << "Usage: vastome_position_sim (--synthetic TEXT | --image LINE.png) "
                   "[--trace offsets.tsv] [--output DIR] [--hold-ms N]\n"
                   "Trace: one image offset in pixels per line, or time_ms<TAB>offset_px.\n";
      std::exit(0);
    } else throw std::runtime_error("unknown argument: " + flag);
  }
  if (args.image_path.empty() == args.synthetic_text.empty()) {
    throw std::runtime_error("choose exactly one of --image or --synthetic");
  }
  return args;
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
                          const std::vector<std::vector<vastome::ReadingEvent>>& steps) {
  std::ifstream input(template_path);
  if (!input) throw std::runtime_error("cannot open viewer template: " + template_path.string());
  std::string html((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  std::ostringstream data;
  data << "{\"text\":" << json_escape(ocr.text)
       << ",\"confidence\":" << ocr.confidence
       << ",\"imageWidth\":" << ocr.normalized_line.cols
       << ",\"imageHeight\":" << ocr.normalized_line.rows
       << ",\"cursorX\":550,\"boxes\":[";
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
    const cv::Mat input = args.image_path.empty()
        ? vastome::render_synthetic_line(args.synthetic_text)
        : cv::imread(args.image_path, cv::IMREAD_COLOR);
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
    const std::vector<double> offsets = args.trace_path.empty()
        ? default_trace(centers, cursor_x, hysteresis)
        : read_trace(args.trace_path);

    std::filesystem::create_directories(args.output_path);
    if (!cv::imwrite((args.output_path / "line.png").string(), ocr.normalized_line)) {
      throw std::runtime_error("could not write OCR line image");
    }
    std::ofstream csv(args.output_path / "events.csv");
    csv << "step,offset_px,direction,char_index,character,braille_bits,cells_delivered,retries,failures\n";
    vastome::VirtualEsp32 device;
    vastome::SimulatedSerialLink link(device);
    vastome::BrailleStreamer streamer(link);
    std::vector<std::vector<vastome::ReadingEvent>> steps;
    std::size_t event_count = 0;
    std::size_t failure_count = 0;
    for (std::size_t step = 0; step < offsets.size(); ++step) {
      const auto events = reader.update(offsets[step]);
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
        csv << step << ',' << offsets[step] << ',' << direction << ','
            << event.character_index << ',' << csv_escape(std::string(1, event.character))
            << ',' << csv_escape(bits) << ',' << stats.cells_sent << ','
            << stats.retries << ',' << stats.failures << '\n';
      }
      steps.push_back(events);
    }
    std::filesystem::path template_path =
        std::filesystem::absolute(argv[0]).parent_path() / "position_viewer.html";
    if (!std::filesystem::exists(template_path)) {
      template_path = "edge/assets/position_viewer.html";
    }
    std::ofstream html(args.output_path / "index.html");
    html << render_viewer(template_path, ocr, offsets, steps);
    std::cout << "OCR: " << ocr.text << " (" << ocr.confidence * 100.0
              << "% confidence)\n"
              << "Trace: " << offsets.size() << " positions, " << event_count
              << " character crossings, " << failure_count << " delivery failures\n"
              << "Open " << (args.output_path / "index.html") << '\n';
    return failure_count == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
