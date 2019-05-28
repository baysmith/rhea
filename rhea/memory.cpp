#include "memory.hpp"

#if USE_ALLOCATOR_FUNCTION
namespace rhea
{

allocator_function* alloc_function;
void* allocator_user_data;

void set_allocator_function(allocator_function* allocator_f, void* user_data)
{
    alloc_function = allocator_f;
    allocator_user_data = user_data;
}

} // namespace rhea
#endif // USE_ALLOCATOR_FUNCTION

#if USE_COUNTING_ALLOCATOR
size_t rhea::counting_allocator_count;
#endif // USE_COUNTING_ALLOCATOR
