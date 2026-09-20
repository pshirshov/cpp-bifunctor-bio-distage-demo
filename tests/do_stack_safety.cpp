#include <mini/implicit.hpp>
#include "test_support.hpp"

#include <iostream>

struct Counts { int acquired = 0; int released = 0; };
struct Guard {
    Counts& counts;
    explicit Guard(Counts& counts) : counts(counts) { ++counts.acquired; }
    ~Guard() { ++counts.released; }
};
enum class End { success, failure, defect };

mini::IO<int, int> recursive(int depth, End end, std::shared_ptr<Counts> counts) {
    using namespace mini;
    const auto F = bio(implicit_scope<IO>());
    return F.do_([](auto F, int depth, End end, auto counts) -> Do<IO, int, int> {
        Guard guard{*counts};
        if (depth == 0) {
            if (end == End::failure) { co_return co_await F.fail(42); }
            if (end == End::defect) { throw std::runtime_error("bottom defect"); }
            co_return 0;
        }
        auto value = co_await recursive(depth - 1, end, counts);
        co_return value + 1;
    }, F, depth, end, counts);
}

template<template<class, class> class Eff>
auto sequential(int depth) {
    using namespace mini;
    const auto F = bio(implicit_scope<Eff>());
    return F.do_([](auto F, int depth) -> Do<Eff, Never, int> {
        int result = 0;
        for (int index = 0; index < depth; ++index) { result = co_await F.pure(result + 1); }
        co_return result;
    }, F, depth);
}

// Behavioral-Active / Blackbox-Group; origin: Specified.
int main() {
    using namespace mini;
    constexpr int depth = 100000;
    auto program = sequential<IO>(depth);
    auto copy = program;
    check(program.unsafe_run() == depth && copy.unsafe_run() == depth, "deep sequential awaits are repeatable and stack-safe");
    check(sequential<std::expected>(depth) == depth, "expected drives sequential awaits iteratively");
    for (auto end : {End::success, End::failure, End::defect}) {
        auto counts = std::make_shared<Counts>();
        auto recursive_program = recursive(depth, end, counts);
        check(counts->acquired == 0, "recursive coroutine construction is lazy");
        auto exit = recursive_program.run_exit();
        if (end == End::success) {
            check(std::get<Success<int>>(exit).value == depth, "non-tail coroutine recursion is stack-safe");
        } else if (end == End::failure) {
            check(std::get<Failure<int>>(exit).error == 42, "deep failure unwinds coroutine frames iteratively");
        } else {
            check(std::holds_alternative<Defect>(exit), "deep defects unwind coroutine frames iteratively");
        }
        check(counts->acquired == depth + 1 && counts->released == counts->acquired,
              "all deep coroutine locals are released on every exit path");
    }
    std::cout << "Coroutine stack safety checks passed\n";
}
