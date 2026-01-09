#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

// BF548 Block header constants
constexpr size_t LDR_BLOCK_HEADER_LEN = 16;

// Block flags
constexpr uint32_t BFLAG_DMACODE_MASK      = 0x0000000F;
constexpr uint32_t BFLAG_SAFE              = 0x00000010;
constexpr uint32_t BFLAG_AUX               = 0x00000020;
constexpr uint32_t BFLAG_FILL              = 0x00000100;
constexpr uint32_t BFLAG_QUICKBOOT         = 0x00000200;
constexpr uint32_t BFLAG_CALLBACK          = 0x00000400;
constexpr uint32_t BFLAG_INIT              = 0x00000800;
constexpr uint32_t BFLAG_IGNORE            = 0x00001000;
constexpr uint32_t BFLAG_INDIRECT          = 0x00002000;
constexpr uint32_t BFLAG_FIRST             = 0x00004000;
constexpr uint32_t BFLAG_FINAL             = 0x00008000;
constexpr uint32_t BFLAG_HDRSIGN_MASK      = 0xFF000000;
constexpr uint32_t BFLAG_HDRSIGN_SHIFT     = 24;
constexpr uint32_t BFLAG_HDRSIGN_MAGIC     = 0xAD;
constexpr uint32_t BFLAG_HDRCHK_MASK       = 0x00FF0000;
constexpr uint32_t BFLAG_HDRCHK_SHIFT      = 16;

struct BlockHeader {
    uint8_t raw[LDR_BLOCK_HEADER_LEN];
    uint32_t block_code;
    uint32_t target_address;
    uint32_t byte_count;
    uint32_t argument;
};

struct Block {
    BlockHeader header;
    std::vector<uint8_t> data;
    size_t offset;

    bool IsInitBlock() const {
        return (header.block_code & BFLAG_INIT) != 0;
    }

    bool IsFirstBlock() const {
        return (header.block_code & BFLAG_FIRST) != 0;
    }

    bool IsFinalBlock() const {
        return (header.block_code & BFLAG_FINAL) != 0;
    }
};

struct DXE {
    std::vector<Block> blocks;
};

class LDRParser {
public:
    LDRParser();
    ~LDRParser();

    bool loadFile(const std::string& filename);
    const std::vector<DXE>& getDXEs() const { return dxes_; }

private:
    std::vector<DXE> dxes_;
    std::string filename_;

    bool readBlockHeader(std::ifstream& file, BlockHeader& header, bool& is_ignore,
                        bool& is_fill, bool& is_final);
    uint8_t computeHeaderChecksum(const uint8_t* data, size_t len) const;
    std::string getDMACodeString(uint32_t dma_code) const;
    std::string getFlagsString(uint32_t block_code) const;
    uint32_t readLittleEndian32(const uint8_t* data) const;
};