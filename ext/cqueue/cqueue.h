#pragma once

#include <memory>

template<typename T>
class cqueue {
public:
    cqueue();

    void reserve(std::size_t n);
    void push(const void* data, std::size_t n);
    std::size_t size();
    void peek(void* data, std::size_t n);
    void pop(void* data, std::size_t n);
    void pop(std::size_t n);

private:
    std::shared_ptr<void> q;
};