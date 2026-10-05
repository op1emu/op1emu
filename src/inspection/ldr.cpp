#include "op1emu/ldr.hpp"

#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace op1emu {
namespace {

constexpr std::size_t kHeaderSize = 16U;
constexpr std::uint32_t kReservedMask = 0x000000c0U;
constexpr std::uint32_t kFill = 0x00000100U;
constexpr std::uint32_t kSave = 0x00000010U;
constexpr std::uint32_t kAux = 0x00000020U;
constexpr std::uint32_t kQuickboot = 0x00000200U;
constexpr std::uint32_t kCallback = 0x00000400U;
constexpr std::uint32_t kInit = 0x00000800U;
constexpr std::uint32_t kIgnore = 0x00001000U;
constexpr std::uint32_t kIndirect = 0x00002000U;
constexpr std::uint32_t kFirst = 0x00004000U;
constexpr std::uint32_t kFinal = 0x00008000U;
constexpr std::uint8_t kSignature = 0xadU;
constexpr std::uint64_t kMaximumFileSize = 64U * 1024U * 1024U;

std::uint8_t byte_at(const std::span<const std::byte> data,
                     const std::size_t offset) {
  return std::to_integer<std::uint8_t>(data[offset]);
}

std::uint32_t read_le32(const std::span<const std::byte> data,
                        const std::size_t offset) {
  return static_cast<std::uint32_t>(byte_at(data, offset)) |
         (static_cast<std::uint32_t>(byte_at(data, offset + 1U)) << 8U) |
         (static_cast<std::uint32_t>(byte_at(data, offset + 2U)) << 16U) |
         (static_cast<std::uint32_t>(byte_at(data, offset + 3U)) << 24U);
}

bool overlaps(const std::uint32_t target, const std::uint32_t count,
              const std::uint32_t start, const std::uint32_t end) {
  if (count == 0U ||
      target > std::numeric_limits<std::uint32_t>::max() - (count - 1U)) {
    return false;
  }
  const auto last = target + count - 1U;
  return target <= end && last >= start;
}

void add_warning(LdrInspection &inspection, LdrBlock &block,
                 std::string message) {
  block.warnings.push_back("block " + std::to_string(block.number) + ": " +
                           message);
  inspection.warnings.push_back(block.warnings.back());
}

void add_error(LdrInspection &inspection, const std::string &message) {
  inspection.errors.push_back(message);
}

const char *validity_name(const LdrValidity validity) {
  switch (validity) {
  case LdrValidity::valid:
    return "valid";
  case LdrValidity::valid_with_warnings:
    return "valid-with-warnings";
  case LdrValidity::invalid:
    return "invalid";
  }
  return "invalid";
}

const char *position_name(const FinalPosition position) {
  switch (position) {
  case FinalPosition::before:
    return "before-first-final";
  case FinalPosition::at:
    return "first-final";
  case FinalPosition::after:
    return "after-first-final (not executed)";
  }
  return "unknown";
}

std::string flag_names(const LdrFlags &flags) {
  std::ostringstream output;
  const auto append = [&output](const bool set, const char *name) {
    if (!set) {
      return;
    }
    if (output.tellp() > 0) {
      output << '|';
    }
    output << name;
  };
  append(flags.save, "SAVE");
  append(flags.aux, "AUX");
  append(flags.fill, "FILL");
  append(flags.quickboot, "QUICKBOOT");
  append(flags.callback, "CALLBACK");
  append(flags.init, "INIT");
  append(flags.ignore, "IGNORE");
  append(flags.indirect, "INDIRECT");
  append(flags.first, "FIRST");
  append(flags.final, "FINAL");
  return output.tellp() == 0 ? "none" : output.str();
}

} // namespace

LdrInspection inspect_ldr(const std::span<const std::byte> data,
                          const LdrFormat format) {
  LdrInspection inspection;
  inspection.format = format;
  inspection.input_size = data.size();
  std::size_t offset = 0U;
  bool saw_final = false;
  std::size_t first_final_end = 0U;

  while (offset < data.size()) {
    if (data.size() - offset < kHeaderSize) {
      if (saw_final) {
        inspection.post_final_bytes = data.size() - first_final_end;
        inspection.warnings.push_back(std::to_string(data.size() - offset) +
                                      " opaque byte(s) follow the first FINAL");
        inspection.bytes_consumed = offset;
      } else {
        add_error(inspection, "truncated LDR header at file offset " +
                                  std::to_string(offset));
      }
      break;
    }

    LdrBlock block;
    block.number = inspection.blocks.size() + 1U;
    block.file_offset = offset;
    block.block_code = read_le32(data, offset);
    block.target = read_le32(data, offset + 4U);
    block.byte_count = read_le32(data, offset + 8U);
    block.argument = read_le32(data, offset + 12U);
    block.dma_code = static_cast<std::uint8_t>(block.block_code & 0x0fU);
    block.header_checksum =
        static_cast<std::uint8_t>((block.block_code >> 16U) & 0xffU);
    block.reserved_bits = block.block_code & kReservedMask;
    block.flags = LdrFlags{
        (block.block_code & kSave) != 0U,
        (block.block_code & kAux) != 0U,
        (block.block_code & kFill) != 0U,
        (block.block_code & kQuickboot) != 0U,
        (block.block_code & kCallback) != 0U,
        (block.block_code & kInit) != 0U,
        (block.block_code & kIgnore) != 0U,
        (block.block_code & kIndirect) != 0U,
        (block.block_code & kFirst) != 0U,
        (block.block_code & kFinal) != 0U,
    };
    block.signature_valid =
        static_cast<std::uint8_t>(block.block_code >> 24U) == kSignature;
    std::uint8_t checksum = 0U;
    for (std::size_t index = 0U; index < kHeaderSize; ++index) {
      checksum ^= byte_at(data, offset + index);
    }
    block.checksum_valid = checksum == 0U;
    block.payload_present = !block.flags.fill && block.byte_count != 0U;
    if (block.flags.ignore) {
      block.ignore_master_offset =
          block.byte_count <= static_cast<std::uint32_t>(
                                  std::numeric_limits<std::int32_t>::max())
              ? static_cast<std::int64_t>(block.byte_count)
              : static_cast<std::int64_t>(block.byte_count) -
                    (std::int64_t{1} << 32U);
    }
    block.final_position =
        saw_final
            ? FinalPosition::after
            : (block.flags.final ? FinalPosition::at : FinalPosition::before);

    if (!block.signature_valid) {
      add_error(inspection, "block " + std::to_string(block.number) +
                                ": invalid BF52x header signature");
    }
    if (!block.checksum_valid) {
      add_error(inspection, "block " + std::to_string(block.number) +
                                ": header XOR checksum mismatch");
    }
    if (block.reserved_bits != 0U) {
      add_error(inspection,
                "block " + std::to_string(block.number) +
                    ": reserved or unknown block-code bits are set");
    }
    if (block.flags.first && block.flags.fill) {
      add_error(inspection, "block " + std::to_string(block.number) +
                                ": FIRST cannot combine with FILL");
    }

    if (block.flags.first) {
      ++inspection.first_count;
      if (!inspection.entry_candidate.has_value()) {
        inspection.entry_candidate = block.target;
        const auto base = static_cast<std::uint64_t>(offset) + kHeaderSize;
        if (base > std::numeric_limits<std::uint64_t>::max() - block.argument) {
          add_error(inspection, "block " + std::to_string(block.number) +
                                    ": relative DXE extent overflows");
        } else {
          inspection.dxe_extent = base + block.argument;
          if (*inspection.dxe_extent > data.size()) {
            add_warning(inspection, block,
                        "relative DXE extent is beyond the physical file");
          }
        }
      }
    }
    if (block.flags.final) {
      ++inspection.final_count;
    }
    inspection.fill_count += block.flags.fill ? 1U : 0U;
    inspection.callback_count += block.flags.callback ? 1U : 0U;
    inspection.indirect_count += block.flags.indirect ? 1U : 0U;
    inspection.init_count += block.flags.init ? 1U : 0U;

    if ((block.target & 3U) != 0U) {
      add_warning(inspection, block, "target address is not four-byte aligned");
    }
    if ((block.byte_count & 3U) != 0U) {
      add_warning(inspection, block, "byte count is not divisible by four");
    }
    if (!block.flags.ignore && block.byte_count != 0U &&
        block.target >
            std::numeric_limits<std::uint32_t>::max() - block.byte_count) {
      add_warning(inspection, block,
                  "target address plus byte count wraps 32-bit address space");
    }
    if (!block.flags.ignore &&
        overlaps(block.target, block.byte_count, 0xffb00000U, 0xffb00fffU)) {
      add_warning(inspection, block,
                  "destination overlaps unsupported scratchpad memory");
    }
    if (!block.flags.ignore &&
        overlaps(block.target, block.byte_count, 0xff807ff0U, 0xff807fffU)) {
      add_warning(inspection, block, "destination overlaps Boot ROM workspace");
    }
    if (block.flags.callback || block.flags.indirect) {
      add_warning(inspection, block,
                  "CALLBACK/INDIRECT does not identify a transformation; OP-1 "
                  "8-of-24 framing "
                  "is LIKELY and the exact transformation is UNKNOWN");
    }

    const auto payload_size = block.flags.fill
                                  ? std::size_t{0U}
                                  : static_cast<std::size_t>(block.byte_count);
    if (payload_size > data.size() - offset - kHeaderSize) {
      add_error(inspection, "block " + std::to_string(block.number) +
                                ": payload is truncated");
      inspection.blocks.push_back(std::move(block));
      break;
    }
    inspection.physical_payload_bytes += payload_size;
    offset += kHeaderSize + payload_size;
    inspection.bytes_consumed = offset;

    if (block.final_position == FinalPosition::after) {
      ++inspection.post_final_records;
    }
    if (block.final_position == FinalPosition::at && !saw_final) {
      saw_final = true;
      first_final_end = offset;
    }
    inspection.blocks.push_back(std::move(block));
  }

  if (inspection.first_count == 0U) {
    add_error(inspection, "LDR has no FIRST record");
  } else if (inspection.first_count > 1U) {
    add_error(inspection, "LDR has multiple FIRST records");
  }
  if (inspection.final_count == 0U) {
    add_error(inspection, "LDR has no FINAL record");
  } else if (inspection.final_count > 1U) {
    add_error(inspection, "LDR has multiple FINAL records");
  }
  if (saw_final) {
    inspection.post_final_bytes = data.size() - first_final_end;
    if (inspection.post_final_bytes != 0U) {
      inspection.warnings.push_back(
          std::to_string(inspection.post_final_bytes) +
          " physical byte(s) follow the first FINAL and are not executed");
    }
  }
  if (inspection.callback_count != 0U && inspection.init_count == 0U) {
    inspection.warnings.push_back("CALLBACK records exist without a local "
                                  "INIT; callback registration is unknown");
  }

  inspection.validity =
      !inspection.errors.empty()
          ? LdrValidity::invalid
          : (inspection.warnings.empty() ? LdrValidity::valid
                                         : LdrValidity::valid_with_warnings);
  return inspection;
}

LdrInspection inspect_ldr_file(const std::filesystem::path &input,
                               const LdrFormat format) {
  std::error_code error;
  const auto size = std::filesystem::file_size(input, error);
  if (error) {
    throw std::runtime_error("cannot determine LDR file size: " +
                             error.message());
  }
  if (size > kMaximumFileSize ||
      size >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      size > static_cast<std::uint64_t>(
                 std::numeric_limits<std::streamsize>::max())) {
    throw std::runtime_error("LDR file exceeds the 64 MiB inspection limit");
  }
  std::vector<std::byte> data(static_cast<std::size_t>(size));
  std::ifstream stream(input, std::ios::binary);
  if (!stream ||
      (size != 0U && !stream.read(reinterpret_cast<char *>(data.data()),
                                  static_cast<std::streamsize>(data.size()))) ||
      stream.peek() != std::char_traits<char>::eof()) {
    throw std::runtime_error("cannot read LDR file");
  }
  return inspect_ldr(data, format);
}

std::string format_ldr_summary(const LdrInspection &inspection,
                               const std::string_view indent) {
  std::ostringstream output;
  output << indent << "Format: Blackfin BF52x\n";
  output << indent << "Status: " << validity_name(inspection.validity) << '\n';
  output << indent << "Physical records: " << inspection.blocks.size() << " ("
         << inspection.fill_count << " FILL, " << inspection.callback_count
         << " CALLBACK, " << inspection.indirect_count << " INDIRECT, "
         << inspection.init_count << " INIT)\n";
  output << indent << "FIRST/FINAL: " << inspection.first_count << '/'
         << inspection.final_count;
  if (inspection.entry_candidate.has_value()) {
    output << ", EVT1/entry candidate: 0x" << std::hex << std::setfill('0')
           << std::setw(8) << *inspection.entry_candidate << std::dec
           << std::setfill(' ');
  }
  output << '\n';
  if (inspection.dxe_extent.has_value()) {
    output << indent << "Relative DXE extent: 0x" << std::hex
           << *inspection.dxe_extent << std::dec << " ("
           << *inspection.dxe_extent << " bytes)\n";
  }
  output << indent
         << "Execution ends at first FINAL; post-final records/bytes: "
         << inspection.post_final_records << '/' << inspection.post_final_bytes
         << '\n';
  output << indent << "Warnings/errors: " << inspection.warnings.size() << '/'
         << inspection.errors.size() << '\n';
  return output.str();
}

std::string format_ldr_inspection(const LdrInspection &inspection) {
  std::ostringstream output;
  output << "Input size: " << inspection.input_size << " bytes\n";
  output << format_ldr_summary(inspection);
  output << "Blocks:\n";
  for (const auto &block : inspection.blocks) {
    output << "  " << block.number << " @0x" << std::hex << std::setfill('0')
           << std::setw(8) << block.file_offset << " code=0x" << std::setw(8)
           << block.block_code << " dma=" << std::dec
           << static_cast<unsigned int>(block.dma_code) << " target=0x"
           << std::hex << std::setw(8) << block.target << " count=0x"
           << std::setw(8) << block.byte_count << " argument=0x" << std::setw(8)
           << block.argument << std::dec << std::setfill(' ')
           << " flags=" << flag_names(block.flags)
           << " payload=" << (block.payload_present ? "present" : "none")
           << " position=" << position_name(block.final_position)
           << " signature=" << (block.signature_valid ? "valid" : "invalid")
           << " xor=" << (block.checksum_valid ? "valid" : "invalid")
           << " reserved=0x" << std::hex << block.reserved_bits << std::dec;
    if (block.ignore_master_offset.has_value()) {
      output << " ignore-master-offset=" << *block.ignore_master_offset;
    }
    output << '\n';
    for (const auto &warning : block.warnings) {
      output << "    warning: " << warning << '\n';
    }
  }
  for (const auto &warning : inspection.warnings) {
    if (warning.starts_with("block ")) {
      continue;
    }
    output << "Warning: " << warning << '\n';
  }
  for (const auto &error : inspection.errors) {
    output << "Error: " << error << '\n';
  }
  return output.str();
}

} // namespace op1emu
