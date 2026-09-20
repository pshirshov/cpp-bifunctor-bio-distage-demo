#pragma once

#include "io.hpp"

#include <ranges>

namespace mini {

template<template<class, class> class F>
struct Monad2;

template<template<class, class> class F>
struct Bifunctor2;

template<template<class, class> class F>
struct Error2;

template<class T>
concept ExpectedEffect = requires { typename T::error_type; typename T::value_type; } &&
    std::same_as<T, std::expected<typename T::value_type, typename T::error_type>>;

template<>
struct Monad2<Result> {
    template<class E, class A>
    static Result<E, A> pure(A value) { return value; }

    template<class E, class A, class Next>
        requires (std::invocable<const Next&, A> && ExpectedEffect<std::invoke_result_t<const Next&, A>> &&
                  CompatibleChannels<E, typename std::invoke_result_t<const Next&, A>::error_type>)
    static auto flat_map(Result<E, A> source, Next next) {
        using NextResult = std::invoke_result_t<const Next&, A>;
        using Out = Result<JoinChannel<E, typename NextResult::error_type>, typename NextResult::value_type>;
        if (!source) {
            return Out(std::unexpected(std::move(source.error())));
        }
        return Out(std::invoke(std::as_const(next), std::move(*source)));
    }
};

template<>
struct Monad2<IO> {
    template<class E, class A>
    static IO<E, A> pure(A value) {
        return IO<E, A>::pure(std::move(value));
    }

    template<class E, class A, class Next>
        requires requires(IO<E, A> source, Next next) { source.flat_map(std::move(next)); }
    static auto flat_map(IO<E, A> source, Next next) {
        return source.flat_map(std::move(next));
    }
};

template<>
struct Bifunctor2<Result> {
    template<class E, class A, class OnError, class OnValue>
    static auto bimap(Result<E, A> source, OnError on_error, OnValue on_value) {
        using E2 = std::remove_cvref_t<std::invoke_result_t<const OnError&, E>>;
        using B = std::remove_cvref_t<std::invoke_result_t<const OnValue&, A>>;
        if (!source) {
            return Result<E2, B>(std::unexpected(
                std::invoke(std::as_const(on_error), std::move(source.error()))));
        }
        return Result<E2, B>(std::invoke(std::as_const(on_value), std::move(*source)));
    }
};

template<>
struct Bifunctor2<IO> {
    template<class E, class A, class OnError, class OnValue>
    static auto bimap(IO<E, A> source, OnError on_error, OnValue on_value) {
        return source.bimap(std::move(on_error), std::move(on_value));
    }
};

template<>
struct Error2<Result> : Monad2<Result>, Bifunctor2<Result> {
    template<class E, class A>
    static Result<E, A> fail(E error) { return std::unexpected(std::move(error)); }

    template<class E, class A, class Recover>
        requires (std::invocable<const Recover&, E> && ExpectedEffect<std::invoke_result_t<const Recover&, E>> &&
                  CompatibleChannels<A, typename std::invoke_result_t<const Recover&, E>::value_type>)
    static auto catch_all(Result<E, A> source, Recover recover) {
        using Recovery = std::invoke_result_t<const Recover&, E>;
        using Out = Result<typename Recovery::error_type, JoinChannel<A, typename Recovery::value_type>>;
        if (!source) {
            return Out(std::invoke(std::as_const(recover), std::move(source.error())));
        }
        return Out(std::move(*source));
    }
};

template<>
struct Error2<IO> : Monad2<IO>, Bifunctor2<IO> {
    template<class E, class A>
    static IO<E, A> fail(E error) {
        return IO<E, A>::fail(std::move(error));
    }

    template<class E, class A, class Recover>
        requires requires(IO<E, A> source, Recover recover) { source.catch_all(std::move(recover)); }
    static auto catch_all(IO<E, A> source, Recover recover) {
        return source.catch_all(std::move(recover));
    }
};

template<> struct Monad2<std::expected> : Monad2<Result> {};
template<> struct Bifunctor2<std::expected> : Bifunctor2<Result> {};
template<> struct Error2<std::expected> : Error2<Result> {};

template<class Effect>
struct EffectTraits;

template<template<class, class> class F, class E, class A>
struct EffectTraits<F<E, A>> {
    using error_type = E;
    using value_type = A;
    using monad = Monad2<F>;
    using bifunctor = Bifunctor2<F>;
    using errors = Error2<F>;
};

template<class A, class E>
struct EffectTraits<std::expected<A, E>> {
    using error_type = E;
    using value_type = A;
    using monad = Monad2<Result>;
    using bifunctor = Bifunctor2<Result>;
    using errors = Error2<Result>;
};

template<template<class, class> class F>
struct EffectFamily {
    template<class E, class A>
    using Apply = F<E, A>;
};

template<>
struct EffectFamily<std::expected> {
    template<class E, class A>
    using Apply = std::expected<A, E>;
};

template<template<class, class> class F, class E, class A>
using Effect = typename EffectFamily<F>::template Apply<E, A>;

template<class Effect, class Next>
    requires requires(Effect source, Next next) {
        EffectTraits<Effect>::monad::flat_map(std::move(source), std::move(next));
    }
auto flat_map(Effect source, Next next) {
    using M = typename EffectTraits<Effect>::monad;
    return M::flat_map(std::move(source), std::move(next));
}

template<class Effect, class Map>
auto map(Effect source, Map transform) {
    using T = EffectTraits<Effect>;
    using M = typename T::monad;
    return M::flat_map(std::move(source), [transform = std::move(transform)](typename T::value_type value) {
        return M::template pure<typename T::error_type>(std::invoke(transform, std::move(value)));
    });
}

template<class Effect, class OnError, class OnValue>
auto bimap(Effect source, OnError on_error, OnValue on_value) {
    using B = typename EffectTraits<Effect>::bifunctor;
    return B::bimap(std::move(source), std::move(on_error), std::move(on_value));
}

template<class Effect, class Map>
auto map_error(Effect source, Map transform) {
    return mini::bimap(std::move(source), std::move(transform), std::identity{});
}

template<class Effect, class Recover>
    requires requires(Effect source, Recover recover) {
        EffectTraits<Effect>::errors::catch_all(std::move(source), std::move(recover));
    }
auto catch_all(Effect source, Recover recover) {
    using M = typename EffectTraits<Effect>::errors;
    return M::catch_all(std::move(source), std::move(recover));
}

namespace detail {

template<class Dictionary, class Range, class Step> requires std::ranges::input_range<const Range>
auto traverse_with(Dictionary dictionary, const Range& items, Step step) {
    using Item = std::ranges::range_value_t<const Range>;
    using T = EffectTraits<std::invoke_result_t<const Step&, Item>>;
    using Values = std::vector<typename T::value_type>;
    auto program = dictionary.template pure<typename T::error_type>(Values{});
    for (const auto& item : items) {
        program = dictionary.flat_map(std::move(program), [item = Item(item), step, dictionary](Values values) {
            return dictionary.flat_map(std::invoke(step, Item(item)), [values = std::move(values), dictionary](typename T::value_type value) {
                auto result = values;
                result.push_back(std::move(value));
                return dictionary.template pure<Never>(std::move(result));
            });
        });
    }
    return program;
}

}

template<class Range, class Step> requires std::ranges::input_range<const Range>
auto traverse(const Range& items, Step step) {
    using T = EffectTraits<std::invoke_result_t<const Step&, std::ranges::range_value_t<const Range>>>;
    return detail::traverse_with(typename T::monad{}, items, std::move(step));
}

template<template<class, class> class F>
struct Sync2;

template<>
struct Sync2<IO> : Error2<IO> {
    template<class E, class Thunk>
    static auto sync(Thunk thunk) {
        using A = std::remove_cvref_t<std::invoke_result_t<const Thunk&>>;
        return IO<E, A>::sync(std::move(thunk));
    }

    template<class Thunk>
    static auto suspend(Thunk thunk) {
        using Out = std::invoke_result_t<const Thunk&>;
        return Out::suspend(std::move(thunk));
    }

    template<class Thunk>
    static auto attempt(Thunk thunk) {
        using A = std::remove_cvref_t<std::invoke_result_t<const Thunk&>>;
        return IO<std::exception_ptr, A>::defer([thunk = std::move(thunk)]() -> Result<std::exception_ptr, A> {
            try {
                return std::invoke(thunk);
            } catch (...) {
                return std::unexpected(std::current_exception());
            }
        });
    }
};

template<template<class, class> class F>
struct Bracket2;

template<>
struct Bracket2<IO> : Sync2<IO> {
    template<class E, class R, class Use, class Release>
    static auto bracket(IO<E, R> acquire, Use use, Release release) {
        return acquire.bracket(std::move(use), std::move(release));
    }
};

template<class Dictionary, class Source>
concept MonadFor = requires(const Dictionary& dictionary, Source source) {
    dictionary.template pure<Never>(std::declval<typename Source::value_type>());
    dictionary.flat_map(source, std::declval<Source (*)(typename Source::value_type)>());
};

template<class Dictionary>
class Bio {
    [[no_unique_address]] Dictionary dictionary_;

public:
    explicit Bio(Dictionary dictionary) : dictionary_(std::move(dictionary)) {}

    template<class A>
    auto pure(A value) const requires requires { dictionary_.template pure<Never>(std::move(value)); } {
        return dictionary_.template pure<Never>(std::move(value));
    }

    template<class E>
    auto fail(E error) const requires requires { dictionary_.template fail<E, Never>(std::move(error)); } {
        return dictionary_.template fail<E, Never>(std::move(error));
    }

    template<class Source, class Next>
    auto flat_map(Source source, Next next) const
        requires requires { dictionary_.flat_map(std::move(source), std::move(next)); } {
        return dictionary_.flat_map(std::move(source), std::move(next));
    }

    template<class Source, class Map> requires MonadFor<Dictionary, Source>
    auto map(Source source, Map transform) const {
        return flat_map(std::move(source), [dictionary = dictionary_, transform = std::move(transform)](typename Source::value_type value) {
            return dictionary.template pure<Never>(std::invoke(transform, std::move(value)));
        });
    }

    template<class Source, class OnError, class OnValue>
    auto bimap(Source source, OnError on_error, OnValue on_value) const
        requires requires { dictionary_.bimap(std::move(source), std::move(on_error), std::move(on_value)); } {
        return dictionary_.bimap(std::move(source), std::move(on_error), std::move(on_value));
    }

    template<class Source, class Map>
    auto map_error(Source source, Map transform) const
        requires requires { dictionary_.bimap(std::move(source), std::move(transform), std::identity{}); } {
        return bimap(std::move(source), std::move(transform), std::identity{});
    }

    template<class Source, class Recover>
    auto catch_all(Source source, Recover recover) const
        requires requires { dictionary_.catch_all(std::move(source), std::move(recover)); } {
        return dictionary_.catch_all(std::move(source), std::move(recover));
    }

    template<class Range, class Step>
        requires (std::ranges::input_range<const Range> &&
                  MonadFor<Dictionary, std::invoke_result_t<const Step&, std::ranges::range_value_t<const Range>>>)
    auto traverse(const Range& items, Step step) const {
        return detail::traverse_with(dictionary_, items, std::move(step));
    }

    template<class Thunk>
    auto sync(Thunk thunk) const requires requires { dictionary_.template sync<Never>(std::move(thunk)); } {
        return dictionary_.template sync<Never>(std::move(thunk));
    }

    template<class Thunk>
    auto suspend(Thunk thunk) const requires requires { dictionary_.suspend(std::move(thunk)); } {
        return dictionary_.suspend(std::move(thunk));
    }

    template<class Thunk>
    auto attempt(Thunk thunk) const requires requires { dictionary_.attempt(std::move(thunk)); } {
        return dictionary_.attempt(std::move(thunk));
    }

    template<class Acquire, class Use, class Release>
    auto bracket(Acquire acquire, Use use, Release release) const
        requires requires { dictionary_.bracket(std::move(acquire), std::move(use), std::move(release)); } {
        return dictionary_.bracket(std::move(acquire), std::move(use), std::move(release));
    }
};

}
