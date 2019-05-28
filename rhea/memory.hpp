#pragma once

#include <iostream>

namespace rhea
{

#if USE_COUNTING_ALLOCATOR
extern size_t counting_allocator_count;
template <typename T>
struct counting_allocator {
    std::allocator<T> alloc_;
    typedef T value_type;
    counting_allocator() = default;
    template <class U>
    constexpr counting_allocator(const counting_allocator<U>&) noexcept
    {
    }
    T* allocate(std::size_t n)
    {
        counting_allocator_count += sizeof(T)*n;
        return (T*)std::malloc(sizeof(T)*n);
    }
    void deallocate(T* p, std::size_t n) noexcept {
        std::free(p);
    }
};

template <class T, class U>
bool operator==(const counting_allocator<T>&, const counting_allocator<U>&)
{
    return true;
}

template <class T, class U>
bool operator!=(const counting_allocator<T>&, const counting_allocator<U>&)
{
    return false;
}
#endif // USE_COUNTING_ALLOCATOR

template <typename T>
struct default_allocator {
    std::allocator<T> alloc_;
    typedef T value_type;
    default_allocator() = default;
    template <class U>
    constexpr default_allocator(const default_allocator<U>&) noexcept
    {
    }
    T* allocate(std::size_t n) { return alloc_.allocate(n); }
    void deallocate(T* p, std::size_t n) noexcept { alloc_.deallocate(p, n); }
};

template <class T, class U>
bool operator==(const default_allocator<T>&, const default_allocator<U>&)
{
    return true;
}

template <class T, class U>
bool operator!=(const default_allocator<T>&, const default_allocator<U>&)
{
    return false;
}


#if USE_COUNTING_ALLOCATOR
template <typename T>
using allocator = counting_allocator<T>;
#else
template <typename T>
using allocator = default_allocator<T>;
#endif

template<typename T>
allocator<T>& get_allocator()
{
    static allocator<T> alloc_;
    return alloc_;
}

} // namespace rhea
