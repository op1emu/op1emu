#include "op1emu/firmware.hpp"
#include "op1emu/ldr.hpp"

#include <lzma.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

namespace op1emu {
namespace {

constexpr std::size_t kCrcSize = 4U;
constexpr std::size_t kLzmaHeaderSize = 13U;
constexpr std::size_t kTarBlockSize = 512U;
constexpr std::uint8_t kOfficialProperties = 0x5dU;
constexpr std::uint32_t kOfficialDictionarySize = 8U * 1024U * 1024U;

std::uint8_t byte_at(const std::vector<std::byte>& data, const std::size_t offset) {
  return std::to_integer<std::uint8_t>(data.at(offset));
}

std::uint32_t read_le32(const std::vector<std::byte>& data, const std::size_t offset) {
  if (offset > data.size() || data.size() - offset < 4U) {
    throw std::runtime_error("truncated 32-bit field");
  }
  return static_cast<std::uint32_t>(byte_at(data, offset)) |
         (static_cast<std::uint32_t>(byte_at(data, offset + 1U)) << 8U) |
         (static_cast<std::uint32_t>(byte_at(data, offset + 2U)) << 16U) |
         (static_cast<std::uint32_t>(byte_at(data, offset + 3U)) << 24U);
}

std::vector<std::byte> read_file(const std::filesystem::path& path, const Limits& limits) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    throw std::runtime_error("cannot determine firmware size: " + error.message());
  }
  if (size > limits.max_input_size) {
    throw std::runtime_error("firmware exceeds input size limit");
  }
  if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::runtime_error("firmware is too large for this host");
  }

  std::vector<std::byte> data(static_cast<std::size_t>(size));
  std::ifstream stream(path, std::ios::binary);
  if (!stream ||
      (size != 0U && !stream.read(reinterpret_cast<char*>(data.data()),
                                  static_cast<std::streamsize>(data.size()))) ||
      stream.peek() != std::char_traits<char>::eof()) {
    throw std::runtime_error("cannot read firmware file");
  }
  return data;
}

std::vector<std::byte> decompress(const std::vector<std::byte>& wrapper, const Limits& limits) {
  if (wrapper.size() < kCrcSize + kLzmaHeaderSize + 1U) {
    throw std::runtime_error("firmware wrapper is truncated");
  }
  const auto header = kCrcSize;
  if (byte_at(wrapper, header) != kOfficialProperties) {
    throw std::runtime_error("unsupported LZMA properties (expected lc=3, lp=0, pb=2)");
  }
  const auto dictionary = read_le32(wrapper, header + 1U);
  if (dictionary != kOfficialDictionarySize) {
    throw std::runtime_error("unsupported LZMA dictionary size");
  }
  if (dictionary > limits.max_dictionary_size) {
    throw std::runtime_error("LZMA dictionary exceeds configured limit");
  }
  for (std::size_t index = header + 5U; index < header + 13U; ++index) {
    if (byte_at(wrapper, index) != 0xffU) {
      throw std::runtime_error("LZMA stream does not use the required unknown-size sentinel");
    }
  }

  lzma_stream stream = LZMA_STREAM_INIT;
  const auto init_result = lzma_alone_decoder(&stream, limits.max_dictionary_size);
  if (init_result != LZMA_OK) {
    throw std::runtime_error("cannot initialize LZMA decoder");
  }
  struct StreamGuard {
    lzma_stream* stream;
    ~StreamGuard() { lzma_end(stream); }
  } guard{&stream};

  const auto* compressed = reinterpret_cast<const std::uint8_t*>(wrapper.data() + kCrcSize);
  stream.next_in = compressed;
  stream.avail_in = wrapper.size() - kCrcSize;

  std::vector<std::byte> output;
  std::array<std::byte, 64U * 1024U> chunk{};
  while (true) {
    stream.next_out = reinterpret_cast<std::uint8_t*>(chunk.data());
    stream.avail_out = chunk.size();
    const auto result = lzma_code(&stream, LZMA_FINISH);
    const auto produced = chunk.size() - stream.avail_out;
    if (produced > limits.max_decompressed_size - output.size()) {
      throw std::runtime_error("decompressed firmware exceeds size limit");
    }
    output.insert(output.end(), chunk.begin(),
                  chunk.begin() + static_cast<std::ptrdiff_t>(produced));
    if (result == LZMA_STREAM_END) {
      if (stream.avail_in != 0U) {
        throw std::runtime_error("trailing data after LZMA end marker");
      }
      break;
    }
    if (result != LZMA_OK) {
      throw std::runtime_error("invalid or incomplete LZMA stream");
    }
  }
  return output;
}

bool all_zero(const std::vector<std::byte>& data, const std::size_t offset,
              const std::size_t count) {
  return std::all_of(data.begin() + static_cast<std::ptrdiff_t>(offset),
                     data.begin() + static_cast<std::ptrdiff_t>(offset + count),
                     [](const std::byte value) { return value == std::byte{0}; });
}

std::string tar_string(const std::vector<std::byte>& archive, const std::size_t offset,
                       const std::size_t length) {
  std::string value;
  value.reserve(length);
  for (std::size_t index = 0; index < length; ++index) {
    const auto character = static_cast<char>(byte_at(archive, offset + index));
    if (character == '\0') {
      break;
    }
    value.push_back(character);
  }
  return value;
}

std::uint64_t parse_octal(const std::vector<std::byte>& archive, const std::size_t offset,
                          const std::size_t length, const char* label) {
  std::uint64_t value = 0U;
  bool saw_digit = false;
  bool trailing_padding = false;
  for (std::size_t index = 0; index < length; ++index) {
    const auto character = static_cast<char>(byte_at(archive, offset + index));
    if (character == '\0' || character == ' ') {
      if (saw_digit) {
        trailing_padding = true;
      }
      continue;
    }
    if (trailing_padding || character < '0' || character > '7') {
      throw std::runtime_error(std::string("invalid tar ") + label);
    }
    saw_digit = true;
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 8U) {
      throw std::runtime_error(std::string("overflowing tar ") + label);
    }
    value = value * 8U + digit;
  }
  return value;
}

std::string validate_path(std::string name, const bool directory) {
  if (directory && !name.empty() && name.back() == '/') {
    name.pop_back();
  }
  if (name.empty() || name.front() == '/' || name.front() == '\\' ||
      name.find('\\') != std::string::npos ||
      (name.size() >= 2U && std::isalpha(static_cast<unsigned char>(name[0])) != 0 &&
       name[1] == ':')) {
    throw std::runtime_error("unsafe tar path: " + name);
  }
  if (std::any_of(name.begin(), name.end(), [](const char value) {
        const auto character = static_cast<unsigned char>(value);
        return character < 0x20U || character == 0x7fU;
      })) {
    throw std::runtime_error("tar path contains control characters");
  }
  std::size_t start = 0U;
  while (start <= name.size()) {
    const auto end = name.find('/', start);
    const auto component = name.substr(start, end == std::string::npos ? end : end - start);
    if (component.empty() || component == "." || component == "..") {
      throw std::runtime_error("unsafe tar path component: " + name);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1U;
  }
  return name;
}

void validate_collision(const std::string& name, const bool directory,
                        std::unordered_set<std::string>& paths,
                        std::unordered_set<std::string>& files) {
  if (!paths.insert(name).second) {
    throw std::runtime_error("duplicate tar path: " + name);
  }
  std::size_t slash = name.find('/');
  while (slash != std::string::npos) {
    if (files.contains(name.substr(0U, slash))) {
      throw std::runtime_error("tar path has a file parent: " + name);
    }
    slash = name.find('/', slash + 1U);
  }
  if (!directory) {
    const auto prefix = name + '/';
    if (std::any_of(paths.begin(), paths.end(),
                    [&prefix](const std::string& path) { return path.starts_with(prefix); })) {
      throw std::runtime_error("tar file collides with a directory: " + name);
    }
    files.insert(name);
  }
}

std::vector<Member> parse_tar(const std::vector<std::byte>& archive, const Limits& limits,
                              std::string& archive_type) {
  std::vector<Member> members;
  std::unordered_set<std::string> paths;
  std::unordered_set<std::string> files;
  std::uint64_t aggregate_size = 0U;
  std::size_t offset = 0U;
  bool found_terminator = false;
  bool gnu = false;

  while (offset <= archive.size() && archive.size() - offset >= kTarBlockSize) {
    if (all_zero(archive, offset, kTarBlockSize)) {
      if (archive.size() - offset < 2U * kTarBlockSize ||
          !all_zero(archive, offset + kTarBlockSize, kTarBlockSize)) {
        throw std::runtime_error("tar archive has an incomplete terminator");
      }
      if (!all_zero(archive, offset, archive.size() - offset)) {
        throw std::runtime_error("nonzero data follows tar terminator");
      }
      found_terminator = true;
      break;
    }
    if (members.size() >= limits.max_members) {
      throw std::runtime_error("tar member count exceeds limit");
    }

    std::uint64_t checksum = 0U;
    for (std::size_t index = 0; index < kTarBlockSize; ++index) {
      checksum += (index >= 148U && index < 156U) ? 0x20U : byte_at(archive, offset + index);
    }
    if (checksum != parse_octal(archive, offset + 148U, 8U, "checksum")) {
      throw std::runtime_error("tar header checksum mismatch");
    }

    const auto magic = tar_string(archive, offset + 257U, 8U);
    if (!magic.starts_with("ustar")) {
      throw std::runtime_error("unsupported tar archive format");
    }
    gnu = gnu || magic.starts_with("ustar  ");
    auto name = tar_string(archive, offset, 100U);
    const auto prefix = tar_string(archive, offset + 345U, 155U);
    if (!prefix.empty()) {
      name = prefix + '/' + name;
    }
    const auto type = static_cast<char>(byte_at(archive, offset + 156U));
    const bool directory = type == '5';
    if (type != '\0' && type != '0' && !directory) {
      throw std::runtime_error("unsupported tar member type for " + name);
    }
    const auto size = parse_octal(archive, offset + 124U, 12U, "size");
    if ((directory && size != 0U) || size > limits.max_member_size) {
      throw std::runtime_error("tar member size is invalid or exceeds limit: " + name);
    }
    if (size > limits.max_aggregate_size - aggregate_size) {
      throw std::runtime_error("tar aggregate size exceeds limit");
    }
    aggregate_size += size;
    const auto normalized = validate_path(name, directory);
    validate_collision(normalized, directory, paths, files);

    const auto payload = offset + kTarBlockSize;
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        static_cast<std::size_t>(size) > archive.size() - payload) {
      throw std::runtime_error("tar member payload is truncated: " + normalized);
    }
    const auto padded_blocks = (size + (kTarBlockSize - 1U)) / kTarBlockSize;
    if (padded_blocks > (std::numeric_limits<std::size_t>::max() / kTarBlockSize)) {
      throw std::runtime_error("tar member extent overflows host size");
    }
    const auto padded_size = static_cast<std::size_t>(padded_blocks) * kTarBlockSize;
    if (padded_size > archive.size() - payload) {
      throw std::runtime_error("tar member padding is truncated: " + normalized);
    }
    members.push_back(Member{normalized, size, directory, payload});
    offset = payload + padded_size;
  }
  if (!found_terminator) {
    throw std::runtime_error("tar archive is missing its two-block terminator");
  }
  archive_type = gnu ? "old GNU tar" : "ustar";
  return members;
}

std::vector<std::string> find_version_strings(const std::vector<std::byte>& archive,
                                              const std::vector<Member>& members) {
  std::set<std::string> found;
  for (const auto& member : members) {
    if (member.directory) {
      continue;
    }
    std::string run;
    const auto size = static_cast<std::size_t>(member.size);
    for (std::size_t index = 0; index <= size; ++index) {
      const auto character =
          index < size ? static_cast<char>(byte_at(archive, member.data_offset + index)) : '\0';
      if (character >= 0x20 && character <= 0x7e && run.size() < 160U) {
        run.push_back(character);
      } else {
        auto lower = run;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](const char value) {
          return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        });
        const bool has_version_246 = lower.find("version 246") != std::string::npos ||
                                     lower.find("version:246") != std::string::npos ||
                                     lower.find("version: 246") != std::string::npos ||
                                     lower.find("version=246") != std::string::npos ||
                                     lower.find("version-246") != std::string::npos ||
                                     lower.find("version_246") != std::string::npos ||
                                     lower.find("rev. 00246;") != std::string::npos ||
                                     lower.find("r. 00246") != std::string::npos;
        if (has_version_246) {
          const auto first = run.find_first_not_of(' ');
          const auto last = run.find_last_not_of(' ');
          found.insert(run.substr(first, last - first + 1U));
          if (found.size() >= 32U) {
            return {found.begin(), found.end()};
          }
        }
        run.clear();
      }
    }
  }
  return {found.begin(), found.end()};
}

bool is_important(const Member& member) {
  if (member.directory) {
    return false;
  }
  const auto extension = std::filesystem::path(member.name).extension().string();
  return extension == ".ldr" || extension == ".elf" || extension == ".bin" || extension == ".so";
}

} // namespace

Firmware inspect_firmware(const std::filesystem::path& input, const Limits& limits) {
  const auto wrapper = read_file(input, limits);
  if (wrapper.size() < kCrcSize) {
    throw std::runtime_error("firmware wrapper is missing its CRC");
  }
  Firmware firmware;
  firmware.input_size = wrapper.size();
  firmware.stored_crc = read_le32(wrapper, 0U);
  firmware.computed_crc =
      lzma_crc32(reinterpret_cast<const std::uint8_t*>(wrapper.data() + kCrcSize),
                 wrapper.size() - kCrcSize, 0U);
  firmware.dictionary_size = read_le32(wrapper, kCrcSize + 1U);
  const auto properties = byte_at(wrapper, kCrcSize);
  firmware.lc = static_cast<std::uint8_t>(properties % 9U);
  const auto remainder = static_cast<std::uint8_t>(properties / 9U);
  firmware.lp = static_cast<std::uint8_t>(remainder % 5U);
  firmware.pb = static_cast<std::uint8_t>(remainder / 5U);
  firmware.archive = decompress(wrapper, limits);
  firmware.members = parse_tar(firmware.archive, limits, firmware.archive_type);
  firmware.version_strings = find_version_strings(firmware.archive, firmware.members);
  return firmware;
}

void extract_firmware(const Firmware& firmware, const std::filesystem::path& output) {
  if (firmware.stored_crc != firmware.computed_crc) {
    throw std::runtime_error("refusing to extract firmware with an invalid CRC");
  }
  if (output.empty() || output.filename().empty()) {
    throw std::runtime_error("output path must name a new directory");
  }
  std::error_code error;
  if (std::filesystem::exists(output, error) || error) {
    throw std::runtime_error("output path already exists or cannot be checked");
  }
  auto parent = output.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  if (!std::filesystem::is_directory(parent, error) || error) {
    throw std::runtime_error("output parent is not an accessible directory");
  }
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto staging =
      parent / ("." + output.filename().string() + ".op1emu-" + std::to_string(nonce));
  if (!std::filesystem::create_directory(staging, error) || error) {
    throw std::runtime_error("cannot create extraction staging directory");
  }
  struct Cleanup {
    std::filesystem::path path;
    bool published = false;
    ~Cleanup() {
      if (!published) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    }
  } cleanup{staging};

  for (const auto& member : firmware.members) {
    const auto destination = staging / std::filesystem::path(member.name);
    if (member.directory) {
      if (!std::filesystem::create_directories(destination, error) && error) {
        throw std::runtime_error("cannot create extracted directory: " + member.name);
      }
      continue;
    }
    if (!std::filesystem::create_directories(destination.parent_path(), error) && error) {
      throw std::runtime_error("cannot create extracted parent: " + member.name);
    }
    std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
    const auto size = static_cast<std::size_t>(member.size);
    if (!stream || (size != 0U && !stream.write(reinterpret_cast<const char*>(
                                                    firmware.archive.data() + member.data_offset),
                                                static_cast<std::streamsize>(size)))) {
      throw std::runtime_error("cannot write extracted member: " + member.name);
    }
    stream.close();
    if (!stream) {
      throw std::runtime_error("cannot finish extracted member: " + member.name);
    }
  }
  std::filesystem::rename(staging, output, error);
  if (error) {
    throw std::runtime_error("cannot publish extracted directory: " + error.message());
  }
  cleanup.published = true;
}

std::string format_inspection(const Firmware& firmware) {
  std::ostringstream output;
  output << "Input size: " << firmware.input_size << " bytes\n";
  output << "Stored CRC-32: 0x" << std::hex << std::setfill('0') << std::setw(8)
         << firmware.stored_crc << "\nComputed CRC-32: 0x" << std::setw(8) << firmware.computed_crc
         << std::dec << std::setfill(' ')
         << "\nCRC valid: " << (firmware.stored_crc == firmware.computed_crc ? "yes" : "no")
         << '\n';
  output << "Compression: LZMA-Alone/LZMA1\nLZMA parameters: lc="
         << static_cast<unsigned int>(firmware.lc)
         << " lp=" << static_cast<unsigned int>(firmware.lp)
         << " pb=" << static_cast<unsigned int>(firmware.pb)
         << " dictionary=" << firmware.dictionary_size
         << " bytes, uncompressed-size=unknown, end-marker=valid\n";
  output << "Decompressed size: " << firmware.archive.size()
         << " bytes\nArchive: " << firmware.archive_type << "\nMembers: " << firmware.members.size()
         << '\n';
  for (const auto& member : firmware.members) {
    output << "  " << (member.directory ? "dir  " : "file ") << std::setw(10) << member.size << "  "
           << member.name << '\n';
  }
  const auto root =
      std::find_if(firmware.members.begin(), firmware.members.end(), [](const Member& member) {
        return !member.directory && member.name == "OP1_vdk.ldr";
      });
  if (root == firmware.members.end()) {
    output << "Root OP1_vdk.ldr: absent\n";
  } else {
    output << "Root OP1_vdk.ldr: present at OP1_vdk.ldr (" << root->size << " bytes)\n";
    const auto size = static_cast<std::size_t>(root->size);
    const auto ldr =
        inspect_ldr(std::span<const std::byte>(firmware.archive).subspan(root->data_offset, size),
                    LdrFormat::bf52x);
    output << "OP1_vdk.ldr inspection:\n" << format_ldr_summary(ldr, "  ");
  }
  output << "Important binaries:\n";
  bool any_important = false;
  for (const auto& member : firmware.members) {
    if (is_important(member)) {
      output << "  " << member.name << " (" << member.size << " bytes)\n";
      any_important = true;
    }
  }
  if (!any_important) {
    output << "  none\n";
  }
  output << "Firmware-version strings:\n";
  if (firmware.version_strings.empty()) {
    output << "  none safely discovered\n";
  } else {
    for (const auto& value : firmware.version_strings) {
      output << "  " << value << '\n';
    }
  }
  return output.str();
}

} // namespace op1emu
