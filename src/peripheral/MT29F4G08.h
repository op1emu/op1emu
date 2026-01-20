#pragma once

#include "cpu/nand.h"
#include <vector>
#include <fstream>

class MT29F4G08 : public NandFlash {
public:
    MT29F4G08(const std::string& storagePath);
    ~MT29F4G08();

    void SendCommand(u8 command) override;
    void SendAddress(u8 address) override;
    u8 ReadData() override;
    void WriteData(u8 data) override;
    void StartPageRead() override;
    void StartPageWrite() override;
    u32 PageWrite(const u8* data, u32 length) override;
    u32 PageRead(u8* data, u32 length) override;
    void SetReadCallback(ReadCallback callback) override;
    bool IsDataReady() const override;
    bool IsBusy() const override;

protected:
    ReadCallback readCallback;

    void HandleCommand(u8 command);
    void ExecuteRead();
    void ExecuteProgram();
    void ExecuteErase();
    u8 HandleReadID();
    void LoadPage(u32 pageNumber);
    void SavePage(u32 pageNumber);
    u32 GetCurrentPage() const;
    u32 GetColumnAddress() const;
    u32 GetBlockAddress() const;

    std::string storagePath;
    std::fstream storageFile;
    std::vector<u8> pageBuffer;
    std::vector<u8> programBuffer;

    u8 currentCommand = 0;
    u8 statusRegister = 0;

    // Address handling (5 cycles: 2 column + 3 row)
    u8 addressCycle = 0;
    u8 addressBytes[5] = {0};

    u32 dataOffset = 0;
    u32 idOffset = 0;
};