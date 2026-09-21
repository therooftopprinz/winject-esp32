#include "frames/air_seq.h"
#include "stream/stream.h"

#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace
{
std::vector<uint8_t> frame(air_seq* tx, const uint8_t* data, size_t len)
{
    std::vector<uint8_t> out(len + air_seq::k_len + 8);
    size_t n = 0;
    EXPECT_TRUE(tx->stamp(out.data(), out.size(), data, len, &n));
    out.resize(n);
    return out;
}
}  // namespace

TEST(AirSeqTest, StampThenAccept)
{
    air_seq tx;
    air_seq rx;
    const uint8_t payload[] = {'h', 'i'};
    const auto pkt = frame(&tx, payload, sizeof(payload));
    ASSERT_EQ(pkt.size(), air_seq::k_len + sizeof(payload));
    EXPECT_EQ(pkt[0], 0);
    EXPECT_EQ(pkt[1], 0);
    EXPECT_EQ(tx.tx(), 1);

    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(rx.accept(pkt.data(), pkt.size(), &body, &plen));
    ASSERT_EQ(plen, sizeof(payload));
    EXPECT_EQ(memcmp(body, payload, plen), 0);
    EXPECT_EQ(rx.lost(), 0u);
}

TEST(AirSeqTest, DetectsGap)
{
    air_seq tx;
    air_seq rx;
    const uint8_t a[] = {1};
    const uint8_t b[] = {2};
    const uint8_t c[] = {3};
    const auto p0 = frame(&tx, a, sizeof(a));
    const auto p1 = frame(&tx, b, sizeof(b));
    const auto p2 = frame(&tx, c, sizeof(c));
    (void)p1;

    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(rx.accept(p0.data(), p0.size(), &body, &plen));
    ASSERT_TRUE(rx.accept(p2.data(), p2.size(), &body, &plen));
    EXPECT_EQ(rx.lost(), 1u);
    EXPECT_EQ(rx.take_lost(), 1u);
    EXPECT_EQ(rx.lost(), 0u);
}

TEST(AirSeqTest, DuplicateReplayRejected)
{
    air_seq tx;
    air_seq rx;
    const uint8_t a[] = {9};
    const auto p0 = frame(&tx, a, sizeof(a));
    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(rx.accept(p0.data(), p0.size(), &body, &plen));
    EXPECT_FALSE(rx.accept(p0.data(), p0.size(), &body, &plen));
    EXPECT_EQ(rx.lost(), 0u);
}

TEST(AirSeqTest, WrapAround)
{
    air_seq tx;
    air_seq rx;
    uint8_t one = 1;
    std::vector<uint8_t> last;
    for (int i = 0; i < 65536; i++)
    {
        last = frame(&tx, &one, 1);
    }
    EXPECT_EQ(tx.tx(), 0);

    const uint8_t* body = nullptr;
    size_t plen = 0;
    // Seed RX with seq 65535 (the last stamped before wrap).
    ASSERT_TRUE(rx.accept(last.data(), last.size(), &body, &plen));
    const auto wrapped = frame(&tx, &one, 1);
    ASSERT_EQ(wrapped[0], 0);
    ASSERT_EQ(wrapped[1], 0);
    ASSERT_TRUE(rx.accept(wrapped.data(), wrapped.size(), &body, &plen));
    EXPECT_EQ(rx.lost(), 0u);

    const auto skip = frame(&tx, &one, 1);  // seq 1; drop it
    const auto next = frame(&tx, &one, 1);  // seq 2
    (void)skip;
    ASSERT_TRUE(rx.accept(next.data(), next.size(), &body, &plen));
    EXPECT_EQ(rx.lost(), 1u);
}

TEST(AirSeqTest, RejectsShort)
{
    air_seq rx;
    const uint8_t one[] = {0x00};
    const uint8_t* body = nullptr;
    size_t plen = 1;
    EXPECT_FALSE(rx.accept(one, sizeof(one), &body, &plen));
}

TEST(AirSeqTest, StampTooSmallDoesNotAdvance)
{
    air_seq tx;
    uint8_t buf[2];
    const uint8_t payload[] = {1, 2, 3};
    size_t n = 99;
    EXPECT_FALSE(tx.stamp(buf, sizeof(buf), payload, sizeof(payload), &n));
    EXPECT_EQ(tx.tx(), 0);
}

TEST(AirSeqTest, FirstPacketIsNotAGap)
{
    air_seq tx;
    air_seq rx;
    uint8_t one = 7;
    (void)frame(&tx, &one, 1);
    (void)frame(&tx, &one, 1);
    const auto third = frame(&tx, &one, 1);
    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(rx.accept(third.data(), third.size(), &body, &plen));
    EXPECT_EQ(rx.lost(), 0u);
}

TEST(FecFormatTest, NoneAndBlock)
{
    char buf[32];
    format_fec(buf, sizeof(buf), 0, 0);
    EXPECT_STREQ(buf, "none");
    format_fec(buf, sizeof(buf), 10, 12);
    EXPECT_STREQ(buf, "block(10,12)");
}
