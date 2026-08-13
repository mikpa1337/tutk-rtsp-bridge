#pragma once

#include <cstdint>   // standard sizes
#include <algorithm> // std::min

#include <array>    // std::array 
#include <bit>      // rotr, rotl

#include <string>
#include <string_view>

#include <chrono>
#include <span>
#include <ranges>

#include <functional> // std::bit_xor

// Reimplementation of the byte-scrambling "encryption" TUTK/IOTC cameras use
// to obfuscate packets on the wire. Every IOTC packet is put through this
// before being sent, and reversed on receive - it's a fixed 16-byte block
// permutation + rotation + XOR, no real key exchange involved.
//
// Fully header-only (used to be split into scrambler.hh/.cc) - nothing here
// is large or templated enough on its own to need a translation unit, and
// tests/test_scrambler.cc includes this directly.
namespace Scrambler
{
    constexpr auto TUTK_XOR_KEY = [] {
        std::array<std::uint8_t, 32> key{};
        std::ranges::copy(
            std::string_view{ "Charlie is the designer of P2P!!" },
            key.begin()
        );
        return key;
        }();

    constexpr std::array<int, 16> perm16{
        11, 9,  8,  15,
        13, 10, 12, 14,
        2,  1,  5,  0,
        6,  4,  7,  3
    };

    constexpr std::array<int, 8> perm8{
        7, 4, 3, 2,
        1, 6, 5, 0
    };

    constexpr std::array<int, 4> perm4{
        2, 3, 0, 1
    };

    constexpr std::array<int, 2> perm2{
        1, 0
    };

    template<std::size_t N>
    void permute(const std::uint8_t* src, std::uint8_t* dst, const std::array<int, N>& p) {
        for (std::size_t i = 0; i < N; ++i) {
            dst[i] = src[p[i]];
        }
    }

    inline void MixBytes(const std::uint8_t* src, std::uint8_t* dst, std::size_t size) {
        switch (size) {
        case 16:
            permute(src, dst, perm16);
            break;
        case 8:
            permute(src, dst, perm8);
            break;
        case 4:
            permute(src, dst, perm4);
            break;
        case 2:
            permute(src, dst, perm2);
            break;
        default:
            std::copy_n(src, size, dst);
        }
    }

    template <class Rotate>
    void RotateWords(std::span<const std::uint8_t, 16> src, std::span<std::uint8_t, 16> dst, Rotate rotate) {
        for (int i = 0; i < 4; ++i) {
            auto word = std::bit_cast<std::uint32_t>(
                std::array<std::uint8_t, 4>{
                src[i * 4 + 0],
                    src[i * 4 + 1],
                    src[i * 4 + 2],
                    src[i * 4 + 3]
            });

            word = rotate(word, i);

            auto bytes = std::bit_cast<std::array<std::uint8_t, 4>>(word);
            std::copy(bytes.begin(), bytes.end(), dst.begin() + i * 4);
        }
    }

    inline void xor_key(std::span<const std::uint8_t, 16> src, std::span<std::uint8_t, 16> dst) {
        std::ranges::transform(src, TUTK_XOR_KEY, dst.begin(), std::bit_xor<>{});
    }

    inline void scramble_block(std::span<const std::uint8_t, 16> src, std::span<std::uint8_t, 16> dst) {
        std::array<std::uint8_t, 16> scratch1, scratch2;

        RotateWords(src, scratch1, [](auto w, int i) {
            return std::rotr(w, 1 + i * 4);
            });

        xor_key(scratch1, scratch2);

        MixBytes(scratch2.data(), scratch1.data(), scratch1.size());

        RotateWords(scratch1, dst, [](auto w, int i) {
            return std::rotr(w, 3 + i * 4);
            });
    }

    inline void descramble_block(std::span<const std::uint8_t, 16> src, std::span<std::uint8_t, 16> dst) {
        std::array<std::uint8_t, 16> scratch1, scratch2;

        RotateWords(src, scratch1, [](auto w, int i) {
            return std::rotl(w, 3 + i * 4);
            });

        MixBytes(scratch1.data(), scratch2.data(), scratch2.size());

        xor_key(scratch2, scratch1);

        RotateWords(scratch1, dst, [](auto w, int i) {
            return std::rotl(w, 1 + i * 4);
            });
    }

    // Scrambles `data` for sending on the wire. `len` mirrors the original
    // TUTK API - only the first `len` bytes are transformed, the rest of
    // `out` is left as a plain copy of `in` (hence "Partial").
    inline void TransCodePartial(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::uint16_t len) {
        if (in.empty())
            return;

        std::ranges::copy(in, out.begin());

        const auto target = std::min<std::size_t>(len, in.size());
        std::size_t offset = 0;

        for (; offset + 16 <= target; offset += 16) {
            scramble_block(
                std::span<const std::uint8_t, 16>{in.data() + offset, 16},
                std::span<std::uint8_t, 16>{out.data() + offset, 16}
            );
        }

        const std::size_t remain = target - offset;

        if (remain) {
            std::array<std::uint8_t, 16> scratch{};

            for (std::size_t i = 0; i < remain; ++i) {
                scratch[i] = in[offset + i] ^ TUTK_XOR_KEY[i];
            }

            MixBytes(scratch.data(), out.data() + offset, remain);
        }
    }

    // Descrambles a received buffer. Mirrors TransCodePartial exactly.
    inline void ReverseTransCodePartial(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::uint16_t len) {
        if (in.empty())
            return;

        std::ranges::copy(in, out.begin());

        const auto target = std::min<std::size_t>(len, in.size());
        std::size_t offset = 0;

        for (; offset + 16 <= target; offset += 16) {
            descramble_block(
                std::span<const std::uint8_t, 16>{in.data() + offset, 16},
                std::span<std::uint8_t, 16>{out.data() + offset, 16}
            );
        }

        const std::size_t remain = target - offset;

        if (remain)
        {
            std::array<std::uint8_t, 16> scratch{};

            // mix remaining bytes
            MixBytes(in.data() + offset, scratch.data(), remain);

            // correct xor this time
            for (std::size_t i = 0; i < remain; ++i) {
                scratch[i] ^= TUTK_XOR_KEY[i];
            }

            // copy the tail bytes correctly
            std::copy_n(scratch.data(), remain, out.data() + offset);
        }
    }
}
