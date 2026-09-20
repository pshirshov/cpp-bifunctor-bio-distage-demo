#pragma once

#include "bio.hpp"

#include <array>
#include <tuple>

namespace mini {

template<class Key>
struct Request {};

template<template<class, class> class Eff, class Scope>
auto instance(Request<Monad2<Eff>>, const Scope&) requires requires { Monad2<Eff>{}; } {
    return Monad2<Eff>{};
}

template<template<class, class> class Eff, class Scope>
auto instance(Request<Bifunctor2<Eff>>, const Scope&) requires requires { Bifunctor2<Eff>{}; } {
    return Bifunctor2<Eff>{};
}

template<template<class, class> class Eff, class Scope>
auto instance(Request<Error2<Eff>>, const Scope&) requires requires { Error2<Eff>{}; } {
    return Error2<Eff>{};
}

template<template<class, class> class Eff, class Scope>
auto instance(Request<Sync2<Eff>>, const Scope&) requires requires { Sync2<Eff>{}; } {
    return Sync2<Eff>{};
}

template<template<class, class> class Eff, class Scope>
auto instance(Request<Bracket2<Eff>>, const Scope&) requires requires { Bracket2<Eff>{}; } {
    return Bracket2<Eff>{};
}

namespace detail {

template<class Key> struct InstanceKey { using type = Key; };
template<> struct InstanceKey<Monad2<std::expected>> { using type = Monad2<Result>; };
template<> struct InstanceKey<Bifunctor2<std::expected>> { using type = Bifunctor2<Result>; };
template<> struct InstanceKey<Error2<std::expected>> { using type = Error2<Result>; };
template<class Key> using InstanceKeyOf = typename InstanceKey<Key>::type;

template<class Provider, class Required>
concept Provides = std::same_as<Provider, Required> ||
    (requires { sizeof(Provider); sizeof(Required); } && std::derived_from<Provider, Required>);

template<class Key, class Value>
struct Given {
    using key_type = Key;
    Value value;
};

}

template<template<class, class> class Eff, class... Bindings>
class ImplicitScope {
    std::tuple<Bindings...> bindings_;

    template<class Key>
    static constexpr std::size_t exact_count = (std::size_t{0} + ... + std::same_as<Key, typename Bindings::key_type>);

    template<class Key>
    static constexpr std::size_t compatible_count = (std::size_t{0} + ... + detail::Provides<typename Bindings::key_type, Key>);

    template<class Key>
    static consteval std::size_t selected_index() {
        constexpr std::array<bool, sizeof...(Bindings)> matches{
            (exact_count<Key> != 0 ? std::same_as<Key, typename Bindings::key_type>
                                  : detail::Provides<typename Bindings::key_type, Key>)...};
        for (std::size_t index = 0; index < matches.size(); ++index) {
            if (matches[index]) { return index; }
        }
        return matches.size();
    }

public:
    explicit ImplicitScope(std::tuple<Bindings...> bindings) : bindings_(std::move(bindings)) {}

    template<class E, class A>
    using effect_type = Effect<Eff, E, A>;

    template<class Key>
    static consteval bool can_summon() {
        using K = detail::InstanceKeyOf<Key>;
        if constexpr (exact_count<K> != 0) {
            return exact_count<K> == 1;
        } else if constexpr (compatible_count<K> != 0) {
            return compatible_count<K> == 1;
        } else {
            return requires(const ImplicitScope& scope) { instance(Request<K>{}, scope); };
        }
    }

    template<template<template<class, class> class> class Capability>
    static consteval bool can_summon() { return can_summon<Capability<Eff>>(); }

    template<class Key> requires (can_summon<Key>())
    auto summon() const {
        using K = detail::InstanceKeyOf<Key>;
        if constexpr (compatible_count<K> != 0) {
            return std::get<selected_index<K>()>(bindings_).value;
        } else {
            // Unqualified lookup deliberately admits providers in the requested key's namespace.
            return instance(Request<K>{}, *this);
        }
    }

    template<template<template<class, class> class> class Capability> requires (can_summon<Capability>())
    auto summon() const { return summon<Capability<Eff>>(); }

    template<class Key, std::copy_constructible Value>
    auto provide(Value value) const {
        using Binding = detail::Given<detail::InstanceKeyOf<Key>, Value>;
        return ImplicitScope<Eff, Bindings..., Binding>{
            std::tuple_cat(bindings_, std::tuple{Binding{std::move(value)}})};
    }

    template<template<template<class, class> class> class Capability, std::copy_constructible Value>
    auto provide(Value value) const { return provide<Capability<Eff>>(std::move(value)); }
};

template<template<class, class> class Eff>
auto implicit_scope() { return ImplicitScope<Eff>{std::tuple{}}; }

namespace detail {

template<class Effect, class Scope>
concept EffectInScope = requires { typename Effect::error_type; typename Effect::value_type; } &&
    std::same_as<Effect, typename Scope::template effect_type<typename Effect::error_type, typename Effect::value_type>>;

template<class Scope>
class ResolvedDictionary {
    Scope scope_;

public:
    explicit ResolvedDictionary(Scope scope) : scope_(std::move(scope)) {}

    template<class E, class A>
    auto pure(A value) const
        requires requires {
            { scope_.template summon<Monad2>().template pure<E>(std::move(value)) } -> std::same_as<typename Scope::template effect_type<E, A>>;
        } {
        return scope_.template summon<Monad2>().template pure<E>(std::move(value));
    }

    template<class E, class A>
    auto fail(E error) const
        requires requires {
            { scope_.template summon<Error2>().template fail<E, A>(std::move(error)) } -> std::same_as<typename Scope::template effect_type<E, A>>;
        } {
        return scope_.template summon<Error2>().template fail<E, A>(std::move(error));
    }

    template<class Source, class Next>
    auto flat_map(Source source, Next next) const
        requires requires {
            { scope_.template summon<Monad2>().flat_map(std::move(source), std::move(next)) } -> EffectInScope<Scope>;
        } {
        return scope_.template summon<Monad2>().flat_map(std::move(source), std::move(next));
    }

    template<class Source, class OnError, class OnValue>
    auto bimap(Source source, OnError on_error, OnValue on_value) const
        requires requires {
            { scope_.template summon<Bifunctor2>().bimap(std::move(source), std::move(on_error), std::move(on_value)) } -> EffectInScope<Scope>;
        } {
        return scope_.template summon<Bifunctor2>().bimap(std::move(source), std::move(on_error), std::move(on_value));
    }

    template<class Source, class Recover>
    auto catch_all(Source source, Recover recover) const
        requires requires {
            { scope_.template summon<Error2>().catch_all(std::move(source), std::move(recover)) } -> EffectInScope<Scope>;
        } {
        return scope_.template summon<Error2>().catch_all(std::move(source), std::move(recover));
    }

    template<class E, class Thunk>
    auto sync(Thunk thunk) const
        requires requires {
            { scope_.template summon<Sync2>().template sync<E>(std::move(thunk)) } -> EffectInScope<Scope>;
        } {
        return scope_.template summon<Sync2>().template sync<E>(std::move(thunk));
    }

    template<class Thunk>
    auto suspend(Thunk thunk) const requires requires {
        { scope_.template summon<Sync2>().suspend(std::move(thunk)) } -> EffectInScope<Scope>;
    } {
        return scope_.template summon<Sync2>().suspend(std::move(thunk));
    }

    template<class Thunk>
    auto attempt(Thunk thunk) const requires requires {
        { scope_.template summon<Sync2>().attempt(std::move(thunk)) } -> EffectInScope<Scope>;
    } {
        return scope_.template summon<Sync2>().attempt(std::move(thunk));
    }

    template<class Acquire, class Use, class Release>
    auto bracket(Acquire acquire, Use use, Release release) const
        requires requires {
            { scope_.template summon<Bracket2>().bracket(std::move(acquire), std::move(use), std::move(release)) } -> EffectInScope<Scope>;
        } {
        return scope_.template summon<Bracket2>().bracket(std::move(acquire), std::move(use), std::move(release));
    }
};

}

template<class Scope>
auto bio(Scope scope) { return Bio{detail::ResolvedDictionary{std::move(scope)}}; }

template<class Scope, class Program>
auto with_bio(Scope scope, Program program) {
    return std::invoke(std::move(program), bio(std::move(scope)));
}

}
