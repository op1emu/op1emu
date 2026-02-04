#include "cqueue.h"
#include <cqueue.hpp>
#include <cstring>

#define CQUEUE() (*(gto::cqueue<T>*)(q.get()))

template<typename T>
cqueue<T>::cqueue()
{
    q = std::make_shared<gto::cqueue<T>>();
}

template<typename T>
std::size_t cqueue<T>::size()
{
    return CQUEUE().size();
}

template<typename T>
void cqueue<T>::reserve(std::size_t n)
{
    CQUEUE().reserve(n);
}

template<typename T>
void cqueue<T>::push(const void* data, std::size_t n)
{
    T* ptr = (T*)data;
    while(n--) {
        CQUEUE().push(*ptr++);
    }
}

template<typename T>
void cqueue<T>::peek(void* data, std::size_t n)
{
    T* ptr = (T*)data;
    for (int i = 0; i < n; i++) {
        *ptr++ = CQUEUE()[i];
    }
}

template<typename T>
void cqueue<T>::pop(void* data, std::size_t n)
{
    T* ptr = (T*)data;
    while (n--) {
        *ptr++ = CQUEUE().pop();
    }
}

template<typename T>
void cqueue<T>::pop(std::size_t n)
{
    while (n--) {
        CQUEUE().pop();
    }
}

template class cqueue<uint8_t>;
template class cqueue<uint16_t>;