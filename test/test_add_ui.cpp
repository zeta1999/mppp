/* Copyright 2009-2016 Francesco Biscani (bluescarni@gmail.com)

This file is part of the mp++ library.

The mp++ library is free software; you can redistribute it and/or modify
it under the terms of either:

  * the GNU Lesser General Public License as published by the Free
    Software Foundation; either version 3 of the License, or (at your
    option) any later version.

or

  * the GNU General Public License as published by the Free Software
    Foundation; either version 3 of the License, or (at your option) any
    later version.

or both in parallel, as here.

The mp++ library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received copies of the GNU General Public License and the
GNU Lesser General Public License along with the mp++ library.  If not,
see https://www.gnu.org/licenses/. */

#include <cstddef>
#include <gmp.h>
#include <limits>
#include <random>
#include <tuple>
#include <type_traits>

#include <mp++.hpp>

#include "test_utils.hpp"

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

static int ntries = 1000;

using namespace mppp;
using namespace mppp::mppp_impl;
using namespace mppp_test;

using sizes = std::tuple<std::integral_constant<std::size_t, 1>, std::integral_constant<std::size_t, 2>,
                         std::integral_constant<std::size_t, 3>, std::integral_constant<std::size_t, 6>,
                         std::integral_constant<std::size_t, 10>>;

static std::mt19937 rng;

struct add_ui_tester {
    template <typename S>
    inline void operator()(const S &) const
    {
        using integer = mp_integer<S::value>;
        // Start with all zeroes.
        mpz_raii m1, m2;
        integer n1, n2;
        add_ui(n1, n2, 0);
        ::mpz_add_ui(&m1.m_mpz, &m2.m_mpz, 0);
        REQUIRE((lex_cast(n1) == lex_cast(m1)));
        REQUIRE(n1.is_static());
        // Ones and zeroes.
        add_ui(n1, n2, 1);
        ::mpz_add_ui(&m1.m_mpz, &m2.m_mpz, 1);
        REQUIRE((lex_cast(n1) == lex_cast(m1)));
        REQUIRE(n1.is_static());
        n2 = integer{1};
        ::mpz_set_si(&m2.m_mpz, 1);
        add_ui(n1, n2, 0);
        ::mpz_add_ui(&m1.m_mpz, &m2.m_mpz, 0);
        REQUIRE((lex_cast(n1) == lex_cast(m1)));
        REQUIRE(n1.is_static());
        n2 = integer{-1};
        ::mpz_set_si(&m2.m_mpz, -1);
        add_ui(n1, n2, 0);
        ::mpz_add_ui(&m1.m_mpz, &m2.m_mpz, 0);
        REQUIRE((lex_cast(n1) == lex_cast(m1)));
        REQUIRE(n1.is_static());
        add_ui(n1, n2, 1);
        ::mpz_add_ui(&m1.m_mpz, &m2.m_mpz, 1);
        REQUIRE((lex_cast(n1) == lex_cast(m1)));
        REQUIRE(n1.is_static());
        add_ui(n1, n2, 123);
        ::mpz_add_ui(&m1.m_mpz, &m2.m_mpz, 123);
        REQUIRE((lex_cast(n1) == lex_cast(m1)));
        REQUIRE(n1.is_static());
        mpz_raii tmp;
        std::uniform_int_distribution<int> sdist(0, 1);
        std::uniform_int_distribution<unsigned long> uldist(0ull, std::numeric_limits<unsigned long>::max());
        // Run a variety of tests with operands with x number of limbs.
        auto random_xy = [&](unsigned x) {
            for (int i = 0; i < ntries; ++i) {
                if (sdist(rng) && sdist(rng) && sdist(rng)) {
                    // Reset rop every once in a while.
                    n1 = integer{};
                }
                random_integer(tmp, x, rng);
                ::mpz_set(&m2.m_mpz, &tmp.m_mpz);
                n2 = integer(mpz_to_str(&tmp.m_mpz));
                if (sdist(rng)) {
                    ::mpz_neg(&m2.m_mpz, &m2.m_mpz);
                    n2.neg();
                }
                if (n2.is_static() && sdist(rng)) {
                    // Promote sometimes, if possible.
                    n2.promote();
                }
                auto rul = uldist(rng);
                add_ui(n1, n2, rul);
                ::mpz_add_ui(&m1.m_mpz, &m2.m_mpz, rul);
                REQUIRE((lex_cast(n1) == lex_cast(m1)));
                // Overlap.
                add_ui(n2, n2, rul);
                ::mpz_add_ui(&m2.m_mpz, &m2.m_mpz, rul);
                REQUIRE((lex_cast(n2) == lex_cast(m2)));
                REQUIRE((lex_cast(n2) == lex_cast(n1)));
                if (S::value == 2 && x == 2) {
                    // Tests specific for 2-limb optimisation.
                    n2 = integer{GMP_NUMB_MAX};
                    mul_2exp(n2,n2,GMP_NUMB_BITS);
                    add(n2,n2,integer{GMP_NUMB_MAX});
                    ::mpz_set(&m2.m_mpz, n2.get_mpz_view());
                    add_ui(n1, n2, 1);
                    ::mpz_add_ui(&m1.m_mpz, &m2.m_mpz, 1);
                    REQUIRE((lex_cast(n1) == lex_cast(m1)));
                }
            }
        };

        random_xy(0);
        random_xy(1);
        random_xy(2);
        random_xy(3);
        random_xy(4);
    }
};

TEST_CASE("add_ui")
{
    tuple_for_each(sizes{}, add_ui_tester{});
}