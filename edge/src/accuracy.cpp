#include "vastome/accuracy.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace vastome {

std::string normalize_for_comparison(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool pending_space = false;
  for (const char character : text) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (std::isspace(byte) != 0) {
      pending_space = !normalized.empty();
      continue;
    }
    if (pending_space) {
      normalized.push_back(' ');
      pending_space = false;
    }
    normalized.push_back(static_cast<char>(std::toupper(byte)));
  }
  return normalized;
}

AccuracyScore character_accuracy(std::string_view expected,
                                 std::string_view actual) {
  const std::string reference = normalize_for_comparison(expected);
  const std::string candidate = normalize_for_comparison(actual);
  std::vector<std::size_t> previous(candidate.size() + 1);
  std::vector<std::size_t> current(candidate.size() + 1);
  for (std::size_t column = 0; column <= candidate.size(); ++column) {
    previous[column] = column;
  }
  for (std::size_t row = 1; row <= reference.size(); ++row) {
    current[0] = row;
    for (std::size_t column = 1; column <= candidate.size(); ++column) {
      const std::size_t substitution =
          previous[column - 1] +
          (reference[row - 1] == candidate[column - 1] ? 0U : 1U);
      current[column] = std::min(
          {previous[column] + 1, current[column - 1] + 1, substitution});
    }
    std::swap(previous, current);
  }
  const std::size_t edits = previous[candidate.size()];
  const std::size_t denominator = std::max<std::size_t>(1, reference.size());
  const double error_rate = static_cast<double>(edits) /
                            static_cast<double>(denominator);
  return {.edits = edits,
          .reference_characters = reference.size(),
          .character_error_rate = error_rate,
          .character_accuracy = std::max(0.0, 1.0 - error_rate)};
}

}  // namespace vastome
