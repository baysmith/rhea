#include <rhea/iostream.hpp>
#include <rhea/simplex_solver.hpp>

#if USE_COUNTING_ALLOCATOR
void* operator new(std::size_t sz) {
    std::printf("Unexpected global allocation, size = %zu\n", sz);
    void *ptr = std::malloc(sz);
    if (ptr)
        return ptr;
    else
        throw std::bad_alloc{};
}

void operator delete(void* ptr) noexcept
{
    std::printf("Unexpected global deallocation");
    std::free(ptr);
}
#endif // USE_COUNTING_ALLOCATOR

main()
{
    rhea::variable left, mid, right;
    rhea::simplex_solver solver;

    // clang-format off
    solver.add_constraints(
    {
        mid == (left + right) / 2,
        right == left + 10,
        right <= 100,
        left >= 0
    });
    // clang-format on
    solver.suggest(mid, 2);

    std::cout << "Expected output: 0 5 10" << std::endl;
    std::cout << left << " " << mid << " " << right << std::endl;
    std::cout << std::endl;

    solver.suggest(mid, 7);

    std::cout << "Expected output: 2 7 12" << std::endl;
    std::cout << left << " " << mid << " " << right << std::endl;
#if USE_COUNTING_ALLOCATOR
    std::cout << "Memory " << rhea::counting_allocator_count << std::endl;
#endif // USE_COUNTING_ALLOCATOR
}
