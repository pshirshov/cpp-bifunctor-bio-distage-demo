#include "application.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    using namespace example;
    using App = Application<IO>;
    using di::key;
    if (argc != 2 || (std::string_view(argv[1]) != "live" && std::string_view(argv[1]) != "test")) {
        std::cerr << "usage: example live|test\n";
        return 2;
    }
    const auto mode = std::string_view(argv[1]) == "live" ? Mode::live : Mode::test;
    auto plan = di::Planner{}.plan(module<IO>(), di::roots(key<App>()), di::activate(mode));
    if (!plan) {
        std::cerr << plan.error().describe() << '\n';
        return 1;
    }
    using AppError = std::variant<di::ProvisionError, LookupError>;
    auto program = plan->produce().map_error([](auto error) { return AppError(std::move(error)); })
        .flat_map([](auto graph) {
            return graph->get(key<App>())->run(UserId{1})
                .map_error([](auto error) { return AppError(error); });
        });
    auto result = program.unsafe_run();
    if (!result) {
        std::cerr << "application failed\n";
        return 1;
    }
    for (const auto& line : *result) {
        std::cout << line << '\n';
    }
}
