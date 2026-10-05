#include "op1emu/firmware.hpp"
#include "op1emu/ldr.hpp"

#include <exception>
#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kUsage = R"(Usage: op1emu <command> [arguments]

Commands:
  inspect <firmware.op1>                 Validate and describe firmware
  inspect-ldr <image.ldr>                Inspect Blackfin BF52x LDR metadata
  extract <firmware.op1> <new-directory> Validate and safely extract firmware
  help                                   Show this help text
)";

} // namespace

int main(int argc, char* argv[]) {
  if (argc == 1) {
    std::cout << kUsage;
    return 0;
  }

  try {
    const std::string_view command{argv[1]};
    if (command == "help" || command == "--help" || command == "-h") {
      std::cout << kUsage;
      return 0;
    }
    if (command == "inspect" && argc == 3) {
      const auto firmware = op1emu::inspect_firmware(argv[2]);
      std::cout << op1emu::format_inspection(firmware);
      return firmware.stored_crc == firmware.computed_crc ? 0 : 1;
    }
    if (command == "inspect-ldr" && argc == 3) {
      const auto inspection = op1emu::inspect_ldr_file(argv[2], op1emu::LdrFormat::bf52x);
      std::cout << op1emu::format_ldr_inspection(inspection);
      return inspection.validity == op1emu::LdrValidity::invalid ? 1 : 0;
    }
    if (command == "extract" && argc == 4) {
      const auto firmware = op1emu::inspect_firmware(argv[2]);
      op1emu::extract_firmware(firmware, argv[3]);
      std::cout << "Extracted " << firmware.members.size() << " members to " << argv[3] << '\n';
      return 0;
    }

    std::cerr << "op1emu: unknown command or invalid arguments: " << command << '\n';
    std::cerr << kUsage;
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "op1emu: " << error.what() << '\n';
    return 1;
  }
}
