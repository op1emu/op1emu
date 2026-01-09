#include "ldr.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cstring>

LDRParser::LDRParser() {}

LDRParser::~LDRParser() {}

uint32_t LDRParser::readLittleEndian32(const uint8_t* data) const {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint8_t LDRParser::computeHeaderChecksum(const uint8_t* data, size_t len) const {
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; ++i) {
        checksum ^= data[i];
    }
    return checksum;
}

bool LDRParser::readBlockHeader(std::ifstream& file, BlockHeader& header,
                                bool& is_ignore, bool& is_fill, bool& is_final) {
    file.read(reinterpret_cast<char*>(header.raw), LDR_BLOCK_HEADER_LEN);
    if (file.gcount() != LDR_BLOCK_HEADER_LEN) {
        return false;
    }

    header.block_code = readLittleEndian32(header.raw);
    header.target_address = readLittleEndian32(header.raw + 4);
    header.byte_count = readLittleEndian32(header.raw + 8);
    header.argument = readLittleEndian32(header.raw + 12);

    is_ignore = (header.block_code & BFLAG_IGNORE) != 0;
    is_fill = (header.block_code & BFLAG_FILL) != 0;
    is_final = (header.block_code & BFLAG_FINAL) != 0;

    return true;
}

bool LDRParser::loadFile(const std::string& filename) {
    filename_ = filename;
    std::ifstream file(filename, std::ios::binary);

    if (!file.is_open()) {
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    size_t current_pos = 0;
    DXE* current_dxe = nullptr;

    while (!file.eof() && current_pos < file_size) {
        BlockHeader header;
        bool is_ignore, is_fill, is_final;

        if (!readBlockHeader(file, header, is_ignore, is_fill, is_final)) {
            break;
        }

        if (current_dxe == nullptr || is_ignore) {
            dxes_.emplace_back();
            current_dxe = &dxes_.back();
        }

        Block block;
        block.header = header;
        block.offset = current_pos;

        if (!is_fill && header.byte_count > 0) {
            block.data.resize(header.byte_count);
            file.read(reinterpret_cast<char*>(block.data.data()), header.byte_count);
            current_pos += header.byte_count;
        }

        current_dxe->blocks.push_back(std::move(block));
        current_pos += LDR_BLOCK_HEADER_LEN;

        if (is_final) {
            break;
        }
    }

    file.close();
    return true;
}