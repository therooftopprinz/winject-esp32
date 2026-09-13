#ifndef WINJECT_MANAGER_BASIC_FEC_H_
#define WINJECT_MANAGER_BASIC_FEC_H_

#include <chrono>
#include <deque>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <vector>

// Packet-block Reed-Solomon erasure FEC (systematic Cauchy MDS via ISA-L).
// k original UDP datagrams become n on-air shards; any k of n recover the
// originals. Systematic shards are native size (2-byte length + payload);
// RS math and parity shards pad to the longest row. Wire header is 8 bytes;
// not compatible with tools/fec.py (reedsolo) parity.
class rs_block_erasure
{
public:
    static constexpr uint8_t k_magic = 0xF1;
    static constexpr uint8_t k_version = 2;
    static constexpr size_t k_header_len = 8;
    static constexpr size_t k_len_prefix = 2;
    static constexpr uint8_t k_flag_parity = 0x01;
    static constexpr int k_default_timeout_ms = 20;
    static constexpr size_t k_max_n = 255;
    static constexpr size_t k_done_max = 128;
    static constexpr size_t k_block_max = 64;

    // Incomplete RX block TTL, and finished-id TTL (duplicate-shard
    // suppression). done_hold is longer so a late shard of a completed block
    // is still dropped, but short enough that a peer restart which reuses
    // block_id from 0 is accepted after the old ids age out.
    int rx_hold_ms() const;
    int done_hold_ms() const;

    bool init(int k, int n, int timeout_ms);
    // Stop TX encode; flush pending first via flush()/announce_down. RX decode
    // still works from shard headers.
    void disable();
    bool enabled() const
    {
        return enabled_;
    }
    int k() const
    {
        return k_;
    }
    int n() const
    {
        return n_;
    }
    const char* impl_name() const;

    // App datagram -> zero or more air shards (full block or nothing).
    void push_app(const uint8_t* data, size_t len,
                  std::vector<std::vector<uint8_t>>* out);
    void flush(std::vector<std::vector<uint8_t>>* out);
    void on_tick(std::vector<std::vector<uint8_t>>* out);

    // Air datagram -> original payloads when a block can be decoded.
    // Non-FEC packets are forwarded unchanged (passthrough).
    void push_air(const uint8_t* data, size_t len,
                  std::vector<std::vector<uint8_t>>* out);

    uint64_t recovered() const
    {
        return recovered_;
    }
    uint64_t blocks() const
    {
        return blocks_;
    }
    uint64_t decode_fail() const
    {
        return decode_fail_;
    }
    uint64_t oversized() const
    {
        return oversized_;
    }
    // Interval since last take. recovered() / decode_fail() stay lifetime.
    uint64_t take_recovered();
    uint64_t take_decode_fail();

    // Encode one block. packets.size() may be < k (empty pads). Empty / short
    // systematic shards omit trailing zeros on the wire; parity is full width.
    bool encode_block(const std::vector<std::vector<uint8_t>>& packets,
                      uint16_t block_id,
                      std::vector<std::vector<uint8_t>>* out) const;
    // Decode from shard index -> body. k/n come from the wire header (or
    // from init() via the overload). Returns false if unrecoverable.
    bool decode_block(int k, int n,
                      const std::unordered_map<int, std::vector<uint8_t>>& frags,
                      std::vector<std::vector<uint8_t>>* payloads,
                      int* recovered) const;
    bool decode_block(
        const std::unordered_map<int, std::vector<uint8_t>>& frags,
        std::vector<std::vector<uint8_t>>* payloads, int* recovered) const;

    static size_t max_original();

private:
    struct rx_block_s
    {
        int k = 0;
        int n = 0;
        std::unordered_map<int, std::vector<uint8_t>> frags;
        std::chrono::steady_clock::time_point first_seen{};
    };

    void expire_rx();
    void expire_done();
    void mark_done(uint16_t block_id);
    std::chrono::steady_clock::time_point now() const;
    static bool pack_header(uint8_t* out, uint16_t block_id, int index, int k,
                            int n, uint8_t flags);
    static bool unpack_header(const uint8_t* data, size_t len,
                              uint16_t* block_id, int* index, int* k, int* n,
                              uint8_t* flags);
    static int gen_decode_matrix(int k, int n, const uint8_t* encode_matrix,
                                 const uint8_t* err_list, int nerrs,
                                 uint8_t* decode_matrix, uint8_t* decode_index);

    bool enabled_ = false;
    int k_ = 0;
    int n_ = 0;
    int p = 0;
    int timeout_ms = k_default_timeout_ms;
    std::vector<uint8_t> encode_matrix;
    std::vector<uint8_t> g_tbls;

    std::vector<std::vector<uint8_t>> pending;
    std::chrono::steady_clock::time_point deadline{};
    bool deadline_set = false;
    uint16_t block_id = 0;

    std::unordered_map<uint16_t, rx_block_s> rx_blocks;
    std::deque<uint16_t> done_order;
    std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> done;

    uint64_t recovered_ = 0;
    uint64_t recovered_seen_ = 0;
    uint64_t blocks_ = 0;
    uint64_t decode_fail_ = 0;
    uint64_t decode_fail_seen_ = 0;
    uint64_t oversized_ = 0;
};

#endif  // WINJECT_MANAGER_BASIC_FEC_H_
