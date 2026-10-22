#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <typeinfo>
#include "emu.h"
#include <utils/log.h>

// Unique across every Emulator and every change to one's device map.
static std::atomic<uint64_t> deviceMapGenerations{0};

Emulator::Emulator()
    : generation_(++deviceMapGenerations), directPages(NUM_PAGE_TABLE_ENTRIES, nullptr)
{
    pageTable = std::make_shared<std::array<u8*, NUM_PAGE_TABLE_ENTRIES>>();
}

void Emulator::BindDevice(Device* dev)
{
    generation_ = ++deviceMapGenerations;
    devices.push_back(dev);
    deviceSegments.emplace(dev->BaseAddress(), dev);
    deviceSegments.emplace(dev->EndAddress(), dev);
    dev->BindFastMem(fastmem);
    dev->UpdatePageTable(*pageTable);
    RebuildDirectPages();
}

static Device* get_device(u32 addr, const std::map<u32, Device*>& devices)
{
    auto iter = devices.lower_bound(addr);
    if (iter != devices.end()) {
        Device* dev = iter->second;
        if (addr >= dev->BaseAddress() && addr <= dev->EndAddress()) {
            return dev;
        }
    }
    LogDebug("get_device nullptr: addr = 0x%x", addr);
    return nullptr;
}

Device* Emulator::Find(u32 addr) const
{
    struct Entry {
        uint64_t generation;
        u32 addr;
        Device* dev;
    };
    thread_local Entry cache[256] = {};
    Entry& entry = cache[(addr >> 2) & 255];
    if (entry.generation == generation_ && entry.addr == addr) return entry.dev;
    Device* dev = get_device(addr, deviceSegments);
    entry = {generation_, addr, dev};
    return dev;
}

void Emulator::MemoryRead(u32 addr, void* buffer, int length)
{
    Watch(addr, length);
    while (length > 0) {
        Device* dev = Find(addr);
        if (!dev) {
            length = 0;
        } else {
            u32 offset = addr - dev->BaseAddress();
            u32 len = std::min((u32)length, dev->Size() - offset);
            dev->Read(offset, buffer, len);
            addr += len;
            buffer = (void*)((u8*)buffer + len);
            length -= len;
        }
    }
}

void Emulator::MemoryWrite(u32 addr, const void* buffer, int length)
{
    Watch(addr, length);
    while (length > 0) {
        Device* dev = Find(addr);
        if (!dev) {
            length = 0;
        } else {
            u32 offset = addr - dev->BaseAddress();
            u32 len = std::min((u32)length, dev->Size() - offset);
            dev->Write(offset, buffer, len);
            addr += len;
            buffer = (void*)((u8*)buffer + len);
            length -= len;
        }
    }
}

bool Emulator::Maps(u32 addr, u32 length)
{
    const Device* dev = Find(addr);
    return dev && uint64_t(addr) + length - 1 <= dev->EndAddress();
}

u8 Emulator::MemoryRead8(u32 vaddr)
{
    u8 value = 0;  // an unmapped address reads 0, as in MemoryRead32
    MemoryRead(vaddr, &value, sizeof(value));
    return value;
}

u16 Emulator::MemoryRead16(u32 vaddr)
{
    u16 value = 0;  // an unmapped address reads 0, as in MemoryRead32
    MemoryRead(vaddr, &value, sizeof(value));
    return value;
}

u32 Emulator::MemoryRead32(u32 vaddr)
{
    Watch(vaddr, 4);
    if (readHooks.find(vaddr) != readHooks.end()) {
        return readHooks[vaddr](vaddr);
    } else {
        Device* dev = Find(vaddr);
        if (!dev) {
            return 0;
        } else {
            u32 offset = vaddr - dev->BaseAddress();
            return dev->Read32(offset);
        }
    }
}

u64 Emulator::MemoryRead64(u32 vaddr)
{
    u64 value = 0;  // an unmapped address reads 0, as in MemoryRead32
    MemoryRead(vaddr, &value, sizeof(value));
    return value;
}

void Emulator::MemoryWrite8(u32 vaddr, u8 value)
{
    MemoryWrite(vaddr, &value, sizeof(value));
}

void Emulator::MemoryWrite16(u32 vaddr, u16 value)
{
    MemoryWrite(vaddr, &value, sizeof(value));
}

void Emulator::MemoryWrite32(u32 vaddr, u32 value)
{
    Watch(vaddr, 4);
    if (writeHooks.find(vaddr) != writeHooks.end()) {
        writeHooks[vaddr](vaddr, value);
    } else {
        Device* dev = Find(vaddr);
        if (dev) {
            u32 offset = vaddr - dev->BaseAddress();
            dev->Write32(offset, value);
        }
    }
}

void Emulator::MemoryWrite64(u32 vaddr, u64 value)
{
    MemoryWrite(vaddr, &value, sizeof(value));
}

bool Emulator::MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected)
{
    Watch(vaddr, 4);
    auto atomic = (std::atomic<u32>*)MemoryMap(vaddr);
    return atomic->compare_exchange_strong(expected, value);
}

void* Emulator::MemoryMap(u32 addr)
{
    Device* dev = Find(addr);
    if (!dev) {
        return nullptr;
    } else {
        u32 offset = addr - dev->BaseAddress();
        return dev->Map(offset);
    }
}

void Emulator::RebuildDirectPages()
{
    constexpr u32 kPageSize = 1u << PAGE_BITS;
    constexpr u32 kMmrBase = 0xFFC00000;
    std::fill(directPages.begin(), directPages.end(), nullptr);
    for (Device* dev : devices) {
        if (typeid(*dev) != typeid(MemoryDevice)) continue;
        if ((dev->BaseAddress() | dev->Size()) & (kPageSize - 1)) continue;
        for (u64 page = dev->BaseAddress(); page < u64(dev->EndAddress()) + 1 && page < kMmrBase;
             page += kPageSize) {
            const u32 first = u32(page), last = u32(page + kPageSize - 1);
            // The device map must send every byte of the page to this device:
            // get_device() picks the first segment end at or above an
            // address, so the last byte must find this device and no other
            // device may end or begin inside the page, as one nested in the
            // RAM does.
            if (get_device(last, deviceSegments) != dev) continue;
            bool other = false;
            for (auto it = deviceSegments.lower_bound(first); it != deviceSegments.end() && it->first <= last; ++it)
                other |= it->second != dev;
            if (other) continue;
            if (readHooks.lower_bound(first) != readHooks.upper_bound(last)) continue;
            if (writeHooks.lower_bound(first) != writeHooks.upper_bound(last)) continue;
            directPages[page >> PAGE_BITS] = static_cast<u8*>(dev->Map(first - dev->BaseAddress()));
        }
    }
    for (size_t i = 0; i < watched_.size(); ++i) {
        watchedDirect_[i] = directPages[watched_[i]];
        directPages[watched_[i]] = nullptr;
    }
}

void Emulator::WatchPages(PageWatcher* watcher, std::vector<u32> pages)
{
    assert(std::adjacent_find(pages.begin(), pages.end(), std::greater_equal<u32>()) == pages.end());
    for (size_t i = 0; i < watched_.size(); ++i) directPages[watched_[i]] = watchedDirect_[i];
    watched_ = std::move(pages);
    watchedDirect_.resize(watched_.size());
    for (size_t i = 0; i < watched_.size(); ++i) {
        watchedDirect_[i] = directPages[watched_[i]];
        directPages[watched_[i]] = nullptr;
    }
    watcher_ = watched_.empty() ? nullptr : watcher;
}

bool Emulator::Watched(u32 addr) const
{
    return watcher_ && std::binary_search(watched_.begin(), watched_.end(), addr >> PAGE_BITS);
}

void Emulator::WatchAccess(u32 addr, u32 length)
{
    if (!length || addr >= 0xFFC00000) return;
    const u32 last = u32(std::min<u64>(u64(addr) + length - 1, 0xFFBFFFFF));
    const auto first = std::lower_bound(watched_.begin(), watched_.end(), addr >> PAGE_BITS);
    if (first != watched_.end() && *first <= last >> PAGE_BITS) watcher_->BeforeAccess();
}

bool Emulator::Quiet(u32 addr) const
{
    return Plain(addr) && !Watched(addr);
}

bool Emulator::Plain(u32 addr) const
{
    constexpr u32 kPageSize = 1u << PAGE_BITS;
    if (addr >= 0xFFC00000) return false;
    const Device* dev = Find(addr);
    if (!dev || typeid(*dev) != typeid(MemoryDevice)) return false;
    const u32 first = addr & ~(kPageSize - 1), last = first + (kPageSize - 1);
    return readHooks.lower_bound(first) == readHooks.upper_bound(last) &&
           writeHooks.lower_bound(first) == writeHooks.upper_bound(last);
}

bool Emulator::HostWorkAfter(u32 addr, u32 length, bool write) const
{
    if (addr < 0xFFC00000) return !Plain(addr) || (Watched(addr) && watcher_->HostWorkAfterAccess());
    const Device* dev = Find(addr);
    if (!dev || uint64_t(addr) + length - 1 > dev->EndAddress()) return true;
    if (readHooks.count(addr) || writeHooks.count(addr)) return true;
    return dev->HostWorkAfterAccess(addr - dev->BaseAddress(), write);
}

// The device an MMR access of sizeof(T) bytes at addr reaches as a whole, with
// no hook to run, or null.
template <typename T>
static Device* WholeMmrAccess(Device* dev, u32 addr, bool hooks)
{
    if (addr < 0xFFC00000 || hooks || !dev) return nullptr;
    return uint64_t(addr) + sizeof(T) - 1 <= dev->EndAddress() ? dev : nullptr;
}

template <typename T>
T Emulator::ReadAttended(u32 addr, bool& hostWork, bool& stable)
{
    const bool hooks = !readHooks.empty() || !writeHooks.empty();
    if (Device* dev = WholeMmrAccess<T>(addr >= 0xFFC00000 ? Find(addr) : nullptr, addr, hooks)) {
        const u32 offset = addr - dev->BaseAddress();
        T value = 0;
        if constexpr (sizeof(T) == 4) value = dev->Read32(offset);  // as MemoryRead32
        else dev->Read(offset, &value, sizeof(T));                  // as MemoryRead
        hostWork = dev->HostWorkAfterAccess(offset, false);
        stable = dev->ReadStable(offset);
        return value;
    }
    T value;
    if constexpr (sizeof(T) == 1) value = MemoryRead8(addr);
    else if constexpr (sizeof(T) == 2) value = MemoryRead16(addr);
    else value = MemoryRead32(addr);
    hostWork = HostWorkAfter(addr, sizeof(T), false);
    // Plain memory with no hook in its page: only stores change it.
    stable = Quiet(addr);
    return value;
}

template <typename T>
void Emulator::WriteAttended(u32 addr, T value, bool& hostWork)
{
    const bool hooks = !readHooks.empty() || !writeHooks.empty();
    if (Device* dev = WholeMmrAccess<T>(addr >= 0xFFC00000 ? Find(addr) : nullptr, addr, hooks)) {
        const u32 offset = addr - dev->BaseAddress();
        if constexpr (sizeof(T) == 4) dev->Write32(offset, value);  // as MemoryWrite32
        else dev->Write(offset, &value, sizeof(T));                 // as MemoryWrite
        hostWork = dev->HostWorkAfterAccess(offset, true);
        return;
    }
    if constexpr (sizeof(T) == 1) MemoryWrite8(addr, value);
    else if constexpr (sizeof(T) == 2) MemoryWrite16(addr, value);
    else MemoryWrite32(addr, value);
    hostWork = HostWorkAfter(addr, sizeof(T), true);
}

template u8 Emulator::ReadAttended<u8>(u32, bool&, bool&);
template u16 Emulator::ReadAttended<u16>(u32, bool&, bool&);
template u32 Emulator::ReadAttended<u32>(u32, bool&, bool&);
template void Emulator::WriteAttended<u8>(u32, u8, bool&);
template void Emulator::WriteAttended<u16>(u32, u16, bool&);
template void Emulator::WriteAttended<u32>(u32, u32, bool&);

bool Emulator::IsMemoryValid(u32 addr)
{
    return Find(addr) != nullptr;
}

void Emulator::PatchSoftwareBreak(u32 addr, u8 num)
{
}

void Emulator::RemoveSoftwareBreak(u32 addr)
{
}