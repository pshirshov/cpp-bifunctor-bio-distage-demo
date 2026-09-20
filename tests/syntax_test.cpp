#include <mini/bio.hpp>
#include "test_support.hpp"

#include <iostream>
#include <string>

template<class A, class E>
std::expected<A, E> evaluate(std::expected<A, E> value) { return value; }

template<class E, class A>
std::expected<A, E> evaluate(const mini::IO<E, A>& effect) { return effect.unsafe_run(); }

template<template<class, class> class Eff>
void combinators() {
    const mini::Bio F{mini::Error2<Eff>{}};
    mini::Effect<Eff, std::string, int> source = F.pure(7);
    static_assert(std::same_as<decltype(source), mini::Effect<Eff, std::string, int>>);
    auto mapped = mini::map(source, [](auto value) { return std::to_string(value); });
    static_assert(std::same_as<decltype(mapped), mini::Effect<Eff, std::string, std::string>>);
    check(evaluate(mapped) == "7", "map infers family, error, input and output types");
    check(evaluate(mini::map(source, std::identity{})) == 7, "map materializes reference-returning callbacks");
    auto bound = mini::flat_map(source, [F](auto value) { return F.pure(value + 1); });
    check(evaluate(bound) == 8, "flat_map infers the dictionary");

    mini::Effect<Eff, std::string, int> failure = F.fail(std::string("error"));
    auto mapped_error = mini::map_error(failure, [](const auto& error) { return error.size(); });
    static_assert(std::same_as<decltype(mapped_error), mini::Effect<Eff, std::size_t, int>>);
    check(evaluate(mapped_error) == std::expected<int, std::size_t>(std::unexpected(std::size_t{5})),
          "map_error infers a new error type and preserves the value type");
    const auto recover = [F](auto error) -> mini::Effect<Eff, int, std::size_t> { return F.pure(error.size()); };
    auto recovered = mini::catch_all(F.fail(std::string("error")), recover);
    static_assert(std::same_as<decltype(recovered), mini::Effect<Eff, int, std::size_t>>);
    check(evaluate(recovered) == std::size_t{5}, "catch_all infers the replacement error channel");
    auto both = mini::bimap(source, [](const auto& error) { return error.size(); }, [](auto value) {
        return std::to_string(value);
    });
    static_assert(std::same_as<decltype(both), mini::Effect<Eff, std::size_t, std::string>>);
    check(evaluate(both) == "7", "bimap infers both output types");
}

template<template<class, class> class Eff>
void traversal() {
    const mini::Bio F{mini::Error2<Eff>{}};
    constexpr bool lazy = std::same_as<mini::Effect<Eff, std::string, int>, mini::IO<std::string, int>>;
    int calls = 0;
    const auto step = [F, &calls](int value) -> mini::Effect<Eff, std::string, int> {
        ++calls;
        return F.pure(value * 2);
    };
    auto empty = mini::traverse(std::vector<int>{}, step);
    static_assert(std::same_as<decltype(empty), mini::Effect<Eff, std::string, std::vector<int>>>);
    check(evaluate(empty) == std::vector<int>{} && calls == 0, "empty traversal infers types without invoking the step");

    auto values = std::vector<int>{1, 2, 3};
    auto program = mini::traverse(values, step);
    values.clear();
    check(calls == (lazy ? 0 : 3), "traversal preserves interpreter evaluation semantics");
    auto copy = program;
    check(evaluate(program) == std::vector<int>({2, 4, 6}), "traversal snapshots elements and preserves order");
    check(evaluate(copy) == std::vector<int>({2, 4, 6}) && calls == (lazy ? 6 : 3),
          "repeat execution does not share a mutable accumulator");

    calls = 0;
    auto failed = mini::traverse(std::vector<int>{1, 2, 3}, [F, &calls](int value) -> mini::Effect<Eff, std::string, int> {
        ++calls;
        if (value == 2) { return F.fail(std::string("stop")); }
        return F.pure(value);
    });
    check(calls == (lazy ? 0 : 2), "eager traversal short-circuits and lazy traversal remains suspended");
    check(evaluate(failed) == std::expected<std::vector<int>, std::string>(std::unexpected("stop")) && calls == 2,
          "typed failure prevents later step invocation");
    check(!evaluate(failed) && calls == (lazy ? 4 : 2), "failed traversal is repeatable");
}

// Behavioral-Active / Blackbox-Atomic; origin: Specified.
int main() {
    static_assert(std::same_as<mini::Effect<std::expected, std::string, int>, std::expected<int, std::string>>);
    static_assert(std::same_as<mini::Effect<mini::IO, std::string, int>, mini::IO<std::string, int>>);
    combinators<mini::IO>();
    combinators<std::expected>();
    traversal<mini::IO>();
    traversal<std::expected>();
    std::vector<int> executed;
    auto program = mini::traverse(std::vector<int>{1, 2, 3}, [&executed](int value) {
        return mini::IO<std::string, int>::sync([&executed, value] {
            executed.push_back(value);
            if (value == 2) {
                throw std::runtime_error("step defect");
            }
            return value;
        });
    });
    check(executed.empty(), "traversal does not execute nested effects during construction");
    check(std::holds_alternative<mini::Defect>(program.run_exit()) && executed == std::vector<int>({1, 2}),
          "a step defect stops traversal without becoming a declared error");
    std::cout << "Inferred syntax checks passed\n";
}
