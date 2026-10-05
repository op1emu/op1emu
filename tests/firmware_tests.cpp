#include "op1emu/firmware.hpp"
#include "op1emu/ldr.hpp"

#include <lzma.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("op1emu-tests-" + std::to_string(nonce));
    std::filesystem::create_directory(path_);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

void put_string(Bytes& data, const std::size_t offset, const std::size_t length,
                const std::string_view value) {
  if (value.size() > length) {
    throw std::runtime_error("test tar field too long");
  }
  std::memcpy(data.data() + offset, value.data(), value.size());
}

void put_octal(Bytes& data, const std::size_t offset, const std::size_t length,
               std::uint64_t value) {
  std::string digits(length - 1U, '0');
  for (std::size_t index = digits.size(); index > 0U && value != 0U; --index) {
    digits[index - 1U] = static_cast<char>('0' + (value & 7U));
    value >>= 3U;
  }
  if (value != 0U) {
    throw std::runtime_error("test octal field overflow");
  }
  put_string(data, offset, digits.size(), digits);
  data[offset + length - 1U] = std::byte{0};
}

void update_checksum(Bytes& header, const std::size_t offset) {
  std::fill(header.begin() + static_cast<std::ptrdiff_t>(offset + 148U),
            header.begin() + static_cast<std::ptrdiff_t>(offset + 156U), std::byte{0x20});
  std::uint64_t checksum = 0U;
  for (std::size_t index = 0; index < 512U; ++index) {
    checksum += std::to_integer<std::uint8_t>(header[offset + index]);
  }
  std::string digits(6U, '0');
  for (std::size_t index = digits.size(); index > 0U; --index) {
    digits[index - 1U] = static_cast<char>('0' + (checksum & 7U));
    checksum >>= 3U;
  }
  put_string(header, offset + 148U, digits.size(), digits);
  header[offset + 154U] = std::byte{0};
  header[offset + 155U] = std::byte{0x20};
}

void append_member(Bytes& tar, const std::string_view name, const std::string_view contents,
                   const char type = '0') {
  const auto header_offset = tar.size();
  tar.resize(tar.size() + 512U, std::byte{0});
  put_string(tar, header_offset, 100U, name);
  put_octal(tar, header_offset + 100U, 8U, 0644U);
  put_octal(tar, header_offset + 108U, 8U, 0U);
  put_octal(tar, header_offset + 116U, 8U, 0U);
  put_octal(tar, header_offset + 124U, 12U, type == '5' ? 0U : contents.size());
  put_octal(tar, header_offset + 136U, 12U, 0U);
  tar[header_offset + 156U] = static_cast<std::byte>(type);
  put_string(tar, header_offset + 257U, 8U, std::string_view{"ustar  \0", 8U});
  update_checksum(tar, header_offset);
  if (type != '5') {
    for (const char character : contents) {
      tar.push_back(static_cast<std::byte>(character));
    }
    const auto padding = (512U - (contents.size() % 512U)) % 512U;
    tar.resize(tar.size() + padding, std::byte{0});
  }
}

Bytes finish_tar(Bytes tar) {
  tar.resize(tar.size() + 1024U, std::byte{0});
  return tar;
}

Bytes compress_tar(const Bytes& tar) {
  lzma_options_lzma options{};
  if (lzma_lzma_preset(&options, LZMA_PRESET_DEFAULT)) {
    throw std::runtime_error("cannot initialize test LZMA options");
  }
  options.dict_size = 8U * 1024U * 1024U;
  options.lc = 3U;
  options.lp = 0U;
  options.pb = 2U;
  lzma_stream stream = LZMA_STREAM_INIT;
  if (lzma_alone_encoder(&stream, &options) != LZMA_OK) {
    throw std::runtime_error("cannot initialize test LZMA encoder");
  }
  struct Guard {
    lzma_stream* stream;
    ~Guard() { lzma_end(stream); }
  } guard{&stream};

  stream.next_in = reinterpret_cast<const std::uint8_t*>(tar.data());
  stream.avail_in = tar.size();
  Bytes compressed;
  std::array<std::byte, 4096U> chunk{};
  while (true) {
    stream.next_out = reinterpret_cast<std::uint8_t*>(chunk.data());
    stream.avail_out = chunk.size();
    const auto result = lzma_code(&stream, LZMA_FINISH);
    const auto produced = chunk.size() - stream.avail_out;
    compressed.insert(compressed.end(), chunk.begin(),
                      chunk.begin() + static_cast<std::ptrdiff_t>(produced));
    if (result == LZMA_STREAM_END) {
      break;
    }
    if (result != LZMA_OK) {
      throw std::runtime_error("cannot encode test fixture");
    }
  }

  Bytes wrapper(4U, std::byte{0});
  wrapper.insert(wrapper.end(), compressed.begin(), compressed.end());
  const auto crc =
      lzma_crc32(reinterpret_cast<const std::uint8_t*>(compressed.data()), compressed.size(), 0U);
  for (std::size_t index = 0; index < 4U; ++index) {
    wrapper[index] = static_cast<std::byte>((crc >> (index * 8U)) & 0xffU);
  }
  return wrapper;
}

std::filesystem::path write_fixture(const std::filesystem::path& directory,
                                    const std::string_view name, const Bytes& bytes) {
  const auto path = directory / name;
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!stream) {
    throw std::runtime_error("cannot write test fixture");
  }
  return path;
}

void expect_failure(const std::function<void()>& operation, const std::string_view label) {
  try {
    operation();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error(std::string("expected failure: ") + std::string(label));
}

void expect_failure_message(const std::function<void()>& operation,
                            const std::string_view expected_message,
                            const std::string_view label) {
  try {
    operation();
  } catch (const std::exception& error) {
    if (error.what() == expected_message) {
      return;
    }
    throw std::runtime_error(std::string(label) + ": unexpected error: " + error.what());
  }
  throw std::runtime_error(std::string("expected failure: ") + std::string(label));
}

Bytes valid_tar() {
  Bytes tar;
  append_member(tar, "OP1_vdk.ldr",
                "firmware version 246\nRev. 00246; 2022/11/09 16:17:00; 8.1.11.1\n"
                "                                 R. 00246\n");
  append_member(tar, "assets/", "", '5');
  append_member(tar, "assets/readme.txt", "fixture\n");
  return finish_tar(std::move(tar));
}

void run_tests() {
  TemporaryDirectory temporary;
  const auto valid_path = write_fixture(temporary.path(), "valid.op1", compress_tar(valid_tar()));
  const auto firmware = op1emu::inspect_firmware(valid_path);
  if (firmware.stored_crc != firmware.computed_crc || firmware.members.size() != 3U ||
      firmware.archive_type != "old GNU tar" || firmware.version_strings.empty()) {
    throw std::runtime_error("valid fixture inspection was incomplete");
  }
  if (std::find(firmware.version_strings.begin(), firmware.version_strings.end(),
                "Rev. 00246; 2022/11/09 16:17:00; 8.1.11.1") == firmware.version_strings.end() ||
      std::find(firmware.version_strings.begin(), firmware.version_strings.end(), "R. 00246") ==
          firmware.version_strings.end()) {
    throw std::runtime_error("version-246 markers were not discovered");
  }
  const auto report = op1emu::format_inspection(firmware);
  if (report.find("CRC valid: yes") == std::string::npos ||
      report.find("Root OP1_vdk.ldr: present") == std::string::npos) {
    throw std::runtime_error("valid fixture report was incomplete");
  }
  const auto output = temporary.path() / "output";
  op1emu::extract_firmware(firmware, output);
  if (!std::filesystem::is_regular_file(output / "OP1_vdk.ldr") ||
      !std::filesystem::is_regular_file(output / "assets/readme.txt")) {
    throw std::runtime_error("valid fixture extraction failed");
  }

  const std::array<std::pair<std::string_view, std::string_view>, 3U> root_lookalikes{{
      {"op1_vdk.ldr", "wrong case"},
      {"nested/OP1_vdk.ldr", "nested"},
      {"OP1_vdk.ldr", "directory"},
  }};
  for (std::size_t index = 0U; index < root_lookalikes.size(); ++index) {
    Bytes lookalike_tar;
    const auto& [name, kind] = root_lookalikes[index];
    append_member(lookalike_tar, name, kind == "directory" ? "" : "lookalike",
                  kind == "directory" ? '5' : '0');
    const auto lookalike_path = write_fixture(
        temporary.path(), "lookalike-" + std::to_string(index) + ".op1",
        compress_tar(finish_tar(std::move(lookalike_tar))));
    expect_failure_message(
        [&] { static_cast<void>(op1emu::inspect_firmware(lookalike_path)); },
        "tar archive must contain a regular root OP1_vdk.ldr member", "root LDR lookalike");
  }

  Bytes missing_root_tar;
  append_member(missing_root_tar, "assets/readme.txt", "fixture\n");
  const auto missing_root_path = write_fixture(
      temporary.path(), "missing-root.op1", compress_tar(finish_tar(std::move(missing_root_tar))));
  expect_failure_message(
      [&] { static_cast<void>(op1emu::inspect_firmware(missing_root_path)); },
      "tar archive must contain a regular root OP1_vdk.ldr member", "missing root LDR");

  const auto tiny = write_fixture(temporary.path(), "tiny.op1", Bytes(3U));
  expect_failure([&] { static_cast<void>(op1emu::inspect_firmware(tiny)); }, "short wrapper");

  auto bad_crc_bytes = compress_tar(valid_tar());
  bad_crc_bytes[4] = std::byte{0};
  bad_crc_bytes[0] ^= std::byte{1};
  const auto bad_crc_path = write_fixture(temporary.path(), "bad-crc.op1", bad_crc_bytes);
  expect_failure_message([&] { static_cast<void>(op1emu::inspect_firmware(bad_crc_path)); },
                         "firmware CRC mismatch", "CRC must precede malformed payload error");
  auto bad_crc_firmware = firmware;
  ++bad_crc_firmware.stored_crc;
  const auto crc_output = temporary.path() / "crc-output";
  expect_failure([&] { op1emu::extract_firmware(bad_crc_firmware, crc_output); }, "CRC extraction");
  if (std::filesystem::exists(crc_output)) {
    throw std::runtime_error("CRC failure published output");
  }

  auto bad_properties = compress_tar(valid_tar());
  bad_properties[4] = std::byte{0};
  const auto bad_properties_path =
      write_fixture(temporary.path(), "bad-properties.op1", bad_properties);
  expect_failure([&] { static_cast<void>(op1emu::inspect_firmware(bad_properties_path)); },
                 "LZMA properties");

  auto trailing = compress_tar(valid_tar());
  trailing.push_back(std::byte{0});
  const auto trailing_path = write_fixture(temporary.path(), "trailing.op1", trailing);
  expect_failure([&] { static_cast<void>(op1emu::inspect_firmware(trailing_path)); },
                 "trailing compressed data");

  auto truncated = compress_tar(valid_tar());
  truncated.pop_back();
  const auto truncated_path = write_fixture(temporary.path(), "truncated.op1", truncated);
  expect_failure([&] { static_cast<void>(op1emu::inspect_firmware(truncated_path)); },
                 "missing LZMA end marker");

  auto checksum_tar = valid_tar();
  checksum_tar[10] ^= std::byte{1};
  const auto checksum_path =
      write_fixture(temporary.path(), "checksum.op1", compress_tar(checksum_tar));
  expect_failure([&] { static_cast<void>(op1emu::inspect_firmware(checksum_path)); },
                 "tar checksum");

  auto bounds_tar = valid_tar();
  put_octal(bounds_tar, 124U, 12U, 1024U * 1024U);
  update_checksum(bounds_tar, 0U);
  const auto bounds_path = write_fixture(temporary.path(), "bounds.op1", compress_tar(bounds_tar));
  expect_failure([&] { static_cast<void>(op1emu::inspect_firmware(bounds_path)); }, "tar bounds");

  Bytes unsupported_tar;
  append_member(unsupported_tar, "link", "", '2');
  const auto unsupported_path =
      write_fixture(temporary.path(), "unsupported.op1", compress_tar(finish_tar(unsupported_tar)));
  expect_failure([&] { static_cast<void>(op1emu::inspect_firmware(unsupported_path)); },
                 "unsupported tar type");

  Bytes duplicate_tar;
  append_member(duplicate_tar, "same", "one");
  append_member(duplicate_tar, "same", "two");
  const auto duplicate_path =
      write_fixture(temporary.path(), "duplicate.op1", compress_tar(finish_tar(duplicate_tar)));
  expect_failure([&] { static_cast<void>(op1emu::inspect_firmware(duplicate_path)); },
                 "duplicate path");

  Bytes normalized_collision_tar;
  append_member(normalized_collision_tar, "same/", "", '5');
  append_member(normalized_collision_tar, "same", "file");
  const auto normalized_collision_path =
      write_fixture(temporary.path(), "normalized-collision.op1",
                    compress_tar(finish_tar(normalized_collision_tar)));
  expect_failure([&] { static_cast<void>(op1emu::inspect_firmware(normalized_collision_path)); },
                 "normalized path collision");

  for (const std::string_view unsafe : {"../escape", "/absolute", "C:/drive", "a\\b"}) {
    Bytes unsafe_tar;
    append_member(unsafe_tar, unsafe, "bad");
    const auto unsafe_path =
        write_fixture(temporary.path(), "unsafe-" + std::to_string(unsafe.size()) + ".op1",
                      compress_tar(finish_tar(unsafe_tar)));
    expect_failure([&] { static_cast<void>(op1emu::inspect_firmware(unsafe_path)); },
                   "unsafe path");
  }

  auto cleanup_firmware = firmware;
  cleanup_firmware.members.push_back(op1emu::Member{std::string(300U, 'x'), 0U, false, 0U});
  const auto cleanup_output = temporary.path() / "cleanup-output";
  expect_failure([&] { op1emu::extract_firmware(cleanup_firmware, cleanup_output); },
                 "staged extraction cleanup");
  if (std::filesystem::exists(cleanup_output)) {
    throw std::runtime_error("failed extraction published output");
  }
  for (const auto& entry : std::filesystem::directory_iterator(temporary.path())) {
    if (entry.path().filename().string().starts_with(".cleanup-output.op1emu-")) {
      throw std::runtime_error("failed extraction left staging output");
    }
  }
}

int run_local_firmware(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    std::cout << "SKIP: local firmware is absent\n";
    return 77;
  }
  const auto firmware = op1emu::inspect_firmware(path);
  if (firmware.stored_crc != firmware.computed_crc || firmware.archive.size() != 26368000U) {
    throw std::runtime_error("local firmware did not match confirmed v246 wrapper");
  }
  const auto root = std::find_if(firmware.members.begin(), firmware.members.end(),
                                 [](const op1emu::Member& member) {
                                   return member.name == "OP1_vdk.ldr" && !member.directory;
                                 });
  if (root == firmware.members.end()) {
    throw std::runtime_error("local firmware lacks root OP1_vdk.ldr");
  }
  const auto ldr =
      op1emu::inspect_ldr(std::span<const std::byte>(firmware.archive)
                              .subspan(root->data_offset, static_cast<std::size_t>(root->size)),
                          op1emu::LdrFormat::bf52x);
  if (root->size != 2171624U || ldr.validity != op1emu::LdrValidity::valid_with_warnings ||
      ldr.blocks.size() != 801U || ldr.bytes_consumed != root->size ||
      ldr.physical_payload_bytes != 2158808U || ldr.first_count != 1U || ldr.final_count != 1U ||
      ldr.fill_count != 413U || ldr.callback_count != 46U || ldr.indirect_count != 46U ||
      ldr.init_count != 0U || ldr.post_final_records != 1U || ldr.post_final_bytes != 256U ||
      ldr.entry_candidate != 0xffa00000U || ldr.dxe_extent != root->size) {
    throw std::runtime_error("local firmware LDR aggregates did not match confirmed v246 data");
  }
  if (ldr.blocks[0].number != 1U || !ldr.blocks[0].flags.first || ldr.blocks[799].number != 800U ||
      !ldr.blocks[799].flags.final || ldr.blocks[799].final_position != op1emu::FinalPosition::at ||
      ldr.blocks[800].number != 801U ||
      ldr.blocks[800].final_position != op1emu::FinalPosition::after ||
      !ldr.blocks[710].flags.fill || ldr.blocks[710].byte_count != 0x0003ffffU) {
    throw std::runtime_error("local firmware LDR landmarks did not match confirmed v246 data");
  }
  if (!std::all_of(ldr.blocks.begin(), ldr.blocks.end(), [](const op1emu::LdrBlock& block) {
        return block.signature_valid && block.checksum_valid && block.dma_code == 6U &&
               block.reserved_bits == 0U;
      })) {
    throw std::runtime_error("local firmware LDR headers did not match confirmed v246 data");
  }
  return 0;
}

} // namespace

int main(const int argc, char* argv[]) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--local-firmware") {
      return run_local_firmware(argv[2]);
    }
    if (argc != 1) {
      throw std::runtime_error("unexpected test arguments");
    }
    run_tests();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "firmware_tests: " << error.what() << '\n';
    return 1;
  }
}
