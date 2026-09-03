#ifndef WINJECT_MANAGER_FEC_H_
#define WINJECT_MANAGER_FEC_H_

#include <chrono>
#include <deque>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Packet-block Reed-Solomon erasure FEC (systematic Cauchy MDS via ISA-L).
// k original UDP datagrams become n on-air shards; any k of n recover the
// originals. Wire header is 8 bytes; not compatible with tools/fec.py
// (reedsolo) parity.
class RsBlockErasure
{
public:
    static constexpr uint8_t kMagic = 0xF1;
    static constexpr uint8_t kVersion = 2;
    static constexpr size_t kHeaderLen = 8;
    static constexpr size_t kLenPrefix = 2;
    static constexpr uint8_t kFlagParity = 0x01;
    static constexpr int kDefaultTimeoutMs = 20;
    static constexpr size_t kMaxN = 255;
    static constexpr size_t kDoneMax = 128;
    static constexpr size_t kBlockMax = 64;

    bool init(int k, int n, int timeout_ms);
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
    uint64_t take_recovered();
    uint64_t take_decode_fail();

    // Encode one block. packets.size() may be < k (empty pads).
    bool encode_block(const std::vector<std::vector<uint8_t>>& packets,
                      uint16_t block_id,
                      std::vector<std::vector<uint8_t>>* out) const;
    // Decode from shard index -> body. Returns false if unrecoverable.
    bool decode_block(
        const std::unordered_map<int, std::vector<uint8_t>>& frags,
        std::vector<std::vector<uint8_t>>* payloads, int* recovered) const;

    static size_t max_original();

private:
    struct RxBlock
    {
        int k = 0;
        int n = 0;
        std::unordered_map<int, std::vector<uint8_t>> frags;
        std::chrono::steady_clock::time_point first_seen{};
    };

    void expire_rx();
    void mark_done(uint16_t block_id);
    static bool pack_header(uint8_t* out, uint16_t block_id, int index, int k,
                            int n, uint8_t flags);
    static bool unpack_header(const uint8_t* data, size_t len,
                              uint16_t* block_id, int* index, int* k, int* n,
                              uint8_t* flags);
    int gen_decode_matrix(const uint8_t* err_list, int nerrs,
                          uint8_t* decode_matrix, uint8_t* decode_index) const;

    bool enabled_ = false;
    int k_ = 0;
    int n_ = 0;
    int p_ = 0;
    int timeout_ms_ = kDefaultTimeoutMs;
    std::vector<uint8_t> encode_matrix_;
    std::vector<uint8_t> g_tbls_;

    std::vector<std::vector<uint8_t>> pending_;
    std::chrono::steady_clock::time_point deadline_{};
    bool deadline_set_ = false;
    uint16_t block_id_ = 0;

    std::unordered_map<uint16_t, RxBlock> rx_blocks_;
    std::deque<uint16_t> done_order_;
    std::unordered_set<uint16_t> done_;

    uint64_t recovered_ = 0;
    uint64_t blocks_ = 0;
    uint64_t decode_fail_ = 0;
    uint64_t oversized_ = 0;
};

#endif  // WINJECT_MANAGER_FEC_H_
