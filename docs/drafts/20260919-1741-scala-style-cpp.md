# Scala-style C++: a small BIO, IO, and staged DI design

Status: historical initial design. The [implemented stack](../../README.md)
supersedes this document's thunk runtime and proposed DI sections.
Baseline: C++23, exceptions and RTTI enabled, standard library only.

The useful target is Scala's programming model: typed effects, explicit
capabilities, effect-polymorphic services, and a planned object graph.
C++ can express those. Ownership, variance, reflection, and execution semantics
need deliberate C++ designs; translating names and method signatures is not
enough.

The initial header and executable have become the
[BIO header](../../include/mini/bio.hpp) and [contract checks](../../tests/bio_test.cpp).
The DI planner, structured type descriptors, resources, and fiber runtime below
are proposals, not implemented components.

## 1. Translation map

| Scala concept | C++ representation | Boundary |
|---|---|---|
| `trait Show[A]` plus an instance | Dictionary class or class-template specialization | Concepts check expressions; dictionaries supply operations |
| `F[_, _]` | `template<class, class> class F` | Sufficient for this encoding; not Scala's general higher-kinded type system |
| `F[E, A]` | `F<E, A>` | No declaration-site covariance |
| `Either[E, A]` | `std::expected<A, E>` | An already computed result, with reversed parameter order |
| `IO[E, A]` | Lazy program plus interpreter | Reusable description, not a started task |
| `Unit` | `std::monostate` or a named `Unit` | Prefer a regular value to special-casing `void` in combinators |
| `Nothing` | A dedicated no-error representation | C++ has no equivalent universal bottom type |
| `Tag[A]` | Type identity plus optional structural metadata | RTTI identity does not supply reflection |
| `TagKK[F]` | A nominal witness for an effect family | The witness must have a canonical identity |
| Tagless final | Templates over the effect and its operation dictionaries | Select the effect at instantiation time |
| distage module | Binding descriptions, graph planner, provisioner | Planning must precede acquisition |

## 2. Typeclasses and higher-kinded encoding

The prototype takes the direct encoding:

```cpp
template<template<class, class> class F>
struct Monad2;

template<template<class, class> class F>
struct Bifunctor2;

template<template<class, class> class F>
struct Error2;

template<class E, class A>
using Result = std::expected<A, E>;
```

Specializations provide operations for `IO` and `Result`. They are compile-time
dictionaries. Static methods here contain no global state. Any scheduler, clock,
allocator, or other runtime dependency must be passed through an instance or an
explicit runtime context when those capabilities are added.

For user-selectable instances, pass a dictionary value explicitly, analogous to
Scala's `using`. This permits two interpretations of the same type in one
program. A global specialization is appropriate only for a canonical instance;
competing specializations across translation units are not an instance-selection
mechanism. Concepts can check that particular operations are available, but
cannot establish the monad laws or universally check every `E`, `A`, and `B`.

An alternative encoding packages the constructor in a normal type:

```cpp
struct IOK {
    template<class E, class A>
    using Apply = IO<E, A>;
};
```

That representation makes partial application and associated metadata easier.
Start with the direct encoding used in the example; introduce a family wrapper
when environment fixing or associated types actually require it.

A runtime interface cannot have a virtual `flat_map<E,A,B>` member template.
C++ prohibits virtual member function templates. Keep that quantification at
compile time; perform runtime dispatch after fixing the types.
[C++ member template rules](https://eel.is/c++draft/temp.mem).

## 3. BIO contracts

BIO supplies operations over a bifunctor effect, independently of its concrete
runtime. Its documented focus includes typed errors and tagless-final programs.
The C++ draft follows that division, without claiming source or API compatibility.
[BIO documentation](https://izumi.7mind.io/bio/index.html).

The core operations have these shapes:

```text
pure:      A                                 -> F<E, A>
map:       F<E, A> × (A -> B)                 -> F<E, B>
bimap:     F<E, A> × (E -> E2) × (A -> B)     -> F<E2, B>
flat_map:  F<E, A> × (A -> F<E, B>)           -> F<E, B>
fail:      E                                 -> F<E, A>
catch_all: F<E, A> × (E -> F<E2, A>)          -> F<E2, A>
```

Two type parameters alone do not make something a bifunctor: `bimap` must obey
identity and composition. For each fixed `E`, `pure` and `flat_map` must obey the
monad laws, under observational equality and pure continuation construction.
Finite executable checks are evidence on sample cases, not proofs of those laws.

The draft fixes `E` across `flat_map`. C++ does not automatically widen
`IO<SpecificError, A>` to `IO<GeneralError, A>`. Convert errors explicitly before
binding; use a named error sum backed by `std::variant` when several domains must
compose. Do not silently erase them into strings or `exception_ptr`.

For production, `pure` should return a no-error effect, with a deliberate
`widen_error<E>` operation. A deleted default constructor is not a substitute for
Scala's `Nothing` subtyping. This prototype instead requires an explicit error
type at construction, keeping the representation ordinary and checkable.

Split further capabilities according to what programs need:

- `Monad2`: pure composition.
- `Bifunctor2` and `Error2`: error mapping, construction, recovery.
- `Sync2`: suspend synchronous work, with explicit exception classification.
- `Bracket2`: scoped acquisition and release.
- `Async2`, `Concurrent2`, `Temporal2`: asynchronous completion, fibers, time.

These are proposed capability boundaries, not a reproduction of BIO's complete
inheritance hierarchy. An eager `Result` instance is useful for pure programs;
it cannot honestly implement lazy suspension, cancellation, or asynchronous IO.

## 4. IO semantics and the runtime boundary

ZIO describes a computation with environment, declared error, and success types.
Execution belongs to its runtime; the full runtime also supports fibers and
interruption. The small C++ experiment isolates its binary error/success shape.
[ZIO's core model](https://zio.dev/reference/core/zio/).

The executable's representation is deliberately small:

```text
IO<E, A> = a deferred std::function<std::expected<A, E>()>
```

Its contract is:

1. `defer` and composition do not execute the supplied computation.
2. Each `unsafe_run` executes the description again. It does not memoize.
3. A declared failure skips success continuations.
4. `catch_all` handles declared errors only. C++ exceptions escape as defects.
5. `pure(value)` captures an already evaluated value. Use `defer` for work;
   ordinary C++ arguments are eagerly evaluated.

The implementation requires copyable error and success values and copyable
callables; use `Unit` instead of `void`. `map`/`bimap` materialize owned values
from callback results, including callbacks such as `std::identity` that return
references. This does not extend the lifetime of pointers, views, or
`reference_wrapper` values supplied by the caller. Moved-from descriptions are
only for destruction or assignment. Concurrent execution is outside the contract.

Nested thunks are **not stack-safe**: executing a long bind chain recursively
calls earlier thunks. Copying and destroying a deep chain can also recurse and
incur substantial cost. The prototype has no performance or bounded-stack claim.

Before calling the result a ZIO-like runtime, replace that representation with
immutable program nodes and an iterative interpreter with explicit continuation
and finalizer stacks. Any erased intermediate values must carry type witnesses;
keep that erasure inside the interpreter. Account for node reclamation as well
as evaluation when checking stack usage.

A proposed production exit model is:

```text
Exit<E, A> = Success(A) | Failure(Cause<E>)
Cause<E>  = Fail(E) | Die(exception_ptr) | Interrupted(FiberId)
          | Then(Cause<E>, Cause<E>) | Both(Cause<E>, Cause<E>)
```

The compound causes preserve use-and-release failures and independent parallel
failures. Catch exceptions at interpreter boundaries as `Die`; use explicit
boundary adapters for exceptions intentionally translated into declared `E`.
This can represent thrown C++ exceptions, not undefined behavior or process
termination.

Fibers then need an explicit scheduler, exactly-once asynchronous completion,
cooperative interruption, interruption masking around acquisition/finalizer
registration, and parent scopes that cancel and await children. Coroutines can
help implement continuations, but `co_await` alone supplies none of these
policies. Keep coroutine syntax as a later frontend, after specifying whether
a coroutine represents a reusable description or one execution.

## 5. Tagless final with runtime-swappable services

The executable contains this interface:

```cpp
template<template<class, class> class F>
struct Users {
    virtual ~Users() = default;
    virtual F<LookupError, User> find(UserId id) const = 0;
};
```

`Users<IO>` has ordinary virtual methods with concrete return types. Its
implementation can therefore be selected at runtime. Selecting `IO` versus
`Result` instantiates a different service type.

The effect-polymorphic program is:

```cpp
template<template<class, class> class F>
F<LookupError, std::string> manager_name(
    std::shared_ptr<const Users<F>> users, UserId id
) {
    return Monad2<F>::flat_map(users->find(id), [users](User user) {
        return map<F>(users->find(user.manager), [](User manager) {
            return manager.name;
        });
    });
}
```

The same body executes with lazy IO and eager Result. The non-null service
pointer is a precondition. Capturing ownership keeps the service alive when the
returned IO outlives the caller's local pointer. Capturing a borrowed `this` or
stack reference would require a different, explicit lifetime precondition.
`shared_ptr` protects object lifetime, not the validity of a resource after its
scope closes.

For an environment-bearing effect, the conceptual starting point is
`R -> IO<E,A>`. A family wrapper can fix `R` to expose `Apply<E,A>` to BIO.
Environment union, intersection, and automatic service extraction need their
own encoding. Constructor injection lets the initial tagless-final design avoid
those requirements.

## 6. RTTI, type tags, and reflection

For exact, in-process type identity the example uses:

```cpp
template<class T> struct TypeToken {};

template<class T>
std::type_index type_tag() { return typeid(TypeToken<T>); }
```

Ordinary `typeid(T)` removes top-level cv-qualification and treats reference
types as their referred-to types. The wrapper makes those distinctions part of
the template specialization. It also distinguishes concrete generic arguments.
[C++ typeid rules](https://eel.is/c++draft/expr.typeid).

This is enough to distinguish `Users<IO>` from `Users<Result>` and
`Key<Service, Primary>` from `Key<Service, Replica>`. Decide separately whether
DI keys deliberately normalize cv/ref qualifiers; normalize consistently at the
binding boundary, rather than relying on incidental RTTI behavior.

RTTI does not let us enumerate a constructor's parameters, recover a type's
template arguments as a traversable tree, or ask arbitrary subtype questions
from two `type_index` values. Neither `name()` nor `hash_code()` is a persistent
type identifier. Compare identities, not hash values or diagnostic strings.
[C++ type information contract](https://eel.is/c++draft/type.info).

If structural tags are required, add registered descriptors containing nominal
identity, a display name, type arguments, and explicitly supported relationships.
Use template specializations to derive descriptors for known type constructors.
Alias types have no independent nominal identity; use wrapper structs when
distinct domain types are intended.

For `TagKK`, the executable uses `Kind2Tag<F>`. For a broader registry, assign a
canonical nominal family witness such as `IOK`; do not assume independently
written alias templates normalize to the same family tag. The initial scope is
one executable. Stable plugin or serialized identities require an explicit ID
and ABI/versioning scheme, with collision and compatibility checks.

## 7. A small distage-style port

distage describes bindings, plans the graph before constructing objects, and
provisions it through a lifecycle. Its tagless-final support fits the separation
above. Those are the architectural properties to preserve.
[distage's planning and provisioning model](https://izumi.7mind.io/distage/basics.html).

Proposed flow:

```text
Module + Roots + Activation
            |
         Planner ----> diagnostic errors, without invoking providers
            |
           Plan
            |
        Provisioner ----> Resource<ObjectGraph>
```

Use these boundaries:

| Component | Responsibility |
|---|---|
| `Key` | Exact service type plus a qualifier |
| `Binding` | Output key, dependency keys, provider, activation tags, source location |
| `Module` | Explicit collection of binding descriptions; no global registration |
| `Planner` | Select bindings, traverse roots, validate dependencies, produce an ordered plan |
| `Provisioner` | Execute providers once per key per graph; own acquisition rollback |
| `Resource<ObjectGraph>` | Define the use scope and observable release outcome |

C++23 RTTI does not provide automatic constructor discovery. Initially use
explicit typed provider functions, deriving their dependency list from their
signature. An ordinary function or non-generic lambda is enough; overloaded or
generic providers require an explicit signature. This repeats constructor
arguments when forwarding to a constructor, a concrete departure from the Scala
macro-based ergonomics.

An alternative is a `Dependencies<...>` declaration adjacent to the constructor
and a generic constructor provider checked with `std::constructible_from`.
Choose one convention in the implementation. Do not pretend that either fully
reflects arbitrary constructors; code generation can remove the repetition later.

The minimum planner should reject duplicate selected bindings, missing reachable
dependencies, and cycles before invoking any provider. Exclude unreachable
bindings from provisioning. Bind subtype-to-interface conversions explicitly;
perform the proper C++ upcast before erasing storage, including pointer adjustment
under multiple inheritance. Keep typed retrieval and checked erased storage
inside the container. Services receive constructor dependencies, not a locator.

For an initial activation policy, use explicit axis/value tags. Among compatible
bindings for a key, a strict superset of tags may override a less specific
candidate. Incomparable candidates are an ambiguity error. Never resolve a tie
by registration order. This is a proposed restricted policy, not a claim of
complete distage activation compatibility.

Represent planning and acquisition failures as separate error types. Provision
dependencies before dependents; release successful acquisitions in reverse order
on both normal exit and partial failure. Register each finalizer immediately
after acquisition. If use and release both fail, retain both causes. A resource
that fails partway through its own acquisition is responsible for its partial
allocation until it returns ownership.

Use C++ RAII for memory and local non-throwing cleanup. Effectful, asynchronous,
or fallible release needs explicit `Resource::use`/scope operations: a destructor
cannot await an effect or report a typed release failure. The public API must
specify whether scoped handles can escape; unrestricted C++ references cannot
enforce a no-escape rule. Runtime validity checks or a stricter ownership API are
needed before promising safety for effects that outlive their resource scope.

## 8. Checks and next implementation boundaries

Historical build command for the original filenames; use the current README
for the implemented stack:

```sh
mkdir -p build
g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -O0 -g \
    examples/bio_draft.cpp -o build/bio_draft
./build/bio_draft
```

Verified on 2026-09-19 with GCC 15.3.0: the command above and a separate
`-O1 -fsanitize=address,undefined -fno-omit-frame-pointer` build both print
`BIO draft checks passed` and exit successfully. Other toolchains were not tested.

The checks exercise representative monad and bifunctor laws, laziness, repeated
execution, failure short-circuiting, error-type mapping and recovery, defect
propagation, interpreter substitution, retained service ownership, and exact
type tags. They use public interfaces and no external services. Taxonomy:
Behavioral-Active / Blackbox / Atomic for combinators; Group for the service
program. Origin: Specified.

The first compilation exposed a reference-result type error in `bimap` with
`std::identity`: it attempted `std::expected<int&&, std::string&&>`. Materializing
owned result types with `std::remove_cvref_t` addresses that failure; the
bifunctor identity check retains the case.

Further work has separate acceptance criteria:

1. **Iterative synchronous runtime:** deep bind chains run and are destroyed
   under a fixed stack limit; declared errors and defects remain distinct.
2. **Resources:** release exactly once after successful acquisition; reverse
   dependency order; acquisition rollback; preservation of release failures.
3. **Staged DI:** missing/duplicate/cyclic plans fail with zero provider calls;
   unreachable providers never execute; shared dependencies initialize once.
4. **Fibers:** deterministic scheduler tests cover completion/interruption races,
   callback cancellation, parent-child lifetime, and finalizer masking.

Do not evaluate the runtime's safety or performance from the thunk experiment.
Its purpose is to make the C++ type encoding and service composition reviewable.
