#include "../examples/application.hpp"
#include "test_support.hpp"

#include <iostream>

template<class A, class E>
std::expected<A, E> evaluate(std::expected<A, E> value) { return value; }

template<class E, class A>
std::expected<A, E> evaluate(const mini::IO<E, A>& value) { return value.unsafe_run(); }

template<template<class, class> class F>
void application_contract() {
    using namespace example;
    for (const auto mode : {Mode::live, Mode::test}) {
        auto plan = di::Planner{}.plan(module<F>(), di::roots(di::key<Application<F>>()), di::activate(mode));
        check(plan.has_value(), "example plans for each effect family and activation");
        auto graph = plan->produce().unsafe_run().value();
        auto app = graph->get(di::key<Application<F>>());
        auto success = app->run(UserId{1});
        auto failure = app->run(UserId{99});
        static_assert(std::same_as<decltype(success), Effect<F, LookupError, std::vector<std::string>>>);
        graph.reset();
        app.reset();
        const auto expected = mode == Mode::live ? std::vector<std::string>{"Hello, Ada!"}
            : std::vector<std::string>{"Hello, Test Ada!", "Welcome, Test Ada."};
        check(evaluate(success) == expected, "same application contract across interpreters and axes");
        check(evaluate(failure) == std::expected<std::vector<std::string>, LookupError>(std::unexpected(LookupError::missing_user)),
              "application preserves declared errors");
    }
}

// Behavioral-Active / Blackbox-Group; origin: Specified.
int main() {
    application_contract<mini::IO>();
    application_contract<std::expected>();
    std::cout << "Full stack example checks passed\n";
}
