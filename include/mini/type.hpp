#pragma once

#include <memory>
#include <stdexcept>
#include <typeindex>
#include <utility>

namespace mini {

template<class T>
struct TypeToken {};

template<class T>
std::type_index type_tag() { return typeid(TypeToken<T>); }

template<template<class, class> class F>
struct Kind2Tag {};

template<class T, class Qualifier>
struct Key {};

namespace detail {

class Value {
public:
    template<class T>
    static Value of(T value) {
        return Value(type_tag<T>(), std::make_shared<const T>(std::move(value)));
    }

    template<class T>
    const T& get() const {
        if (type_ != type_tag<T>() || !storage_) {
            throw std::logic_error("erased value type invariant violated");
        }
        return *static_cast<const T*>(storage_.get());
    }

private:
    Value(std::type_index type, std::shared_ptr<const void> storage)
        : type_(type), storage_(std::move(storage)) {}

    std::type_index type_;
    std::shared_ptr<const void> storage_;
};

}
}
