#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "vastome/braille.hpp"
#include "vastome/protocol.hpp"

namespace vastome {

class VirtualBrailleCell {
 public:
  void apply(std::uint8_t bits);
  [[nodiscard]] const std::array<bool, 6>& pins() const;
  [[nodiscard]] std::string render() const;

 private:
  std::array<bool, 6> pins_{};
};

class VirtualEsp32 {
 public:
  [[nodiscard]] std::string receive(std::string_view wire);
  [[nodiscard]] const VirtualBrailleCell& cell() const;

 private:
  VirtualBrailleCell cell_;
  std::uint32_t last_sequence_{};
};

struct LinkFaults {
  std::size_t drop_every{};
  std::size_t corrupt_every{};
};

struct LinkEvent {
  std::size_t transmission{};
  std::string request;
  std::string response;
  std::string device_state;
  bool dropped{false};
  bool corrupted{false};
};

class FrameTransport {
 public:
  virtual ~FrameTransport() = default;
  [[nodiscard]] virtual std::string exchange(std::string wire) = 0;
};

class SimulatedSerialLink final : public FrameTransport {
 public:
  explicit SimulatedSerialLink(VirtualEsp32& device, LinkFaults faults = {});
  [[nodiscard]] std::string exchange(std::string wire) override;
  [[nodiscard]] std::size_t transmissions() const;
  [[nodiscard]] const std::vector<LinkEvent>& events() const;

 private:
  VirtualEsp32& device_;
  LinkFaults faults_;
  std::size_t transmissions_{};
  std::vector<LinkEvent> events_;
};

struct StreamStats {
  std::size_t cells_sent{};
  std::size_t retries{};
  std::size_t failures{};
};

class BrailleStreamer {
 public:
  using CellCallback =
      std::function<void(const BrailleCell&, const VirtualBrailleCell&)>;

  explicit BrailleStreamer(FrameTransport& link,
                           std::size_t maximum_retries = 2);
  [[nodiscard]] StreamStats stream(const std::vector<BrailleCell>& cells,
                                   std::uint16_t hold_ms,
                                   CellCallback callback = {});

 private:
  FrameTransport& link_;
  std::size_t maximum_retries_;
  std::uint32_t next_sequence_{1};
};

}  // namespace vastome
