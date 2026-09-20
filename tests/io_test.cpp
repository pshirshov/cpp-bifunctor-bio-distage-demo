#include <mini/bio.hpp>
#include "test_support.hpp"

#include <iostream>
#include <string>

using namespace mini;

struct EffectFinalizer {
    IO<int, int> operator()() const noexcept { return IO<int, int>::pure(1); }
};

template<class F>
concept AcceptedFinalizer = requires(F finalize) { IO<int, int>::pure(1).ensuring(finalize); };

static_assert(!AcceptedFinalizer<EffectFinalizer>, "an effect-valued finalizer must not be silently discarded");

struct ThrowingCopy {
    explicit ThrowingCopy(std::shared_ptr<bool> enabled) : enabled(std::move(enabled)) {}
    ThrowingCopy(ThrowingCopy&&) noexcept = default;
    ThrowingCopy& operator=(ThrowingCopy&&) noexcept = default;
    ThrowingCopy(const ThrowingCopy& other) : enabled(other.enabled) {
        if (*enabled) {
            throw std::runtime_error("copy failed");
        }
    }
    std::shared_ptr<bool> enabled;
};

// Behavioral-Active / Blackbox-Atomic; origin: Specified.
int main() {
    auto consumed = IO<std::string, int>::pure(7).flat_map([](int&& value) {
        return IO<std::string, std::string>::pure(std::to_string(value));
    });
    check(consumed.unsafe_run() == "7", "continuations receive owned values");

    int constructed = 0;
    auto suspended = IO<int, int>::suspend([&constructed] {
        ++constructed;
        return IO<int, int>::pure(5);
    });
    check(constructed == 0, "suspend delays program construction");
    check(suspended.unsafe_run() == 5 && suspended.unsafe_run() == 5 && constructed == 2,
          "suspend constructs a fresh program on every run");

    auto throwing_map = IO<int, int>::pure(1).map([](int) -> int { throw std::runtime_error("mapping"); });
    check(std::holds_alternative<Defect>(throwing_map.run_exit()), "map exceptions are defects");
    auto throwing_bind = IO<int, int>::pure(1).flat_map([](int) -> IO<int, int> {
        throw std::runtime_error("binding");
    });
    check(std::holds_alternative<Defect>(throwing_bind.run_exit()), "bind construction exceptions are defects");
    auto throwing_recovery = IO<int, int>::fail(1).catch_all([](int) -> IO<int, int> {
        throw std::runtime_error("recovery");
    });
    check(std::holds_alternative<Defect>(throwing_recovery.run_exit()), "recovery exceptions are defects");

    auto attempted = Sync2<IO>::attempt([]() -> int { throw std::runtime_error("boundary"); });
    check(std::holds_alternative<Failure<std::exception_ptr>>(attempted.run_exit()),
          "attempt explicitly imports thrown exceptions into the error channel");
    auto total = Sync2<IO>::sync<int>([] { return 42; });
    check(total.unsafe_run() == 42, "sync suspends synchronous work");

    std::vector<int> releases;
    releases.reserve(16);
    auto final = throwing_map.ensuring([&releases]() noexcept { releases.push_back(1); })
        .ensuring([&releases]() noexcept { releases.push_back(2); });
    check(std::holds_alternative<Defect>(final.run_exit()), "finalization preserves defects");
    check(releases == std::vector<int>({1, 2}), "nested finalizers run inside-out");

    int acquisitions = 0;
    int closed = 0;
    auto acquire = IO<std::string, int>::sync([&acquisitions] { return ++acquisitions; });
    const auto close = [&closed](const int&) noexcept { ++closed; };
    auto resourceful = Bracket2<IO>::bracket(acquire,
        [](int resource) { return IO<std::string, int>::pure(resource); }, close);
    check(acquisitions == 0 && closed == 0, "bracket construction is lazy");
    check(resourceful.unsafe_run() == 1 && resourceful.unsafe_run() == 2 && closed == 2,
          "bracket acquires and releases once per execution");
    auto failed_use = Bracket2<IO>::bracket(acquire,
        [](int) { return IO<std::string, int>::fail("use failed"); }, close);
    check(failed_use.unsafe_run() == Result<std::string, int>(std::unexpected("use failed")) && closed == 3,
          "bracket releases on declared failure");
    auto defective_use = Bracket2<IO>::bracket(acquire,
        [](int) -> IO<std::string, int> { throw std::runtime_error("use construction"); }, close);
    check(std::holds_alternative<Defect>(defective_use.run_exit()) && closed == 4,
          "bracket releases when use construction throws");
    auto failed_acquire = Bracket2<IO>::bracket(IO<std::string, int>::fail("acquire failed"),
        [](int value) { return IO<std::string, int>::pure(value); }, close);
    check(!failed_acquire.unsafe_run() && closed == 4, "failed acquisition does not register release");

    auto persistent = IO<int, int>::pure(10);
    auto left = persistent.map([](int value) { return value + 1; });
    auto right = persistent.map([](int value) { return value + 2; });
    check(persistent.unsafe_run() == 10 && left.unsafe_run() == 11 && right.unsafe_run() == 12,
          "branching preserves immutable descriptions");

    auto copy_failure = std::make_shared<bool>(false);
    auto value = IO<int, ThrowingCopy>::pure(ThrowingCopy(copy_failure));
    *copy_failure = true;
    check(std::holds_alternative<Defect>(value.run_exit()), "result materialization exceptions are defects");

    int copy_resource_releases = 0;
    auto copying_resource = IO<int, ThrowingCopy>::sync([copy_failure] { return ThrowingCopy(copy_failure); });
    auto protected_use = Bracket2<IO>::bracket(copying_resource,
        [](ThrowingCopy) { return IO<int, int>::pure(1); },
        [&copy_resource_releases](const ThrowingCopy&) noexcept { ++copy_resource_releases; });
    check(std::holds_alternative<Defect>(protected_use.run_exit()) && copy_resource_releases == 1,
          "release remains registered when copying the resource for use throws");
    std::cout << "IO runtime checks passed\n";
}
