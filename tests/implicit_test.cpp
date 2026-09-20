#include <mini/implicit.hpp>
#include "test_support.hpp"

#include <iostream>
#include <optional>
#include <string>

namespace sample {

struct Calls { int pure = 0; int fail = 0; int bimap = 0; int recover = 0; };

template<template<class, class> class Eff>
struct CountingError : mini::Error2<Eff> {
    explicit CountingError(std::shared_ptr<Calls> state) : state(std::move(state)) {}
    std::shared_ptr<Calls> state;

    template<class E, class A>
    auto pure(A value) const {
        ++state->pure;
        return mini::Monad2<Eff>::template pure<E>(std::move(value));
    }

    template<class E, class A>
    auto fail(E error) const {
        ++state->fail;
        return mini::Error2<Eff>::template fail<E, A>(std::move(error));
    }

    template<class Source, class OnError, class OnValue>
    auto bimap(Source source, OnError on_error, OnValue on_value) const {
        ++state->bimap;
        return mini::Bifunctor2<Eff>::bimap(std::move(source), std::move(on_error), std::move(on_value));
    }

    template<class Source, class Recover>
    auto catch_all(Source source, Recover recover) const {
        ++state->recover;
        return mini::Error2<Eff>::catch_all(std::move(source), std::move(recover));
    }
};

template<class T> struct Show {};
struct IntShow { std::string prefix; std::string show(int value) const { return prefix + std::to_string(value); } };

template<class Element>
struct VectorShow {
    Element element;
    template<class T>
    std::string show(const std::vector<T>& values) const {
        std::string result = "[";
        for (const auto& value : values) {
            if (result.size() > 1) { result += ","; }
            result += element.show(value);
        }
        return result + "]";
    }
};

template<class Scope>
auto instance(mini::Request<Show<int>>, const Scope&) { return IntShow{""}; }

template<class T, class Scope>
    requires requires(const Scope& scope) { scope.template summon<Show<T>>(); }
auto instance(mini::Request<Show<std::vector<T>>>, const Scope& scope) {
    using Element = decltype(scope.template summon<Show<T>>());
    return VectorShow<Element>{scope.template summon<Show<T>>()};
}

struct Missing {};

}

template<class F>
concept CanSync = requires(F f) { f.sync([] { return 42; }); };

template<class F>
concept CanPure = requires(F f) { f.pure(42); };

template<class A, class E>
auto evaluate(std::expected<A, E> value) { return value; }

template<class E, class A>
auto evaluate(const mini::IO<E, A>& value) { return value.unsafe_run(); }

template<template<class, class> class Eff>
void resolution() {
    using namespace mini;
    using namespace sample;
    auto canonical = implicit_scope<Eff>();
    static_assert(decltype(canonical)::template can_summon<Monad2>());
    static_assert(decltype(canonical)::template can_summon<Error2>());
    static_assert(!decltype(canonical)::template can_summon<Missing>());
    auto stronger_calls = std::make_shared<Calls>();
    auto stronger = canonical.template provide<Error2>(CountingError<Eff>{stronger_calls});
    static_assert(std::same_as<decltype(stronger.template summon<Monad2>()), CountingError<Eff>>);
    auto F = bio(stronger);
    auto source = F.pure(42);
    static_assert(std::same_as<decltype(source), Effect<Eff, Never, int>>);
    check(stronger_calls->pure == 1, "a unique stronger provider retains its concrete type");
    auto exact_calls = std::make_shared<Calls>();
    auto exact = stronger.template provide<Monad2>(CountingError<Eff>{exact_calls});
    auto chosen = bio(exact).pure(7);
    check(exact_calls->pure == 1 && stronger_calls->pure == 1, "an exact registration overrides a stronger provider");
    (void)chosen;
    auto separate_calls = std::make_shared<Calls>();
    auto combined = exact.template provide<Bifunctor2>(CountingError<Eff>{separate_calls});
    auto all = bio(combined);
    auto failure = all.fail(std::string("error"));
    auto mapped = all.map_error(failure, [](const std::string& error) { return error.size(); });
    check(evaluate(mapped).error() == 5 && separate_calls->bimap == 1 && stronger_calls->bimap == 0,
          "independent capability overrides route through the same accessor");
    auto recovered = all.catch_all(failure, [all](const std::string&) { return all.pure(42); });
    check(evaluate(recovered) == 42 && stronger_calls->fail == 1 && stronger_calls->recover == 1,
          "error operations use the error provider, not the monad override");
    const auto exact_before = exact_calls->pure;
    bio(canonical).pure(1);
    check(exact_calls->pure == exact_before && stronger_calls->pure == 1, "extending a scope does not mutate its parent");
    auto duplicate = exact.template provide<Monad2>(CountingError<Eff>{exact_calls});
    static_assert(!decltype(duplicate)::template can_summon<Monad2>());
    static_assert(!CanPure<decltype(bio(duplicate))>);

    auto shown = canonical.template summon<Show<std::vector<std::vector<int>>>>();
    static_assert(std::same_as<decltype(shown), VectorShow<VectorShow<IntShow>>>);
    check(shown.show(std::vector<std::vector<int>>{{1, 2}, {3}}) == "[[1,2],[3]]", "ADL derives nested instances recursively");
    auto formatted = stronger.template provide<Show<int>>(IntShow{"#"});
    auto local = formatted.template summon<Show<std::vector<int>>>();
    check(local.show(std::vector<int>{1, 2}) == "[#1,#2]", "derivation uses the same local context");
    static_assert(!decltype(formatted)::template can_summon<Show<std::vector<Missing>>>());
    static_assert(std::same_as<decltype(formatted.template summon<Monad2>()), CountingError<Eff>>);
    auto conflicting = formatted.template provide<Show<int>>(IntShow{"!"});
    static_assert(!decltype(conflicting)::template can_summon<Show<std::vector<int>>>());
    auto direct = conflicting.template provide<Show<std::vector<int>>>(VectorShow<IntShow>{IntShow{"direct:"}});
    check(direct.template summon<Show<std::vector<int>>>().show(std::vector<int>{1}) == "[direct:1]",
          "an exact registration avoids unnecessary recursive resolution");

    auto collection = with_bio(stronger, [](auto F) {
        return F.traverse(std::vector<int>{1, 2}, [F](int value) { return F.pure(value); });
    });
    const auto before_execution = stronger_calls->pure;
    check(evaluate(collection) == std::vector<int>({1, 2}), "traversal routes through the context");
    constexpr bool lazy = std::same_as<Effect<Eff, Never, int>, IO<Never, int>>;
    check(stronger_calls->pure == before_execution + (lazy ? 4 : 0), "derived combinators preserve the monad override");
}

// Behavioral-Active / Blackbox-Group; origin: Specified.
int main() {
    using namespace mini;
    using namespace sample;
    resolution<IO>();
    resolution<std::expected>();
    auto canonical = implicit_scope<IO>();
    auto wrong_family = canonical.provide<Monad2>(Monad2<std::expected>{});
    static_assert(!CanPure<decltype(bio(wrong_family))>);
    static_assert(CanSync<decltype(bio(canonical))>);
    static_assert(!CanSync<decltype(bio(implicit_scope<std::expected>()))>);
    auto ambiguous = canonical.provide<Error2>(Error2<IO>{}).provide<Sync2>(Sync2<IO>{});
    static_assert(!decltype(ambiguous)::can_summon<Monad2>());
    static_assert(decltype(ambiguous)::can_summon<Error2>());
    static_assert(!CanPure<decltype(bio(ambiguous))>);
    auto unambiguous = ambiguous.provide<Monad2>(Monad2<IO>{});
    static_assert(decltype(unambiguous)::can_summon<Monad2>());

    auto calls = std::make_shared<Calls>();
    std::weak_ptr<Calls> observed = calls;
    std::optional<IO<Never, int>> program;
    {
        auto scope = canonical.provide<Monad2>(CountingError<IO>{calls});
        program = with_bio(scope, [](auto F) {
            return F.flat_map(F.pure(1), [F](int value) { return F.pure(value + 1); });
        });
    }
    check(calls->pure == 1, "IO continuations remain lazy through context resolution");
    calls.reset();
    check(!observed.expired(), "an escaped IO owns its captured implicit context");
    check(program->unsafe_run() == 2 && program->unsafe_run() == 2 && observed.lock()->pure == 3,
          "local overrides survive scope destruction and repeated execution");
    program.reset();
    check(observed.expired(), "destroying the IO releases its context");
    auto eager = with_bio(implicit_scope<std::expected>(), [](auto F) {
        return F.flat_map(F.pure(41), [F](int value) { return F.pure(value + 1); });
    });
    check(eager == 42, "with_bio supports the eager interpreter");
    auto F = bio(canonical);
    int executions = 0;
    auto suspended = F.suspend([F, &executions] { return F.sync([&executions] { return ++executions; }); });
    check(executions == 0 && suspended.unsafe_run() == 1 && suspended.unsafe_run() == 2,
          "canonical sync and suspend remain lazy and repeatable");
    auto attempted = F.attempt([]() -> int { throw std::runtime_error("declared"); });
    check(!attempted.unsafe_run(), "canonical attempt resolves the sync capability");
    int released = 0;
    auto scoped = F.bracket(F.pure(42), [F](int value) { return F.pure(value); },
                           [&released](const int&) noexcept { ++released; });
    check(scoped.unsafe_run() == 42 && released == 1, "canonical bracket resolves resource operations");
    std::cout << "Implicit resolution checks passed\n";
}
