#include <rhea/iostream.hpp>
#include <rhea/simplex_solver.hpp>

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
}
