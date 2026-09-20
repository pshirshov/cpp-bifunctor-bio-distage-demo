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

auto increment_twice(auto scope, int initial) {
    return mini::with_bio(std::move(scope), [initial](auto F) {
        return F.flat_map(F.pure(initial), [F](auto value) {
            return F.map(F.pure(value + 1), [](auto next) { return next + 1; });
        });
    });
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
