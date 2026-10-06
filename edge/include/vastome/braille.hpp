#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace vastome {

struct BrailleCell {
  std::array<bool, 6> dots{};
  char source{' '};
  bool indicator{false};

  [[nodiscard]] std::uint8_t bits() const;
  [[nodiscard]] std::string bit_string() const;
  [[nodiscard]] std::string unicode() const;
};

struct MappedBrailleCell {
  BrailleCell cell;
  std::size_t source_index{};
};

class BrailleEncoder {
 public:
  [[nodiscard]] std::vector<BrailleCell> encode(std::string_view text) const;
  [[nodiscard]] std::vector<MappedBrailleCell> encode_mapped(
      std::string_view text) const;
  [[nodiscard]] static BrailleCell from_dots(std::initializer_list<int> dots,
                                             char source = ' ',
                                             bool indicator = false);
};

}  // namespace vastome
