#pragma once

#include "bio.hpp"

#include <coroutine>
#include <optional>
#include <tuple>

namespace mini {

namespace detail { struct DoAccess; }

template<template<class, class> class Eff, class E, class A>
class Do {
public:
    using error_type = E;
    using value_type = A;
    using effect_type = Effect<Eff, E, A>;
    using step_type = Effect<Eff, E, Unit>;

    struct promise_type {
        std::optional<step_type> pending;
        std::optional<A> result;
        std::exception_ptr defect;

        Do get_return_object() { return Do(Handle::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void unhandled_exception() noexcept { defect = std::current_exception(); }
        void return_value(A value) { result.emplace(std::move(value)); }

        template<class Source>
            requires (requires { typename Source::error_type; typename Source::value_type; } &&
                      std::same_as<Source, Effect<Eff, typename Source::error_type, typename Source::value_type>> &&
                      ChannelWidensTo<typename Source::error_type, E> &&
                      std::copy_constructible<typename Source::value_type>)
        auto await_transform(Source source) {
            using B = typename Source::value_type;
            struct Awaiter {
                promise_type& promise;
                Source source;
                std::optional<B> value;

                bool await_ready() const noexcept { return false; }
                void await_suspend(std::coroutine_handle<>) {
                    // Normalize the heterogeneous value channel before the selected dictionary binds it.
                    promise.pending.emplace(mini::map(std::move(source), [this](B next) {
                        value.emplace(std::move(next));
                        return Unit{};
                    }));
                }
                B await_resume() {
                    if (!value) { throw std::logic_error("do continuation resumed without an awaited value"); }
                    return std::move(*value);
                }
            };
            return Awaiter{*this, std::move(source), std::nullopt};
        }
    };

    Do(const Do&) = delete;
    Do& operator=(const Do&) = delete;
    Do(Do&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    ~Do() { if (handle_) { handle_.destroy(); } }

private:
    using Handle = std::coroutine_handle<promise_type>;
    explicit Do(Handle handle) : handle_(handle) {}
    Handle handle_;
    friend struct detail::DoAccess;
};

namespace detail {

struct DoAccess {
    template<class Task>
    static auto handle(Task& task) {
        if (!task.handle_) { throw std::logic_error("cannot drive a moved-from do block"); }
        return task.handle_;
    }
};

template<class Task> struct DoTraits;
template<template<class, class> class Eff, class E, class A>
struct DoTraits<Do<Eff, E, A>> { using effect_type = Effect<Eff, E, A>; };

template<class Factory, class... Args>
using DoTask = std::invoke_result_t<const Factory&, Args...>;

template<template<class, class> class Eff, class Factory, class... Args>
concept DoFactoryFor = std::copy_constructible<Factory> && (std::copy_constructible<Args> && ...) &&
    std::invocable<const Factory&, Args...> && requires {
        typename DoTraits<DoTask<Factory, Args...>>::effect_type;
    } && std::same_as<typename DoTask<Factory, Args...>::effect_type,
                     Effect<Eff, typename DoTask<Factory, Args...>::error_type,
                                 typename DoTask<Factory, Args...>::value_type>>;

template<class Dictionary, class Task>
concept DoDictionaryFor = requires(const Dictionary& dictionary, typename Task::value_type value,
                                  typename Task::step_type step) {
    { dictionary.template pure<typename Task::error_type>(std::move(value)) } -> std::same_as<typename Task::effect_type>;
    { dictionary.flat_map(std::move(step), std::declval<typename Task::effect_type (*)(Unit)>()) }
        -> std::same_as<typename Task::effect_type>;
    { dictionary.flat_map(std::move(step), std::declval<typename Task::step_type (*)(Unit)>()) }
        -> std::same_as<typename Task::step_type>;
};

template<class Task, class Dictionary, class Factory>
struct DoState {
    using task_type = Task;
    Dictionary dictionary;
    Factory factory;
    // The callable stays at a stable address and outlives the frame, including its lambda captures.
    Task task;

    template<class... Args>
    DoState(Dictionary dictionary, Factory factory, Args... args)
        : dictionary(std::move(dictionary)), factory(std::move(factory)),
          task(std::invoke(std::as_const(this->factory), std::move(args)...)) {}

    auto resume() {
        auto handle = DoAccess::handle(task);
        handle.resume();
        auto& promise = handle.promise();
        if (promise.defect) { std::rethrow_exception(promise.defect); }
        if (handle.done() ? !promise.result.has_value() : !promise.pending.has_value()) {
            throw std::logic_error("do block suspended or completed without a result");
        }
        return handle;
    }
};

template<class State>
auto resume_io_do(std::shared_ptr<State> state) -> typename State::task_type::effect_type {
    using Task = typename State::task_type;
    using Out = typename Task::effect_type;
    auto handle = state->resume();
    auto& promise = handle.promise();
    if (handle.done()) {
        return state->dictionary.template pure<typename Task::error_type>(std::move(*promise.result));
    }
    auto step = std::move(*promise.pending);
    promise.pending.reset();
    return state->dictionary.flat_map(std::move(step), [state](Unit) -> Out {
        return Out::suspend([state] { return resume_io_do(state); });
    });
}

}

template<template<class, class> class Eff>
struct Do2;

template<>
struct Do2<IO> : Error2<IO> {
    template<class Dictionary, class Factory, class... Args>
        requires (detail::DoFactoryFor<IO, Factory, Args...> &&
                  detail::DoDictionaryFor<Dictionary, detail::DoTask<Factory, Args...>>)
    static auto do_(Dictionary dictionary, Factory factory, Args... args) {
        using Task = detail::DoTask<Factory, Args...>;
        using State = detail::DoState<Task, Dictionary, Factory>;
        using Out = typename Task::effect_type;
        return Out::suspend([dictionary, factory, arguments = std::tuple<Args...>(std::move(args)...)] {
            auto state = std::apply([&](const auto&... values) {
                return std::make_shared<State>(dictionary, factory, values...);
            }, arguments);
            return detail::resume_io_do(std::move(state));
        });
    }
};

template<>
struct Do2<Result> : Error2<Result> {
    template<class Dictionary, class Factory, class... Args>
        requires (detail::DoFactoryFor<Result, Factory, Args...> &&
                  detail::DoDictionaryFor<Dictionary, detail::DoTask<Factory, Args...>>)
    static auto do_(Dictionary dictionary, Factory factory, Args... args) {
        using Task = detail::DoTask<Factory, Args...>;
        using E = typename Task::error_type;
        using Out = typename Task::effect_type;
        detail::DoState<Task, Dictionary, Factory> state(std::move(dictionary), std::move(factory), std::move(args)...);
        for (;;) {
            auto handle = state.resume();
            auto& promise = handle.promise();
            if (handle.done()) { return state.dictionary.template pure<E>(std::move(*promise.result)); }
            auto pending = std::move(*promise.pending);
            promise.pending.reset();
            auto step = state.dictionary.flat_map(std::move(pending), [](Unit) { return Result<E, Unit>(Unit{}); });
            if (!step) { return Out(std::unexpected(std::move(step.error()))); }
        }
    }
};

template<> struct Do2<std::expected> : Do2<Result> {};

}
