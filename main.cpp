#include <fmt/format.h>
#include <fmt/ostream.h>  // for ranges::view:all, which has an operator<< for fmt to use

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <boost/multiprecision/cpp_int.hpp>

// for sha256
#include <sodium.h>

#include <chrono>
#include <string>

static char rippleAlphabet[] =
    "rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz";

// compute the digest of the digest of the message, and put the first four bytes
// in out Why the "digest of the digest"?
void
checksum(void* out, void const* msg, std::size_t size)
{
    unsigned char tmp1[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(tmp1, reinterpret_cast<unsigned char const*>(msg), size);
    unsigned char tmp2[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(tmp2, tmp1, crypto_hash_sha256_BYTES);
    std::memcpy(out, tmp2, 4);
}

namespace ReferenceImpl {
std::string
encodeBase58(
    void const* message,
    std::size_t size,
    void* temp,
    std::size_t temp_size,
    char const* const alphabet)
{
    std::array<unsigned char, 4> cs;
    checksum(cs.data(), message, size);

    // Hack hack hack
    // Overwrite the first four bytes with the checksum
    std::memcpy(const_cast<void*>(message), cs.data(), 4);

    auto pbegin = reinterpret_cast<unsigned char const*>(message);
    auto const pend = pbegin + size;

    // Skip & count leading zeroes.
    int zeroes = 0;
    while (pbegin != pend && *pbegin == 0)
    {
        pbegin++;
        zeroes++;
    }

    auto const b58begin = reinterpret_cast<unsigned char*>(temp);
    auto const b58end = b58begin + temp_size;

    std::fill(b58begin, b58end, 0);

    while (pbegin != pend)
    {
        int carry = *pbegin;
        // Apply "b58 = b58 * 256 + ch".
        for (auto iter = b58end; iter != b58begin; --iter)
        {
            carry += 256 * (iter[-1]);
            iter[-1] = carry % 58;
            carry /= 58;
        }
        assert(carry == 0);
        pbegin++;
    }

    // Skip leading zeroes in base58 result.
    auto iter = b58begin;
    while (iter != b58end && *iter == 0)
        ++iter;

    // Translate the result into a string.
    std::string str;
    str.reserve(zeroes + (b58end - iter));
    str.assign(zeroes, alphabet[0]);
    while (iter != b58end)
        str += alphabet[*(iter++)];
    return str;
}
}  // namespace ReferenceImpl

namespace NewImpl {
std::string
encodeBase58(void const* message, std::size_t size, char const* const alphabet)
{
    using namespace boost::multiprecision;

    if (size > 256 / 8)
    {
        assert(0);
        throw std::runtime_error("Can only encode up to 256 bits");
    }

    std::array<unsigned char, 4> cs;
    checksum(cs.data(), message, size);

    // Hack hack hack
    // Overwrite the first four bytes with the checksum
    std::memcpy(const_cast<void*>(message), cs.data(), 4);

    checked_uint256_t toDecodeMP;
    {
        auto const asU8 = reinterpret_cast<std::uint8_t const*>(message);
        import_bits(toDecodeMP, asU8, asU8 + size, 8, true);
    }

    // 58^10
    std::uint64_t const b5810 = 430804206899405824;
    // log(2^256,58^10) ~= 4.3. So 5 coeff should be enough
    boost::container::static_vector<std::uint64_t, 5> coeff;

    while (toDecodeMP > 0)
    {
        auto const d = toDecodeMP % b5810;
        coeff.emplace_back(0);
        export_bits(d, &coeff.back(), 64);
        toDecodeMP /= b5810;
    }

    // now encode from base58^10 to base58
    // note: we could use avx instructions to do this in parallel
    //       however:
    //       1) avx does not have integer division instructions,
    //       so we'd have to use tricks that convert division to
    //       multiplication.
    //       2) We're currently at at a 2.5x speedup. If the last part of this calculation
    //          took zero time, it gets us to a 2.8x speedup. So the extra complication
    //          isn't worth is.
    //       Note: See www.agner.org/optimize/#vectorclass for a avx class that can do integer
    //             division using multiplication only.

    std::string result;
    // log(2^256,58) ~= 43.7
    result.reserve(44);

    for (auto c : coeff)
    {
        // Do all ten iterations, even when c goes to zero
        // If not, coefficients like {1,0,1} will not encode correctly
        for (int i = 0; i < 10; ++i)
        {
            auto const b58 = c % 58;
            c /= 58;
            result.push_back(alphabet[b58]);
        }
    }

    // Strip off trailing zeros (leading zeros really, but the result is
    // reversed)
    while (!result.empty() && result.back() == alphabet[0])
        result.pop_back();
    // Result is reversed.
    std::reverse(result.begin(), result.end());
    return result;
}
}  // namespace NewImpl

int
main()
{
    using namespace boost::multiprecision;

    // 2^160-1
    static checked_uint256_t const toDecodeMP{
        "1461501637330902918203684832716283019655932542975"};

    std::array<std::uint8_t, 160 / 8> toDecodeBigEndian{};
    auto const exportedEnd =
        export_bits(toDecodeMP, toDecodeBigEndian.begin(), 8, true);
    auto const toDecodeNBytes =
        std::distance(toDecodeBigEndian.begin(), exportedEnd);
    fmt::print("toDecodeNBytes: {}\n", toDecodeNBytes);

    auto const bufsize = toDecodeNBytes * 3;
    boost::container::small_vector<std::uint8_t, 1024> tempBuf(bufsize);

    {
        std::array<std::uint8_t, 160 / 8> from{toDecodeBigEndian};
        auto const referenceEncoded = ReferenceImpl::encodeBase58(
            from.data(),
            toDecodeNBytes,
            tempBuf.data(),
            tempBuf.size(),
            rippleAlphabet);
        fmt::print("Ref: {}\n", referenceEncoded);
    }
    {
        std::array<std::uint8_t, 160 / 8> from{toDecodeBigEndian};
        auto const newEncoded = NewImpl::encodeBase58(
            from.data(), toDecodeNBytes, rippleAlphabet);
        fmt::print("New: {}\n", newEncoded);
    }

    // Get timing numbers
    int const iters = 1000000;

    using clock = std::chrono::high_resolution_clock;
    using time_point = clock::time_point;

    {
        // ref impl
        auto const start = clock::now();
        for (int i = 0; i < iters; ++i)
        {
            auto const referenceEncoded = ReferenceImpl::encodeBase58(
                toDecodeBigEndian.data(),
                toDecodeNBytes,
                tempBuf.data(),
                tempBuf.size(),
                rippleAlphabet);
            // Don't let the optimizer remove the call
            if (referenceEncoded[0] == '%')
                return 1;
        }
        auto const stop = clock::now();
        auto const duration =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                stop - start);
        fmt::print("Ref: {}\n", duration.count());
    }

    {
        // new impl
        auto const start = clock::now();
        for (int i = 0; i < iters; ++i)
        {
            auto const newEncoded = NewImpl::encodeBase58(
                toDecodeBigEndian.data(), toDecodeNBytes, rippleAlphabet);
            // Don't let the optimizer remove the call
            if (newEncoded[0] == '%')
                return 1;
        }
        auto const stop = clock::now();
        auto const duration =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                stop - start);
        fmt::print("New: {}\n", duration.count());
    }

    return 0;
}
