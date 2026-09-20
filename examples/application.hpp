#pragma once

#include <mini/di.hpp>
#include <mini/implicit.hpp>

namespace example {

using namespace mini;
namespace di = mini::di;

enum class Mode { live, test };
enum class LookupError { missing_user };
struct UserId { int value; };
struct User { std::string name; };
struct Dataset { std::string name; };

template<template<class, class> class Eff, class A>
using Lookup = Effect<Eff, LookupError, A>;

template<template<class, class> class Eff>
struct Users {
    virtual ~Users() = default;
    virtual Lookup<Eff, User> find(UserId id) const = 0;
};

template<template<class, class> class Eff>
class SeededUsers final : public Users<Eff> {
public:
    explicit SeededUsers(std::shared_ptr<Dataset> data) : data_(std::move(data)) {}
    Lookup<Eff, User> find(UserId id) const override {
        const auto F = bio(implicit_scope<Eff>());
        if (id.value != 1) {
            return F.fail(LookupError::missing_user);
        }
        return F.pure(User{data_->name});
    }

private:
    std::shared_ptr<Dataset> data_;
};

template<template<class, class> class Eff>
struct Greeting {
    virtual ~Greeting() = default;
    virtual Lookup<Eff, std::string> greet(User user) const = 0;
};

template<template<class, class> class Eff>
struct Hello final : Greeting<Eff> {
    Lookup<Eff, std::string> greet(User user) const override {
        const auto F = bio(implicit_scope<Eff>());
        return F.pure("Hello, " + user.name + "!");
    }
};

template<template<class, class> class Eff>
struct Welcome final : Greeting<Eff> {
    Lookup<Eff, std::string> greet(User user) const override {
        const auto F = bio(implicit_scope<Eff>());
        return F.pure("Welcome, " + user.name + ".");
    }
};

template<template<class, class> class Eff>
class Application {
public:
    Application(std::shared_ptr<Users<Eff>> users, std::shared_ptr<di::Set<Greeting<Eff>>> greetings)
        : users_(std::move(users)), greetings_(std::move(greetings)) {}

    auto run(UserId id) const {
        const auto F = bio(implicit_scope<Eff>());
        return F.flat_map(users_->find(id), [F, greetings = greetings_](auto user) {
            return F.traverse(*greetings, [user](const auto& greeting) {
                return greeting->greet(user);
            });
        });
    }

private:
    std::shared_ptr<Users<Eff>> users_;
    std::shared_ptr<di::Set<Greeting<Eff>>> greetings_;
};

template<template<class, class> class F>
di::Module module() {
    using di::key;
    using di::deps;
    using di::when;
    using Greetings = di::Set<Greeting<F>>;
    di::Module module;
    module.provide(key<Dataset>(), deps(), [] {
        return std::make_shared<Dataset>(Dataset{"Ada"});
    }, when(Mode::live));
    module.provide(key<Dataset>(), deps(), [] {
        return std::make_shared<Dataset>(Dataset{"Test Ada"});
    }, when(Mode::test));
    module.bind<SeededUsers<F>>(key<Users<F>>(), deps(key<Dataset>()), when());
    module.bind<Hello<F>>(deps(), when());
    module.bind<Welcome<F>>(deps(), when());
    module.alias(key<Greeting<F>>("hello"), key<Hello<F>>(), when());
    module.add(key<Greetings>(), key<Greeting<F>>("hello"), when());
    module.add(key<Greetings>(), key<Welcome<F>>(), when(Mode::test));
    module.bind<Application<F>>(deps(key<Users<F>>(), key<Greetings>()), when());
    return module;
}

}
