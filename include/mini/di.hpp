#pragma once

#include "bio.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <tuple>

namespace mini::di {

struct Key {
    std::type_index type;
    std::string name;

    bool operator==(const Key&) const = default;
    bool operator<(const Key& other) const {
        return std::tie(type, name) < std::tie(other.type, other.name);
    }
    std::string describe() const {
        return std::string(type.name()) + (name.empty() ? "" : " named '" + name + "'");
    }
};

template<class T>
struct Ref { Key key; };

template<class T>
Ref<T> key(std::string name) { return {Key{type_tag<T>(), std::move(name)}}; }

template<class T>
Ref<T> key() { return key<T>(""); }

template<class... T>
auto deps(Ref<T>... references) { return std::tuple(std::move(references)...); }

template<class... T>
std::vector<Key> roots(Ref<T>... references) { return {std::move(references.key)...}; }

struct AxisPoint {
    std::type_index axis;
    std::uint64_t value;
};

template<class E> requires std::is_enum_v<E>
AxisPoint point(E value) { return {type_tag<E>(), static_cast<std::uint64_t>(value)}; }

struct Tags { std::vector<AxisPoint> points; };
struct Activation { std::vector<AxisPoint> points; };

template<class... E>
Tags when(E... values) { return {{point(values)...}}; }

template<class... E>
Activation activate(E... values) { return {{point(values)...}}; }

enum class PlanErrorCode { missing_binding, ambiguous_binding, cycle, conflicting_axis };

struct PlanError {
    PlanErrorCode code;
    std::string message;
    std::vector<Key> path;

    std::string describe() const {
        std::string result = message;
        for (const auto& item : path) {
            result += "\n  " + item.describe();
        }
        return result;
    }
};

struct ProvisionError {
    Key key;
    std::string message;
    std::exception_ptr cause;
};

template<class T>
class Set {
public:
    explicit Set(std::vector<std::shared_ptr<T>> values) {
        for (auto& value : values) {
            if (!value) {
                throw std::logic_error("null set element");
            }
            if (std::find(values_.begin(), values_.end(), value) == values_.end()) {
                values_.push_back(std::move(value));
            }
        }
    }
    const auto& values() const { return values_; }
    auto begin() const { return values_.begin(); }
    auto end() const { return values_.end(); }
    std::size_t size() const { return values_.size(); }

private:
    std::vector<std::shared_ptr<T>> values_;
};

namespace internal {

using Value = mini::detail::Value;
using Provider = std::function<IO<ProvisionError, Value>(const std::vector<Value>&)>;

struct Step {
    Key key;
    std::vector<Key> dependencies;
    Provider provider;
};

struct Binding { Step step; Tags tags; };

struct Contribution {
    Key dependency;
    Tags tags;
    std::function<Value(const Value&)> convert;
};

struct SetBinding {
    std::vector<Contribution> contributions;
    std::function<Value(const std::vector<Value>&)> assemble;
};

template<class... D, std::size_t... I>
auto arguments(const std::vector<Value>& values, std::index_sequence<I...>) {
    if (values.size() != sizeof...(D)) {
        throw std::logic_error("provider arity invariant violated");
    }
    return std::tuple(values[I].template get<std::shared_ptr<D>>()...);
}

}

class Planner;

class Module {
public:
    template<class T, class... D, class Factory>
    void provide_effect(Ref<T> target, std::tuple<Ref<D>...> dependencies, Factory factory, Tags tags) {
        using Effect = std::invoke_result_t<const Factory&, std::shared_ptr<D>...>;
        static_assert(std::same_as<typename Effect::error_type, ProvisionError>);
        static_assert(std::convertible_to<typename Effect::value_type, std::shared_ptr<T>>);
        auto keys = std::apply([](const auto&... item) { return std::vector<Key>{item.key...}; }, dependencies);
        internal::Provider provider = [factory = std::move(factory), output = target.key](const auto& values) {
            return IO<ProvisionError, internal::Value>::suspend([factory, output, values] {
                try {
                    auto effect = std::apply(factory, internal::arguments<D...>(values, std::index_sequence_for<D...>{}));
                    return effect.flat_map([output](typename Effect::value_type value) {
                        using Out = IO<ProvisionError, internal::Value>;
                        if (!value) {
                            return Out::fail({output, "provider returned a null service", nullptr});
                        }
                        return Out::pure(internal::Value::of(std::shared_ptr<T>(std::move(value))));
                    });
                } catch (...) {
                    return IO<ProvisionError, internal::Value>::fail({output, "provider construction threw", std::current_exception()});
                }
            });
        };
        bindings_.push_back({{std::move(target.key), std::move(keys), std::move(provider)}, std::move(tags)});
    }

    template<class T, class... D, class Factory>
    void provide(Ref<T> target, std::tuple<Ref<D>...> dependencies, Factory factory, Tags tags) {
        provide_effect(target, std::move(dependencies),
            [factory = std::move(factory), output = target.key](std::shared_ptr<D>... arguments) {
                return IO<ProvisionError, std::shared_ptr<T>>::defer([
                    factory, output, arguments = std::tuple(std::move(arguments)...)
                ]() -> Result<ProvisionError, std::shared_ptr<T>> {
                    try {
                        return std::shared_ptr<T>(std::apply(factory, arguments));
                    } catch (...) {
                        return std::unexpected(ProvisionError{output, "provider threw", std::current_exception()});
                    }
                });
            }, std::move(tags));
    }

    template<class Impl, class T, class... D>
    void bind(Ref<T> target, std::tuple<Ref<D>...> dependencies, Tags tags) {
        static_assert(std::constructible_from<Impl, std::shared_ptr<D>...>);
        provide(std::move(target), std::move(dependencies), [](std::shared_ptr<D>... arguments) {
            return std::make_shared<Impl>(std::move(arguments)...);
        }, std::move(tags));
    }

    template<class Impl, class... D>
    void bind(std::tuple<Ref<D>...> dependencies, Tags tags) {
        bind<Impl>(key<Impl>(), std::move(dependencies), std::move(tags));
    }

    template<class T>
    void instance(Ref<T> target, std::shared_ptr<T> value, Tags tags) {
        provide(std::move(target), deps(), [value = std::move(value)] { return value; }, std::move(tags));
    }

    template<class T, class U>
    void alias(Ref<T> target, Ref<U> source, Tags tags) {
        provide(std::move(target), deps(std::move(source)), [](std::shared_ptr<U> value) {
            return std::shared_ptr<T>(std::move(value));
        }, std::move(tags));
    }

    template<class T>
    void set(Ref<Set<T>> target) {
        sets_.try_emplace(std::move(target.key), internal::SetBinding{{}, [](const auto& values) {
            std::vector<std::shared_ptr<T>> elements;
            for (const auto& value : values) {
                elements.push_back(value.template get<std::shared_ptr<T>>());
            }
            return internal::Value::of(std::make_shared<Set<T>>(std::move(elements)));
        }});
    }

    template<class T, class U>
    void add(Ref<Set<T>> target, Ref<U> source, Tags tags) {
        set(target);
        sets_.at(target.key).contributions.push_back({std::move(source.key), std::move(tags), [](const auto& value) {
            return internal::Value::of(std::shared_ptr<T>(value.template get<std::shared_ptr<U>>()));
        }});
    }

    void include(const Module& other) {
        if (this == &other) {
            throw std::invalid_argument("a module cannot include itself");
        }
        bindings_.insert(bindings_.end(), other.bindings_.begin(), other.bindings_.end());
        for (const auto& [key, set] : other.sets_) {
            auto [entry, inserted] = sets_.try_emplace(key, set);
            if (!inserted) {
                auto& contributions = entry->second.contributions;
                contributions.insert(contributions.end(), set.contributions.begin(), set.contributions.end());
            }
        }
    }

private:
    friend class Planner;
    std::vector<internal::Binding> bindings_;
    std::map<Key, internal::SetBinding> sets_;
};

class Plan;

class ObjectGraph {
public:
    ObjectGraph() = default;
    ObjectGraph(const ObjectGraph&) = delete;
    ObjectGraph& operator=(const ObjectGraph&) = delete;
    ~ObjectGraph() {
        while (!values_.empty()) {
            values_.pop_back();
        }
    }

    template<class T>
    std::shared_ptr<T> get(const Ref<T>& reference) const {
        return lookup(reference.key).template get<std::shared_ptr<T>>();
    }

private:
    friend class Plan;
    const internal::Value& lookup(const Key& key) const {
        const auto found = index_.find(key);
        if (found == index_.end()) {
            throw std::out_of_range("service was not provisioned: " + key.describe());
        }
        return values_.at(found->second);
    }
    void insert(const Key& key, internal::Value value) {
        if (!index_.emplace(key, values_.size()).second) {
            throw std::logic_error("a plan provisioned the same key twice");
        }
        values_.push_back(std::move(value));
    }

    std::map<Key, std::size_t> index_;
    std::vector<internal::Value> values_;
};

class Plan {
public:
    std::vector<Key> order() const {
        std::vector<Key> result;
        for (const auto& step : *steps_) {
            result.push_back(step.key);
        }
        return result;
    }

    std::string render() const {
        std::ostringstream result;
        for (const auto& step : *steps_) {
            result << step.key.describe() << " <-";
            for (const auto& dependency : step.dependencies) {
                result << " [" << dependency.describe() << "]";
            }
            result << '\n';
        }
        return result.str();
    }

    IO<ProvisionError, std::shared_ptr<ObjectGraph>> produce() const {
        return IO<ProvisionError, std::shared_ptr<ObjectGraph>>::suspend([steps = steps_] {
            auto graph = std::make_shared<ObjectGraph>();
            auto program = IO<ProvisionError, Unit>::pure(Unit{});
            for (std::size_t index = 0; index < steps->size(); ++index) {
                program = program.flat_map([steps, index, graph](Unit) {
                    const auto& step = steps->at(index);
                    std::vector<internal::Value> arguments;
                    for (const auto& dependency : step.dependencies) {
                        arguments.push_back(graph->lookup(dependency));
                    }
                    return step.provider(arguments).map([graph, key = step.key](internal::Value value) {
                        graph->insert(key, std::move(value));
                        return Unit{};
                    });
                });
            }
            return program.map([graph](Unit) { return graph; });
        });
    }

private:
    friend class Planner;
    explicit Plan(std::vector<internal::Step> steps)
        : steps_(std::make_shared<const std::vector<internal::Step>>(std::move(steps))) {}
    std::shared_ptr<const std::vector<internal::Step>> steps_;
};

class Planner {
public:
    Result<PlanError, Plan> plan(const Module& module, const std::vector<Key>& roots, const Activation& activation) const {
        using Index = std::map<Key, std::vector<const internal::Binding*>>;
        Index index;
        std::map<std::type_index, std::uint64_t> choices;
        for (const auto& point : activation.points) {
            auto [entry, inserted] = choices.emplace(point.axis, point.value);
            if (!inserted && entry->second != point.value) {
                return std::unexpected(PlanError{PlanErrorCode::conflicting_axis, "conflicting activation points", {}});
            }
        }
        const auto valid = [](const Tags& tags) {
            std::map<std::type_index, std::uint64_t> seen;
            for (const auto& point : tags.points) {
                auto [entry, inserted] = seen.emplace(point.axis, point.value);
                if (!inserted && entry->second != point.value) {
                    return false;
                }
            }
            return true;
        };
        const auto active = [&choices](const Tags& tags) {
            return std::all_of(tags.points.begin(), tags.points.end(), [&choices](const AxisPoint& point) {
                const auto choice = choices.find(point.axis);
                return choice != choices.end() && choice->second == point.value;
            });
        };
        for (const auto& binding : module.bindings_) {
            if (!valid(binding.tags)) {
                return std::unexpected(PlanError{PlanErrorCode::conflicting_axis, "conflicting binding tags", {binding.step.key}});
            }
            if (active(binding.tags)) {
                index[binding.step.key].push_back(&binding);
            }
        }
        for (const auto& [key, set] : module.sets_) {
            for (const auto& contribution : set.contributions) {
                if (!valid(contribution.tags)) {
                    return std::unexpected(PlanError{PlanErrorCode::conflicting_axis, "conflicting set contribution tags", {key}});
                }
            }
        }
        const auto resolve = [&](const Key& key) -> Result<PlanError, internal::Step> {
            const auto bindings = index.find(key);
            const auto set = module.sets_.find(key);
            const auto count = bindings == index.end() ? 0 : bindings->second.size();
            if (count > 1 || (count != 0 && set != module.sets_.end())) {
                return std::unexpected(PlanError{PlanErrorCode::ambiguous_binding, "multiple active bindings", {key}});
            }
            if (count == 1) {
                return bindings->second.front()->step;
            }
            if (set == module.sets_.end()) {
                return std::unexpected(PlanError{PlanErrorCode::missing_binding, "no active binding", {key}});
            }
            std::vector<Key> dependencies;
            std::vector<internal::Contribution> contributions;
            for (const auto& contribution : set->second.contributions) {
                if (active(contribution.tags)) {
                    dependencies.push_back(contribution.dependency);
                    contributions.push_back(contribution);
                }
            }
            return internal::Step{key, std::move(dependencies), [contributions, assemble = set->second.assemble](const auto& values) {
                std::vector<internal::Value> elements;
                for (std::size_t index = 0; index < contributions.size(); ++index) {
                    elements.push_back(contributions[index].convert(values.at(index)));
                }
                return IO<ProvisionError, internal::Value>::pure(assemble(elements));
            }};
        };

        enum class State { visiting, complete };
        struct Frame { Key key; std::size_t next_dependency; };
        std::map<Key, State> states;
        std::map<Key, internal::Step> resolved;
        std::vector<Frame> stack;
        std::vector<internal::Step> ordered;
        const auto push = [&](const Key& key) -> Result<PlanError, Unit> {
            auto step = resolve(key);
            if (!step) {
                auto error = std::move(step.error());
                error.path.clear();
                for (const auto& frame : stack) {
                    error.path.push_back(frame.key);
                }
                error.path.push_back(key);
                return std::unexpected(std::move(error));
            }
            resolved.emplace(key, std::move(*step));
            states.emplace(key, State::visiting);
            stack.push_back({key, 0});
            return Unit{};
        };
        for (const auto& root : roots) {
            if (states.contains(root)) {
                continue;
            }
            if (auto added = push(root); !added) {
                return std::unexpected(std::move(added.error()));
            }
            while (!stack.empty()) {
                auto& frame = stack.back();
                const auto& step = resolved.at(frame.key);
                if (frame.next_dependency == step.dependencies.size()) {
                    states.at(frame.key) = State::complete;
                    ordered.push_back(step);
                    stack.pop_back();
                    continue;
                }
                const auto dependency = step.dependencies[frame.next_dependency++];
                const auto state = states.find(dependency);
                if (state != states.end() && state->second == State::visiting) {
                    std::vector<Key> cycle;
                    const auto start = std::find_if(stack.begin(), stack.end(), [&](const Frame& entry) {
                        return entry.key == dependency;
                    });
                    for (auto entry = start; entry != stack.end(); ++entry) {
                        cycle.push_back(entry->key);
                    }
                    cycle.push_back(dependency);
                    return std::unexpected(PlanError{PlanErrorCode::cycle, "dependency cycle", std::move(cycle)});
                }
                if (state == states.end()) {
                    if (auto added = push(dependency); !added) {
                        return std::unexpected(std::move(added.error()));
                    }
                }
            }
        }
        return Plan(std::move(ordered));
    }
};

}
