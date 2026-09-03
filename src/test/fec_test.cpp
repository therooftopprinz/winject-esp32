#include "fec.h"

#include <gtest/gtest.h>
#include <string.h>
#include <vector>

namespace
{
std::vector<uint8_t> pkt(size_t n, uint8_t seed)
{
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; i++)
    {
        out[i] = static_cast<uint8_t>(seed + i);
    }
    return out;
}

std::unordered_map<int, std::vector<uint8_t>> frags_except(
    const std::vector<std::vector<uint8_t>>& air, const std::vector<int>& drop)
{
    std::unordered_map<int, std::vector<uint8_t>> out;
    for (size_t i = 0; i < air.size(); i++)
    {
        bool skip = false;
        for (int d : drop)
        {
            if (static_cast<int>(i) == d)
            {
                skip = true;
                break;
            }
        }
        if (skip)
        {
            continue;
        }
        const auto& p = air[i];
        out[static_cast<int>(i)] = std::vector<uint8_t>(
            p.begin() + static_cast<long>(RsBlockErasure::kHeaderLen), p.end());
    }
    return out;
}
}  // namespace

TEST(FecTest, RoundtripNoLoss)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    std::vector<std::vector<uint8_t>> orig;
    for (int i = 0; i < 10; i++)
    {
        orig.push_back(
            pkt(40 + static_cast<size_t>(i) * 3, static_cast<uint8_t>(i)));
    }
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 1, &air));
    ASSERT_EQ(air.size(), 15u);
    for (const auto& p : air)
    {
        EXPECT_LE(p.size(), 1476u);
        EXPECT_EQ(p[0], RsBlockErasure::kMagic);
        EXPECT_EQ(p[1], RsBlockErasure::kVersion);
    }
    auto frags = frags_except(air, {});
    std::vector<std::vector<uint8_t>> got;
    int rec = -1;
    ASSERT_TRUE(fec.decode_block(frags, &got, &rec));
    EXPECT_EQ(rec, 0);
    EXPECT_EQ(got, orig);
}

TEST(FecTest, RecoversFiveErasures)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    std::vector<std::vector<uint8_t>> orig;
    for (int i = 0; i < 10; i++)
    {
        orig.push_back(
            pkt(80 + static_cast<size_t>(i), static_cast<uint8_t>(0xA0 + i)));
    }
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 7, &air));

    auto drop_data = frags_except(air, {0, 1, 2, 3, 4});
    std::vector<std::vector<uint8_t>> got;
    int rec = 0;
    ASSERT_TRUE(fec.decode_block(drop_data, &got, &rec));
    EXPECT_EQ(rec, 5);
    EXPECT_EQ(got, orig);

    auto drop_parity = frags_except(air, {10, 11, 12, 13, 14});
    rec = -1;
    ASSERT_TRUE(fec.decode_block(drop_parity, &got, &rec));
    EXPECT_EQ(rec, 0);
    EXPECT_EQ(got, orig);

    auto drop_mix = frags_except(air, {1, 4, 8, 11, 14});
    ASSERT_TRUE(fec.decode_block(drop_mix, &got, &rec));
    EXPECT_EQ(got, orig);
}

TEST(FecTest, SixErasuresFail)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    std::vector<std::vector<uint8_t>> orig(10, pkt(32, 1));
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 2, &air));
    auto frags = frags_except(air, {0, 1, 2, 3, 4, 5});
    std::vector<std::vector<uint8_t>> got;
    EXPECT_FALSE(fec.decode_block(frags, &got, nullptr));
}

TEST(FecTest, PartialBlockPads)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    std::vector<std::vector<uint8_t>> orig = {pkt(5, 'a'), pkt(9, 'b'),
                                              pkt(3, 'c')};
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 3, &air));
    auto frags = frags_except(air, {5, 6, 7, 10, 12});
    std::vector<std::vector<uint8_t>> got;
    ASSERT_TRUE(fec.decode_block(frags, &got, nullptr));
    EXPECT_EQ(got, orig);
}

TEST(FecTest, FeedFullBlock)
{
    RsBlockErasure enc;
    RsBlockErasure dec;
    ASSERT_TRUE(enc.init(10, 15, 20));
    ASSERT_TRUE(dec.init(10, 15, 20));
    std::vector<std::vector<uint8_t>> orig;
    std::vector<std::vector<uint8_t>> got;
    for (int i = 0; i < 10; i++)
    {
        orig.push_back(
            pkt(20 + static_cast<size_t>(i), static_cast<uint8_t>(i)));
        std::vector<std::vector<uint8_t>> air;
        enc.push_app(orig.back().data(), orig.back().size(), &air);
        if (i < 9)
        {
            EXPECT_TRUE(air.empty());
        }
        for (const auto& p : air)
        {
            std::vector<std::vector<uint8_t>> payloads;
            dec.push_air(p.data(), p.size(), &payloads);
            got.insert(got.end(), payloads.begin(), payloads.end());
        }
    }
    EXPECT_EQ(got, orig);
    EXPECT_EQ(enc.blocks(), 1u);
    EXPECT_EQ(dec.blocks(), 1u);
}

TEST(FecTest, PassthroughUnknownMagic)
{
    RsBlockErasure dec;
    ASSERT_TRUE(dec.init(10, 15, 20));
    const uint8_t raw[] = {0x80, 0x21, 0x00, 0x01, 'R', 'T', 'P'};
    std::vector<std::vector<uint8_t>> payloads;
    dec.push_air(raw, sizeof(raw), &payloads);
    ASSERT_EQ(payloads.size(), 1u);
    EXPECT_EQ(payloads[0], std::vector<uint8_t>(raw, raw + sizeof(raw)));
}

TEST(FecTest, MaxPayloadFitsWifi)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    const size_t max_orig = RsBlockErasure::max_original();
    std::vector<std::vector<uint8_t>> orig = {pkt(max_orig, 0x33)};
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 4, &air));
    for (const auto& p : air)
    {
        EXPECT_LE(p.size(), 1476u);
    }
    auto frags = frags_except(air, {0, 2, 9, 11, 13});
    std::vector<std::vector<uint8_t>> got;
    ASSERT_TRUE(fec.decode_block(frags, &got, nullptr));
    EXPECT_EQ(got, orig);
}
