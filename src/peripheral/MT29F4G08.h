#pragma once

#include "cpu/nand.h"
#include <vector>
#include <fstream>
#include <unordered_map>

class BlackFinCpu;

class MT29F4G08 : public NandFlash {
public:
    // snapshot=true: the backing file is opened read-only and never written;
    // dirtied pages are kept in an in-memory overlay instead. This sidesteps a
    // firmware fragility: the OP-1's yaffs2 writes block summaries mid-session,
    // and a kill without a clean unmount can leave them stale — the next
    // boot's mount scan then loses recently-written files (assert
    // "db2.cpp:273 Can't open /yaffs2/content/op1.db", stuck on the "Missing
    // content folder / Please connect USB" screen). Guest changes (settings,
    // tape recordings) are discarded on exit.
    MT29F4G08(BlackFinCpu& cpu, const std::string& storagePath, bool snapshot = false);
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

    void SetBusy();
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

    BlackFinCpu& cpu;
    std::string storagePath;
    std::fstream storageFile;
    bool snapshot_ = false;
    std::unordered_map<u32, std::vector<u8>> dirtyPages_;
    std::vector<u8> pageBuffer;
    std::vector<u8> programBuffer;

    u8 currentCommand = 0;
    u8 statusRegister = 0;

    // Address handling (5 cycles: 2 column + 3 row)
    u8 addressCycle = 0;
    u8 addressBytes[5] = {0};

    u32 dataOffset = 0;
    u32 idOffset = 0;

    bool isBusy = false;
};