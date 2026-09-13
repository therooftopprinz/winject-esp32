#include "frames/basic_fec.h"

#include <algorithm>
#include <arpa/inet.h>
#include <string.h>

#include "net_util.h"

extern "C"
{
#include "erasure_code.h"
}

#ifdef WINJECT_ISAL_NEON
extern "C" void winject_ec_encode_data(int len, int k, int rows,
                                       unsigned char* g_tbls,
                                       unsigned char** data,
                                       unsigned char** coding);
#define WINJECT_EC_ENCODE winject_ec_encode_data
#else
#define WINJECT_EC_ENCODE ec_encode_data_base
#endif

namespace
{
constexpr size_t k_min_shard = 16;  // ISA-L NEON kernels need >= 16 bytes.

void store_be16(uint8_t* p, uint16_t v)
{
    const uint16_t n = htons(v);
    memcpy(p, &n, sizeof(n));
}

uint16_t load_be16(const uint8_t* p)
{
    uint16_t n;
    memcpy(&n, p, sizeof(n));
    return ntohs(n);
}

size_t align_shard(size_t row)
{
    return row < k_min_shard ? k_min_shard : row;
}
}  // namespace

size_t rs_block_erasure::max_original()
{
    return k_stream_payload_max - k_header_len - k_len_prefix;
}

const char* rs_block_erasure::impl_name() const
{
#ifdef WINJECT_ISAL_NEON
    return "isa-l neon";
#else
    return "isa-l base";
#endif
}

bool rs_block_erasure::init(int k, int n, int timeout_ms)
{
    enabled_ = false;
    if (k < 1 || n <= k || n > static_cast<int>(k_max_n) || timeout_ms < 0)
    {
        return false;
    }
    k_ = k;
    n_ = n;
    p = n - k;
    this->timeout_ms = timeout_ms;
    encode_matrix.assign(static_cast<size_t>(n_) * static_cast<size_t>(k_), 0);
    g_tbls.assign(static_cast<size_t>(k_) * static_cast<size_t>(p) * 32, 0);
    gf_gen_cauchy1_matrix(encode_matrix.data(), n_, k_);
    ec_init_tables_base(k_, p, encode_matrix.data() + k_ * k_,
                        g_tbls.data());
    pending.clear();
    deadline_set = false;
    block_id = 0;
    rx_blocks.clear();
    done_order.clear();
    done.clear();
    recovered_ = 0;
    recovered_seen_ = 0;
    blocks_ = 0;
    decode_fail_ = 0;
    decode_fail_seen_ = 0;
    oversized_ = 0;
    enabled_ = true;
    return true;
}

void rs_block_erasure::disable()
{
    enabled_ = false;
    pending.clear();
    deadline_set = false;
    k_ = 0;
    n_ = 0;
    p = 0;
    encode_matrix.clear();
    g_tbls.clear();
    rx_blocks.clear();
    done_order.clear();
    done.clear();
}

bool rs_block_erasure::pack_header(uint8_t* out, uint16_t block_id, int index,
                                 int k, int n, uint8_t flags)
{
    if (out == nullptr)
    {
        return false;
    }
    out[0] = k_magic;
    out[1] = k_version;
    store_be16(out + 2, block_id);
    out[4] = static_cast<uint8_t>(index);
    out[5] = static_cast<uint8_t>(k);
    out[6] = static_cast<uint8_t>(n);
    out[7] = flags;
    return true;
}

bool rs_block_erasure::unpack_header(const uint8_t* data, size_t len,
                                   uint16_t* block_id, int* index, int* k,
                                   int* n, uint8_t* flags)
{
    if (data == nullptr || len < k_header_len || data[0] != k_magic ||
        data[1] != k_version)
    {
        return false;
    }
    const int idx = data[4];
    const int kk = data[5];
    const int nn = data[6];
    if (kk < 1 || nn <= kk || nn > static_cast<int>(k_max_n) || idx >= nn)
    {
        return false;
    }
    *block_id = load_be16(data + 2);
    *index = idx;
    *k = kk;
    *n = nn;
    *flags = data[7];
    return true;
}

bool rs_block_erasure::encode_block(
    const std::vector<std::vector<uint8_t>>& packets, uint16_t block_id,
    std::vector<std::vector<uint8_t>>* out) const
{
    if (!enabled_ || out == nullptr || packets.size() > static_cast<size_t>(k_))
    {
        return false;
    }
    std::vector<std::vector<uint8_t>> data_shards(static_cast<size_t>(k_));
    size_t max_row = 0;
    for (int i = 0; i < k_; i++)
    {
        const std::vector<uint8_t>* pkt = i < static_cast<int>(packets.size())
                                              ? &packets[static_cast<size_t>(i)]
                                              : nullptr;
        const size_t plen = pkt != nullptr ? pkt->size() : 0;
        if (plen > max_original())
        {
            return false;
        }
        data_shards[static_cast<size_t>(i)].resize(k_len_prefix + plen);
        store_be16(data_shards[static_cast<size_t>(i)].data(),
                   static_cast<uint16_t>(plen));
        if (plen > 0)
        {
            memcpy(data_shards[static_cast<size_t>(i)].data() + k_len_prefix,
                   pkt->data(), plen);
        }
        max_row = std::max(max_row, data_shards[static_cast<size_t>(i)].size());
    }
    const size_t shard_len = align_shard(max_row);
    if (k_header_len + shard_len > k_stream_payload_max)
    {
        return false;
    }
    for (int i = 0; i < k_; i++)
    {
        data_shards[static_cast<size_t>(i)].resize(shard_len, 0);
    }
    std::vector<std::vector<uint8_t>> parity(static_cast<size_t>(p));
    std::vector<unsigned char*> data_ptrs(static_cast<size_t>(k_));
    std::vector<unsigned char*> coding_ptrs(static_cast<size_t>(p));
    for (int i = 0; i < k_; i++)
    {
        data_ptrs[static_cast<size_t>(i)] =
            data_shards[static_cast<size_t>(i)].data();
    }
    for (int i = 0; i < p; i++)
    {
        parity[static_cast<size_t>(i)].assign(shard_len, 0);
        coding_ptrs[static_cast<size_t>(i)] =
            parity[static_cast<size_t>(i)].data();
    }
    WINJECT_EC_ENCODE(static_cast<int>(shard_len), k_, p,
                      const_cast<unsigned char*>(g_tbls.data()),
                      data_ptrs.data(), coding_ptrs.data());

    out->clear();
    out->resize(static_cast<size_t>(n_));
    for (int i = 0; i < n_; i++)
    {
        auto& pkt = (*out)[static_cast<size_t>(i)];
        const uint8_t flags = i >= k_ ? k_flag_parity : 0;
        size_t body_len = shard_len;
        const uint8_t* body = nullptr;
        if (i < k_)
        {
            const size_t plen = i < static_cast<int>(packets.size())
                                    ? packets[static_cast<size_t>(i)].size()
                                    : 0;
            body_len = k_len_prefix + plen;
            body = data_shards[static_cast<size_t>(i)].data();
        }
        else
        {
            body = parity[static_cast<size_t>(i - k_)].data();
        }
        pkt.resize(k_header_len + body_len);
        pack_header(pkt.data(), block_id, i, k_, n_, flags);
        memcpy(pkt.data() + k_header_len, body, body_len);
    }
    return true;
}

int rs_block_erasure::gen_decode_matrix(int k, int n,
                                        const uint8_t* encode_matrix,
                                        const uint8_t* err_list, int nerrs,
                                        uint8_t* decode_matrix,
                                        uint8_t* decode_index)
{
    if (encode_matrix == nullptr || err_list == nullptr ||
        decode_matrix == nullptr || decode_index == nullptr || k < 1 ||
        n <= k)
    {
        return -1;
    }
    std::vector<uint8_t> in_err(static_cast<size_t>(n), 0);
    for (int i = 0; i < nerrs; i++)
    {
        if (err_list[i] >= n)
        {
            return -1;
        }
        in_err[err_list[i]] = 1;
    }
    std::vector<uint8_t> b(static_cast<size_t>(k) * static_cast<size_t>(k));
    std::vector<uint8_t> invert(static_cast<size_t>(k) *
                                static_cast<size_t>(k));
    int r = 0;
    for (int i = 0; i < k; i++, r++)
    {
        while (r < n && in_err[static_cast<size_t>(r)])
        {
            r++;
        }
        if (r >= n)
        {
            return -1;
        }
        memcpy(b.data() + static_cast<size_t>(k) * static_cast<size_t>(i),
               encode_matrix + static_cast<size_t>(k) * static_cast<size_t>(r),
               static_cast<size_t>(k));
        decode_index[i] = static_cast<uint8_t>(r);
    }
    if (gf_invert_matrix(b.data(), invert.data(), k) < 0)
    {
        return -1;
    }
    for (int e = 0; e < nerrs; e++)
    {
        const int idx = err_list[e];
        if (idx < k)
        {
            memcpy(decode_matrix +
                       static_cast<size_t>(k) * static_cast<size_t>(e),
                   invert.data() +
                       static_cast<size_t>(k) * static_cast<size_t>(idx),
                   static_cast<size_t>(k));
            continue;
        }
        for (int i = 0; i < k; i++)
        {
            uint8_t s = 0;
            for (int j = 0; j < k; j++)
            {
                s ^= gf_mul(
                    invert[static_cast<size_t>(j) * static_cast<size_t>(k) +
                           static_cast<size_t>(i)],
                    encode_matrix[static_cast<size_t>(k) *
                                       static_cast<size_t>(idx) +
                                   static_cast<size_t>(j)]);
            }
            decode_matrix[static_cast<size_t>(k) * static_cast<size_t>(e) +
                          static_cast<size_t>(i)] = s;
        }
    }
    return 0;
}

bool rs_block_erasure::decode_block(
    const std::unordered_map<int, std::vector<uint8_t>>& frags,
    std::vector<std::vector<uint8_t>>* payloads, int* recovered) const
{
    if (!enabled_)
    {
        return false;
    }
    return decode_block(k_, n_, frags, payloads, recovered);
}

bool rs_block_erasure::decode_block(
    int k, int n, const std::unordered_map<int, std::vector<uint8_t>>& frags,
    std::vector<std::vector<uint8_t>>* payloads, int* recovered) const
{
    if (payloads == nullptr || k < 1 || n <= k ||
        n > static_cast<int>(k_max_n) ||
        frags.size() < static_cast<size_t>(k))
    {
        return false;
    }
    const int parity = n - k;
    std::vector<uint8_t> encode_matrix(static_cast<size_t>(n) *
                                       static_cast<size_t>(k));
    gf_gen_cauchy1_matrix(encode_matrix.data(), n, k);

    size_t shard_len = 0;
    for (const auto& kv : frags)
    {
        if (kv.first < 0 || kv.first >= n)
        {
            return false;
        }
        shard_len = std::max(shard_len, kv.second.size());
    }
    if (shard_len < k_len_prefix)
    {
        return false;
    }
    // Systematic rows may be native-size; pad to the longest (usually parity)
    // so ISA-L columns match encode.
    shard_len = align_shard(shard_len);
    std::vector<std::vector<uint8_t>> padded(static_cast<size_t>(n));
    std::vector<uint8_t> have(static_cast<size_t>(n), 0);
    for (const auto& kv : frags)
    {
        auto& row = padded[static_cast<size_t>(kv.first)];
        row = kv.second;
        if (row.size() < shard_len)
        {
            row.resize(shard_len, 0);
        }
        have[static_cast<size_t>(kv.first)] = 1;
    }

    std::vector<std::vector<uint8_t>> data_copy(static_cast<size_t>(k));
    bool have_all_data = true;
    for (int i = 0; i < k; i++)
    {
        if (!have[static_cast<size_t>(i)])
        {
            have_all_data = false;
            continue;
        }
        data_copy[static_cast<size_t>(i)] = padded[static_cast<size_t>(i)];
    }

    int rec = 0;
    if (!have_all_data)
    {
        uint8_t err_list[k_max_n];
        int nerrs = 0;
        for (int i = 0; i < n; i++)
        {
            if (!have[static_cast<size_t>(i)])
            {
                err_list[nerrs++] = static_cast<uint8_t>(i);
            }
        }
        if (nerrs > parity)
        {
            return false;
        }
        std::vector<uint8_t> decode_matrix(static_cast<size_t>(nerrs) *
                                           static_cast<size_t>(k));
        uint8_t decode_index[k_max_n];
        if (gen_decode_matrix(k, n, encode_matrix.data(), err_list, nerrs,
                              decode_matrix.data(), decode_index) != 0)
        {
            return false;
        }
        std::vector<uint8_t> decode_tbls(static_cast<size_t>(k) *
                                         static_cast<size_t>(nerrs) * 32);
        std::vector<unsigned char*> src_ptrs(static_cast<size_t>(k));
        std::vector<std::vector<uint8_t>> recover(static_cast<size_t>(nerrs));
        std::vector<unsigned char*> rec_ptrs(static_cast<size_t>(nerrs));
        for (int i = 0; i < k; i++)
        {
            const int idx = decode_index[i];
            if (!have[static_cast<size_t>(idx)])
            {
                return false;
            }
            src_ptrs[static_cast<size_t>(i)] =
                padded[static_cast<size_t>(idx)].data();
        }
        for (int i = 0; i < nerrs; i++)
        {
            recover[static_cast<size_t>(i)].assign(shard_len, 0);
            rec_ptrs[static_cast<size_t>(i)] =
                recover[static_cast<size_t>(i)].data();
        }
        ec_init_tables_base(k, nerrs, decode_matrix.data(),
                            decode_tbls.data());
        WINJECT_EC_ENCODE(static_cast<int>(shard_len), k, nerrs,
                          decode_tbls.data(), src_ptrs.data(), rec_ptrs.data());
        for (int i = 0; i < nerrs; i++)
        {
            const int idx = err_list[i];
            if (idx < k)
            {
                data_copy[static_cast<size_t>(idx)] =
                    std::move(recover[static_cast<size_t>(i)]);
                rec++;
            }
        }
    }

    payloads->clear();
    for (int i = 0; i < k; i++)
    {
        const auto& row = data_copy[static_cast<size_t>(i)];
        if (row.size() < k_len_prefix)
        {
            return false;
        }
        const uint16_t orig_len = load_be16(row.data());
        if (orig_len == 0)
        {
            continue;
        }
        if (k_len_prefix + orig_len > row.size())
        {
            return false;
        }
        payloads->emplace_back(row.data() + k_len_prefix,
                               row.data() + k_len_prefix + orig_len);
    }
    if (recovered != nullptr)
    {
        *recovered = rec;
    }
    return true;
}

void rs_block_erasure::push_app(const uint8_t* data, size_t len,
                              std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (!enabled_ || out == nullptr || data == nullptr)
    {
        return;
    }
    if (len > max_original())
    {
        oversized_++;
        return;
    }
    pending.emplace_back(data, data + len);
    if (!deadline_set)
    {
        deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
        deadline_set = true;
    }
    if (static_cast<int>(pending.size()) >= k_)
    {
        flush(out);
    }
}

void rs_block_erasure::flush(std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (!enabled_ || out == nullptr || pending.empty())
    {
        return;
    }
    if (!encode_block(pending, block_id, out))
    {
        oversized_++;
        out->clear();
        pending.clear();
        deadline_set = false;
        return;
    }
    block_id = static_cast<uint16_t>(block_id + 1);
    pending.clear();
    deadline_set = false;
    blocks_++;
}

int rs_block_erasure::rx_hold_ms() const
{
    return std::max(timeout_ms * 5, 100);
}

int rs_block_erasure::done_hold_ms() const
{
    return std::max(timeout_ms * 50, rx_hold_ms());
}

std::chrono::steady_clock::time_point rs_block_erasure::now() const
{
    return std::chrono::steady_clock::now();
}

void rs_block_erasure::on_tick(std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    // Always expire incomplete RX blocks (RX decode needs no encode init).
    expire_rx();
    if (!enabled_ || !deadline_set || timeout_ms <= 0)
    {
        return;
    }
    if (std::chrono::steady_clock::now() < deadline)
    {
        return;
    }
    flush(out);
}

void rs_block_erasure::expire_rx()
{
    expire_done();
    if (rx_blocks.empty())
    {
        return;
    }
    const auto t = now();
    const auto hold = std::chrono::milliseconds(rx_hold_ms());
    for (auto it = rx_blocks.begin(); it != rx_blocks.end();)
    {
        if (t - it->second.first_seen > hold)
        {
            decode_fail_++;
            it = rx_blocks.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void rs_block_erasure::expire_done()
{
    const auto t = now();
    const auto hold = std::chrono::milliseconds(done_hold_ms());
    while (!done_order.empty())
    {
        const uint16_t id = done_order.front();
        auto it = done.find(id);
        if (it == done.end())
        {
            done_order.pop_front();
            continue;
        }
        if (t - it->second <= hold)
        {
            break;
        }
        done.erase(it);
        done_order.pop_front();
    }
}

void rs_block_erasure::mark_done(uint16_t block_id)
{
    const auto t = now();
    auto inserted = done.emplace(block_id, t);
    if (inserted.second)
    {
        done_order.push_back(block_id);
    }
    else
    {
        inserted.first->second = t;
    }
    while (done_order.size() > k_done_max)
    {
        done.erase(done_order.front());
        done_order.pop_front();
    }
}

void rs_block_erasure::push_air(const uint8_t* data, size_t len,
                              std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (out == nullptr || data == nullptr || len == 0)
    {
        return;
    }
    // RX is always FEC-aware: k/n come from the shard header. Encode still
    // requires init(). Non-FEC datagrams pass through unchanged.
    if (data[0] != k_magic)
    {
        out->emplace_back(data, data + len);
        return;
    }
    expire_rx();
    uint16_t block_id = 0;
    int index = 0;
    int k = 0;
    int n = 0;
    uint8_t flags = 0;
    if (!unpack_header(data, len, &block_id, &index, &k, &n, &flags))
    {
        decode_fail_++;
        return;
    }
    if (done.find(block_id) != done.end())
    {
        return;
    }
    rx_block_s* buf = nullptr;
    auto it = rx_blocks.find(block_id);
    if (it == rx_blocks.end())
    {
        while (rx_blocks.size() >= k_block_max)
        {
            auto oldest = rx_blocks.begin();
            for (auto j = rx_blocks.begin(); j != rx_blocks.end(); ++j)
            {
                if (j->second.first_seen < oldest->second.first_seen)
                {
                    oldest = j;
                }
            }
            decode_fail_++;
            rx_blocks.erase(oldest);
        }
        rx_block_s nb;
        nb.k = k;
        nb.n = n;
        nb.first_seen = now();
        it = rx_blocks.emplace(block_id, std::move(nb)).first;
    }
    else if (it->second.k != k || it->second.n != n)
    {
        decode_fail_++;
        return;
    }
    buf = &it->second;
    if (buf->frags.count(index) != 0)
    {
        return;
    }
    buf->frags.emplace(index,
                       std::vector<uint8_t>(data + k_header_len, data + len));
    if (buf->frags.size() < static_cast<size_t>(k))
    {
        return;
    }
    int rec = 0;
    const bool ok = decode_block(k, n, buf->frags, out, &rec);
    rx_blocks.erase(block_id);
    mark_done(block_id);
    if (!ok)
    {
        decode_fail_++;
        if (out != nullptr)
        {
            out->clear();
        }
        return;
    }
    recovered_ += static_cast<uint64_t>(rec);
    blocks_++;
}

uint64_t rs_block_erasure::take_recovered()
{
    const uint64_t n = recovered_ - recovered_seen_;
    recovered_seen_ = recovered_;
    return n;
}

uint64_t rs_block_erasure::take_decode_fail()
{
    const uint64_t n = decode_fail_ - decode_fail_seen_;
    decode_fail_seen_ = decode_fail_;
    return n;
}
