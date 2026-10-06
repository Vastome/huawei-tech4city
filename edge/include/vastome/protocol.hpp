#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vastome {

struct BrailleFrame {
  std::uint32_t sequence{};
  std::uint8_t bits{};
  std::uint16_t hold_ms{350};
};

struct DeviceReply {
  bool accepted{false};
  std::uint32_t sequence{};
  std::string reason;
};

[[nodiscard]] std::uint16_t crc16_ccitt(std::string_view data);
[[nodiscard]] std::string serialize_frame(const BrailleFrame& frame);
[[nodiscard]] std::optional<BrailleFrame> parse_frame(std::string_view wire,
                                                       std::string* error);
[[nodiscard]] std::string serialize_reply(const DeviceReply& reply);
[[nodiscard]] std::optional<DeviceReply> parse_reply(std::string_view wire);

}  // namespace vastome
