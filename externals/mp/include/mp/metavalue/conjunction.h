/* This file is part of the mp project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <mp/metavalue/logic_if.h>
#include <mp/metavalue/value.h>

namespace mp {

namespace detail {

template <class...>
struct conjunction_impl;

template <>
struct conjunction_impl<> {
    using type = false_type;
};

template <class V>
struct conjunction_impl<V> {
    using type = V;
};

template <class V1, class... Vs>
struct conjunction_impl<V1, Vs...> {
    using type = logic_if<V1, typename conjunction_impl<Vs...>::type, V1>;
};

} // namespace detail

/// Conjunction of metavalues Vs with short-circuiting and type preservation.
template <class... Vs>
using conjunction = typename detail::conjunction_impl<Vs...>::type;

/// Conjunction of metavalues Vs with short-circuiting and type preservation.
template <class... Vs>
constexpr auto conjunction_v = conjunction<Vs...>::value;

} // namespace mp
