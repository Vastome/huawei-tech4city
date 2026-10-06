#include "vastome/braille.hpp"

#include <array>
#include <cctype>
#include <map>

namespace vastome {
namespace {

using DotList = std::initializer_list<int>;

const std::map<char, std::array<bool, 6>>& letter_table() {
  static const std::map<char, std::array<bool, 6>> table = [] {
    std::map<char, std::array<bool, 6>> result;
    const auto add = [&result](const char character, const DotList dots) {
      std::array<bool, 6> cell{};
      for (const int dot : dots) {
        if (dot >= 1 && dot <= 6) {
          cell[static_cast<std::size_t>(dot - 1)] = true;
        }
      }
      result.emplace(character, cell);
    };
    add('a', {1}); add('b', {1, 2}); add('c', {1, 4});
    add('d', {1, 4, 5}); add('e', {1, 5}); add('f', {1, 2, 4});
    add('g', {1, 2, 4, 5}); add('h', {1, 2, 5}); add('i', {2, 4});
    add('j', {2, 4, 5}); add('k', {1, 3}); add('l', {1, 2, 3});
    add('m', {1, 3, 4}); add('n', {1, 3, 4, 5}); add('o', {1, 3, 5});
    add('p', {1, 2, 3, 4}); add('q', {1, 2, 3, 4, 5});
    add('r', {1, 2, 3, 5}); add('s', {2, 3, 4});
    add('t', {2, 3, 4, 5}); add('u', {1, 3, 6});
    add('v', {1, 2, 3, 6}); add('w', {2, 4, 5, 6});
    add('x', {1, 3, 4, 6}); add('y', {1, 3, 4, 5, 6});
    add('z', {1, 3, 5, 6});
    add(',', {2}); add(';', {2, 3}); add(':', {2, 5});
    add('.', {2, 5, 6}); add('!', {2, 3, 5}); add('?', {2, 3, 6});
    add('-', {3, 6}); add(static_cast<char>(39), {3}); add('"', {5});
    add('(', {1, 2, 6}); add(')', {3, 4, 5}); add('/', {3, 4});
    return result;
  }();
  return table;
}

BrailleCell cell_from_array(const std::array<bool, 6>& dots, const char source,
                            const bool indicator = false) {
  return {.dots = dots, .source = source, .indicator = indicator};
}

}  // namespace

std::uint8_t BrailleCell::bits() const {
  std::uint8_t result = 0;
  for (std::size_t index = 0; index < dots.size(); ++index) {
    if (dots[index]) {
      result = static_cast<std::uint8_t>(result | (1U << index));
    }
  }
  return result;
}

std::string BrailleCell::bit_string() const {
  std::string result;
  result.reserve(6);
  for (const bool raised : dots) {
    result.push_back(raised ? '1' : '0');
  }
  return result;
}

std::string BrailleCell::unicode() const {
  const std::uint32_t codepoint = 0x2800U + bits();
  std::string result;
  result.push_back(static_cast<char>(0xE0U | ((codepoint >> 12U) & 0x0FU)));
  result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
  result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  return result;
}

BrailleCell BrailleEncoder::from_dots(const std::initializer_list<int> dots,
                                      const char source,
                                      const bool indicator) {
  BrailleCell cell{.source = source, .indicator = indicator};
  for (const int dot : dots) {
    if (dot >= 1 && dot <= 6) {
      cell.dots[static_cast<std::size_t>(dot - 1)] = true;
    }
  }
  return cell;
}

std::vector<BrailleCell> BrailleEncoder::encode(std::string_view text) const {
  const auto mapped = encode_mapped(text);
  std::vector<BrailleCell> output;
  output.reserve(mapped.size());
  for (const auto& entry : mapped) output.push_back(entry.cell);
  return output;
}

std::vector<MappedBrailleCell> BrailleEncoder::encode_mapped(
    std::string_view text) const {
  std::vector<MappedBrailleCell> output;
  output.reserve(text.size() * 2);
  bool number_mode = false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char raw = text[index];
    const auto append = [&output, index](BrailleCell cell) {
      output.push_back({.cell = cell, .source_index = index});
    };
    const unsigned char byte = static_cast<unsigned char>(raw);
    if (std::isdigit(byte) != 0) {
      if (!number_mode) {
        append(from_dots({3, 4, 5, 6}, '#', true));
        number_mode = true;
      }
      const char digit = raw == '0' ? 'j' : static_cast<char>('a' + raw - '1');
      append(cell_from_array(letter_table().at(digit), raw));
      continue;
    }
    number_mode = false;
    if (raw == ' ') {
      append(BrailleCell{.source = raw});
      continue;
    }
    if (std::isupper(byte) != 0) {
      append(from_dots({6}, '^', true));
    }
    const char lowered = static_cast<char>(std::tolower(byte));
    const auto match = letter_table().find(lowered);
    if (match != letter_table().end()) {
      append(cell_from_array(match->second, raw));
    } else {
      append(cell_from_array(letter_table().at('?'), '?'));
    }
  }
  return output;
}

}  // namespace vastome
