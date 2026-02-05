#include "MT29F4G08.h"
#include "cpu/cpu.h"
#include "utils/log.h"
#include <cstring>
#include <algorithm>

// MT29F4G08 specifications
static constexpr u32 PAGE_SIZE = 2048;
static constexpr u32 OOB_SIZE = 64;
static constexpr u32 PAGE_TOTAL_SIZE = PAGE_SIZE + OOB_SIZE;
static constexpr u32 PAGES_PER_BLOCK = 64;
static constexpr u32 BLOCK_SIZE = PAGE_SIZE * PAGES_PER_BLOCK;
static constexpr u32 TOTAL_BLOCKS = 4096;
static constexpr u32 TOTAL_PAGES = TOTAL_BLOCKS * PAGES_PER_BLOCK;
// Storage layout: all pages first, then all OOB areas
static constexpr u64 OOB_AREA_OFFSET = static_cast<u64>(TOTAL_PAGES) * PAGE_SIZE;
static constexpr u64 DEVICE_SIZE = OOB_AREA_OFFSET + (static_cast<u64>(TOTAL_PAGES) * OOB_SIZE); // ~ 4GB

static constexpr u8 COLUMN_CYCLES = 2;
static constexpr u8 ROW_CYCLES = 3;
static constexpr u8 TOTAL_ADDRESS_CYCLES = 5;
static constexpr u8 ERASE_ADDRESS_CYCLES = 3;

enum NandStatus : int {
    WriteEnabled = 0x80,
    Ready = 0x40,
    AReady = 0x20,
    Fail = 0x01,
};

// ONFI Commands
enum NandCommand : u8 {
    CMD_READ1 = 0x00,
    CMD_READ2 = 0x30,
    CMD_RANDOM_READ1 = 0x05,
    CMD_RANDOM_READ2 = 0xE0,
    CMD_READ_STATUS = 0x70,
    CMD_PAGE_PROGRAM1 = 0x80,
    CMD_PAGE_PROGRAM2 = 0x10,
    CMD_RANDOM_WRITE = 0x85,
    CMD_BLOCK_ERASE1 = 0x60,
    CMD_BLOCK_ERASE2 = 0xD0,
    CMD_RESET = 0xFF,
};

static constexpr u8 ERASED_VALUE = 0xFF;

MT29F4G08::MT29F4G08(BlackFinCpu& cpu, const std::string& storagePath)
    : cpu(cpu)
    , storagePath(storagePath)
    , pageBuffer(PAGE_TOTAL_SIZE, ERASED_VALUE)
    , programBuffer(PAGE_TOTAL_SIZE, ERASED_VALUE)
    , statusRegister(NandStatus::Ready | NandStatus::WriteEnabled)
{
    // Try to open existing file, or create new one
    storageFile.open(storagePath, std::ios::in | std::ios::out | std::ios::binary);
    if (!storageFile.is_open()) {
        // Create new file
        storageFile.open(storagePath, std::ios::out | std::ios::binary);
        if (storageFile.is_open()) {
            // Initialize with erased state (0xFF)
            std::vector<u8> eraseBuffer(PAGE_TOTAL_SIZE, ERASED_VALUE);
            for (u32 page = 0; page < TOTAL_PAGES; ++page) {
                storageFile.write(reinterpret_cast<char*>(eraseBuffer.data()), PAGE_TOTAL_SIZE);
            }
            storageFile.close();
            storageFile.open(storagePath, std::ios::in | std::ios::out | std::ios::binary);
        }
    }

    if (!storageFile.is_open()) {
        LogError("MT29F4G08: Failed to open storage file: %s", storagePath.c_str());
    }
}

MT29F4G08::~MT29F4G08() {
    if (storageFile.is_open()) {
        storageFile.close();
    }
}

void MT29F4G08::SendCommand(u8 command) {
    addressCycle = 0;
    HandleCommand(command);
}

void MT29F4G08::HandleCommand(u8 command) {
    switch (command) {
        case CMD_RESET:
            currentCommand = CMD_READ1;
            addressCycle = 0;
            dataOffset = 0;
            idOffset = 0;
            SetBusy();
            break;
        case CMD_READ1:
            addressCycle = 0;
            break;
        case CMD_READ2:
            if (currentCommand == CMD_READ1) {
                ExecuteRead();
            }
            break;
        case CMD_RANDOM_READ1:
            addressCycle = 0;
            break;
        case CMD_RANDOM_READ2:
            if (currentCommand == CMD_RANDOM_READ1) {
                dataOffset = GetColumnAddress();
            }
            break;
        case CMD_BLOCK_ERASE1:
            // Expecting 3 address cycles for block address
            addressCycle = 2;
            break;
        case CMD_BLOCK_ERASE2:
            if (currentCommand == CMD_BLOCK_ERASE1) {
                ExecuteErase();
            }
            break;
        case CMD_READ_STATUS:
            // Status will be read via ReadData()
            break;
        case CMD_PAGE_PROGRAM1:
            addressCycle = 0;
            dataOffset = 0;
            programBuffer.assign(PAGE_TOTAL_SIZE, ERASED_VALUE);
            break;
        case CMD_RANDOM_WRITE:
            addressCycle = 0;
            dataOffset = 0;
            break;
        case CMD_PAGE_PROGRAM2:
            if (currentCommand == CMD_PAGE_PROGRAM1 || currentCommand == CMD_RANDOM_WRITE) {
                ExecuteProgram();
            }
            break;
        default:
            LogWarn("MT29F4G08: Unknown command 0x%02X", command);
            break;
    }
    currentCommand = command;
}

void MT29F4G08::SendAddress(u8 address) {
    // Normal addressing: 5 cycles (2 column + 3 row)
    if (addressCycle < TOTAL_ADDRESS_CYCLES) {
        addressBytes[addressCycle++] = address;
    }
    if (currentCommand == CMD_RANDOM_WRITE && addressCycle == COLUMN_CYCLES) {
        dataOffset = GetColumnAddress();
    } else if (currentCommand == CMD_PAGE_PROGRAM1 && addressCycle == TOTAL_ADDRESS_CYCLES) {
        dataOffset = GetColumnAddress();
    }
}

void MT29F4G08::SetBusy() {
    isBusy = true;
    cpu.QueueEvent([this]() {
        isBusy = false;
    }, std::chrono::nanoseconds(100)); // Simulate 100ns operation time
}

void MT29F4G08::StartPageRead() {
}

void MT29F4G08::StartPageWrite() {
}

u8 MT29F4G08::ReadData() {
    if (currentCommand == CMD_READ_STATUS) {
        return statusRegister;
    }
    if (dataOffset < PAGE_TOTAL_SIZE) {
        return pageBuffer[dataOffset++];
    }
    return ERASED_VALUE;
}

void MT29F4G08::WriteData(u8 data) {
    if (dataOffset < PAGE_TOTAL_SIZE) {
        // NAND flash behavior: can only change 1s to 0s
        programBuffer[dataOffset] &= data;
        dataOffset++;
    }
}

u32 MT29F4G08::PageWrite(const u8* data, u32 length) {
    u32 column = GetColumnAddress();
    u32 maxWrite = std::min(length, PAGE_TOTAL_SIZE - dataOffset);
    for (u32 i = 0; i < maxWrite; ++i) {
        programBuffer[dataOffset + i] &= data[i];
    }
    dataOffset += maxWrite;
    return maxWrite;
}

u32 MT29F4G08::PageRead(u8* data, u32 length) {
    u32 readLen = std::min(length, PAGE_TOTAL_SIZE - dataOffset);
    std::memcpy(data, pageBuffer.data() + dataOffset, readLen);
    dataOffset += readLen;
    return readLen;
}

void MT29F4G08::SetReadCallback(ReadCallback callback) {
    readCallback = callback;
}

bool MT29F4G08::IsDataReady() const {
    if (currentCommand == CMD_READ2) {
        return dataOffset < PAGE_TOTAL_SIZE;
    }
    return currentCommand == CMD_READ_STATUS;
}

bool MT29F4G08::IsBusy() const {
    return isBusy;
}

u32 MT29F4G08::GetColumnAddress() const {
    // Column address is in first 2 bytes (12 bits used for 2048 + 64 byte page)
    return (addressBytes[0] | (static_cast<u32>(addressBytes[1] & 0x0F) << 8));
}

u32 MT29F4G08::GetCurrentPage() const {
    // Row address is in bytes 2-4 (3 bytes for page/block address)
    return addressBytes[2] |
           (static_cast<u32>(addressBytes[3]) << 8) |
           (static_cast<u32>(addressBytes[4] & 0x03) << 16);
}

u32 MT29F4G08::GetBlockAddress() const {
    u32 pageNumber = GetCurrentPage();
    return pageNumber / PAGES_PER_BLOCK;
}

void MT29F4G08::LoadPage(u32 pageNumber) {
    if (pageNumber >= TOTAL_PAGES) {
        std::fill(pageBuffer.begin(), pageBuffer.end(), ERASED_VALUE);
        return;
    }

    if (!storageFile.is_open()) {
        std::fill(pageBuffer.begin(), pageBuffer.end(), ERASED_VALUE);
        return;
    }

    // Read page data
    u64 pageOffset = static_cast<u64>(pageNumber) * PAGE_SIZE;
    storageFile.seekg(pageOffset, std::ios::beg);
    storageFile.read(reinterpret_cast<char*>(pageBuffer.data()), PAGE_SIZE);

    // Read OOB data
    u64 oobOffset = OOB_AREA_OFFSET + (static_cast<u64>(pageNumber) * OOB_SIZE);
    storageFile.seekg(oobOffset, std::ios::beg);
    storageFile.read(reinterpret_cast<char*>(pageBuffer.data() + PAGE_SIZE), OOB_SIZE);

    if (!storageFile) {
        std::fill(pageBuffer.begin(), pageBuffer.end(), ERASED_VALUE);
        storageFile.clear();
    }
}

void MT29F4G08::SavePage(u32 pageNumber) {
    if (pageNumber >= TOTAL_PAGES) {
        return;
    }

    if (!storageFile.is_open()) {
        return;
    }

    // Load current page data
    LoadPage(pageNumber);

    // Apply program operation (AND with program buffer - can only clear bits)
    for (u32 i = 0; i < PAGE_TOTAL_SIZE; ++i) {
        pageBuffer[i] &= programBuffer[i];
    }

    // Write page data
    u64 pageOffset = static_cast<u64>(pageNumber) * PAGE_SIZE;
    storageFile.seekp(pageOffset, std::ios::beg);
    storageFile.write(reinterpret_cast<char*>(pageBuffer.data()), PAGE_SIZE);

    // Write OOB data
    u64 oobOffset = OOB_AREA_OFFSET + (static_cast<u64>(pageNumber) * OOB_SIZE);
    storageFile.seekp(oobOffset, std::ios::beg);
    storageFile.write(reinterpret_cast<char*>(pageBuffer.data() + PAGE_SIZE), OOB_SIZE);

    storageFile.flush();

    if (!storageFile) {
        storageFile.clear();
    }
}

void MT29F4G08::ExecuteRead() {
    SetBusy();
    u32 pageNumber = GetCurrentPage();
    u32 column = GetColumnAddress();

    LoadPage(pageNumber);
    dataOffset = column;

    if (readCallback) {
        readCallback(*this);
    }
}

void MT29F4G08::ExecuteProgram() {
    SetBusy();
    u32 pageNumber = GetCurrentPage();
    SavePage(pageNumber);
}

void MT29F4G08::ExecuteErase() {
    SetBusy();
    u32 blockNumber = GetBlockAddress();

    if (blockNumber >= TOTAL_BLOCKS) {
        return;
    }

    if (!storageFile.is_open()) {
        return;
    }

    // Erase all pages in the block
    std::vector<u8> erasedPage(PAGE_SIZE, ERASED_VALUE);
    std::vector<u8> erasedOob(OOB_SIZE, ERASED_VALUE);
    u32 startPage = blockNumber * PAGES_PER_BLOCK;

    for (u32 i = 0; i < PAGES_PER_BLOCK; ++i) {
        u32 pageNumber = startPage + i;

        // Erase page data
        u64 pageOffset = static_cast<u64>(pageNumber) * PAGE_SIZE;
        storageFile.seekp(pageOffset, std::ios::beg);
        storageFile.write(reinterpret_cast<char*>(erasedPage.data()), PAGE_SIZE);

        // Erase OOB data
        u64 oobOffset = OOB_AREA_OFFSET + (static_cast<u64>(pageNumber) * OOB_SIZE);
        storageFile.seekp(oobOffset, std::ios::beg);
        storageFile.write(reinterpret_cast<char*>(erasedOob.data()), OOB_SIZE);
    }
    storageFile.flush();

    if (!storageFile) {
        storageFile.clear();
    }
}