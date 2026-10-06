#include "vastome/protocol.hpp"

#include <charconv>
#include <iomanip>
#include <sstream>
#include <vector>

namespace vastome {
namespace {

std::vector<std::string_view> split(std::string_view text, const char delimiter) {
  std::vector<std::string_view> pieces;
  while (true) {
    const std::size_t position = text.find(delimiter);
    pieces.push_back(text.substr(0, position));
    if (position == std::string_view::npos) {
      return pieces;
    }
    text.remove_prefix(position + 1);
  }
}

template <typename Number>
bool parse_number(std::string_view text, Number& output, const int base = 10) {
  const auto [pointer, error] =
      std::from_chars(text.data(), text.data() + text.size(), output, base);
  return error == std::errc{} && pointer == text.data() + text.size();
}

std::string crc_hex(const std::uint16_t crc) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
         << crc;
  return stream.str();
}

}  // namespace

std::uint16_t crc16_ccitt(std::string_view data) {
  std::uint16_t crc = 0xFFFFU;
  for (const char character : data) {
    crc ^= static_cast<std::uint16_t>(
        static_cast<unsigned char>(character) << 8U);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000U) != 0
                ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                : static_cast<std::uint16_t>(crc << 1U);
    }
  }
  return crc;
}

std::string serialize_frame(const BrailleFrame& frame) {
  std::ostringstream payload;
  payload << "BRL|" << frame.sequence << '|' << std::uppercase << std::hex
          << std::setw(2) << std::setfill('0')
          << static_cast<unsigned int>(frame.bits & 0x3FU) << std::dec << '|'
          << frame.hold_ms;
  return payload.str() + '|' + crc_hex(crc16_ccitt(payload.str())) + "\n";
}

std::optional<BrailleFrame> parse_frame(std::string_view wire,
                                        std::string* error) {
  while (!wire.empty() && (wire.back() == '\n' || wire.back() == '\r')) {
    wire.remove_suffix(1);
  }
  const std::vector<std::string_view> fields = split(wire, '|');
  if (fields.size() != 5 || fields[0] != "BRL") {
    if (error != nullptr) *error = "invalid format";
    return std::nullopt;
  }
  const std::size_t checksum_separator = wire.rfind('|');
  const std::string_view payload = wire.substr(0, checksum_separator);
  unsigned int checksum = 0;
  if (!parse_number(fields[4], checksum, 16) || checksum > 0xFFFFU ||
      static_cast<std::uint16_t>(checksum) != crc16_ccitt(payload)) {
    if (error != nullptr) *error = "checksum mismatch";
    return std::nullopt;
  }
  BrailleFrame frame;
  unsigned int bits = 0;
  unsigned int hold_ms = 0;
  if (!parse_number(fields[1], frame.sequence) ||
      !parse_number(fields[2], bits, 16) || bits > 0x3FU ||
      !parse_number(fields[3], hold_ms) || hold_ms > 60000U) {
    if (error != nullptr) *error = "invalid value";
    return std::nullopt;
  }
  frame.bits = static_cast<std::uint8_t>(bits);
  frame.hold_ms = static_cast<std::uint16_t>(hold_ms);
  return frame;
}

std::string serialize_reply(const DeviceReply& reply) {
  std::ostringstream stream;
  stream << (reply.accepted ? "ACK" : "ERR") << '|' << reply.sequence << '|'
         << reply.reason << '\n';
  return stream.str();
}

std::optional<DeviceReply> parse_reply(std::string_view wire) {
  while (!wire.empty() && (wire.back() == '\n' || wire.back() == '\r')) {
    wire.remove_suffix(1);
  }
  const std::vector<std::string_view> fields = split(wire, '|');
  if (fields.size() != 3 || (fields[0] != "ACK" && fields[0] != "ERR")) {
    return std::nullopt;
  }
  DeviceReply reply{.accepted = fields[0] == "ACK",
                    .reason = std::string(fields[2])};
  if (!parse_number(fields[1], reply.sequence)) {
    return std::nullopt;
  }
  return reply;
}

}  // namespace vastome
