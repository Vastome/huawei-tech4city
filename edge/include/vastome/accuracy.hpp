#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace vastome {

struct AccuracyScore {
  std::size_t edits{};
  std::size_t reference_characters{};
  double character_error_rate{};
  double character_accuracy{};
};

std::string normalize_for_comparison(std::string_view text);
AccuracyScore character_accuracy(std::string_view expected,
                                 std::string_view actual);

}  // namespace vastome
