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
    return k_wifi_payload_max - k_header_len - k_len_prefix;
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
    p_ = n - k;
    timeout_ms_ = timeout_ms;
    encode_matrix_.assign(static_cast<size_t>(n_) * static_cast<size_t>(k_), 0);
    g_tbls_.assign(static_cast<size_t>(k_) * static_cast<size_t>(p_) * 32, 0);
    gf_gen_cauchy1_matrix(encode_matrix_.data(), n_, k_);
    ec_init_tables_base(k_, p_, encode_matrix_.data() + k_ * k_,
                        g_tbls_.data());
    pending_.clear();
    deadline_set_ = false;
    block_id_ = 0;
    rx_blocks_.clear();
    done_order_.clear();
    done_.clear();
    recovered_ = 0;
    blocks_ = 0;
    decode_fail_ = 0;
    oversized_ = 0;
    enabled_ = true;
    return true;
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
    if (k_header_len + shard_len > k_wifi_payload_max)
    {
        return false;
    }
    for (int i = 0; i < k_; i++)
    {
        data_shards[static_cast<size_t>(i)].resize(shard_len, 0);
    }
    std::vector<std::vector<uint8_t>> parity(static_cast<size_t>(p_));
    std::vector<unsigned char*> data_ptrs(static_cast<size_t>(k_));
    std::vector<unsigned char*> coding_ptrs(static_cast<size_t>(p_));
    for (int i = 0; i < k_; i++)
    {
        data_ptrs[static_cast<size_t>(i)] =
            data_shards[static_cast<size_t>(i)].data();
    }
    for (int i = 0; i < p_; i++)
    {
        parity[static_cast<size_t>(i)].assign(shard_len, 0);
        coding_ptrs[static_cast<size_t>(i)] =
            parity[static_cast<size_t>(i)].data();
    }
    WINJECT_EC_ENCODE(static_cast<int>(shard_len), k_, p_,
                      const_cast<unsigned char*>(g_tbls_.data()),
                      data_ptrs.data(), coding_ptrs.data());

    out->clear();
    out->resize(static_cast<size_t>(n_));
    for (int i = 0; i < n_; i++)
    {
        auto& pkt = (*out)[static_cast<size_t>(i)];
        pkt.resize(k_header_len + shard_len);
        const uint8_t flags = i >= k_ ? k_flag_parity : 0;
        pack_header(pkt.data(), block_id, i, k_, n_, flags);
        const uint8_t* body = i < k_
                                  ? data_shards[static_cast<size_t>(i)].data()
                                  : parity[static_cast<size_t>(i - k_)].data();
        memcpy(pkt.data() + k_header_len, body, shard_len);
    }
    return true;
}

int rs_block_erasure::gen_decode_matrix(const uint8_t* err_list, int nerrs,
                                      uint8_t* decode_matrix,
                                      uint8_t* decode_index) const
{
    std::vector<uint8_t> in_err(static_cast<size_t>(n_), 0);
    for (int i = 0; i < nerrs; i++)
    {
        if (err_list[i] >= n_)
        {
            return -1;
        }
        in_err[err_list[i]] = 1;
    }
    std::vector<uint8_t> b(static_cast<size_t>(k_) * static_cast<size_t>(k_));
    std::vector<uint8_t> invert(static_cast<size_t>(k_) *
                                static_cast<size_t>(k_));
    int r = 0;
    for (int i = 0; i < k_; i++, r++)
    {
        while (r < n_ && in_err[static_cast<size_t>(r)])
        {
            r++;
        }
        if (r >= n_)
        {
            return -1;
        }
        memcpy(b.data() + static_cast<size_t>(k_) * static_cast<size_t>(i),
               encode_matrix_.data() +
                   static_cast<size_t>(k_) * static_cast<size_t>(r),
               static_cast<size_t>(k_));
        decode_index[i] = static_cast<uint8_t>(r);
    }
    if (gf_invert_matrix(b.data(), invert.data(), k_) < 0)
    {
        return -1;
    }
    for (int e = 0; e < nerrs; e++)
    {
        const int idx = err_list[e];
        if (idx < k_)
        {
            memcpy(decode_matrix +
                       static_cast<size_t>(k_) * static_cast<size_t>(e),
                   invert.data() +
                       static_cast<size_t>(k_) * static_cast<size_t>(idx),
                   static_cast<size_t>(k_));
            continue;
        }
        for (int i = 0; i < k_; i++)
        {
            uint8_t s = 0;
            for (int j = 0; j < k_; j++)
            {
                s ^= gf_mul(
                    invert[static_cast<size_t>(j) * static_cast<size_t>(k_) +
                           static_cast<size_t>(i)],
                    encode_matrix_[static_cast<size_t>(k_) *
                                       static_cast<size_t>(idx) +
                                   static_cast<size_t>(j)]);
            }
            decode_matrix[static_cast<size_t>(k_) * static_cast<size_t>(e) +
                          static_cast<size_t>(i)] = s;
        }
    }
    return 0;
}

bool rs_block_erasure::decode_block(
    const std::unordered_map<int, std::vector<uint8_t>>& frags,
    std::vector<std::vector<uint8_t>>* payloads, int* recovered) const
{
    if (!enabled_ || payloads == nullptr ||
        frags.size() < static_cast<size_t>(k_))
    {
        return false;
    }
    size_t shard_len = 0;
    for (const auto& kv : frags)
    {
        if (kv.first < 0 || kv.first >= n_)
        {
            return false;
        }
        if (shard_len == 0)
        {
            shard_len = kv.second.size();
        }
        else if (kv.second.size() != shard_len)
        {
            return false;
        }
    }
    if (shard_len < k_len_prefix)
    {
        return false;
    }

    std::vector<std::vector<uint8_t>> data_copy(static_cast<size_t>(k_));
    bool have_all_data = true;
    for (int i = 0; i < k_; i++)
    {
        auto it = frags.find(i);
        if (it == frags.end())
        {
            have_all_data = false;
            continue;
        }
        data_copy[static_cast<size_t>(i)] = it->second;
    }

    int rec = 0;
    if (!have_all_data)
    {
        uint8_t err_list[k_max_n];
        int nerrs = 0;
        for (int i = 0; i < n_; i++)
        {
            if (frags.find(i) == frags.end())
            {
                err_list[nerrs++] = static_cast<uint8_t>(i);
            }
        }
        if (nerrs > p_)
        {
            return false;
        }
        std::vector<uint8_t> decode_matrix(static_cast<size_t>(nerrs) *
                                           static_cast<size_t>(k_));
        uint8_t decode_index[k_max_n];
        if (gen_decode_matrix(err_list, nerrs, decode_matrix.data(),
                              decode_index) != 0)
        {
            return false;
        }
        std::vector<uint8_t> decode_tbls(static_cast<size_t>(k_) *
                                         static_cast<size_t>(nerrs) * 32);
        std::vector<unsigned char*> src_ptrs(static_cast<size_t>(k_));
        std::vector<std::vector<uint8_t>> recover(static_cast<size_t>(nerrs));
        std::vector<unsigned char*> rec_ptrs(static_cast<size_t>(nerrs));
        for (int i = 0; i < k_; i++)
        {
            auto it = frags.find(decode_index[i]);
            if (it == frags.end())
            {
                return false;
            }
            src_ptrs[static_cast<size_t>(i)] =
                const_cast<unsigned char*>(it->second.data());
        }
        for (int i = 0; i < nerrs; i++)
        {
            recover[static_cast<size_t>(i)].assign(shard_len, 0);
            rec_ptrs[static_cast<size_t>(i)] =
                recover[static_cast<size_t>(i)].data();
        }
        ec_init_tables_base(k_, nerrs, decode_matrix.data(),
                            decode_tbls.data());
        WINJECT_EC_ENCODE(static_cast<int>(shard_len), k_, nerrs,
                          decode_tbls.data(), src_ptrs.data(), rec_ptrs.data());
        for (int i = 0; i < nerrs; i++)
        {
            const int idx = err_list[i];
            if (idx < k_)
            {
                data_copy[static_cast<size_t>(idx)] =
                    std::move(recover[static_cast<size_t>(i)]);
                rec++;
            }
        }
    }

    payloads->clear();
    for (int i = 0; i < k_; i++)
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
    pending_.emplace_back(data, data + len);
    if (!deadline_set_)
    {
        deadline_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms_);
        deadline_set_ = true;
    }
    if (static_cast<int>(pending_.size()) >= k_)
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
    if (!enabled_ || out == nullptr || pending_.empty())
    {
        return;
    }
    if (!encode_block(pending_, block_id_, out))
    {
        oversized_++;
        out->clear();
        pending_.clear();
        deadline_set_ = false;
        return;
    }
    block_id_ = static_cast<uint16_t>(block_id_ + 1);
    pending_.clear();
    deadline_set_ = false;
    blocks_++;
}

void rs_block_erasure::on_tick(std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    expire_rx();
    if (!enabled_ || !deadline_set_ || timeout_ms_ <= 0)
    {
        return;
    }
    if (std::chrono::steady_clock::now() < deadline_)
    {
        return;
    }
    flush(out);
}

void rs_block_erasure::expire_rx()
{
    if (rx_blocks_.empty())
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const int hold_ms = std::max(timeout_ms_ * 5, 100);
    const auto hold = std::chrono::milliseconds(hold_ms);
    for (auto it = rx_blocks_.begin(); it != rx_blocks_.end();)
    {
        if (now - it->second.first_seen > hold)
        {
            decode_fail_++;
            it = rx_blocks_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void rs_block_erasure::mark_done(uint16_t block_id)
{
    if (done_.insert(block_id).second)
    {
        done_order_.push_back(block_id);
    }
    while (done_order_.size() > k_done_max)
    {
        done_.erase(done_order_.front());
        done_order_.pop_front();
    }
}

void rs_block_erasure::push_air(const uint8_t* data, size_t len,
                              std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (!enabled_ || out == nullptr || data == nullptr || len == 0)
    {
        return;
    }
    if (data[0] != k_magic)
    {
        out->emplace_back(data, data + len);
        return;
    }
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
    if (k != k_ || n != n_)
    {
        decode_fail_++;
        return;
    }
    if (done_.count(block_id) != 0)
    {
        return;
    }
    rx_block_s* buf = nullptr;
    auto it = rx_blocks_.find(block_id);
    if (it == rx_blocks_.end())
    {
        while (rx_blocks_.size() >= k_block_max)
        {
            auto oldest = rx_blocks_.begin();
            for (auto j = rx_blocks_.begin(); j != rx_blocks_.end(); ++j)
            {
                if (j->second.first_seen < oldest->second.first_seen)
                {
                    oldest = j;
                }
            }
            decode_fail_++;
            rx_blocks_.erase(oldest);
        }
        rx_block_s nb;
        nb.k = k;
        nb.n = n;
        nb.first_seen = std::chrono::steady_clock::now();
        it = rx_blocks_.emplace(block_id, std::move(nb)).first;
    }
    buf = &it->second;
    if (buf->frags.count(index) != 0)
    {
        return;
    }
    buf->frags.emplace(index,
                       std::vector<uint8_t>(data + k_header_len, data + len));
    if (buf->frags.size() < static_cast<size_t>(k_))
    {
        return;
    }
    int rec = 0;
    const bool ok = decode_block(buf->frags, out, &rec);
    rx_blocks_.erase(block_id);
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
    const uint64_t n = recovered_;
    recovered_ = 0;
    return n;
}

uint64_t rs_block_erasure::take_decode_fail()
{
    const uint64_t n = decode_fail_;
    decode_fail_ = 0;
    return n;
}
