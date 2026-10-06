#include "vastome/simulator.hpp"

#include <sstream>
#include <utility>

namespace vastome {

void VirtualBrailleCell::apply(const std::uint8_t bits) {
  for (std::size_t index = 0; index < pins_.size(); ++index) {
    pins_[index] = (bits & (1U << index)) != 0;
  }
}

const std::array<bool, 6>& VirtualBrailleCell::pins() const { return pins_; }

std::string VirtualBrailleCell::render() const {
  const auto dot = [this](const std::size_t index) {
    return pins_[index] ? "●" : "○";
  };
  std::ostringstream stream;
  stream << dot(0) << ' ' << dot(3) << '\n'
         << dot(1) << ' ' << dot(4) << '\n'
         << dot(2) << ' ' << dot(5);
  return stream.str();
}

std::string VirtualEsp32::receive(std::string_view wire) {
  std::string error;
  const std::optional<BrailleFrame> frame = parse_frame(wire, &error);
  if (!frame.has_value()) {
    return serialize_reply({.accepted = false,
                            .sequence = last_sequence_,
                            .reason = std::move(error)});
  }
  if (frame->sequence <= last_sequence_) {
    return serialize_reply({.accepted = true,
                            .sequence = frame->sequence,
                            .reason = "duplicate"});
  }
  cell_.apply(frame->bits);
  last_sequence_ = frame->sequence;
  return serialize_reply({.accepted = true,
                          .sequence = frame->sequence,
                          .reason = "applied"});
}

const VirtualBrailleCell& VirtualEsp32::cell() const { return cell_; }

SimulatedSerialLink::SimulatedSerialLink(VirtualEsp32& device,
                                         const LinkFaults faults)
    : device_(device), faults_(faults) {}

std::string SimulatedSerialLink::exchange(std::string wire) {
  ++transmissions_;
  LinkEvent event{.transmission = transmissions_, .request = wire};
  if (faults_.drop_every != 0 && transmissions_ % faults_.drop_every == 0) {
    event.dropped = true;
    events_.push_back(std::move(event));
    return {};
  }
  if (faults_.corrupt_every != 0 &&
      transmissions_ % faults_.corrupt_every == 0 && wire.size() > 8) {
    wire[8] = wire[8] == '0' ? '1' : '0';
    event.corrupted = true;
    event.request = wire;
  }
  event.response = device_.receive(wire);
  event.device_state = device_.cell().render();
  events_.push_back(event);
  return event.response;
}

std::size_t SimulatedSerialLink::transmissions() const {
  return transmissions_;
}

const std::vector<LinkEvent>& SimulatedSerialLink::events() const {
  return events_;
}

BrailleStreamer::BrailleStreamer(FrameTransport& link,
                                 const std::size_t maximum_retries)
    : link_(link), maximum_retries_(maximum_retries) {}

StreamStats BrailleStreamer::stream(const std::vector<BrailleCell>& cells,
                                    const std::uint16_t hold_ms,
                                    CellCallback callback) {
  StreamStats stats;
  for (const BrailleCell& cell : cells) {
    const std::uint32_t sequence = next_sequence_++;
    bool delivered = false;
    for (std::size_t attempt = 0; attempt <= maximum_retries_; ++attempt) {
      const std::string response = link_.exchange(serialize_frame(
          {.sequence = sequence, .bits = cell.bits(), .hold_ms = hold_ms}));
      const std::optional<DeviceReply> reply = parse_reply(response);
      delivered = reply.has_value() && reply->accepted &&
                  reply->sequence == sequence;
      if (delivered) break;
      if (attempt < maximum_retries_) ++stats.retries;
    }
    if (!delivered) {
      ++stats.failures;
      continue;
    }
    ++stats.cells_sent;
    if (callback) {
      VirtualBrailleCell rendered;
      rendered.apply(cell.bits());
      callback(cell, rendered);
    }
  }
  return stats;
}

}  // namespace vastome
