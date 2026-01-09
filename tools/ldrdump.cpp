#include "loader/ldr.h"
#include <iostream>
#include <iomanip>
#include <sstream>

std::string getDMACodeString(uint32_t dma_code) {
    switch (dma_code) {
        case 0: return "dma-reserved";
        case 1: return "8bit-dma-from-8bit";
        case 2: return "8bit-dma-from-16bit";
        case 3: return "8bit-dma-from-32bit";
        case 4: return "8bit-dma-from-64bit";
        case 5: return "8bit-dma-from-128bit";
        case 6: return "16bit-dma-from-16bit";
        case 7: return "16bit-dma-from-32bit";
        case 8: return "16bit-dma-from-64bit";
        case 9: return "16bit-dma-from-128bit";
        case 10: return "32bit-dma-from-32bit";
        case 11: return "32bit-dma-from-64bit";
        case 12: return "32bit-dma-from-128bit";
        case 13: return "64bit-dma-from-64bit";
        case 14: return "64bit-dma-from-128bit";
        case 15: return "128bit-dma-from-128bit";
        default: return "unknown";
    }
}

std::string getFlagsString(uint32_t block_code) {
    std::stringstream ss;

    if (block_code & BFLAG_SAFE) ss << "safe ";
    if (block_code & BFLAG_AUX) ss << "aux ";
    if (block_code & BFLAG_FILL) ss << "fill ";
    if (block_code & BFLAG_QUICKBOOT) ss << "quickboot ";
    if (block_code & BFLAG_CALLBACK) ss << "callback ";
    if (block_code & BFLAG_INIT) ss << "init ";
    if (block_code & BFLAG_IGNORE) ss << "ignore ";
    if (block_code & BFLAG_INDIRECT) ss << "indirect ";
    if (block_code & BFLAG_FIRST) ss << "first ";
    if (block_code & BFLAG_FINAL) ss << "final ";

    return ss.str();
}

uint8_t computeHeaderChecksum(const uint8_t* data, size_t len) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; ++i) {
        checksum ^= data[i];
    }
    return checksum;
}

void displayLDRInfo(const LDRParser& parser, const std::string& filename) {
    const auto& dxes = parser.getDXEs();

    std::cout << "LDR File: " << filename << std::endl;
    std::cout << "Number of DXEs: " << dxes.size() << std::endl << std::endl;

    for (size_t d = 0; d < dxes.size(); ++d) {
        std::cout << "DXE " << (d + 1) << " at 0x"
                  << std::hex << std::setfill('0') << std::setw(8)
                  << dxes[d].blocks[0].offset << std::dec << ":" << std::endl;

        for (size_t b = 0; b < dxes[d].blocks.size(); ++b) {
            const Block& block = dxes[d].blocks[b];
            const BlockHeader& h = block.header;

            std::cout << "  Block " << std::setw(2) << (b + 1)
                      << " at 0x" << std::hex << std::setfill('0')
                      << std::setw(8) << block.offset << std::dec << std::endl;

            std::cout << "    Target Address: 0x" << std::hex << std::setw(8)
                      << h.target_address << " ( "
                      << (h.target_address > 0xFF000000 ? "L1" : "SDRAM")
                      << " )" << std::dec << std::endl;

            std::cout << "    Block Code: 0x" << std::hex << std::setw(8)
                      << h.block_code << std::dec << std::endl;

            std::cout << "    Byte Count: 0x" << std::hex << std::setw(8)
                      << h.byte_count << " ( " << std::dec << h.byte_count
                      << " bytes )" << std::endl;

            std::cout << "    Argument: 0x" << std::hex << std::setw(8)
                      << h.argument << std::dec << " ( ";

            uint32_t dma_code = h.block_code & BFLAG_DMACODE_MASK;
            std::cout << getDMACodeString(dma_code) << " ";
            std::cout << getFlagsString(h.block_code);
            std::cout << ")" << std::endl;

            uint8_t hdr_chk = computeHeaderChecksum(h.raw, LDR_BLOCK_HEADER_LEN);
            if (hdr_chk != 0) {
                std::cout << "    WARNING: Header checksum invalid!" << std::endl;
            }
        }
        std::cout << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <ldr_file>" << std::endl;
        return 1;
    }

    std::string filename = argv[1];
    LDRParser parser;

    if (!parser.loadFile(filename)) {
        std::cerr << "Error: Failed to load LDR file: " << filename << std::endl;
        return 1;
    }

    displayLDRInfo(parser, filename);

    return 0;
}
