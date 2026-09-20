#include <mini/bio.hpp>
#include "test_support.hpp"

#include <iostream>
#include <string>

enum class Error { missing };
enum class OtherError { unavailable };

template<class A, class E>
std::expected<A, E> evaluate(std::expected<A, E> value) { return value; }

template<class E, class A>
std::expected<A, E> evaluate(const mini::IO<E, A>& effect) { return effect.unsafe_run(); }

template<template<class, class> class Eff>
mini::Effect<Eff, Error, int> lookup(bool found) {
    const mini::Bio F{mini::Error2<Eff>{}};
    if (!found) { return F.fail(Error::missing); }
    return F.pure(42);
}

template<class F>
concept CanFail = requires(F f) { f.fail(Error::missing); };

template<class F>
concept CanSync = requires(F f) { f.sync([] { return 42; }); };

template<class F, class Source, class Next>
concept CanBind = requires(F f, Source source, Next next) { f.flat_map(source, next); };

template<class F, class Source, class Recover>
concept CanRecover = requires(F f, Source source, Recover recover) { f.catch_all(source, recover); };

template<template<class, class> class Eff>
void channels() {
    using namespace mini;
    const Bio F{Error2<Eff>{}};
    auto pure = F.pure(42);
    auto failure = F.fail(Error::missing);
    static_assert(std::same_as<decltype(pure), Effect<Eff, Never, int>>);
    static_assert(std::same_as<decltype(failure), Effect<Eff, Error, Never>>);
    check(evaluate(pure) == evaluate(F.pure(42)), "infallible expected results remain comparable");
    check(evaluate(failure) == evaluate(F.fail(Error::missing)), "failure-only expected results remain comparable");
    check(evaluate(pure) == std::expected<int, std::string>(42), "bottom errors support heterogeneous result equality");
    check(evaluate(lookup<Eff>(true)) == 42, "pure widens at a declared return boundary");
    check(evaluate(lookup<Eff>(false)).error() == Error::missing, "fail widens at a declared return boundary");

    auto failed = F.flat_map(pure, [F](int) { return F.fail(Error::missing); });
    static_assert(std::same_as<decltype(failed), Effect<Eff, Error, Never>>);
    check(evaluate(failed).error() == Error::missing, "bind joins Never with a declared error");
    auto recovered = F.catch_all(failure, [F](Error) { return F.pure(42); });
    static_assert(std::same_as<decltype(recovered), Effect<Eff, Never, int>>);
    check(evaluate(recovered) == 42, "recovery joins the success channels and removes the error");
    auto mapped = F.map(pure, [](int value) { return std::to_string(value); });
    static_assert(std::same_as<decltype(mapped), Effect<Eff, Never, std::string>>);
    check(evaluate(mapped) == "42", "map keeps an infallible result concrete");
    auto mapped_error = F.map_error(failure, [](Error) { return OtherError::unavailable; });
    static_assert(std::same_as<decltype(mapped_error), Effect<Eff, OtherError, Never>>);
    check(evaluate(mapped_error).error() == OtherError::unavailable, "map_error preserves an impossible success");
    auto both = F.bimap(lookup<Eff>(true), [](Error) { return OtherError::unavailable; },
                        [](int value) { return std::to_string(value); });
    check(evaluate(both) == "42", "bimap forwards the supplied error dictionary");

    int calls = 0;
    auto skipped = F.flat_map(lookup<Eff>(false), [F, &calls](int value) {
        ++calls;
        return F.pure(value);
    });
    static_assert(std::same_as<decltype(skipped), Effect<Eff, Error, int>>);
    check(evaluate(skipped).error() == Error::missing && calls == 0, "bind joins a declared error with Never");
    auto unchanged = F.catch_all(lookup<Eff>(true), [F, &calls](Error) {
        ++calls;
        return F.fail(OtherError::unavailable);
    });
    static_assert(std::same_as<decltype(unchanged), Effect<Eff, OtherError, int>>);
    check(evaluate(unchanged) == 42 && calls == 0, "an impossible recovery success widens without losing source success");
    auto failed_again = F.catch_all(lookup<Eff>(false), [F](Error) { return F.fail(OtherError::unavailable); });
    check(evaluate(failed_again).error() == OtherError::unavailable, "failure-only recovery preserves its replacement error");

    const auto other_error = [F](int) { return F.fail(OtherError::unavailable); };
    static_assert(!CanBind<decltype(F), Effect<Eff, Error, int>, decltype(other_error)>);
    const auto other_value = [F](Error) { return F.pure(std::string("other")); };
    static_assert(!CanRecover<decltype(F), Effect<Eff, Error, int>, decltype(other_value)>);
    static_assert(!std::convertible_to<Effect<Eff, Error, int>, Effect<Eff, OtherError, int>>);
    static_assert(!std::convertible_to<Effect<Eff, Error, int>, Effect<Eff, Never, int>>);
    static_assert(!std::convertible_to<Effect<Eff, Error, int>, Effect<Eff, Error, Never>>);
    static_assert(!CanFail<Bio<Monad2<Eff>>>);
    static_assert(!CanSync<Bio<Error2<Eff>>>);

    auto traversed = F.traverse(std::vector<int>{1, 2, 3}, [F](int value) { return F.pure(value * 2); });
    static_assert(std::same_as<decltype(traversed), Effect<Eff, Never, std::vector<int>>>);
    check(evaluate(traversed) == std::vector<int>({2, 4, 6}), "traverse accepts bottom-error callbacks");
    auto empty = F.traverse(std::vector<int>{}, [F](int) { return F.fail(Error::missing); });
    check(evaluate(empty)->empty(), "empty traversal of a failure-only callback still succeeds");
    auto failed_traversal = F.traverse(std::vector<int>{1, 2}, [F](int) { return F.fail(Error::missing); });
    check(evaluate(failed_traversal).error() == Error::missing, "traverse accepts bottom-success callbacks");
}

struct CountingMonad : mini::Monad2<mini::IO> {
    explicit CountingMonad(std::shared_ptr<int> count) : calls(std::move(count)) {}
    std::shared_ptr<int> calls;

    template<class E, class A>
    auto pure(A value) const {
        ++*calls;
        return mini::IO<E, A>::pure(std::move(value));
    }
};

// Behavioral-Active / Blackbox-Group; origin: Specified.
int main() {
    using namespace mini;
    static_assert(!std::default_initializable<Never> && std::copy_constructible<Never>);
    static_assert(!std::is_trivially_copyable_v<Never>);
    static_assert(std::convertible_to<Never, int> && std::convertible_to<Never, Error>);
    static_assert(std::same_as<JoinChannel<Never, Error>, Error>);
    static_assert(std::same_as<JoinChannel<Error, Never>, Error>);
    static_assert(std::same_as<JoinChannel<Never, Never>, Never>);
    static_assert(!CompatibleChannels<Error, OtherError>);
    channels<IO>();
    channels<std::expected>();
    const Bio F{Bracket2<IO>{}};
    static_assert(CanSync<decltype(F)>);
    int executions = 0;
    auto pure = F.sync([&executions] { ++executions; return 42; });
    static_assert(std::same_as<decltype(pure), IO<Never, int>>);
    IO<Error, int> widened = pure;
    check(executions == 0, "bottom widening is lazy");
    check(widened.unsafe_run() == 42 && widened.unsafe_run() == 42 && executions == 2,
          "bottom widening preserves repeatable execution");
    auto defect = F.sync([]() -> int { throw std::runtime_error("defect"); });
    IO<Error, int> widened_defect = defect;
    check(std::holds_alternative<Defect>(widened_defect.run_exit()), "Never does not hide defects");
    auto no_channels = F.suspend([]() -> IO<Never, Never> { throw std::runtime_error("defect"); });
    IO<Error, int> both_widened = no_channels;
    check(std::holds_alternative<Defect>(both_widened.run_exit()), "both bottom channels can widen without evaluation");
    auto attempted = F.attempt([]() -> int { throw std::runtime_error("declared"); });
    static_assert(std::same_as<decltype(attempted), IO<std::exception_ptr, int>>);
    check(!attempted.unsafe_run(), "attempt explicitly imports exceptions into the error channel");
    int releases = 0;
    auto scoped = F.bracket(F.pure(42), [F](int) { return F.fail(Error::missing); },
                           [&releases](const int&) noexcept { ++releases; });
    static_assert(std::same_as<decltype(scoped), IO<Error, Never>>);
    check(scoped.unsafe_run().error() == Error::missing && releases == 1,
          "bracket joins errors without losing resource release");
    auto successful_scope = F.bracket(lookup<IO>(true), [F](int value) { return F.pure(value); },
                                     [&releases](const int&) noexcept { ++releases; });
    static_assert(std::same_as<decltype(successful_scope), IO<Error, int>>);
    check(successful_scope.unsafe_run() == 42 && releases == 2, "bracket joins errorful acquisition with infallible use");
    const auto incompatible = [](int) { return std::expected<int, Error>(42); };
    static_assert(!CanBind<decltype(F), IO<Error, int>, decltype(incompatible)>);

    auto calls = std::make_shared<int>(0);
    const Bio counted{CountingMonad{calls}};
    auto program = counted.map(counted.pure(1), [](int value) { return value + 1; });
    check(*calls == 1, "the accessor uses the explicitly supplied dictionary");
    check(program.unsafe_run() == 2 && *calls == 2, "derived operations preserve that dictionary");
    const auto before = *calls;
    auto collection = counted.traverse(std::vector<int>{1, 2}, [counted](int value) { return counted.pure(value); });
    check(*calls == before + 1, "traverse builds its accumulator with the supplied dictionary");
    check(collection.unsafe_run() == std::vector<int>({1, 2}) && *calls == before + 5,
          "traverse preserves the supplied dictionary in delayed append operations");
    std::cout << "Scoped accessor and bottom-channel checks passed\n";
}
