#include "tcp_stream.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <string.h>

#include <chrono>
#include <thread>
#include <vector>

namespace
{
void exchange(TcpStream* from, TcpStream* to)
{
    uint8_t buf[2048];
    while (from->has_tx() || from->has_ack())
    {
        bool is_ack = false;
        const size_t n = from->pull_tx(buf, sizeof(buf), &is_ack);
        ASSERT_GT(n, 0u);
        to->on_radio_rx(buf, n);
    }
}

void write_data_frame(uint8_t* p, uint16_t seq, const uint8_t* payload,
                      size_t plen)
{
    p[0] = TcpStream::kTypeData;
    p[1] = 0;
    const uint16_t ns = htons(seq);
    const uint16_t z = 0;
    const uint16_t nl = htons(static_cast<uint16_t>(plen));
    memcpy(p + 2, &ns, 2);
    memcpy(p + 4, &z, 2);
    memcpy(p + 6, &nl, 2);
    memcpy(p + 8, payload, plen);
}

void write_ack_frame(uint8_t* p, uint16_t ack)
{
    p[0] = TcpStream::kTypeAck;
    p[1] = 0;
    const uint16_t z = 0;
    const uint16_t na = htons(ack);
    memcpy(p + 2, &z, 2);
    memcpy(p + 4, &na, 2);
    memcpy(p + 6, &z, 2);
}

uint16_t read_ack(const uint8_t* p)
{
    uint16_t v;
    memcpy(&v, p + 4, 2);
    return ntohs(v);
}

void handshake(TcpStream* a, TcpStream* b)
{
    a->local_up();
    exchange(a, b);
    b->local_up();
    exchange(b, a);
    // CONNECT ACKs may still be pending on the peer that received CONNECT last.
    exchange(a, b);
    exchange(b, a);
    ASSERT_TRUE(a->established());
    ASSERT_TRUE(b->established());
    EXPECT_FALSE(a->has_ack());
    EXPECT_FALSE(b->has_ack());
}
}  // namespace

TEST(TcpStreamTest, ConnectAndPayload)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);

    const uint8_t hello[] = {'h', 'i'};
    a.on_tcp_bytes(hello, sizeof(hello));
    exchange(&a, &b);
    exchange(&b, &a);

    uint8_t out[16];
    size_t n = 0;
    ASSERT_TRUE(b.pull_tcp(out, sizeof(out), &n));
    ASSERT_EQ(n, sizeof(hello));
    EXPECT_EQ(memcmp(out, hello, n), 0);
}

TEST(TcpStreamTest, LocalFinEndsPeer)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);

    a.local_down();
    exchange(&a, &b);
    EXPECT_TRUE(b.ended_by_peer());
    EXPECT_FALSE(b.wants_connect());
}

TEST(TcpStreamTest, LocalDownQueuesCloseUntilPulled)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);

    a.local_down();
    ASSERT_TRUE(a.has_ack());
    uint8_t buf[64];
    bool is_ack = false;
    const size_t n = a.pull_tx(buf, sizeof(buf), &is_ack);
    ASSERT_EQ(n, TcpStream::kHeaderSize);
    EXPECT_TRUE(is_ack);
    EXPECT_EQ(buf[0], TcpStream::kTypeClose);
}

TEST(TcpStreamTest, SecondSessionAfterPeerReset)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);

    a.local_down();
    exchange(&a, &b);
    EXPECT_TRUE(b.ended_by_peer());
    b.reset();
    a.reset();

    a.local_up();
    exchange(&a, &b);
    EXPECT_TRUE(b.wants_connect());
    b.local_up();
    exchange(&b, &a);
    EXPECT_TRUE(a.established());
    EXPECT_TRUE(b.established());

    const uint8_t hello[] = {'o', 'k'};
    a.on_tcp_bytes(hello, sizeof(hello));
    exchange(&a, &b);
    exchange(&b, &a);
    uint8_t out[16];
    size_t n = 0;
    ASSERT_TRUE(b.pull_tcp(out, sizeof(out), &n));
    ASSERT_EQ(n, sizeof(hello));
}

TEST(TcpStreamTest, LocalAbortEndsPeer)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);

    a.local_abort();
    exchange(&a, &b);
    EXPECT_TRUE(b.ended_by_peer());
    EXPECT_FALSE(b.wants_connect());
}

TEST(TcpStreamTest, PiggybackAckAtSendTime)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);

    const uint8_t from_b[] = {'x'};
    b.on_tcp_bytes(from_b, sizeof(from_b));
    exchange(&b, &a);
    // A has received seq 0; next expected rx_seq_ == 1.

    const uint8_t from_a[] = {'y'};
    a.on_tcp_bytes(from_a, sizeof(from_a));
    uint8_t buf[2048];
    bool is_ack = true;
    // Drain A's ACK for B's data first.
    ASSERT_TRUE(a.has_ack());
    ASSERT_GT(a.pull_tx(buf, sizeof(buf), &is_ack), 0u);
    EXPECT_TRUE(is_ack);

    is_ack = true;
    const size_t n = a.pull_tx(buf, sizeof(buf), &is_ack);
    ASSERT_GT(n, TcpStream::kHeaderSize);
    EXPECT_FALSE(is_ack);
    EXPECT_EQ(buf[0], TcpStream::kTypeData);
    EXPECT_EQ(read_ack(buf), 1);
}

TEST(TcpStreamTest, StaleAckDoesNotDropWindow)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);

    for (int i = 0; i < 3; i++)
    {
        const uint8_t byte = static_cast<uint8_t>('a' + i);
        a.on_tcp_bytes(&byte, 1);
        uint8_t buf[2048];
        bool is_ack = false;
        ASSERT_GT(a.pull_tx(buf, sizeof(buf), &is_ack), 0u);
        EXPECT_FALSE(is_ack);
        b.on_radio_rx(buf, TcpStream::kHeaderSize + 1);
    }

    uint8_t ack[TcpStream::kHeaderSize];
    write_ack_frame(ack, 2);
    a.on_radio_rx(ack, sizeof(ack));

    write_ack_frame(ack, 0);  // stale
    a.on_radio_rx(ack, sizeof(ack));

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    a.on_tick();
    uint8_t buf[2048];
    bool is_ack = true;
    const size_t n = a.pull_tx(buf, sizeof(buf), &is_ack);
    ASSERT_GT(n, TcpStream::kHeaderSize);
    EXPECT_FALSE(is_ack);
    EXPECT_EQ(buf[0], TcpStream::kTypeData);
}

TEST(TcpStreamTest, OutOfOrderBufferedThenDelivered)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);

    // Queue three 1-byte segments from A.
    for (int i = 0; i < 3; i++)
    {
        const uint8_t byte = static_cast<uint8_t>('a' + i);
        a.on_tcp_bytes(&byte, 1);
    }
    uint8_t frames[3][64];
    size_t lens[3];
    for (int i = 0; i < 3; i++)
    {
        bool is_ack = false;
        lens[i] = a.pull_tx(frames[i], sizeof(frames[i]), &is_ack);
        ASSERT_GT(lens[i], TcpStream::kHeaderSize);
        EXPECT_FALSE(is_ack);
    }

    // Deliver seq 1 and 2 first (hole at 0) — must be buffered, not discarded.
    b.on_radio_rx(frames[1], lens[1]);
    b.on_radio_rx(frames[2], lens[2]);
    uint8_t out[16];
    size_t n = 0;
    EXPECT_FALSE(b.pull_tcp(out, sizeof(out), &n));

    // Fill the hole; all three should deliver in order.
    b.on_radio_rx(frames[0], lens[0]);
    ASSERT_TRUE(b.pull_tcp(out, sizeof(out), &n));
    ASSERT_EQ(n, 3u);
    EXPECT_EQ(out[0], 'a');
    EXPECT_EQ(out[1], 'b');
    EXPECT_EQ(out[2], 'c');

    // Cumulative ACK should now be 3; SACK may also be present before hole fill.
    exchange(&b, &a);
}

TEST(TcpStreamTest, SackSkipsRetransmitOfReceivedSegment)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);

    for (int i = 0; i < 2; i++)
    {
        const uint8_t byte = static_cast<uint8_t>('x' + i);
        a.on_tcp_bytes(&byte, 1);
    }
    uint8_t f0[64];
    uint8_t f1[64];
    bool is_ack = false;
    const size_t n0 = a.pull_tx(f0, sizeof(f0), &is_ack);
    const size_t n1 = a.pull_tx(f1, sizeof(f1), &is_ack);
    ASSERT_GT(n0, TcpStream::kHeaderSize);
    ASSERT_GT(n1, TcpStream::kHeaderSize);

    // B gets only seq 1 (OOO). ACK carries cum=0 and SACK {1,1}.
    b.on_radio_rx(f1, n1);
    ASSERT_TRUE(b.has_ack());
    uint8_t ackbuf[64];
    const size_t an = b.pull_tx(ackbuf, sizeof(ackbuf), &is_ack);
    ASSERT_TRUE(is_ack);
    ASSERT_GE(an, TcpStream::kHeaderSize + TcpStream::kSackBlockSize);
    EXPECT_EQ(read_ack(ackbuf), 0);
    a.on_radio_rx(ackbuf, an);

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    a.on_tick();
    // Only the hole (seq 0) should be eligible; seq 1 is SACKed.
    uint8_t retx[64];
    const size_t rn = a.pull_tx(retx, sizeof(retx), &is_ack);
    ASSERT_GT(rn, TcpStream::kHeaderSize);
    EXPECT_FALSE(is_ack);
    uint16_t seq;
    memcpy(&seq, retx + 2, 2);
    EXPECT_EQ(ntohs(seq), 0);
    EXPECT_FALSE(a.has_tx());  // seq 1 still sacked / in_flight, not retx
}

TEST(TcpStreamTest, AcceptsTcpStopsAtIngestCap)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);
    std::vector<uint8_t> chunk(TcpStream::kMaxSegment, 0x5a);
    while (a.accepts_tcp())
    {
        const size_t n =
            chunk.size() < a.tcp_in_room() ? chunk.size() : a.tcp_in_room();
        a.on_tcp_bytes(chunk.data(), n);
    }
    EXPECT_EQ(a.tcp_in_room(), 0u);
    EXPECT_GE(a.tcp_in_size(), TcpStream::kTcpInMax);
    EXPECT_EQ(a.unacked_count(), TcpStream::kWindow);
}

TEST(TcpStreamTest, SegmentsFitEthernetUdpMtu)
{
    EXPECT_LE(TcpStream::kHeaderSize + TcpStream::kMaxSegment,
              TcpStream::kMaxUdpPayload);
}

TEST(TcpStreamTest, DataBeforeLocalUpIsBuffered)
{
    TcpStream a;
    TcpStream b;
    a.local_up();
    exchange(&a, &b);
    EXPECT_TRUE(b.wants_connect());
    EXPECT_FALSE(b.established());

    const uint8_t hello[] = {'h', 'i'};
    uint8_t frame[16];
    write_data_frame(frame, 0, hello, sizeof(hello));
    b.on_radio_rx(frame, TcpStream::kHeaderSize + sizeof(hello));
    b.local_up();

    uint8_t out[16];
    size_t n = 0;
    ASSERT_TRUE(b.pull_tcp(out, sizeof(out), &n));
    ASSERT_EQ(n, sizeof(hello));
    EXPECT_EQ(memcmp(out, hello, n), 0);
}

TEST(TcpStreamTest, ShortDataFrameDoesNotAdvanceRx)
{
    TcpStream a;
    TcpStream b;
    handshake(&a, &b);

    const uint8_t hello[20] = {'h', 'i'};
    a.on_tcp_bytes(hello, sizeof(hello));
    uint8_t buf[2048];
    bool is_ack = false;
    const size_t n = a.pull_tx(buf, sizeof(buf), &is_ack);
    ASSERT_GT(n, TcpStream::kHeaderSize + 4);

    b.on_radio_rx(buf, n - 4);
    uint8_t out[32];
    size_t got = 0;
    EXPECT_FALSE(b.pull_tcp(out, sizeof(out), &got));

    b.on_radio_rx(buf, n);
    ASSERT_TRUE(b.pull_tcp(out, sizeof(out), &got));
    ASSERT_EQ(got, sizeof(hello));
    EXPECT_EQ(memcmp(out, hello, got), 0);
}
