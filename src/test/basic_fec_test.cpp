#include "frames/basic_fec.h"
#include "net_util.h"

#include <chrono>
#include <gtest/gtest.h>
#include <string.h>
#include <thread>
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
            p.begin() + static_cast<long>(rs_block_erasure::k_header_len), p.end());
    }
    return out;
}
}  // namespace

TEST(FecTest, RoundtripNoLoss)
{
    rs_block_erasure fec;
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
        EXPECT_LE(p.size(), k_stream_payload_max);
        EXPECT_EQ(p[0], rs_block_erasure::k_magic);
        EXPECT_EQ(p[1], rs_block_erasure::k_version);
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
    rs_block_erasure fec;
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
    rs_block_erasure fec;
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
    rs_block_erasure fec;
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
    rs_block_erasure enc;
    rs_block_erasure dec;  // RX needs no init; k/n come from shard headers.
    ASSERT_TRUE(enc.init(10, 15, 20));
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
    rs_block_erasure dec;  // no init
    const uint8_t raw[] = {0x80, 0x21, 0x00, 0x01, 'R', 'T', 'P'};
    std::vector<std::vector<uint8_t>> payloads;
    dec.push_air(raw, sizeof(raw), &payloads);
    ASSERT_EQ(payloads.size(), 1u);
    EXPECT_EQ(payloads[0], std::vector<uint8_t>(raw, raw + sizeof(raw)));
}

TEST(FecTest, MaxPayloadFitsWifi)
{
    rs_block_erasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    const size_t max_orig = rs_block_erasure::max_original();
    std::vector<std::vector<uint8_t>> orig = {pkt(max_orig, 0x33)};
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 4, &air));
    for (const auto& p : air)
    {
        EXPECT_LE(p.size(), k_stream_payload_max);
    }
    auto frags = frags_except(air, {0, 2, 9, 11, 13});
    std::vector<std::vector<uint8_t>> got;
    ASSERT_TRUE(fec.decode_block(frags, &got, nullptr));
    EXPECT_EQ(got, orig);
}

TEST(FecTest, SystematicNativeSize)
{
    rs_block_erasure fec;
    ASSERT_TRUE(fec.init(3, 4, 20));
    std::vector<std::vector<uint8_t>> orig = {pkt(100, 1), pkt(300, 2),
                                              pkt(200, 3)};
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 1, &air));
    ASSERT_EQ(air.size(), 4u);
    EXPECT_EQ(air[0].size(), rs_block_erasure::k_header_len +
                                 rs_block_erasure::k_len_prefix + 100u);
    EXPECT_EQ(air[1].size(), rs_block_erasure::k_header_len +
                                 rs_block_erasure::k_len_prefix + 300u);
    EXPECT_EQ(air[2].size(), rs_block_erasure::k_header_len +
                                 rs_block_erasure::k_len_prefix + 200u);
    EXPECT_EQ(air[3].size(), rs_block_erasure::k_header_len +
                                 rs_block_erasure::k_len_prefix + 300u);

    auto drop_largest = frags_except(air, {1});
    std::vector<std::vector<uint8_t>> got;
    int rec = 0;
    ASSERT_TRUE(fec.decode_block(drop_largest, &got, &rec));
    EXPECT_EQ(rec, 1);
    EXPECT_EQ(got, orig);

    auto drop_small = frags_except(air, {0});
    rec = -1;
    ASSERT_TRUE(fec.decode_block(drop_small, &got, &rec));
    EXPECT_EQ(rec, 1);
    EXPECT_EQ(got, orig);
}

TEST(FecTest, PartialEmptyNotPadded)
{
    rs_block_erasure fec;
    ASSERT_TRUE(fec.init(5, 6, 20));
    std::vector<std::vector<uint8_t>> orig = {pkt(400, 9)};
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 1, &air));
    ASSERT_EQ(air.size(), 6u);
    EXPECT_EQ(air[0].size(), rs_block_erasure::k_header_len +
                                 rs_block_erasure::k_len_prefix + 400u);
    for (int i = 1; i < 5; i++)
    {
        EXPECT_EQ(air[static_cast<size_t>(i)].size(),
                  rs_block_erasure::k_header_len +
                      rs_block_erasure::k_len_prefix);
    }
    EXPECT_EQ(air[5].size(), rs_block_erasure::k_header_len +
                                 rs_block_erasure::k_len_prefix + 400u);
    size_t on_air = 0;
    for (const auto& p : air)
    {
        on_air += p.size();
    }
    EXPECT_LT(on_air, 5 * 400u);

    auto frags = frags_except(air, {0});
    std::vector<std::vector<uint8_t>> got;
    ASSERT_TRUE(fec.decode_block(frags, &got, nullptr));
    EXPECT_EQ(got, orig);
}

TEST(FecTest, DuplicateBlockIdDroppedThenExpires)
{
    rs_block_erasure enc;
    rs_block_erasure dec;
    ASSERT_TRUE(enc.init(1, 2, 1));
    ASSERT_TRUE(dec.init(1, 2, 1));
    const std::vector<uint8_t> orig = pkt(8, 0x11);
    std::vector<std::vector<uint8_t>> air;
    enc.push_app(orig.data(), orig.size(), &air);
    ASSERT_EQ(air.size(), 2u);

    std::vector<std::vector<uint8_t>> got;
    dec.push_air(air[0].data(), air[0].size(), &got);
    ASSERT_EQ(got, std::vector<std::vector<uint8_t>>{orig});

    got.clear();
    dec.push_air(air[0].data(), air[0].size(), &got);
    dec.push_air(air[1].data(), air[1].size(), &got);
    EXPECT_TRUE(got.empty());

    std::this_thread::sleep_for(
        std::chrono::milliseconds(dec.done_hold_ms() + 50));
    dec.push_air(air[0].data(), air[0].size(), &got);
    EXPECT_EQ(got, std::vector<std::vector<uint8_t>>{orig});
}
