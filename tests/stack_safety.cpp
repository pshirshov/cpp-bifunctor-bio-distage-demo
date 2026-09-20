#include <mini/implicit.hpp>

#include <iostream>
#include <stdexcept>

mini::IO<int, int> recursive(int depth) {
    const auto F = mini::bio(mini::implicit_scope<mini::IO>());
    return F.suspend([F, depth]() -> mini::IO<int, int> {
        if (depth == 0) {
            return F.pure(0);
        }
        return F.map(recursive(depth - 1), [](int value) { return value + 1; });
    });
}

int main() {
    constexpr int depth = 100000;
    const auto F = mini::bio(mini::implicit_scope<mini::IO>());
    {
        mini::IO<int, int> program = F.pure(0);
        for (int index = 0; index < depth; ++index) {
            program = F.flat_map(std::move(program), [F](int value) {
                return F.pure(value + 1);
            });
        }
        std::cout << "Running " << depth << " binds\n" << std::flush;
        if (program.unsafe_run() != depth) {
            throw std::logic_error("deep bind returned the wrong result");
        }
        auto copy = program;
        if (copy.unsafe_run() != depth) {
            throw std::logic_error("copied program returned the wrong result");
        }
        std::cout << "Destroying both descriptions\n" << std::flush;
    }
    if (recursive(depth).unsafe_run() != depth) {
        throw std::logic_error("suspended non-tail recursion failed");
    }
    {
        mini::IO<int, int> recovery = F.fail(0);
        for (int index = 0; index < depth; ++index) {
            recovery = F.catch_all(recovery, [F](int error) { return F.fail(error + 1); });
        }
        if (recovery.unsafe_run() != mini::Result<int, int>(std::unexpected(depth))) {
            throw std::logic_error("deep error recovery failed");
        }
    }
    {
        int finalizers = 0;
        auto defective = mini::IO<int, int>::sync([]() -> int { throw std::runtime_error("defect"); });
        for (int index = 0; index < depth; ++index) {
            defective = defective.ensuring([&finalizers]() noexcept { ++finalizers; });
        }
        if (!std::holds_alternative<mini::Defect>(defective.run_exit()) || finalizers != depth) {
            throw std::logic_error("deep finalization failed");
        }
    }
    std::cout << "Stack safety checks passed\n";
}
