#include <mini/di.hpp>
#include "test_support.hpp"

#include <iostream>

namespace di = mini::di;
using mini::IO;
using mini::Unit;
using di::activate;
using di::deps;
using di::key;
using di::roots;
using di::when;

struct Config { int value; };
struct Service {
    explicit Service(std::shared_ptr<Config> config) : config(std::move(config)) {}
    std::shared_ptr<Config> config;
};
struct Unused {};
struct A {};
struct B {};
enum class Repo { live, test };
enum class Region { eu, us };

di::Plan planned(const di::Module& module, std::vector<di::Key> roots, di::Activation activation) {
    auto result = di::Planner{}.plan(module, roots, activation);
    if (!result) {
        throw std::runtime_error(result.error().describe());
    }
    return std::move(*result);
}

void expect_error(const di::Module& module, std::vector<di::Key> roots,
                  di::Activation activation, di::PlanErrorCode code) {
    auto result = di::Planner{}.plan(module, roots, activation);
    check(!result && result.error().code == code, "planner reports the expected error category");
}

void bindings_and_planning() {
    int calls = 0;
    int unreachable = 0;
    di::Module module;
    module.provide(key<Config>(), deps(), [&calls] {
        ++calls;
        return std::make_shared<Config>(Config{42});
    }, when());
    module.bind<Service>(deps(key<Config>()), when());
    module.alias(key<Service>("alias"), key<Service>(), when());
    module.provide(key<Unused>(), deps(key<A>()), [&unreachable](std::shared_ptr<A>) {
        ++unreachable;
        return std::make_shared<Unused>();
    }, when());
    auto plan = planned(module, roots(key<Service>(), key<Service>("alias")), activate());
    check(calls == 0 && unreachable == 0, "planning does not invoke providers");
    check(plan.order() == std::vector<di::Key>({key<Config>().key, key<Service>().key, key<Service>("alias").key}),
          "plan is reachable and dependency ordered");
    check(!plan.render().empty(), "plan is inspectable");
    auto provisioning = plan.produce();
    check(calls == 0, "provisioning effect construction is lazy");
    auto first = provisioning.unsafe_run().value();
    check(calls == 1 && unreachable == 0, "singleton construction and unreachable pruning");
    check(first->get(key<Service>()) == first->get(key<Service>("alias")), "alias shares the original object");
    check(first->get(key<Service>())->config == first->get(key<Config>()), "dependencies share graph instances");
    auto second = provisioning.unsafe_run().value();
    check(calls == 2 && second->get(key<Service>()) != first->get(key<Service>()), "each execution creates a fresh graph");

    module.instance(key<Config>(), std::make_shared<Config>(Config{99}), when());
    check(plan.produce().unsafe_run().has_value(), "plan snapshots survive module mutation");
    expect_error(module, roots(key<Service>()), activate(), di::PlanErrorCode::ambiguous_binding);
    expect_error(module, roots(key<Unused>()), activate(), di::PlanErrorCode::missing_binding);
    check(calls == 3 && unreachable == 0, "invalid plans execute no providers");

    di::Module cycle;
    cycle.provide(key<A>(), deps(key<B>()), [](std::shared_ptr<B>) { return std::make_shared<A>(); }, when());
    cycle.provide(key<B>(), deps(key<A>()), [](std::shared_ptr<A>) { return std::make_shared<B>(); }, when());
    auto failure = di::Planner{}.plan(cycle, roots(key<A>()), activate());
    check(!failure && failure.error().code == di::PlanErrorCode::cycle &&
          failure.error().path == std::vector<di::Key>({key<A>().key, key<B>().key, key<A>().key}), "cycle diagnostic contains a closed path");
}

void axes_and_sets() {
    di::Module module;
    module.instance(key<Config>(), std::make_shared<Config>(Config{1}), when(Repo::live, Region::eu));
    module.instance(key<Config>(), std::make_shared<Config>(Config{2}), when(Repo::test));
    check(planned(module, roots(key<Config>()), activate(Repo::test)).produce().unsafe_run().value()->get(key<Config>())->value == 2,
          "axis selects one binding");
    check(planned(module, roots(key<Config>()), activate(Repo::live, Region::eu)).produce().unsafe_run().value()->get(key<Config>())->value == 1,
          "multiple axis conditions are conjunctive");
    expect_error(module, roots(key<Config>()), activate(Repo::live), di::PlanErrorCode::missing_binding);
    expect_error(module, roots(key<Config>()), activate(Repo::live, Repo::test), di::PlanErrorCode::conflicting_axis);
    module.instance(key<A>(), std::make_shared<A>(), when(Repo::live, Repo::test));
    expect_error(module, roots(key<A>()), activate(Repo::live), di::PlanErrorCode::conflicting_axis);

    di::Module set_module;
    auto one = std::make_shared<Config>(Config{1});
    set_module.instance(key<Config>("one"), one, when());
    set_module.alias(key<Config>("same"), key<Config>("one"), when());
    set_module.instance(key<Config>("two"), std::make_shared<Config>(Config{2}), when(Repo::test));
    set_module.set(key<di::Set<Config>>("empty"));
    set_module.add(key<di::Set<Config>>(), key<Config>("one"), when());
    set_module.add(key<di::Set<Config>>(), key<Config>("same"), when());
    set_module.add(key<di::Set<Config>>(), key<Config>("two"), when(Repo::test));
    set_module.add(key<di::Set<Config>>(), key<Config>("not-bound"), when(Repo::live));
    auto graph = planned(set_module, roots(key<di::Set<Config>>(), key<di::Set<Config>>("empty")), activate(Repo::test))
        .produce().unsafe_run().value();
    check(graph->get(key<di::Set<Config>>())->size() == 2, "sets deduplicate aliases by object identity");
    check(graph->get(key<di::Set<Config>>())->values().front() == one, "set traversal preserves contribution order");
    check(graph->get(key<di::Set<Config>>("empty"))->size() == 0, "explicit empty set is injectable");
    expect_error(set_module, roots(key<di::Set<Config>>()), activate(Repo::live), di::PlanErrorCode::missing_binding);
    di::Module included;
    included.include(set_module);
    included.add(key<di::Set<Config>>(), key<Config>("one"), when());
    check(planned(included, roots(key<di::Set<Config>>()), activate(Repo::test)).produce().unsafe_run().value()
        ->get(key<di::Set<Config>>())->size() == 2, "included modules merge set contributions");
}

struct Left { virtual ~Left() = default; int left = 1; };
struct Right { virtual ~Right() = default; int right = 2; };
struct Both : Left, Right {};

void pointer_adjustment() {
    di::Module module;
    module.bind<Both>(deps(), when());
    module.alias(key<Right>(), key<Both>(), when());
    module.add(key<di::Set<Right>>(), key<Both>(), when());
    auto graph = planned(module, roots(key<Right>(), key<di::Set<Right>>()), activate()).produce().unsafe_run().value();
    auto original = graph->get(key<Both>());
    check(graph->get(key<Right>()).get() == static_cast<Right*>(original.get()), "aliases adjust multiple-inheritance pointers");
    check(graph->get(key<di::Set<Right>>())->values().front() == graph->get(key<Right>()), "set elements use the same safe upcast");
}

struct Tracked {
    Tracked(int id, std::shared_ptr<std::vector<int>> log) : id(id), log(std::move(log)) {}
    ~Tracked() { log->push_back(id); }
    int id;
    std::shared_ptr<std::vector<int>> log;
};

void provisioning_and_lifetimes() {
    auto log = std::make_shared<std::vector<int>>();
    log->reserve(16);
    di::Module module;
    module.provide(key<Tracked>("first"), deps(), [log] { return std::make_shared<Tracked>(1, log); }, when());
    module.provide(key<Tracked>("second"), deps(key<Tracked>("first")), [log](std::shared_ptr<Tracked>) {
        return std::make_shared<Tracked>(2, log);
    }, when());
    auto plan = planned(module, roots(key<Tracked>("second")), activate());
    {
        auto graph = plan.produce().unsafe_run().value();
        check(log->empty(), "produced objects stay alive in graph");
    }
    check(*log == std::vector<int>({2, 1}), "graph ownership releases dependents before dependencies");
    log->clear();
    module.provide(key<A>(), deps(key<Tracked>("second")), [](std::shared_ptr<Tracked>) -> std::shared_ptr<A> {
        throw std::runtime_error("provider failure");
    }, when());
    auto failed = planned(module, roots(key<A>()), activate()).produce().unsafe_run();
    check(!failed && failed.error().key == key<A>().key && failed.error().cause, "provider exceptions become provisioning errors");
    check(*log == std::vector<int>({2, 1}), "failed provisioning rolls back previously acquired objects");

    di::Module nulls;
    nulls.instance(key<A>(), std::shared_ptr<A>{}, when());
    check(!planned(nulls, roots(key<A>()), activate()).produce().unsafe_run(), "null services fail provisioning");

    int evaluations = 0;
    di::Module effects;
    effects.provide_effect(key<A>(), deps(), [&evaluations] {
        return IO<di::ProvisionError, std::shared_ptr<A>>::sync([&evaluations] {
            ++evaluations;
            return std::make_shared<A>();
        });
    }, when());
    auto program = planned(effects, roots(key<A>()), activate()).produce();
    check(evaluations == 0, "effectful provider remains suspended");
    check(program.unsafe_run().has_value() && evaluations == 1, "effectful provider executes inside the IO runtime");
}

// Behavioral-Active / Blackbox-Group; origin: Specified.
int main() {
    bindings_and_planning();
    axes_and_sets();
    pointer_adjustment();
    provisioning_and_lifetimes();
    std::cout << "DI checks passed\n";
}
