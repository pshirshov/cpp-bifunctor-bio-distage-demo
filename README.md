# Minimal C++ BIO and staged DI

A header-only C++23 implementation of bifunctor IO, BIO-style operation
dictionaries, and staged dependency injection with bindings, sets, and activation
axes. Standard library only; requires exceptions and RTTI.

The DI feature selection follows the core model of
[DICS](https://github.com/PlayQ/dics/blob/master/docs/core.md): bindings describe a
graph, planning validates it, and provisioning constructs its reachable objects.
This is an independent C++ implementation with the explicit semantics below,
not an API-compatible port.

## Build and run

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
./build/debug/example live
./build/debug/example test
./build/debug/implicit_example
```

Example output:

```text
$ ./build/debug/example live
Hello, Ada!
$ ./build/debug/example test
Hello, Test Ada!
Welcome, Test Ada.
```

The axis selects a dataset and gates a contribution to a greeting service set.
The [application](examples/application.hpp) is parameterized by its effect family
and tested unchanged with both lazy `IO` and eager `std::expected`. The
[entry point](examples/main.cpp) composes provisioning and the application into
one IO, explicitly combining their error types into a `std::variant`.

For the instrumented build:

```sh
cmake -S . -B build/sanitized -DCMAKE_BUILD_TYPE=Debug -DMINI_SANITIZE=ON
cmake --build build/sanitized
ctest --test-dir build/sanitized --output-on-failure
```

On Linux, also check the trampoline under a small native stack:

```sh
bash -c 'ulimit -c 0; ulimit -s 512; ./build/debug/stack_safety'
bash -c 'ulimit -c 0; ulimit -s 512; ./build/debug/do_stack_safety'
```

## IO and typeclasses

[io.hpp](include/mini/io.hpp) implements immutable program descriptions with
`pure`, `fail`, `defer`, `sync`, `suspend`, `map`, `flat_map`, `bimap`,
`map_error`, `catch_all`, `ensuring`, and `bracket`.

```cpp
using mini::IO;

auto program = IO<std::string, int>::sync([] { return 40; })
    .map([](auto n) { return n + 2; })
    .map_error([](auto message) { return MyError{std::move(message)}; });

// Construction has not run the callbacks. Each execution runs them again.
auto result = program.unsafe_run(); // std::expected<int, MyError>
```

`pure(value)` receives an already evaluated C++ value. `sync` suspends a function
returning a value; `defer` suspends a function returning `std::expected<A,E>`.
`suspend` delays construction of another IO and enables recursive programs
without recursive native calls. There is no implicit memoization.

`run_exit()` returns `Success<A>`, `Failure<E>`, or `Defect{exception_ptr}`.
Thrown exceptions in evaluation and continuation construction are defects;
`catch_all` recovers only declared errors. `unsafe_run()` returns
`std::expected<A,E>` and rethrows defects. `Sync2<IO>::attempt` explicitly imports
exceptions into a declared `exception_ptr` error channel.

`flat_map` accepts equal error types or joins `Never` with the other error type.
`catch_all` applies the same rule to its two success types. Unrelated channels
are rejected; use `map_error` to align error domains.
The representation requires copyable errors, values, and callable captures.
Continuations receive owned values; references returned by `map`/`bimap` are
materialized as values. Use `mini::Unit` for no useful result; `mini::Never`
marks a channel with no values, not a successful empty result.

[bio.hpp](include/mini/bio.hpp) provides `Monad2`, `Bifunctor2`, and `Error2`
instances for `IO`, native `std::expected`, and its error-first `Result` alias.
`Sync2` and `Bracket2` are available for IO. Operations are invoked through
compile-time dictionaries; the free functions infer the dictionary from the
input effect:

```cpp
auto increment(auto input) {
    return mini::map(std::move(input), [](auto n) { return n + 1; });
}
```

`flat_map`, `map`, `bimap`, `map_error`, and `catch_all` need no explicit
template arguments. `EffectTraits` identifies the concrete effect, including
native `std::expected<A,E>`. At service boundaries, `Effect<F,E,A>` normalizes
the parameter order: it means `IO<E,A>` for `F = IO` and `std::expected<A,E>`
for `F = std::expected`. The example's `Lookup<F,A>` alias fixes its error
domain once.

Constructors have no input effect to infer from. Bind a capability dictionary
once to a local accessor named `F`. Class template argument deduction determines
the accessor type:

```cpp
const mini::Bio F{mini::Error2<Eff>{}};
auto found = F.pure(User{"Ada"});
auto missing = F.fail(LookupError::missing_user);
```

These are concrete effects, not conversion proxies. `found` has type
`Effect<Eff,Never,User>` and `missing` has type `Effect<Eff,LookupError,Never>`.
Neither operation needs an explicit unused channel. Declared return types
supply widening at service boundaries:

```cpp
Lookup<Eff, User> find(UserId id) const override {
    const auto F = bio(implicit_scope<Eff>());
    if (id.value != 1) {
        return F.fail(LookupError::missing_user);
    }
    return F.pure(User{data_->name});
}
```

This works unchanged for `Eff = IO` and `Eff = std::expected`. A dictionary
can instead be passed as an argument or stored in a service; the accessor owns
a copy, and derived operations retain that dictionary rather than reselecting
an instance from the effect's type. Dictionaries and captures must be copyable.
`Bio{Monad2<Eff>{}}` provides `pure`, `map`, `flat_map`, and `traverse` but no
`fail`. `Error2` adds error operations; `Sync2<IO>` adds `sync`, `suspend`, and
`attempt`; `Bracket2<IO>` adds `bracket`; `Do2` adds coroutine sequencing
for IO and expected. A stronger dictionary is selected
explicitly, not discovered from arbitrary variables in scope. There is no
mutable ambient accessor. Name the effect family `Eff` to leave `F` available
as a value name.

### Typed implicit contexts

[implicit.hpp](include/mini/implicit.hpp) adds automatic instance resolution.
`bio(implicit_scope<Eff>())` provides the standard instances for an effect
family without naming each dictionary. The direct `Bio{dictionary}` form
remains available when a program should expose only that dictionary's
capabilities. A resolving accessor instead exposes all resolvable capabilities;
for example, the canonical expected context has no `sync`, whereas IO does.

Provide a local override once and pass the context through the program:

```cpp
auto scope = implicit_scope<IO>().provide<Monad2>(TracingMonad{trace});
const auto F = bio(scope);
auto program = F.do_([](auto F) -> Do<IO, Never, int> {
    auto value = co_await F.pure(41);
    co_return value + 1;
}, F);
```

The [runnable example](examples/implicit_scope.cpp) returns an IO from a scope
that is then destroyed, and runs the same generic program with native expected.
`with_bio` invokes its callback immediately to construct the program; it is not
a dynamic scope or an IO execution boundary.

For each requested capability, resolution chooses:

1. A unique exact registration.
2. Otherwise, a unique registered capability derived from the requested one.
3. Otherwise, an `instance(Request<Key>{}, scope)` factory found by ADL,
   including the provided canonical BIO factories.

Ambiguous local registrations block fallback. Duplicate exact keys are an
error when requested, not last-registration-wins. Two stronger providers are
ambiguous even when one is stronger than the other; an exact registration
disambiguates the request. Inheritance is checked on registered capability keys,
not arbitrary methods found on provider objects. The selected provider keeps
its concrete type, so overrides are not sliced away. Native expected's BIO keys
are normalized to the error-first Result dictionaries for hierarchy matching.

`scope.provide<Capability>(value)` returns a new context; the parent is unchanged.
Providers must implement the registered operations. The compiler checks these
at use sites, and the BIO accessor rejects operations returning another effect
family. Each operation resolves its own capability: a local monad override does
not replace unrelated error or synchronous operations. Providers must agree on
effect semantics; instance coherence and algebraic laws are not compiler-proven.

Custom typeclasses use concrete keys with `scope.provide<Key>(value)` and
`scope.summon<Key>()`; effect capabilities also have the shorthand
`scope.summon<Monad2>()`. `Scope::can_summon<Key>()` queries availability at
compile time. It reports resolution availability, not proof of a provider's laws
or every operation's signature.

For recursive derivation, define a constrained factory in the key's namespace.
For example, given a `Show<T>` key and a `VectorShow<ElementDictionary>` provider:

```cpp
template<class T, class Scope>
    requires requires(const Scope& scope) { scope.template summon<Show<T>>(); }
auto instance(mini::Request<Show<std::vector<T>>>, const Scope& scope) {
    using Element = decltype(scope.template summon<Show<T>>());
    return VectorShow<Element>{scope.template summon<Show<T>>()};
}
```

The recursive request uses the same context, including local overrides.
Missing or ambiguous evidence rejects the call at compile time. Derivation
rules must terminate; there is no general cycle-solving algorithm. Factory
overload resolution follows C++/ADL rules, not Scala's lexical implicit search.
Unrelated keys are never automatically default-constructed. Adding a custom
typeclass does not add new member names to `F`; use `summon` or extend the BIO
accessor's operation vocabulary explicitly.

Scopes own copyable providers, and accessors and captured continuations own
scope copies. `summon` returns a provider by value. Factories run when summoned;
there is no implicit caching. Supply owning handles when provider state must be
shared across copies. Raw references and provider-created effects that capture
an expiring `this` remain the provider author's responsibility. A scope is an
ordinary typed value that can also be injected through DI; no global registry,
thread-local state, or caller-local variable search is involved.

### Coroutine do-notation

[do.hpp](include/mini/do.hpp) supplies `Do<Eff,E,A>` and the `Do2` capability.
`F.do_(factory, arguments...)` returns an ordinary `Effect<Eff,E,A>`, with all
three types inferred from the factory's coroutine return type. Include
`mini/implicit.hpp` for the resolving accessor, or `mini/do.hpp` for an explicit
`Bio{Do2<Eff>{}}`. `Monad2` alone does not expose `do_`.

The application's sequencing is now:

```cpp
auto run(UserId id) const {
    const auto F = bio(implicit_scope<Eff>());
    return F.do_([](auto users, auto greetings, UserId id)
        -> Do<Eff, LookupError, Lines>
    {
        auto user = co_await users->find(id);
        Lines lines;
        for (const auto& greeting : *greetings) {
            lines.push_back(co_await greeting->greet(user));
        }
        co_return lines;
    }, users_, greetings_, id);
}
```

Here `Lines` is `std::vector<std::string>`. `co_await` extracts the success
value or stops the block on failure; `co_return` supplies its final success
value. Ordinary loops, branches, structured bindings, and RAII locals work
inside the block. Awaited effects must have the same effect family and either
the declared error type or `Never`. Unrelated error types need `map_error`;
there is no inferred error union. Use `Unit`, not `void`, for empty values,
and `co_return co_await F.fail(error)` for a failure-only return.

For IO, the wrapper snapshots its copyable factory and arguments without
executing the body. Each execution creates a new frame, including a fresh
local accumulator. Each await hands control back to the existing trampoline;
there is no nested `unsafe_run`. Expected instead drives the coroutine eagerly
in a loop. Sequential awaits are stack-safe for both; recursive IO programs
are stack-safe when expressed through awaited, lazy `F.do_` calls. Ordinary
recursive calls in the eager expected interpreter still use the native stack.

The frame is destroyed on success, typed failure, or a defect, including
locals that span awaits. Typed failures are not exceptions and cannot be
intercepted by a coroutine's C++ `catch` block. Awaited IO defects also stop
the block without resuming it. Unhandled body exceptions become IO defects;
the expected interpreter rethrows them. Existing `catch_all`, `ensuring`,
and `bracket` compose with the resulting effects.

The wrapper owns the factory at a stable address until the frame is destroyed,
so owning lambda captures are supported. The examples prefer captureless
coroutines with value parameters. References, reference captures, and a captured
`this` remain borrowed; neither copying a factory nor creating a coroutine
extends their referents' lifetimes. Factories must be const-invocable and return
a fresh `Do` frame. A `Do` handle itself is move-only and is not a replayable IO.

The driver retains the supplied context: each await uses its selected monad's
`flat_map`, and completion uses its `pure`. `Do2` itself can also be overridden
through `scope.provide<Do2>(provider)`. Internally, canonical effect operations
store the heterogeneous awaited value in its frame and normalize the step to
`Effect<Eff,E,Unit>` before the scoped bind. Consequently bind instrumentation
observes a Unit success channel, not the original awaited value type.

This bridge requires single-shot sequencing: within an execution, a supplied
monad must evaluate each awaited step and invoke its success continuation at
most once. It does not support branching/multi-shot continuations or turn an
arbitrary monad into a coroutine runtime. There are no asynchronous awaitables,
fibers, cancellation, or scheduling. `Do2` is an explicit additional capability,
not a consequence of the monad laws.

The ordinary loop grows one vector per execution rather than copying its
contents at every bind. The existing `traverse` combinator remains available:
it collects results in iteration order, handles empty input, and stops
invoking the step after failure. It copies range elements during construction;
callbacks are lazy for IO and eager for `std::expected`. Each IO execution has
its own accumulator. This minimal implementation copies the accumulated vector
at each step, so collecting N values has quadratic copying cost.

### Bottom channels, not language-level subtyping

`IO<Never,A>` implicitly widens to `IO<E,A>`; `IO<E,Never>` widens to
`IO<E,A>`. Both channels can widen together. IO shares the existing instruction
graph without evaluating it or casting the stored values. Composition uses
`JoinChannel<Never,E> = E`, `JoinChannel<E,Never> = E`, and
`JoinChannel<E,E> = E`; other joins are ill-formed. Bracket uses the same error
join for acquisition and use.

Native `std::expected` cannot be extended with custom constructors. To make
its existing converting constructors support these boundaries, `Never` has an
implicit impossible-value conversion to object types. Its default constructor
is deleted, its copy operations are nontrivial (preventing `bit_cast` creation),
and any attempted copy, conversion, or comparison of a `Never` value terminates.
Valid effects never execute those operations: they use the other channel or
terminate with a defect. This is an explicit bottom-type approximation, not
C++ subtyping or general covariance. A native expected also retains its usual
standard-library conversion rules; our composition rules remain stricter.

`IO<Never,A>` may still throw a defect or not terminate. `IO<E,Never>` may fail,
defect, or not terminate, but cannot successfully return a value. `F.sync`
uses `Never` for declared errors; use `F.attempt` to import exceptions into a
declared error channel.

C++ still cannot deduce one return type from different `pure` and `fail`
branches in an `auto`-returning lambda. Give such a lambda a declared effect
return type, as at the service boundary above. No automatic error union or
deferred expression DSL is provided.

This follows BIO's separation of effect-polymorphic operations from a concrete
effect runtime. The capability set is intentionally small.
[BIO reference](https://izumi.7mind.io/bio/index.html).

### Trampolining and lifetime

The evaluator is a loop over terminal, synchronous, suspended, and continuation
nodes. Pending continuations live in a heap vector. A bind stores an explicit
edge to its source; evaluating a continuation returns the next node to the loop.
No combinator calls `unsafe_run` internally.

Node handles use reference counting. Copying a description increments one
counter. Releasing a source spine detaches and deletes nodes iteratively, so
execution, copying, and destruction of deep bind chains do not consume a native
frame per bind. The stress test covers 100,000 binds, suspended non-tail recursion,
typed recoveries, and finalizers, including rerunning and destroying copies.

Callbacks remain ordinary C++: the runtime cannot make recursion inside a user
callback or destructor stack-safe. In particular, arbitrary IO ownership hidden
inside nested user callable captures is outside the iterative source-edge
reclamation guarantee. Use `suspend` to express recursive computation.

This runtime is synchronous. There are no fibers, scheduler, interruption,
asynchronous callbacks, or concurrency guarantees for user callbacks. Atomic
node reference counts protect ownership, not mutable state captured by users.
Owning a pointer also does not extend the validity of an external resource that
someone has explicitly closed.

### Synchronous resource scope

`ensuring` takes a `noexcept` callback returning exactly `void`. `Bracket2<IO>`
acquires lazily and releases once on success, declared failure, or a defect in
use, including an exception while constructing use or copying its resource.
Failed acquisition does not release. Nested finalizers run inside-out.

```cpp
auto scoped = mini::Bracket2<mini::IO>::bracket(
    acquire,
    [](Handle handle) { return use(handle); },
    [](const Handle& handle) noexcept { handle.close_noexcept(); }
);
```

Release must be synchronous and non-failing. Returning an IO from the release
callback is rejected rather than discarded. Effectful/fallible finalizers and
combined failure causes require a larger resource model; they are not provided.

## Staged DI

[di.hpp](include/mini/di.hpp) has typed references, immutable plans, and a
per-execution object graph. Providers take and return `shared_ptr` services.
Dependencies are listed once as typed references; constructors need no reflection.

```cpp
namespace di = mini::di;
using di::key;
using di::deps;
using di::when;

enum class Environment { live, test };
di::Module module;
module.provide(key<Config>(), deps(), [] {
    return std::make_shared<Config>(/* configuration */);
}, when(Environment::live));
module.bind<ServiceImpl>(key<Service>(), deps(key<Config>()), when());
module.alias(key<Service>("primary"), key<Service>(), when());
module.add(key<di::Set<Service>>(), key<Service>("primary"), when());

auto plan = di::Planner{}.plan(module, di::roots(key<di::Set<Service>>()),
                               di::activate(Environment::live));
// Check plan before use. Planning has invoked no providers.
auto graph_io = plan->produce();
```

Supported operations:

| Operation | Contract |
|---|---|
| `instance(ref, shared_ptr, tags)` | Reuse a supplied instance |
| `provide(ref, deps(...), factory, tags)` | Invoke a synchronous provider during execution |
| `provide_effect(ref, deps(...), factory, tags)` | Compose an `IO<ProvisionError, shared_ptr<T>>` provider |
| `bind<Impl>(ref, deps(...), tags)` | Infer the interface from the key and construct its implementation |
| `bind<Impl>(deps(...), tags)` | Bind an implementation to its own unnamed key |
| `alias(target, source, tags)` | Share an existing object, performing a checked C++ conversion |
| `set(key<Set<T>>(...))` | Declare an injectable empty set |
| `add(set_ref, element_ref, tags)` | Contribute a binding to a set; implicitly declare the set |
| `include(module)` | Concatenate bindings and merge set contributions |

The planner traverses only requested roots and their dependencies. It rejects
missing reachable bindings, multiple active bindings, and cycles before running
any provider. Malformed, contradictory tags are rejected even on unreachable
bindings. `Plan::order()` and `render()` expose the dependency order; a plan owns
its snapshot and survives later module modification or destruction. Planning
uses an explicit traversal stack.

Each enum type defines an axis. Tags on one binding are conjunctive. A tagged
binding is active only when **every axis is explicitly selected with the same
value**. Omitted axes deactivate tagged bindings, even when they are the only
candidate. Untagged bindings are always active. There is no specificity override
or last-registration-wins rule; multiple active definitions are an error.
This strict gating is the port's policy, not a reproduction of every DICS edge
case. Conflicting activation values are errors; repeated identical values are
idempotent.

Sets collect explicitly registered active contributions. They deduplicate by
converted object pointer identity, including aliases, and traverse in contribution
order. They are not value-equality sets or automatic subtype scans. A contribution
is a strong dependency; an active contribution with no binding fails planning.

`Plan::produce()` is lazy and repeatable. Every run creates a new graph and calls
each reachable provider once. Instance bindings continue to share the originally
supplied instance across graphs. Null services are provisioning errors. Exceptions
from synchronous providers become `ProvisionError` with the originating exception;
declared failures and defects from an effectful provider retain their respective
channels.

The graph releases its own references in reverse construction order, including
partial-provisioning rollback. External `shared_ptr` owners may extend object
lifetimes; this is RAII ownership, not forced shutdown of escaped services.
Providers must clean up their own partial construction before returning ownership.
Destructors must not throw. Business services receive dependencies, not a locator.

[type.hpp](include/mini/type.hpp) supplies exact in-process tags and a checked
internal erased-value boundary. Templates and cv/ref qualifications remain
distinguishable. Names and RTTI hashes are not persistent IDs; there is no
cross-ABI plugin identity, structural type reflection, or automatic constructor
discovery.

## Verification scope

Verified on 2026-09-20 with GCC 15.3.0: all thirteen CTest cases pass in the
Debug build and in the AddressSanitizer/UndefinedBehaviorSanitizer build.
Both Debug stress executables also pass with a 512 KiB native stack.
Other compilers and platforms have not been verified.

Tests exercise sample monad/bifunctor laws, lazy and repeated execution, typed
errors versus defects, scoped release, stack safety, named bindings, aliases and
multiple-inheritance pointer adjustment, sets, axes, plan validation, graph
ownership, provisioning failure, and the same complete application across two
effect interpreters and two activations. Inferred combinators and traversal are
checked with both IO and native `std::expected`, including empty input, ordering,
short-circuiting, and repeat execution.
Accessor checks cover capability restrictions, supplied dictionary identity,
bottom-channel widening, native expected interop, and rejected channel joins.
Implicit-context checks cover precedence, ambiguity, ADL-based recursive
derivation, independent providers, family validation, and escaped IO ownership.
Coroutine checks cover inferred values, both interpreters, loops, repeatability,
typed short-circuiting, defects, frame/closure ownership, local dictionaries,
and rejected error channels, effect families, and unavailable capabilities.
Stress cases cover 100,000 sequential awaits and 100,000 nested IO coroutine
calls, including frame cleanup on success, typed failure, and defects.

The tests use public contracts: Behavioral-Active / Blackbox-Atomic for IO and
Blackbox-Group for DI/application composition. No external systems or generated
mocks are involved. Law checks are finite examples, not formal proofs.

The original thunk implementation was reproduced failing at 100,000 binds with
AddressSanitizer reporting a stack overflow in recursive `unsafe_run`. The same
case now tests the trampoline, including destruction. The initial design record
is retained under [docs/drafts](docs/drafts/20260919-1741-scala-style-cpp.md).
