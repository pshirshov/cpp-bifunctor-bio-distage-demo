#pragma once

#include "type.hpp"

#include <atomic>
#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <type_traits>
#include <variant>
#include <vector>

namespace mini {

using Unit = std::monostate;

struct Never final {
    Never() = delete;
    Never(const Never&) noexcept { std::terminate(); }
    Never& operator=(const Never&) noexcept { std::terminate(); }

    // An impossible value may be eliminated, never manufactured; expected uses this for widening.
    template<class T> requires (std::is_object_v<T> && !std::same_as<T, Never>)
    operator T() const noexcept { std::terminate(); }

    template<class T>
    friend bool operator==(const Never&, const T&) noexcept { std::terminate(); }
};

template<class From, class To>
concept ChannelWidensTo = std::same_as<From, To> || std::same_as<From, Never>;

template<class L, class R>
concept CompatibleChannels = ChannelWidensTo<L, R> || ChannelWidensTo<R, L>;

template<class L, class R> requires CompatibleChannels<L, R>
using JoinChannel = std::conditional_t<std::same_as<L, Never>, R, L>;

template<class E, class A>
using Result = std::expected<A, E>;

template<class A>
struct Success { A value; };

template<class E>
struct Failure { E error; };

struct Defect { std::exception_ptr exception; };

template<class E, class A>
using Exit = std::variant<Success<A>, Failure<E>, Defect>;

template<class E, class A>
class IO;

template<class T>
concept IOEffect = requires { typename T::error_type; typename T::value_type; } &&
    std::same_as<T, IO<typename T::error_type, typename T::value_type>>;

namespace detail {

using Outcome = Exit<Value, Value>;
struct Node;

class NodePtr {
public:
    NodePtr() noexcept : node_(nullptr) {}
    explicit NodePtr(Node* node) noexcept : node_(node) {}
    NodePtr(const NodePtr& other) noexcept;
    NodePtr(NodePtr&& other) noexcept : node_(std::exchange(other.node_, nullptr)) {}
    NodePtr& operator=(NodePtr other) noexcept {
        std::swap(node_, other.node_);
        return *this;
    }
    ~NodePtr();
    Node& operator*() const { return *node_; }
    Node* operator->() const { return node_; }
    explicit operator bool() const { return node_ != nullptr; }

private:
    Node* node_;
};

struct Eval { std::function<Outcome()> run; };
struct Suspend { std::function<NodePtr()> next; };
struct Continue { std::function<NodePtr(const Outcome&)> resume; };
using Instruction = std::variant<Outcome, Eval, Suspend, Continue>;

struct Node {
    Node(NodePtr source_node, Instruction operation)
        : source(std::move(source_node)), instruction(std::move(operation)) {}

    std::atomic_size_t references{1};
    NodePtr source;
    Instruction instruction;
};

inline NodePtr::NodePtr(const NodePtr& other) noexcept : node_(other.node_) {
    if (node_) {
        node_->references.fetch_add(1, std::memory_order_relaxed);
    }
}

inline NodePtr::~NodePtr() {
    auto node = node_;
    while (node && node->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Detach the owned edge before deletion: releasing a bind spine must not recurse.
        auto source = std::exchange(node->source.node_, nullptr);
        delete node;
        node = source;
    }
}

inline NodePtr terminal(Outcome outcome) {
    return NodePtr(new Node(NodePtr{}, std::move(outcome)));
}

inline Outcome evaluate(NodePtr current) {
    std::vector<NodePtr> continuations;
    for (;;) {
        Outcome outcome = Defect{nullptr};
        try {
            if (!current) {
                throw std::logic_error("cannot run a moved-from IO");
            }
            const auto& instruction = current->instruction;
            if (std::holds_alternative<Continue>(instruction)) {
                continuations.push_back(current);
                current = current->source;
                continue;
            }
            if (const auto* suspended = std::get_if<Suspend>(&instruction)) {
                auto next = suspended->next();
                current = std::move(next);
                continue;
            }
            if (const auto* eval = std::get_if<Eval>(&instruction)) {
                outcome = eval->run();
            } else {
                outcome = std::get<Outcome>(instruction);
            }
        } catch (...) {
            outcome = Defect{std::current_exception()};
        }
        if (continuations.empty()) {
            return outcome;
        }
        auto frame = std::move(continuations.back());
        continuations.pop_back();
        try {
            current = std::get<Continue>(frame->instruction).resume(outcome);
        } catch (...) {
            current = terminal(Defect{std::current_exception()});
        }
    }
}

}

template<class E, class A>
class IO {
    static_assert(std::copy_constructible<E> && std::copy_constructible<A>);
    template<class, class> friend class IO;

public:
    using error_type = E;
    using value_type = A;
    using result_type = Result<E, A>;

    template<class E0, class A0> requires (ChannelWidensTo<E0, E> && ChannelWidensTo<A0, A>)
    IO(const IO<E0, A0>& source) : node_(source.node_) {}

    static IO pure(A value) {
        return IO(detail::terminal(Success<detail::Value>{detail::Value::of(std::move(value))}));
    }

    static IO fail(E error) {
        return IO(detail::terminal(Failure<detail::Value>{detail::Value::of(std::move(error))}));
    }

    template<class Thunk>
    static IO defer(Thunk thunk) {
        static_assert(std::same_as<std::invoke_result_t<const Thunk&>, result_type>);
        return IO(detail::NodePtr(new detail::Node({}, detail::Eval{[thunk = std::move(thunk)] {
            auto result = std::invoke(thunk);
            if (!result) {
                return detail::Outcome(Failure<detail::Value>{detail::Value::of(std::move(result.error()))});
            }
            return detail::Outcome(Success<detail::Value>{detail::Value::of(std::move(*result))});
        }})));
    }

    template<class Thunk>
    static IO sync(Thunk thunk) {
        static_assert(std::same_as<std::remove_cvref_t<std::invoke_result_t<const Thunk&>>, A>);
        return defer([thunk = std::move(thunk)] { return result_type(std::invoke(thunk)); });
    }

    template<class Thunk>
    static IO suspend(Thunk thunk) {
        static_assert(std::same_as<std::invoke_result_t<const Thunk&>, IO>);
        return IO(detail::NodePtr(new detail::Node({}, detail::Suspend{[thunk = std::move(thunk)] {
            return std::invoke(thunk).node_;
        }})));
    }

    template<class Next>
        requires (std::invocable<const Next&, A> && IOEffect<std::invoke_result_t<const Next&, A>> &&
                  CompatibleChannels<E, typename std::invoke_result_t<const Next&, A>::error_type>)
    auto flat_map(Next next) const {
        using NextIO = std::invoke_result_t<const Next&, A>;
        using Out = IO<JoinChannel<E, typename NextIO::error_type>, typename NextIO::value_type>;
        return Out(continue_with([next = std::move(next)](const detail::Outcome& outcome) {
            if (const auto* success = std::get_if<Success<detail::Value>>(&outcome)) {
                return std::invoke(next, A(success->value.template get<A>())).node_;
            }
            return detail::terminal(outcome);
        }));
    }

    template<class Map>
    auto map(Map transform) const {
        using B = std::remove_cvref_t<std::invoke_result_t<const Map&, A>>;
        return flat_map([transform = std::move(transform)](A value) {
            return IO<E, B>::pure(std::invoke(transform, std::move(value)));
        });
    }

    template<class OnError, class OnValue>
    auto bimap(OnError on_error, OnValue on_value) const {
        using E2 = std::remove_cvref_t<std::invoke_result_t<const OnError&, E>>;
        using B = std::remove_cvref_t<std::invoke_result_t<const OnValue&, A>>;
        return IO<E2, B>(continue_with([
            on_error = std::move(on_error), on_value = std::move(on_value)
        ](const detail::Outcome& outcome) {
            if (const auto* success = std::get_if<Success<detail::Value>>(&outcome)) {
                return IO<E2, B>::pure(std::invoke(on_value, A(success->value.template get<A>()))).node_;
            }
            if (const auto* failure = std::get_if<Failure<detail::Value>>(&outcome)) {
                return IO<E2, B>::fail(std::invoke(on_error, E(failure->error.template get<E>()))).node_;
            }
            return detail::terminal(outcome);
        }));
    }

    template<class Map>
    auto map_error(Map transform) const { return bimap(std::move(transform), std::identity{}); }

    template<class Recover>
        requires (std::invocable<const Recover&, E> && IOEffect<std::invoke_result_t<const Recover&, E>> &&
                  CompatibleChannels<A, typename std::invoke_result_t<const Recover&, E>::value_type>)
    auto catch_all(Recover recover) const {
        using RecoveryIO = std::invoke_result_t<const Recover&, E>;
        using Out = IO<typename RecoveryIO::error_type, JoinChannel<A, typename RecoveryIO::value_type>>;
        return Out(continue_with([recover = std::move(recover)](const detail::Outcome& outcome) {
            if (const auto* failure = std::get_if<Failure<detail::Value>>(&outcome)) {
                return std::invoke(recover, E(failure->error.template get<E>())).node_;
            }
            return detail::terminal(outcome);
        }));
    }

    template<class Finalize>
        requires (std::is_nothrow_invocable_v<const Finalize&> &&
                  std::same_as<std::invoke_result_t<const Finalize&>, void>)
    IO ensuring(Finalize finalize) const {
        return IO(continue_with([finalize = std::move(finalize)](const detail::Outcome& outcome) {
            std::invoke(finalize);
            return detail::terminal(outcome);
        }));
    }

    template<class Use, class Release>
        requires (std::invocable<const Use&, A> && IOEffect<std::invoke_result_t<const Use&, A>> &&
                  CompatibleChannels<E, typename std::invoke_result_t<const Use&, A>::error_type>)
    auto bracket(Use use, Release release) const {
        using UseIO = std::invoke_result_t<const Use&, A>;
        using Out = IO<JoinChannel<E, typename UseIO::error_type>, typename UseIO::value_type>;
        static_assert(std::is_nothrow_invocable_r_v<void, const Release&, const A&>);
        static_assert(std::same_as<std::invoke_result_t<const Release&, const A&>, void>);
        return Out(continue_with([use = std::move(use), release = std::move(release)](const detail::Outcome& outcome) {
            const auto* success = std::get_if<Success<detail::Value>>(&outcome);
            if (!success) {
                return detail::terminal(outcome);
            }
            const auto& resource = success->value;
            struct Guard {
                const A& value;
                const Release& release;
                bool armed;
                ~Guard() { if (armed) { std::invoke(release, value); } }
            } guard{resource.template get<A>(), release, true};
            // Protect the acquired value before copying it or constructing the use effect.
            auto protected_use = Out::suspend([use, resource]() -> Out {
                return std::invoke(use, A(resource.template get<A>()));
            }).ensuring([release, resource]() noexcept {
                std::invoke(release, resource.template get<A>());
            });
            guard.armed = false;
            return protected_use.node_;
        }));
    }

    Exit<E, A> run_exit() const {
        try {
            const auto outcome = detail::evaluate(node_);
            if (const auto* success = std::get_if<Success<detail::Value>>(&outcome)) {
                return Success<A>{success->value.template get<A>()};
            }
            if (const auto* failure = std::get_if<Failure<detail::Value>>(&outcome)) {
                return Failure<E>{failure->error.template get<E>()};
            }
            return std::get<Defect>(outcome);
        } catch (...) {
            return Defect{std::current_exception()};
        }
    }

    result_type unsafe_run() const {
        auto outcome = run_exit();
        if (auto* success = std::get_if<Success<A>>(&outcome)) {
            return std::move(success->value);
        }
        if (auto* failure = std::get_if<Failure<E>>(&outcome)) {
            return std::unexpected(std::move(failure->error));
        }
        std::rethrow_exception(std::get<Defect>(outcome).exception);
    }

private:
    explicit IO(detail::NodePtr node) : node_(std::move(node)) {}

    detail::NodePtr continue_with(std::function<detail::NodePtr(const detail::Outcome&)> resume) const {
        return detail::NodePtr(new detail::Node(node_, detail::Continue{std::move(resume)}));
    }

    detail::NodePtr node_;
};

}
