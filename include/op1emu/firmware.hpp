#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace op1emu {

struct Limits {
  std::uint64_t max_input_size = 32U * 1024U * 1024U;
  std::uint64_t max_dictionary_size = 16U * 1024U * 1024U;
  std::uint64_t max_decompressed_size = 64U * 1024U * 1024U;
  std::size_t max_members = 4096U;
  std::uint64_t max_member_size = 32U * 1024U * 1024U;
  std::uint64_t max_aggregate_size = 64U * 1024U * 1024U;
};

struct Member {
  std::string name;
  std::uint64_t size{};
  bool directory{};
  std::size_t data_offset{};
};

struct Firmware {
  std::uint64_t input_size{};
  std::uint32_t stored_crc{};
  std::uint32_t computed_crc{};
  std::uint32_t dictionary_size{};
  std::uint8_t lc{};
  std::uint8_t lp{};
  std::uint8_t pb{};
  std::string archive_type;
  std::vector<Member> members;
  std::vector<std::string> version_strings;
  std::vector<std::byte> archive;
};

Firmware inspect_firmware(const std::filesystem::path& input, const Limits& limits = {});
void extract_firmware(const Firmware& firmware, const std::filesystem::path& output);
std::string format_inspection(const Firmware& firmware);

} // namespace op1emu
