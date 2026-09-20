#include <mini/implicit.hpp>
#include "test_support.hpp"

#include <iostream>
#include <string>

enum class Error { missing };
enum class OtherError { unavailable };

struct Calls { int started = 0; int released = 0; int after = 0; int pure = 0; int binds = 0; int drivers = 0; };
struct Guard {
    std::shared_ptr<Calls> calls;
    ~Guard() { ++calls->released; }
};

template<template<class, class> class Eff>
struct CountingMonad : mini::Monad2<Eff> {
    std::shared_ptr<Calls> calls;
    explicit CountingMonad(std::shared_ptr<Calls> calls) : calls(std::move(calls)) {}

    template<class E, class A>
    auto pure(A value) const {
        ++calls->pure;
        return mini::Monad2<Eff>::template pure<E>(std::move(value));
    }

    template<class Source, class Next>
    auto flat_map(Source source, Next next) const {
        ++calls->binds;
        return mini::Monad2<Eff>::flat_map(std::move(source), std::move(next));
    }
};

template<template<class, class> class Eff>
struct CountingDo : mini::Do2<Eff> {
    std::shared_ptr<Calls> calls;
    explicit CountingDo(std::shared_ptr<Calls> calls) : calls(std::move(calls)) {}

    template<class Dictionary, class Factory, class... Args>
    auto do_(Dictionary dictionary, Factory factory, Args... args) const {
        ++calls->drivers;
        return mini::Do2<Eff>::do_(std::move(dictionary), std::move(factory), std::move(args)...);
    }
};

template<class A, class E>
auto evaluate(std::expected<A, E> value) { return value; }
template<class E, class A>
auto evaluate(const mini::IO<E, A>& value) { return value.unsafe_run(); }

template<class Task, class Source>
concept CanAwait = requires(typename Task::promise_type promise, Source source) {
    promise.await_transform(std::move(source));
};

template<class Accessor, class Factory>
concept CanDo = requires(Accessor F, Factory factory) { F.do_(factory); };

template<template<class, class> class Eff>
void contract() {
    using namespace mini;
    auto calls = std::make_shared<Calls>();
    const auto scope = implicit_scope<Eff>().template provide<Monad2>(CountingMonad<Eff>{calls});
    const auto F = bio(scope);
    static_assert(decltype(scope)::template can_summon<Do2>());
    auto program = F.do_([](auto F, auto calls, int initial) -> Do<Eff, Error, std::vector<int>> {
        ++calls->started;
        Guard guard{calls};
        std::vector<int> result;
        for (int index = 0; index < 3; ++index) {
            auto next = co_await F.pure(initial + index);
            if (next > 0) { result.push_back(next); }
        }
        co_return result;
    }, F, calls, 2);
    static_assert(std::same_as<decltype(program), Effect<Eff, Error, std::vector<int>>>);
    constexpr bool lazy = std::same_as<Effect<Eff, Error, int>, IO<Error, int>>;
    check(calls->started == (lazy ? 0 : 1), "do follows the effect family's evaluation strategy");
    check(evaluate(program) == std::vector<int>({2, 3, 4}), "await infers values and permits ordinary loops and branches");
    check(calls->started == 1 && calls->released == 1, "successful do destroys its frame locals");
    check(calls->pure == 4 && calls->binds == 3, "await and co_return use the supplied monad dictionary");
    auto copy = program;
    check(evaluate(copy) == std::vector<int>({2, 3, 4}), "copied do descriptions preserve their input snapshots");
    check(calls->started == (lazy ? 2 : 1) && calls->released == calls->started,
          "each IO run has fresh locals; expected is already evaluated");

    auto failed = F.do_([](auto F, auto calls) -> Do<Eff, Error, int> {
        Guard guard{calls};
        (void)co_await F.pure(1);
        try {
            (void)co_await F.fail(Error::missing);
        } catch (...) {
            ++calls->after;
        }
        ++calls->after;
        co_return 0;
    }, F, calls);
    const auto before_failure = calls->released;
    check(evaluate(failed).error() == Error::missing && calls->after == 0,
          "typed failure short-circuits without throwing through coroutine catch handlers");
    check(calls->released == before_failure + (lazy ? 1 : 0), "typed failure destroys the suspended frame");
    auto recovered = F.catch_all(failed, [F](Error) { return F.pure(42); });
    check(evaluate(recovered) == 42, "ordinary error recovery composes with do");

    auto bottom = F.do_([](auto F) -> Do<Eff, Error, Never> {
        co_return co_await F.fail(Error::missing);
    }, F);
    check(evaluate(bottom).error() == Error::missing, "failure-only do preserves Never");
    auto unit = F.do_([]() -> Do<Eff, Never, Unit> { co_return Unit{}; });
    check(evaluate(unit).has_value(), "Unit and no-await do blocks are supported");
    using Task = Do<Eff, Error, int>;
    static_assert(CanAwait<Task, Effect<Eff, Never, int>>);
    static_assert(CanAwait<Task, Effect<Eff, Error, Never>>);
    static_assert(!CanAwait<Task, Effect<Eff, OtherError, int>>);
    static_assert(!CanAwait<Do<Eff, Never, int>, Effect<Eff, Error, int>>);
    static_assert(!CanAwait<Task, std::suspend_always>);
    static_assert(!CanAwait<Task, std::expected<void, Error>>);
    auto infallible = []() -> Do<Eff, Never, int> { co_return 42; };
    static_assert(CanDo<decltype(F), decltype(infallible)>);
    static_assert(!CanDo<Bio<Monad2<Eff>>, decltype(infallible)>);
    static_assert(!CanDo<decltype(F), decltype([] { return 42; })>);
    auto duplicate = scope.template provide<Do2>(Do2<Eff>{}).template provide<Do2>(Do2<Eff>{});
    static_assert(!CanDo<decltype(bio(duplicate)), decltype(infallible)>);
    auto direct = Bio{Do2<Eff>{}}.do_(infallible);
    check(evaluate(direct) == 42, "explicit Do2 dictionaries also support the accessor");
    auto local_driver = scope.template provide<Do2>(CountingDo<Eff>{calls});
    auto selected = bio(local_driver).do_(infallible);
    check(evaluate(selected) == 42 && calls->drivers == 1, "Do2 itself supports scoped overrides without slicing");
}

// Behavioral-Active / Blackbox-Group; origin: Specified.
int main() {
    using namespace mini;
    contract<IO>();
    contract<std::expected>();
    contract<Result>();
    const auto F = bio(implicit_scope<IO>());
    static_assert(!CanAwait<Do<IO, Error, int>, Result<Error, int>>);
    static_assert(!CanAwait<Do<Result, Error, int>, IO<Error, int>>);
    auto other_family = []() -> Do<std::expected, Never, int> { co_return 1; };
    static_assert(!CanDo<decltype(F), decltype(other_family)>);
    auto wrong_monad = bio(implicit_scope<IO>().provide<Monad2>(Monad2<Result>{}));
    auto io_body = []() -> Do<IO, Never, int> { co_return 1; };
    static_assert(!CanDo<decltype(wrong_monad), decltype(io_body)>);
    auto wrong_driver = bio(implicit_scope<IO>().provide<Do2>(Do2<Result>{}));
    static_assert(!CanDo<decltype(wrong_driver), decltype(io_body)>);

    auto calls = std::make_shared<Calls>();
    for (bool during_await : {false, true}) {
        auto defective = F.do_([](auto F, auto calls, bool during_await) -> Do<IO, Error, int> {
            Guard guard{calls};
            if (during_await) {
                (void)co_await F.sync([]() -> int { throw std::runtime_error("await defect"); });
            }
            throw std::runtime_error("body defect");
            co_return 0;
        }, F, calls, during_await);
        auto protected_program = defective.ensuring([calls]() noexcept { ++calls->after; });
        check(std::holds_alternative<Defect>(protected_program.run_exit()), "body and awaited exceptions remain IO defects");
        check(calls->released == calls->after, "frame cleanup precedes enclosing finalizers on defects");
    }
    const auto eager = bio(implicit_scope<std::expected>());
    bool threw = false;
    try {
        (void)eager.do_([]() -> Do<std::expected, Error, int> {
            throw std::runtime_error("expected defect");
            co_return 0;
        });
    } catch (const std::runtime_error&) { threw = true; }
    check(threw, "expected do propagates exceptions instead of inventing typed errors");

    std::weak_ptr<Calls> observed;
    std::optional<IO<Never, int>> escaped;
    {
        auto state = std::make_shared<Calls>();
        observed = state;
        const auto local = bio(implicit_scope<IO>().provide<Monad2>(CountingMonad<IO>{state}));
        escaped = local.do_([state, local]() -> Do<IO, Never, int> {
            Guard guard{state};
            auto value = co_await local.pure(41);
            co_return value + 1;
        });
    }
    check(!observed.expired(), "IO retains the coroutine factory and its scoped dictionary");
    check(escaped->unsafe_run() == 42 && escaped->unsafe_run() == 42 && observed.lock()->released == 2,
          "capturing coroutine factories survive scope destruction and repeated execution");
    escaped.reset();
    check(observed.expired(), "destroying the description releases factories and dictionaries without cycles");
    std::cout << "Coroutine do checks passed\n";
}
