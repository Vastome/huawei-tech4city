#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vastome/braille.hpp"

namespace vastome {

enum class ReadingDirection { forward, reverse };

struct ReadingEvent {
  std::size_t character_index{};
  char character{};
  ReadingDirection direction{ReadingDirection::forward};
  std::vector<BrailleCell> cells;
};

// Character centers are in line-image pixels, ordered from left to right.
// A negative image offset moves the printed line left past a fixed cursor.
class ReadingCursor {
 public:
  ReadingCursor(std::string_view text, std::vector<double> centers,
                double cursor_x, double hysteresis_px = 6.0);

  // The first position establishes a baseline and emits nothing.
  [[nodiscard]] std::vector<ReadingEvent> update(double image_offset_x);
  [[nodiscard]] std::size_t boundary() const;

 private:
  std::string text_;
  std::vector<double> centers_;
  std::vector<std::vector<BrailleCell>> cells_;
  double cursor_x_{};
  double hysteresis_px_{};
  std::optional<double> previous_offset_;
  std::size_t boundary_{};
};

}  // namespace vastome
