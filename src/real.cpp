// Copyright 2016-2019 Francesco Biscani (bluescarni@gmail.com)
//
// This file is part of the mp++ library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#include <mp++/config.hpp>
#include <mp++/detail/gmp.hpp>
#include <mp++/detail/mpfr.hpp>
#include <mp++/detail/utils.hpp>
#include <mp++/integer.hpp>
#include <mp++/real.hpp>
#if defined(MPPP_WITH_QUADMATH)
#include <mp++/real128.hpp>
#endif

namespace mppp
{

namespace detail
{

namespace
{

// Some misc tests to check that the mpfr struct conforms to our expectations.
struct expected_mpfr_struct_t {
    ::mpfr_prec_t _mpfr_prec;
    ::mpfr_sign_t _mpfr_sign;
    ::mpfr_exp_t _mpfr_exp;
    ::mp_limb_t *_mpfr_d;
};

static_assert(sizeof(expected_mpfr_struct_t) == sizeof(mpfr_struct_t) && offsetof(mpfr_struct_t, _mpfr_prec) == 0u
                  && offsetof(mpfr_struct_t, _mpfr_sign) == offsetof(expected_mpfr_struct_t, _mpfr_sign)
                  && offsetof(mpfr_struct_t, _mpfr_exp) == offsetof(expected_mpfr_struct_t, _mpfr_exp)
                  && offsetof(mpfr_struct_t, _mpfr_d) == offsetof(expected_mpfr_struct_t, _mpfr_d)
                  && std::is_same<::mp_limb_t *, decltype(std::declval<mpfr_struct_t>()._mpfr_d)>::value,
              "Invalid mpfr_t struct layout and/or MPFR types.");

#if MPPP_CPLUSPLUS >= 201703L

// If we have C++17, we can use structured bindings to test the layout of mpfr_struct_t
// and its members' types.
constexpr void test_mpfr_struct_t()
{
    auto [prec, sign, exp, ptr] = mpfr_struct_t{};
    static_assert(std::is_same<decltype(ptr), ::mp_limb_t *>::value);
    ignore(prec, sign, exp, ptr);
}

#endif

} // namespace

void mpfr_to_stream(const ::mpfr_t r, std::ostream &os, int base)
{
    // All chars potentially used by MPFR for representing the digits up to base 62, sorted.
    constexpr char all_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    // Check the base.
    if (mppp_unlikely(base < 2 || base > 62)) {
        throw std::invalid_argument("Cannot convert a real to a string in base " + to_string(base)
                                    + ": the base must be in the [2,62] range");
    }
    // Special values first.
    if (mpfr_nan_p(r)) {
        // NOTE: up to base 16 we can use nan, inf, etc., but with larger
        // bases we have to use the syntax with @.
        os << (base <= 16 ? "nan" : "@nan@");
        return;
    }
    if (mpfr_inf_p(r)) {
        if (mpfr_sgn(r) < 0) {
            os << '-';
        }
        os << (base <= 16 ? "inf" : "@inf@");
        return;
    }

    // Get the string fractional representation via the MPFR function,
    // and wrap it into a smart pointer.
    ::mpfr_exp_t exp(0);
    std::unique_ptr<char, void (*)(char *)> str(::mpfr_get_str(nullptr, &exp, base, 0, r, MPFR_RNDN), ::mpfr_free_str);
    // LCOV_EXCL_START
    if (mppp_unlikely(!str)) {
        throw std::runtime_error("Error in the conversion of a real to string: the call to mpfr_get_str() failed");
    }
    // LCOV_EXCL_STOP

    // Print the string, inserting a decimal point after the first digit.
    bool dot_added = false;
    for (auto cptr = str.get(); *cptr != '\0'; ++cptr) {
        os << (*cptr);
        if (!dot_added) {
            if (base <= 10) {
                // For bases up to 10, we can use the followig guarantee
                // from the standard:
                // http://stackoverflow.com/questions/13827180/char-ascii-relation
                // """
                // The mapping of integer values for characters does have one guarantee given
                // by the Standard: the values of the decimal digits are contiguous.
                // (i.e., '1' - '0' == 1, ... '9' - '0' == 9)
                // """
                // http://eel.is/c++draft/lex.charset#3
                if (*cptr >= '0' && *cptr <= '9') {
                    os << '.';
                    dot_added = true;
                }
            } else {
                // For bases larger than 10, we do a binary search among all the allowed characters.
                // NOTE: we need to search into the whole all_chars array (instead of just up to all_chars
                // + base) because apparently mpfr_get_str() seems to ignore lower/upper case when the base
                // is small enough (e.g., it uses 'a' instead of 'A' when printing in base 11).
                // NOTE: the range needs to be sizeof() - 1 because sizeof() also includes the terminator.
                if (std::binary_search(all_chars, all_chars + (sizeof(all_chars) - 1u), *cptr)) {
                    os << '.';
                    dot_added = true;
                }
            }
        }
    }
    assert(dot_added);

    // Adjust the exponent. Do it in multiprec in order to avoid potential overflow.
    integer<1> z_exp{exp};
    --z_exp;
    const auto exp_sgn = z_exp.sgn();
    if (exp_sgn && !mpfr_zero_p(r)) {
        // Add the exponent at the end of the string, if both the value and the exponent
        // are nonzero.
        // NOTE: for bases greater than 10 we need '@' for the exponent, rather than 'e' or 'E'.
        // https://www.mpfr.org/mpfr-current/mpfr.html#Assignment-Functions
        os << (base <= 10 ? 'e' : '@');
        if (exp_sgn == 1) {
            // Add extra '+' if the exponent is positive, for consistency with
            // real128's string format (and possibly other formats too?).
            os << '+';
        }
        os << z_exp;
    }
}

// NOTE: the use of ATOMIC_VAR_INIT ensures that the initialisation of default_prec
// is constant initialisation:
//
// http://en.cppreference.com/w/cpp/atomic/ATOMIC_VAR_INIT
//
// This essentially means that this initialisation happens before other types of
// static initialisation:
//
// http://en.cppreference.com/w/cpp/language/initialization
//
// This ensures that static reals, which are subject to dynamic initialization, are initialised
// when this variable has already been constructed, and thus access to it will be safe.
std::atomic<::mpfr_prec_t> real_default_prec = ATOMIC_VAR_INIT(::mpfr_prec_t(0));

} // namespace detail

#if defined(MPPP_WITH_QUADMATH)

void real::assign_real128(const real128 &x)
{
    // Get the IEEE repr. of x.
    const auto t = x.get_ieee();
    // A utility function to write the significand of x
    // as a big integer inside m_mpfr.
    auto write_significand = [this, &t]() {
        // The 4 32-bits part of the significand, from most to least
        // significant digits.
        const auto p1 = std::get<2>(t) >> 32;
        const auto p2 = std::get<2>(t) % (1ull << 32);
        const auto p3 = std::get<3>(t) >> 32;
        const auto p4 = std::get<3>(t) % (1ull << 32);
        // Build the significand, from most to least significant.
        // NOTE: unsigned long is guaranteed to be at least 32 bit.
        ::mpfr_set_ui(&this->m_mpfr, static_cast<unsigned long>(p1), MPFR_RNDN);
        ::mpfr_mul_2ui(&this->m_mpfr, &this->m_mpfr, 32ul, MPFR_RNDN);
        ::mpfr_add_ui(&this->m_mpfr, &this->m_mpfr, static_cast<unsigned long>(p2), MPFR_RNDN);
        ::mpfr_mul_2ui(&this->m_mpfr, &this->m_mpfr, 32ul, MPFR_RNDN);
        ::mpfr_add_ui(&this->m_mpfr, &this->m_mpfr, static_cast<unsigned long>(p3), MPFR_RNDN);
        ::mpfr_mul_2ui(&this->m_mpfr, &this->m_mpfr, 32ul, MPFR_RNDN);
        ::mpfr_add_ui(&this->m_mpfr, &this->m_mpfr, static_cast<unsigned long>(p4), MPFR_RNDN);
    };
    // Check if the significand is zero.
    const bool sig_zero = !std::get<2>(t) && !std::get<3>(t);
    if (std::get<1>(t) == 0u) {
        // Zero or subnormal numbers.
        if (sig_zero) {
            // Zero.
            ::mpfr_set_zero(&m_mpfr, 1);
        } else {
            // Subnormal.
            write_significand();
            ::mpfr_div_2ui(&m_mpfr, &m_mpfr, 16382ul + 112ul, MPFR_RNDN);
        }
    } else if (std::get<1>(t) == 32767u) {
        // NaN or inf.
        if (sig_zero) {
            // inf.
            ::mpfr_set_inf(&m_mpfr, 1);
        } else {
            // NaN.
            ::mpfr_set_nan(&m_mpfr);
        }
    } else {
        // Write the significand into this.
        write_significand();
        // Add the hidden bit on top.
        const auto r_2_112 = detail::real_constants<>::get_2_112();
        ::mpfr_add(&m_mpfr, &m_mpfr, &r_2_112.first, MPFR_RNDN);
        // Multiply by 2 raised to the adjusted exponent.
        ::mpfr_mul_2si(&m_mpfr, &m_mpfr, static_cast<long>(std::get<1>(t)) - (16383l + 112), MPFR_RNDN);
    }
    if (std::get<0>(t)) {
        // Negate if the sign bit is set.
        ::mpfr_neg(&m_mpfr, &m_mpfr, MPFR_RNDN);
    }
}

real128 real::convert_to_real128() const
{
    // Handle the special cases first.
    if (nan_p()) {
        return real128_nan();
    }
    // NOTE: the number 2**18 = 262144 is chosen so that it's amply outside the exponent
    // range of real128 (a 15 bit value with some offset) but well within the
    // range of long (around +-2**31 guaranteed by the standard).
    //
    // NOTE: the reason why we do these checks with large positive and negative exponents
    // is that they ensure we can safely convert _mpfr_exp to long later.
    if (inf_p() || m_mpfr._mpfr_exp > (1l << 18)) {
        return sgn() > 0 ? real128_inf() : -real128_inf();
    }
    if (zero_p() || m_mpfr._mpfr_exp < -(1l << 18)) {
        // Preserve the signedness of zero.
        return signbit() ? -real128{} : real128{};
    }
    // NOTE: this is similar to the code in real128.hpp for the constructor from integer,
    // with some modification due to the different padding in MPFR vs GMP (see below).
    const auto prec = get_prec();
    // Number of limbs in this.
    auto nlimbs = prec / ::mp_bits_per_limb + static_cast<bool>(prec % ::mp_bits_per_limb);
    assert(nlimbs != 0);
    // NOTE: in MPFR the most significant (nonzero) bit of the significand
    // is always at the high end of the most significand limb. In other words,
    // MPFR pads the multiprecision significand on the right, the opposite
    // of GMP integers (which have padding on the left, i.e., in the top limb).
    //
    // NOTE: contrary to real128, the MPFR format does not have a hidden bit on top.
    //
    // Init retval with the highest limb.
    //
    // NOTE: MPFR does not support nail builds in GMP, so we don't have to worry about that.
    real128 retval{m_mpfr._mpfr_d[--nlimbs]};
    // Init the number of read bits.
    // NOTE: we have read a full limb in the line above, so mp_bits_per_limb bits. If mp_bits_per_limb > 113,
    // then the constructor of real128 truncated the input limb value to 113 bits of precision, so effectively
    // we have read 113 bits only in such a case.
    unsigned read_bits = detail::c_min(static_cast<unsigned>(::mp_bits_per_limb), real128_sig_digits());
    while (nlimbs && read_bits < real128_sig_digits()) {
        // Number of bits to be read from the current limb. Either mp_bits_per_limb or less.
        const unsigned rbits
            = detail::c_min(static_cast<unsigned>(::mp_bits_per_limb), real128_sig_digits() - read_bits);
        // Shift up by rbits.
        // NOTE: cast to int is safe, as rbits is no larger than mp_bits_per_limb which is
        // representable by int.
        retval = scalbn(retval, static_cast<int>(rbits));
        // Add the next limb, removing lower bits if they are not to be read.
        retval += m_mpfr._mpfr_d[--nlimbs] >> (static_cast<unsigned>(::mp_bits_per_limb) - rbits);
        // Update the number of read bits.
        // NOTE: due to the definition of rbits, read_bits can never reach past real128_sig_digits().
        // Hence, this addition can never overflow (as sig_digits is unsigned itself).
        read_bits += rbits;
    }
    // NOTE: from earlier we know the exponent is well within the range of long, and read_bits
    // cannot be larger than 113.
    retval = scalbln(retval, static_cast<long>(m_mpfr._mpfr_exp) - static_cast<long>(read_bits));
    return sgn() > 0 ? retval : -retval;
}

#endif

} // namespace mppp