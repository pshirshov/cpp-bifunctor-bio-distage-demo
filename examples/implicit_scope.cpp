#include <mini/implicit.hpp>

#include <iostream>

struct Trace { int pure_calls = 0; };

struct TracingMonad : mini::Monad2<mini::IO> {
    explicit TracingMonad(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) {}

    template<class E, class A>
    auto pure(A value) const {
        ++trace_->pure_calls;
        return mini::IO<E, A>::pure(std::move(value));
    }

private:
    std::shared_ptr<Trace> trace_;
};

template<template<class, class> class Eff, class... Bindings>
auto increment_twice(mini::ImplicitScope<Eff, Bindings...> scope, int initial) {
    const auto F = mini::bio(std::move(scope));
    return F.do_([](auto F, int initial) -> mini::Do<Eff, mini::Never, int> {
        auto value = co_await F.pure(initial);
        auto next = co_await F.pure(value + 1);
        co_return next + 1;
    }, F, initial);
}

int main() {
    using namespace mini;
    auto trace = std::make_shared<Trace>();
    auto program = [&trace] {
        auto scope = implicit_scope<IO>().provide<Monad2>(TracingMonad{trace});
        return increment_twice(scope, 40);
    }();
    auto result = program.unsafe_run();
    std::cout << "IO: " << result.value() << " (" << trace->pure_calls << " local pure calls)\n";
    auto eager = increment_twice(implicit_scope<std::expected>(), 40);
    std::cout << "expected: " << eager.value() << '\n';
}
