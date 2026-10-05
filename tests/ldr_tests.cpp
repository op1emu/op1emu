#include "op1emu/ldr.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;

constexpr std::uint32_t kFill = 0x00000100U;
constexpr std::uint32_t kCallback = 0x00000400U;
constexpr std::uint32_t kInit = 0x00000800U;
constexpr std::uint32_t kIgnore = 0x00001000U;
constexpr std::uint32_t kIndirect = 0x00002000U;
constexpr std::uint32_t kFirst = 0x00004000U;
constexpr std::uint32_t kFinal = 0x00008000U;

void put_le32(Bytes& data, const std::size_t offset, const std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
    data[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void fix_checksum(Bytes& data, const std::size_t offset) {
  data[offset + 2U] = std::byte{0};
  std::uint8_t checksum = 0U;
  for (std::size_t index = 0U; index < 16U; ++index) {
    checksum ^= std::to_integer<std::uint8_t>(data[offset + index]);
  }
  data[offset + 2U] = static_cast<std::byte>(checksum);
}

std::size_t append_block(Bytes& data, const std::uint32_t flags, const std::uint32_t target,
                         const std::uint32_t count, const std::uint32_t argument = 0U,
                         const std::byte payload = std::byte{0x5a}) {
  const auto offset = data.size();
  data.resize(offset + 16U, std::byte{0});
  put_le32(data, offset, 0xad000006U | flags);
  put_le32(data, offset + 4U, target);
  put_le32(data, offset + 8U, count);
  put_le32(data, offset + 12U, argument);
  fix_checksum(data, offset);
  if ((flags & kFill) == 0U) {
    data.resize(data.size() + count, payload);
  }
  return offset;
}

Bytes valid_stream() {
  Bytes data;
  append_block(data, kFirst, 0x10000000U, 4U, 20U);
  append_block(data, kFinal, 0x10000004U, 0U);
  return data;
}

bool contains(const std::vector<std::string>& messages, const std::string_view text) {
  return std::any_of(messages.begin(), messages.end(), [text](const std::string& message) {
    return message.find(text) != std::string::npos;
  });
}

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void run_tests() {
  const auto valid = op1emu::inspect_ldr(valid_stream(), op1emu::LdrFormat::bf52x);
  require(valid.validity == op1emu::LdrValidity::valid, "valid stream was rejected");
  require(valid.blocks.size() == 2U && valid.first_count == 1U && valid.final_count == 1U,
          "valid stream metadata is incomplete");
  require(valid.entry_candidate == 0x10000000U && valid.dxe_extent == 36U,
          "FIRST metadata was not resolved");
  require(valid.blocks[0].payload_present && !valid.blocks[1].payload_present,
          "payload presence is incorrect");

  Bytes fill;
  append_block(fill, kFirst, 0x10000000U, 0U);
  append_block(fill, kFill, 0x20000000U, std::numeric_limits<std::uint32_t>::max());
  append_block(fill, kFinal, 0x10000000U, 0U);
  const auto fill_result = op1emu::inspect_ldr(fill, op1emu::LdrFormat::bf52x);
  require(fill_result.blocks.size() == 3U && fill_result.blocks[1].file_offset == 16U &&
              fill_result.blocks[2].file_offset == 32U && fill_result.fill_count == 1U,
          "FILL did not advance by one physical header");
  require(fill_result.validity == op1emu::LdrValidity::valid_with_warnings,
          "odd UINT32_MAX FILL count should warn");

  auto truncated_header = valid_stream();
  truncated_header.resize(7U);
  const auto short_header = op1emu::inspect_ldr(truncated_header, op1emu::LdrFormat::bf52x);
  require(short_header.validity == op1emu::LdrValidity::invalid &&
              contains(short_header.errors, "truncated LDR header"),
          "truncated header was not rejected");

  Bytes huge_payload;
  append_block(huge_payload, kFirst, 0x10000000U, 0U);
  const auto huge_offset = append_block(huge_payload, 0U, 0x20000000U, 0U);
  put_le32(huge_payload, huge_offset + 8U, std::numeric_limits<std::uint32_t>::max());
  fix_checksum(huge_payload, huge_offset);
  const auto short_payload = op1emu::inspect_ldr(huge_payload, op1emu::LdrFormat::bf52x);
  require(short_payload.validity == op1emu::LdrValidity::invalid &&
              contains(short_payload.errors, "payload is truncated"),
          "UINT32_MAX payload count was not bounded");

  auto bad_signature = valid_stream();
  bad_signature[3] = std::byte{0xac};
  fix_checksum(bad_signature, 0U);
  const auto signature = op1emu::inspect_ldr(bad_signature, op1emu::LdrFormat::bf52x);
  require(contains(signature.errors, "signature"), "bad signature was not rejected");

  auto bad_xor = valid_stream();
  bad_xor[4] ^= std::byte{1};
  const auto checksum = op1emu::inspect_ldr(bad_xor, op1emu::LdrFormat::bf52x);
  require(contains(checksum.errors, "XOR checksum"), "bad checksum was not rejected");

  auto reserved = valid_stream();
  reserved[0] |= std::byte{0xc0};
  fix_checksum(reserved, 0U);
  const auto unknown = op1emu::inspect_ldr(reserved, op1emu::LdrFormat::bf52x);
  require(unknown.blocks[0].reserved_bits == 0xc0U &&
              contains(unknown.errors, "reserved or unknown"),
          "reserved/unknown flags were not rejected");

  Bytes signed_ignore;
  const auto ignore_offset = append_block(signed_ignore, kFirst | kIgnore, 0x10000000U, 0U);
  put_le32(signed_ignore, ignore_offset + 8U, 0xfffffff0U);
  fix_checksum(signed_ignore, ignore_offset);
  const auto ignore_result = op1emu::inspect_ldr(signed_ignore, op1emu::LdrFormat::bf52x);
  require(ignore_result.blocks[0].byte_count == 0xfffffff0U &&
              ignore_result.blocks[0].ignore_master_offset == -16,
          "signed master-mode IGNORE interpretation is incorrect");

  Bytes missing_first;
  append_block(missing_first, kFinal, 0x10000000U, 0U);
  require(contains(op1emu::inspect_ldr(missing_first, op1emu::LdrFormat::bf52x).errors, "no FIRST"),
          "missing FIRST was not rejected");

  Bytes missing_final;
  append_block(missing_final, kFirst, 0x10000000U, 0U);
  require(contains(op1emu::inspect_ldr(missing_final, op1emu::LdrFormat::bf52x).errors, "no FINAL"),
          "missing FINAL was not rejected");

  Bytes duplicate;
  append_block(duplicate, kFirst, 0x10000000U, 0U);
  append_block(duplicate, kFirst, 0x10000000U, 0U);
  append_block(duplicate, kFinal, 0x10000000U, 0U);
  append_block(duplicate, kFinal, 0x10000000U, 0U);
  const auto duplicate_result = op1emu::inspect_ldr(duplicate, op1emu::LdrFormat::bf52x);
  require(contains(duplicate_result.errors, "multiple FIRST") &&
              contains(duplicate_result.errors, "multiple FINAL"),
          "duplicate markers were not rejected");

  Bytes first_fill;
  append_block(first_fill, kFirst | kFill, 0x10000000U, 0U);
  append_block(first_fill, kFinal, 0x10000000U, 0U);
  require(contains(op1emu::inspect_ldr(first_fill, op1emu::LdrFormat::bf52x).errors,
                   "FIRST cannot combine with FILL"),
          "FIRST|FILL was not rejected");

  Bytes alignment;
  append_block(alignment, kFirst, 0x10000001U, 0U);
  append_block(alignment, kFill, 0xfffffff0U, 0x21U);
  append_block(alignment, kFinal, 0x10000000U, 0U);
  const auto alignment_result = op1emu::inspect_ldr(alignment, op1emu::LdrFormat::bf52x);
  require(contains(alignment_result.warnings, "target address is not") &&
              contains(alignment_result.warnings, "not divisible") &&
              contains(alignment_result.warnings, "wraps 32-bit"),
          "alignment/address-wrap warnings are incomplete");

  Bytes special;
  append_block(special, kFirst, 0x10000000U, 0U);
  append_block(special, kInit, 0xff807ff0U, 4U);
  append_block(special, kCallback | kIndirect, 0xffb00000U, 4U);
  append_block(special, kFinal, 0x10000000U, 0U);
  const auto special_result = op1emu::inspect_ldr(special, op1emu::LdrFormat::bf52x);
  require(special_result.init_count == 1U && special_result.callback_count == 1U &&
              special_result.indirect_count == 1U &&
              contains(special_result.warnings, "Boot ROM workspace") &&
              contains(special_result.warnings, "scratchpad") &&
              contains(special_result.warnings, "transformation is UNKNOWN"),
          "INIT/CALLBACK/INDIRECT metadata is incomplete");

  Bytes post_final;
  append_block(post_final, kFirst, 0x10000000U, 0U);
  append_block(post_final, kFinal, 0x10000000U, 0U);
  append_block(post_final, kCallback | kIndirect, 0xff907e00U, 4U);
  const auto post = op1emu::inspect_ldr(post_final, op1emu::LdrFormat::bf52x);
  require(post.validity == op1emu::LdrValidity::valid_with_warnings &&
              post.post_final_records == 1U && post.post_final_bytes == 20U &&
              post.blocks[1].final_position == op1emu::FinalPosition::at &&
              post.blocks[2].final_position == op1emu::FinalPosition::after,
          "post-final record classification is incorrect");
  require(op1emu::format_ldr_inspection(post).find("not executed") != std::string::npos,
          "formatted inspection did not preserve execution boundary");

  auto opaque = valid_stream();
  opaque.push_back(std::byte{0x12});
  const auto opaque_result = op1emu::inspect_ldr(opaque, op1emu::LdrFormat::bf52x);
  require(opaque_result.validity == op1emu::LdrValidity::valid_with_warnings &&
              opaque_result.post_final_bytes == 1U &&
              contains(opaque_result.warnings, "opaque byte"),
          "opaque post-final bytes were not reported safely");
}

} // namespace

int main() {
  try {
    run_tests();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ldr_tests: " << error.what() << '\n';
    return 1;
  }
}
