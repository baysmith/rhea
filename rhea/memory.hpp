#pragma once

#include <iostream>

namespace rhea
{

#if USE_ALLOCATOR_FUNCTION
typedef void *allocator_function(void *user_data, void *ptr, size_t new_size, size_t old_size);
extern allocator_function* alloc_function;
extern void* allocator_user_data;
void set_allocator_function(allocator_function* allocator_f, void *user_data);

template <typename T>
struct function_allocator {
    typedef T value_type;
    function_allocator() = default;
    template <class U>
    constexpr function_allocator(const function_allocator<U>&) noexcept
    {
    }
    T* allocate(std::size_t n)
    {
        void* p = alloc_function(allocator_user_data, 0, n * sizeof(T), 0);
        return (T*)p;
    }
    void deallocate(T* p, std::size_t n) noexcept {
        alloc_function(allocator_user_data, p, 0, n * sizeof(T));
    }
};

template <class T, class U>
bool operator==(const function_allocator<T>&, const function_allocator<U>&)
{
    return true;
}

template <class T, class U>
bool operator!=(const function_allocator<T>&, const function_allocator<U>&)
{
    return false;
}
#endif // USE_ALLOCATOR_FUNCTION

#if USE_COUNTING_ALLOCATOR
extern size_t counting_allocator_count;
template <typename T>
struct counting_allocator {
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


#if USE_ALLOCATOR_FUNCTION
template <typename T>
using allocator = function_allocator<T>;
#else
#if USE_COUNTING_ALLOCATOR
template <typename T>
using allocator = counting_allocator<T>;
#else
template <typename T>
using allocator = default_allocator<T>;
#endif // USE_COUNTING_ALLOCATOR
#endif // USE_ALLOCATOR_FUNCTION

template<typename T>
allocator<T>& get_allocator()
{
    static allocator<T> alloc_;
    return alloc_;
}

} // namespace rhea
