#include "loader/ldr.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static void Check(bool ok, const char* expression) {
    if (!ok) throw std::runtime_error(expression);
}
#define CHECK(expression) Check((expression), #expression)

class TempFiles {
public:
    TempFiles() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::random_device random;
        const auto prefix = "op1-ldr-parser-" + std::to_string(stamp) + "-" +
                            std::to_string(random()) + "-";
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            directory_ = fs::temp_directory_path() / (prefix + std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(directory_, error)) return;
            if (error) throw fs::filesystem_error("create test directory", directory_, error);
        }
        throw std::runtime_error("cannot create unique test directory");
    }
    ~TempFiles() {
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
    }
    std::string Write(const char* name, const Bytes& bytes) const {
        const auto path = directory_ / name;
        std::ofstream file(path, std::ios::binary);
        CHECK(file.is_open());
        if (!bytes.empty())
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        file.close();
        CHECK(file.good());
        return path.string();
    }
private:
    fs::path directory_;
};

static void Append(Bytes& bytes, const Bytes& tail) {
    bytes.insert(bytes.end(), tail.begin(), tail.end());
}

static Bytes Header(uint32_t flags, uint32_t count, uint32_t argument = 0) {
    Bytes bytes;
    for (uint32_t word : {flags, uint32_t{0xFFA00000}, count, argument})
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<uint8_t>(word >> shift));
    return bytes;
}

static Bytes Record(uint32_t flags, const Bytes& payload) {
    auto bytes = Header(flags, static_cast<uint32_t>(payload.size()));
    Append(bytes, payload);
    return bytes;
}

static void RunTests() {
    TempFiles files;
    const Bytes rts{0x10, 0x00};
    const auto smoke = Record(BFLAG_FIRST | BFLAG_FINAL, rts);
    {
        // Same deliberately unsigned, unchecked header as ldr_smoke.py.
        LDRParser parser;
        CHECK(parser.loadFile(files.Write("smoke.ldr", smoke)));
        CHECK(parser.getDXEs().size() == 1);
        const auto& blocks = parser.getDXEs()[0].blocks;
        CHECK(blocks.size() == 1);
        CHECK(blocks[0].data == rts);
        CHECK(blocks[0].offset == 0);
        CHECK(blocks[0].header.target_address == 0xFFA00000);
        CHECK(blocks[0].IsFirstBlock() && blocks[0].IsFinalBlock());
    }
    const auto header = Header(BFLAG_FIRST | BFLAG_FINAL, 4);
    for (size_t size = 1; size < LDR_BLOCK_HEADER_LEN; ++size) {
        LDRParser parser;
        CHECK(!parser.loadFile(files.Write("short-header.ldr",
                                           Bytes(header.begin(), header.begin() + size))));
    }
    for (size_t size = 0; size < 4; ++size) {
        auto bytes = header;
        bytes.insert(bytes.end(), size, 0xA5);
        LDRParser parser;
        CHECK(!parser.loadFile(files.Write("short-payload.ldr", bytes)));
    }
    const auto first = Record(BFLAG_FIRST, rts);
    for (size_t size = 1; size < LDR_BLOCK_HEADER_LEN; ++size) {
        auto bytes = first;
        bytes.insert(bytes.end(), header.begin(), header.begin() + size);
        LDRParser parser;
        CHECK(!parser.loadFile(files.Write("short-second-header.ldr", bytes)));
    }
    {
        auto bytes = first;
        Append(bytes, Header(BFLAG_FINAL, 4));
        LDRParser parser;
        CHECK(!parser.loadFile(files.Write("missing-second-payload.ldr", bytes)));
    }
    {
        // Keep after the smaller malformed cases: the old parser fails there
        // before this case can provoke its unchecked, multi-gigabyte allocation.
        LDRParser parser;
        CHECK(!parser.loadFile(files.Write("huge-count.ldr",
                                           Header(BFLAG_FINAL, 0xFFFFFFFFu))));
    }
    {
        // A fill count describes generated data, not bytes stored in the file.
        LDRParser parser;
        CHECK(parser.loadFile(files.Write("fill.ldr",
              Header(BFLAG_FIRST | BFLAG_FILL | BFLAG_FINAL, 8, 0xA5A5A5A5))));
        CHECK(parser.getDXEs().size() == 1);
        CHECK(parser.getDXEs()[0].blocks.size() == 1);
        CHECK(parser.getDXEs()[0].blocks[0].data == Bytes(8, 0xA5));
    }
    {
        auto bytes = smoke;
        bytes.insert(bytes.end(), 256, 0xFF);
        LDRParser parser;
        CHECK(parser.loadFile(files.Write("trailer.ldr", bytes)));
        CHECK(parser.getDXEs().size() == 1);
        CHECK(parser.getDXEs()[0].blocks.size() == 1);
        CHECK(parser.getDXEs()[0].blocks[0].data == rts);
    }
    {
        auto bytes = first;
        Append(bytes, Header(BFLAG_IGNORE | BFLAG_INIT, 0));
        Append(bytes, Header(BFLAG_FILL, 4, 0xA5A5A5A5));
        Append(bytes, Record(BFLAG_FIRST | BFLAG_FINAL, rts));
        LDRParser parser;
        CHECK(parser.loadFile(files.Write("multi-dxe.ldr", bytes)));
        const auto& dxes = parser.getDXEs();
        CHECK(dxes.size() == 2);
        CHECK(dxes[0].blocks.size() == 1);
        CHECK(dxes[0].blocks[0].offset == 0);
        CHECK(dxes[0].blocks[0].data == rts);
        CHECK(dxes[1].blocks.size() == 3);
        CHECK(dxes[1].blocks[0].offset == 18);
        CHECK(dxes[1].blocks[0].data.empty());
        CHECK(dxes[1].blocks[0].IsInitBlock());
        CHECK(dxes[1].blocks[1].offset == 34);
        CHECK(dxes[1].blocks[1].data == Bytes(4, 0xA5));
        CHECK(dxes[1].blocks[2].offset == 50);
        CHECK(dxes[1].blocks[2].data == rts);
    }
    {
        LDRParser parser;
        CHECK(parser.loadFile(files.Write("zero-length.ldr", Header(BFLAG_FINAL, 0))));
        CHECK(parser.getDXEs().size() == 1);
        CHECK(parser.getDXEs()[0].blocks.size() == 1);
        CHECK(parser.getDXEs()[0].blocks[0].data.empty());
    }
    {
        // An exact record boundary remains accepted without requiring FINAL.
        LDRParser parser;
        CHECK(parser.loadFile(files.Write("nonfinal.ldr", first)));
        CHECK(parser.getDXEs().size() == 1);
        CHECK(parser.getDXEs()[0].blocks.size() == 1);
        CHECK(parser.getDXEs()[0].blocks[0].data == rts);
    }
}

int main() {
    try {
        RunTests();
        std::puts("LDR parser tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
