#include "vastome/reading_cursor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace vastome {

ReadingCursor::ReadingCursor(std::string_view text, std::vector<double> centers,
                             const double cursor_x,
                             const double hysteresis_px)
    : text_(text), centers_(std::move(centers)), cursor_x_(cursor_x),
      hysteresis_px_(hysteresis_px) {
  if (text_.empty() || centers_.size() != text_.size() ||
      !std::isfinite(cursor_x_) || !std::isfinite(hysteresis_px_) ||
      hysteresis_px_ < 0.0 ||
      !std::all_of(centers_.begin(), centers_.end(),
                   [](double center) { return std::isfinite(center); }) ||
      !std::is_sorted(centers_.begin(), centers_.end())) {
    throw std::invalid_argument("reading cursor requires ordered character centers");
  }
  cells_.resize(text_.size());
  const BrailleEncoder encoder;
  for (const auto& mapped : encoder.encode_mapped(text_)) {
    cells_[mapped.source_index].push_back(mapped.cell);
  }
}

std::vector<ReadingEvent> ReadingCursor::update(const double image_offset_x) {
  if (!std::isfinite(image_offset_x)) {
    throw std::invalid_argument("image offset must be finite");
  }
  std::vector<ReadingEvent> events;
  if (!previous_offset_.has_value()) {
    boundary_ = static_cast<std::size_t>(std::count_if(
        centers_.begin(), centers_.end(), [this, image_offset_x](double center) {
          return center + image_offset_x <= cursor_x_;
        }));
  } else if (image_offset_x < *previous_offset_) {
    while (boundary_ < centers_.size() &&
           centers_[boundary_] + image_offset_x <= cursor_x_ - hysteresis_px_) {
      events.push_back({.character_index = boundary_,
                        .character = text_[boundary_],
                        .direction = ReadingDirection::forward,
                        .cells = cells_[boundary_]});
      ++boundary_;
    }
  } else if (image_offset_x > *previous_offset_) {
    while (boundary_ > 0 &&
           centers_[boundary_ - 1] + image_offset_x >=
               cursor_x_ + hysteresis_px_) {
      --boundary_;
      events.push_back({.character_index = boundary_,
                        .character = text_[boundary_],
                        .direction = ReadingDirection::reverse,
                        .cells = cells_[boundary_]});
    }
  }
  previous_offset_ = image_offset_x;
  return events;
}

std::size_t ReadingCursor::boundary() const { return boundary_; }

}  // namespace vastome
