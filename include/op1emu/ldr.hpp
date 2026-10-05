#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace op1emu {

enum class LdrFormat { bf52x };
enum class LdrValidity { valid, valid_with_warnings, invalid };
enum class FinalPosition { before, at, after };

struct LdrFlags {
  bool save{};
  bool aux{};
  bool fill{};
  bool quickboot{};
  bool callback{};
  bool init{};
  bool ignore{};
  bool indirect{};
  bool first{};
  bool final{};
};

struct LdrBlock {
  std::size_t number{};
  std::size_t file_offset{};
  std::uint32_t block_code{};
  std::uint8_t dma_code{};
  std::uint32_t target{};
  std::uint32_t byte_count{};
  std::uint32_t argument{};
  std::optional<std::int64_t> ignore_master_offset;
  std::uint8_t header_checksum{};
  std::uint32_t reserved_bits{};
  LdrFlags flags;
  bool signature_valid{};
  bool checksum_valid{};
  bool payload_present{};
  FinalPosition final_position{FinalPosition::before};
  std::vector<std::string> warnings;
};

struct LdrInspection {
  LdrFormat format{LdrFormat::bf52x};
  LdrValidity validity{LdrValidity::valid};
  std::size_t input_size{};
  std::size_t bytes_consumed{};
  std::size_t physical_payload_bytes{};
  std::size_t first_count{};
  std::size_t final_count{};
  std::size_t fill_count{};
  std::size_t callback_count{};
  std::size_t indirect_count{};
  std::size_t init_count{};
  std::size_t post_final_records{};
  std::size_t post_final_bytes{};
  std::optional<std::uint32_t> entry_candidate;
  std::optional<std::uint64_t> dxe_extent;
  std::vector<LdrBlock> blocks;
  std::vector<std::string> warnings;
  std::vector<std::string> errors;
};

LdrInspection inspect_ldr(std::span<const std::byte> data, LdrFormat format);
LdrInspection inspect_ldr_file(const std::filesystem::path &input,
                               LdrFormat format);
std::string format_ldr_summary(const LdrInspection &inspection,
                               std::string_view indent = {});
std::string format_ldr_inspection(const LdrInspection &inspection);

} // namespace op1emu
