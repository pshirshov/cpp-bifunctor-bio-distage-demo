#include <mini/bio.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mini::tests {

struct UserId { int value; };
struct User { std::string name; UserId manager; };
enum class LookupError { missing_user };

template<template<class, class> class F>
struct Users {
    virtual ~Users() = default;
    virtual F<LookupError, User> find(UserId id) const = 0;
};

template<template<class, class> class F>
class SampleUsers final : public Users<F> {
public:
    F<LookupError, User> find(UserId id) const override {
        using M = Error2<F>;
        switch (id.value) {
        case 1: return M::template pure<LookupError>(User{"Ada", UserId{2}});
        case 2: return M::template pure<LookupError>(User{"Grace", UserId{2}});
        default: return M::template fail<LookupError, User>(LookupError::missing_user);
        }
    }
};

template<template<class, class> class F>
F<LookupError, std::string> manager_name(std::shared_ptr<const Users<F>> users, UserId id) {
    return Monad2<F>::flat_map(users->find(id), [users](User user) {
        return map(users->find(user.manager), [](User manager) { return manager.name; });
    });
}

void check(bool condition, std::string_view contract) {
    if (!condition) {
        throw std::logic_error(std::string(contract));
    }
}

template<class E, class A>
Result<E, A> evaluate(Result<E, A> result) { return result; }

template<class E, class A>
Result<E, A> evaluate(const IO<E, A>& effect) { return effect.unsafe_run(); }

// Behavioral-Active, Blackbox-Atomic; origin: Specified.
template<template<class, class> class F>
void check_laws() {
    using M = Error2<F>;
    const auto pure = [](int value) { return M::template pure<std::string>(value); };
    const auto step = [pure](int value) {
        return value < 0 ? M::template fail<std::string, int>("negative") : pure(value + 1);
    };
    const auto twice = [pure](int value) { return pure(value * 2); };
    for (const int value : {-1, 0, 3}) {
        check(evaluate(M::flat_map(pure(value), step)) == evaluate(step(value)), "left identity");
    }
    for (const auto& effect : {pure(-1), pure(3), M::template fail<std::string, int>("failure")}) {
        check(evaluate(M::flat_map(effect, pure)) == evaluate(effect), "right identity");
        check(evaluate(M::flat_map(M::flat_map(effect, step), twice)) ==
              evaluate(M::flat_map(effect, [step, twice](int value) {
                  return M::flat_map(step(value), twice);
              })), "associativity");
        check(evaluate(M::bimap(effect, std::identity{}, std::identity{})) == evaluate(effect),
              "bifunctor identity");
        const auto append = [](std::string error) { return error + "!"; };
        const auto length = [](std::string error) { return error.size(); };
        const auto increment = [](int value) { return value + 1; };
        const auto scale = [](int value) { return value * 2; };
        check(evaluate(M::bimap(M::bimap(effect, append, increment), length, scale)) ==
              evaluate(M::bimap(effect,
                  [append, length](std::string error) { return length(append(error)); },
                  [increment, scale](int value) { return scale(increment(value)); })),
              "bifunctor composition");
    }
}

// Behavioral-Active, Blackbox-Group; origin: Specified.
template<template<class, class> class F>
void check_program() {
    std::shared_ptr<const Users<F>> users = std::make_shared<SampleUsers<F>>();
    auto success = manager_name<F>(users, UserId{1});
    auto missing = manager_name<F>(users, UserId{99});
    users.reset();
    check(evaluate(success) == Result<LookupError, std::string>("Grace"), "interpreter substitution");
    check(evaluate(missing) == Result<LookupError, std::string>(std::unexpected(LookupError::missing_user)),
          "service typed failure");
}

// Behavioral-Active, Blackbox-Atomic; origin: Specified.
void check_io() {
    using M = Error2<IO>;
    int executions = 0;
    int continuations = 0;
    auto source = IO<std::string, int>::defer([&executions] {
        ++executions;
        return Result<std::string, int>(7);
    });
    auto program = M::flat_map(source, [&continuations](int value) {
        ++continuations;
        return M::pure<std::string>(value + 1);
    });
    check(executions == 0 && continuations == 0, "composition is lazy");
    check(program.unsafe_run() == 8 && program.unsafe_run() == 8, "repeatable result");
    check(executions == 2 && continuations == 2, "effects rerun without memoization");

    auto failure = M::fail<std::string, int>("declared");
    auto skipped = M::flat_map(failure, [&continuations](int value) {
        ++continuations;
        return M::pure<std::string>(value);
    });
    check(skipped.unsafe_run() == Result<std::string, int>(std::unexpected("declared")),
          "flat_map preserves typed failure");
    check(continuations == 2, "failure short-circuits continuation");
    auto mapped = M::bimap(failure,
        [](std::string error) { return error.size(); }, [](int value) { return value * 2; });
    check(mapped.unsafe_run() == Result<std::size_t, int>(std::unexpected(std::size_t{8})),
          "bimap changes error type");

    auto recovered = M::catch_all(failure, [](std::string) { return M::pure<int>(42); });
    check(recovered.unsafe_run() == 42, "typed error recovery changes error channel");
    int recoveries = 0;
    auto success = M::catch_all(M::pure<std::string>(3), [&recoveries](std::string) {
        ++recoveries;
        return M::pure<int>(0);
    });
    check(success.unsafe_run() == 3 && recoveries == 0, "success bypasses recovery");

    auto defect = IO<std::string, int>::defer([]() -> Result<std::string, int> {
        throw std::runtime_error("defect");
    });
    bool propagated = false;
    try {
        M::catch_all(defect, [&recoveries](std::string) {
            ++recoveries;
            return M::pure<std::string>(0);
        }).unsafe_run();
    } catch (const std::runtime_error& error) {
        propagated = std::string_view(error.what()) == "defect";
    }
    check(propagated && recoveries == 0, "defects bypass typed error recovery");
}

struct Primary {};
struct Replica {};

void check_tags() {
    check(type_tag<int>() == type_tag<int>(), "stable in-process type equality");
    check(type_tag<int>() != type_tag<const int>(), "exact const qualification");
    check(type_tag<int>() != type_tag<int&>(), "exact reference qualification");
    check(type_tag<std::vector<int>>() != type_tag<std::vector<std::string>>(), "generic arguments");
    check(type_tag<Kind2Tag<IO>>() != type_tag<Kind2Tag<Result>>(), "effect family witnesses");
    check(type_tag<Users<IO>>() != type_tag<Users<Result>>(), "effect-specific service identity");
    check(type_tag<Key<Users<IO>, Primary>>() != type_tag<Key<Users<IO>, Replica>>(), "qualified keys");
}

}

int main() {
    mini::tests::check_laws<mini::Result>();
    mini::tests::check_laws<mini::IO>();
    mini::tests::check_program<mini::Result>();
    mini::tests::check_program<mini::IO>();
    mini::tests::check_io();
    mini::tests::check_tags();
    std::cout << "BIO checks passed\n";
}
