# WInject-ESP32 target architecture (replace)

This document is the **implementation-ready target** for the ESP32 radio data path and upstream console. When implemented, it **replaces** the current Addr2-airport / one-datagram-per-MPDU model described in [winject.md](winject.md). Until the firmware lands, [winject.md](winject.md) remains the as-built reference.

**Readiness:** wire format, module contracts, thread model, NVS blob v3, file tree, boot wiring, and Passes 0–9 below are locked for coding. Host manager follow-up stays out of scope.

**Replace notice:** Today’s packed Addr2-airport / one-datagram-per-MPDU model goes away. Addressing is a **bus** (`lcid` / `uint8`): no `src`, no peer airport pair, no `set_winject_id`. On the air: **Addr1||Addr2** pack up to five PDU slots (`bus8` + `size11` each; `size==0` = absent); **Addr3** is mode BSSID + **domain** (`CA:FE:BA:BE:DH:DL` / `BA:DD:CA:FE:DH:DL`). Body is concatenated payloads only — **no LC mux header**. Different flows use different bus ids (e.g. data vs ACK).

Arbitrary Addr1/Addr2 on inject rely on the existing `ieee80211_raw_frame_sanity_check` override (`return 0`) and `-Wl,-z,muldefs`. That is the same hook already used so standalone SA is not the chip STA MAC.

**Non-goals:** Host FEC / PDCP / AM (manager-side). No radiotap on the UDP path. No FCS on the UDP path. Firmware still owns the 802.11 MAC header.

---

## 802.11 frame

IBSS data, `ToDS=0`, `FromDS=0` (unchanged control fields):

```
Offset  Size  Field
0       2     Frame Control  0x0008
2       2     Duration       0x0000
4       6     Addr1          PDU slot bit packing (with Addr2)
10      6     Addr2          PDU slot bit packing (with Addr1)
16      6     Addr3          mode BSSID + domain (see Addr3)
22      2     Sequence       firmware-owned
24      N     Body           concatenated LCP payloads (present slots only)
```

Max inject length and payload ceiling stay as today (`WIFI_RADIO_INJECT_MAX` / `WIFI_PAYLOAD_MAX`, currently 1500 / 1476). Sum of present PDU sizes must equal body length and fit in the body.

### Addr1 || Addr2 — PDU slots (96 bits)

No dedicated count field. Treat `Addr1[0..5] || Addr2[0..5]` as a **96-bit LSB-first** bit stream (bit 0 = LSB of `Addr1[0]`).

Five fixed slots × (`bus` `uint8` + `size` `uint11`) = **95 bits**, plus **1 bit** for Addr1 I/G:

```
emit ig           (1 bit, 1 = group)   # 802.11 Addr1 I/G; avoids TX ACK wait
for slot i = 0..4:
    emit bus[i]   (8 bits, LSB-first)
    emit size[i]  (11 bits, LSB-first)
```

| Field | Meaning |
|-------|---------|
| `ig` | always **1** on TX (group DA). RX ignores. Uses the former trailing spare bit. |
| `bus` | logical-channel / bus id for that PDU (`0` = broadcast). Also called **lcid**. Not a peer radio address — there is no `src` on the air. |
| `size` | payload length in bytes; **`0` = slot absent** (`bus` ignored, no body bytes) |

**Present PDUs** = slots with `size > 0`, in slot order `0…4`. Body = those payloads concatenated. RX: `sum(sizes) == body_len` or drop; each `size ≤ WIFI_PAYLOAD_MAX` and sum ≤ `WIFI_PAYLOAD_MAX`. Max present PDUs per MPDU: **5**.

On-air demux is by PDU **`bus` only**.

**ESP32:** `esp_wifi_80211_tx` sends arbitrary DA/SA. The ROM `ieee80211_raw_frame_sanity_check` would otherwise reject them; this project already replaces that symbol (`return 0`) plus `-Wl,-z,muldefs`. Promiscuous RX still delivers the MPDU (filter is Addr3 domain + bus match). Commodity STAs ignore these addresses; only winject radios in monitor/promiscuous hear them.

`BFC_TUNNEL_DEVICE` uses the same Addr1/Addr2 packing (only Addr3 prefix differs) so combine stays the same. STA eFuse MAC is board identity / `status` only, not on the air.

### Addr3 — mode BSSID + domain

Last two octets = big-endian **domain** (`uint16`). Full 16 bits.

| Mode                 | Addr3 prefix (4 bytes) | Full form                         |
|----------------------|------------------------|-----------------------------------|
| `STANDALONE`         | `CA:FE:BA:BE`          | `CA:FE:BA:BE:DH:DL`               |
| `BFC_TUNNEL_DEVICE`  | `BA:DD:CA:FE`          | `BA:DD:CA:FE:DH:DL`               |

Replace today’s full 6-byte `WIFI_BSSID_*` macros in `config.h` with 4-byte prefixes (e.g. `WIFI_BSSID_PREFIX_STANDALONE` / `WIFI_BSSID_PREFIX_TUNNEL`). Last two octets always come from `domain`.

| Domain | Last two octets | Standalone Addr3           | Tunnel Addr3               |
|--------|-----------------|----------------------------|----------------------------|
| `0x0001` | `00:01`       | `CA:FE:BA:BE:00:01`        | `BA:DD:CA:FE:00:01`        |
| `0x1234` | `12:34`       | `CA:FE:BA:BE:12:34`        | `BA:DD:CA:FE:12:34`        |

Domain `0` is unset / invalid on the air. RX: Addr3 prefix must match mode; domain must equal the local domain (unset or mismatch → drop).

### Header examples (golden vectors)

**One PDU.** Domain `0x1234`, slot 0 `bus=0xB2` `size=3`, slots 1…4 absent, body `AA BB CC`:

```
Addr1  65:07:00:00:00:00
Addr2  00:00:00:00:00:00
Addr3  CA:FE:BA:BE:12:34
Body   AA BB CC
```

**Two PDUs.** Domain `0x1234`, slot 0 `bus=0xB2` `size=2` (`AA BB`), slot 1 `bus=0xC3` `size=3` (`DD EE FF`), slots 2…4 absent:

```
Addr1  65:05:30:3C:00:00
Addr2  00:00:00:00:00:00
Addr3  CA:FE:BA:BE:12:34
Body   AA BB DD EE FF
```

Self-TX frames whose present `bus` ids do not match any local `sur` filter are ignored on RX (no local forward). Frames that do match a `sur` filter are forwarded like any other (no self-TX blackhole).

---

## Domain (`set_domain`)

Replaces today’s notion of a per-radio winject id. **No `set_winject_id`.**

| Command | Alias | Arguments |
|---------|-------|-----------|
| `set_domain` | `sdom` | `<domain>` — 1…65535 (`0` reserved / unset) |
| `unset_domain` | `udom` | (none) — domain unset; TX/RX invalid until set |

**Alias note:** today’s `sd` stays `set_modulation`. Domain uses **`sdom` / `udom`** so the short forms do not collide.

- Written into Addr3 last two octets on every TX (`wifi_tx::set_domain`).
- RX drops frames whose domain ≠ local domain (`wifi_rx::set_domain` / `accept_mpdu`).
- Console/settings fan out to **both** `wifi_tx` and `wifi_rx` atomics.
- Persisted with `save` / `use`.
- `status` prints `domain <n>` or `domain unset`.

---

## Bus (`bus=<lcid>`)

Canonical console key: **`bus`**. Value is one byte (1–2 hex), the logical-channel id (**lcid**). `bus=0` is broadcast.

There is no `src:dst` airport string and no `port=` key.

| Path | Console | Bus role |
|------|---------|----------|
| air → UDP | `set_upstream_rx` / `sur` | **filter** — match present air slots with this bus |
| UDP → air | `set_upstream_tx` / `sut` | **target** — stamp this bus into the air slot |

**Breaking vs today’s firmware aliases:** today’s `sur` was UDP→air and `sut` was air→UDP. Target names follow the **upstream/host** words: `upstream_rx` = host receives (air→UDP), `upstream_tx` = host transmits (UDP→air). Radio-side class names stay `lc_rx_*` / `lc_tx_*`.

On TX (`sut`):

- Fill one Addr1/Addr2 slot with `bus = bind.bus`, `size = payload_len` (combine may fill more slots)
- Addr3 ← mode prefix + local domain
- Body ← payload(s) only

**RX matching (`sur`)**

1. Addr3 prefix matches mode BSSID; domain equals local domain.
2. Unpack five slots from Addr1||Addr2; `sum(sizes) == body_len`.
3. For each present slot, look up `sur` bindings and forward that blob to each matching UDP dest.

**Bus match (exact, not wildcard):**

| Air slot `bus` | Matches `sur` bind |
|----------------|--------------------|
| `N` (1…255) | only `bind.bus == N` |
| `0` (broadcast) | only `bind.bus == 0` |

A broadcast air slot does **not** fan out to every `sur` bind. A non-zero air bus does **not** match a `bus=0` bind.

**Example — two radios, one data bus `b2` A→B and return bus `a1` B→A**

Domain `0x1234`:

```
# A (sends on bus b2, receives on bus a1)
set_domain 1234
set_upstream_tx bus=b2 9000
set_upstream_rx bus=a1 192.168.32.10 9001

# B (sends on bus a1, receives on bus b2)
set_domain 1234
set_upstream_tx bus=a1 9000
set_upstream_rx bus=b2 192.168.32.11 9001
```

Air A→B: one present slot `bus=b2`, body = LCP. B’s `sur bus=b2` matches and UDP-forwards to `…:9001`.

**Example — TCP pair (four buses)**

Data and ACK are separate bus ids:

| Direction | Bus |
|-----------|-----|
| A data → B data | `B_data` |
| B ack → A ack | `A_ack` |
| B data → A data | `A_data` |
| A ack → B ack | `B_ack` |

---

## PDU combine (`wifi_tx`)

Body after the 24-byte MAC header is **payloads only** (no mux header).

### Example — one PDU

`bus=0xB2`, payload `AA BB CC` → `size=3`. Slots 1…4 empty (`size=0`).

```
body:   AA BB CC
```

Addr3 = mode + domain (e.g. standalone `CA:FE:BA:BE:12:34`).

### Example — two PDUs

Slot 0: `bus=0xB2`, `size=2`, payload `AA BB`  
Slot 1: `bus=0xC3`, `size=3`, payload `DD EE FF`  
Slots 2…4: `size=0`

```
body:   AA BB DD EE FF
```

### TX combine rules (`wifi_tx`)

1. Pop `lc_tx` head. That packet is the **output** buffer (already has 802.11 headroom).
2. Slot 0 ← `{bus=bus_id, size=out.size()}`. Present count = 1.
3. While present count `< 5`, queue non-empty, and `out.size() + next_payload ≤ WIFI_PAYLOAD_MAX`, pop next donor (any `bus`), **memcpy** its payload onto the tail of the first packet, fill the next free slot with `{bus=donor.bus, size=donor.size()}`. Donor dtor → TX pool.
4. Pack five slots into Addr1||Addr2 (empty slots `size=0`); stamp Addr3 = mode + domain; write 802.11 header in headroom (first payload does not move). Inject from `data()` after lowering offset. On `ENOMEM`, retry in the send loop (existing inject-retry spirit).

Solo send (one present slot): step 3 is skipped; payload is never copied.

---

## Data-plane modules

```
TX:  lc_tx_endpoint → lc_tx → wifi_tx
RX:  lc_rx_endpoint ← lc_rx ← wifi_rx
CI:  channel_info_endpoint ← (lc_tx | wifi_rx)   // subscribers; not on the data path
```

These replace `upstream_rx`, `upstream_tx`, and the current `frame` airport SA tables. `wifi` still owns PHY. **Domain** is set on both `wifi_tx` and `wifi_rx` (`set_domain`, atomic per side); console/settings fan out to both. No winject id.

Module names follow the **radio**. Console aliases follow the **host/upstream** words (and **swap** today’s firmware `sur`/`sut` roles):

| Direction | New | Today’s class | Console (target) | Bus role |
|-----------|-----|---------------|------------------|----------|
| UDP → air (inject) | `lc_tx_endpoint` → `lc_tx` → `wifi_tx` | `upstream_rx` | `sut` / `set_upstream_tx` | target |
| air → UDP (forward) | `wifi_rx` → `lc_rx` → `lc_rx_endpoint` | `upstream_tx` | `sur` / `set_upstream_rx` | filter |

Shapes below are the public/contract surface. Guided lock: types first, then one class at a time.

### Types

```cpp
using bus_t = uint8_t;  // lcid; 0 = broadcast

inline bool bus_is_broadcast(bus_t b);  // b == 0

static constexpr uint8_t WIFI_PDU_SLOTS = 5;

struct pdu_slot_t {
    bus_t bus;       // lcid on the air
    uint16_t size;   // 11-bit value; 0 = absent
};

// host = IPv4, network byte order (today’s upstream_dest_s.host)
struct ip_port_t {
    uint32_t host;
    uint16_t port;
};

class packet_allocator;

// RAII handle over one WIFI_TX_PACKET_CAP buffer from packet_allocator.
// Move transfers the handle. share() adds another handle to the same buffer
// (refcount). Only the last destructor / reset() returns the buf to the pool.
// All constructors are private; only allocate() / share() / move create handles.
class packet {
public:
    packet(packet&&) noexcept;
    packet& operator=(packet&&) noexcept;
    ~packet();                   // drop ref; last ref → allocator; no-op if invalid

    packet(const packet&) = delete;
    packet& operator=(const packet&) = delete;

    packet share() const;        // invalid → invalid; else refs++ , same buf,
                                 // copies offset/size window (independent after)

    void set_packet_offset(size_t offset);
    void set_packet_size(size_t size);
    void reset();                // drop ref now; becomes invalid

    bool is_valid() const;       // buf && offset + size <= capacity
    uint8_t* data();             // buf + offset
    const uint8_t* data() const;
    size_t size() const;
    size_t offset() const;
    size_t capacity() const;     // always WIFI_TX_PACKET_CAP when valid

private:
    friend class packet_allocator;
    friend class lc_tx;          // queue ring + invalid pop; still not public
    friend class lc_rx;          // queue ring + invalid pop; still not public
    packet() = default;          // invalid
    packet(packet_allocator& alloc, uint8_t* buf, size_t capacity);  // refs = 1

    packet_allocator* alloc_ = nullptr;
    uint8_t* buf_ = nullptr;
    size_t capacity_ = 0;
    size_t offset_ = 0;
    size_t size_ = 0;
};

// Two globals: packet_allocator::tx() and packet_allocator::rx().
// Every allocate() is a WIFI_TX_PACKET_CAP-byte buffer.
class packet_allocator {
public:
    static packet_allocator& tx();
    static packet_allocator& rx();

    static constexpr size_t k_buf_size = WIFI_TX_PACKET_CAP;  // 1500

    bool init(size_t count);
    packet allocate();           // never blocks; invalid packet if pool empty
    size_t available() const;    // free slots (refcount==0)
    void set_on_space(bfc::light_function<void()> cb);  // after last-ref deallocate

private:
    friend class packet;
    void add_ref(uint8_t* buf);
    void release(uint8_t* buf);  // last ref → free slot + on_space
};
```

No public constructors. `packet p;` is a compile error. Obtain a packet from `allocate()`, `share()`, or move. Empty pool → `allocate()` still returns a packet, `is_valid() == false`. `capacity()` is the whole buffer, not tailroom. Writable at `data()` is `capacity() - offset()`. Setters store the value; `is_valid()` is the check.

**Sharing:** each pooled buffer has a refcount. `allocate()` → ref 1. `share()` → ref++. Move does not change the count. `~packet` / `reset()` → ref--; **only the last** returns the buffer to the free list. Each handle has its own `offset_` / `size_` window into the same `buf_` (RX demux: one MPDU handle, per-PDU shares with tighter windows). TX path can stay single-ref (no `share()`).

Two allocators, both `WIFI_TX_PACKET_CAP` slots. `tx()` feeds `lc_tx_endpoint` / `lc_tx` / `wifi_tx`. `rx()` feeds `wifi_rx` / `lc_rx`. A packet always returns to the allocator that issued it. Init counts: TX `WIFI_RADIO_TX_QUEUE` (16), RX `WIFI_RADIO_RX_QUEUE` (16) — lock with those modules.

#### Reactors

Every data-plane object owns a reactor (no shared netmgr/console reactor). The reactor creates and **pins** its FreeRTOS task. Add to both `bfc::select_reactor` and `bfc::task_reactor`:

```cpp
bool start_pinned(const char* name,
                  BaseType_t core,      // 0, 1, or tskNO_AFFINITY
                  UBaseType_t prio,
                  uint32_t stack_bytes = 6144);
```

`start_pinned` is `xTaskCreatePinnedToCore` then `run()`. Do not also call `run()` from another task. Existing `stop()` leaves the loop; the pinned task then exits. `wake_up()` is unchanged.

Default pins (overridable at `start`):

| Owner | Reactor | Default core | Default prio |
|-------|---------|--------------|--------------|
| `lc_tx_endpoint` | `select_reactor` | CPU1 | `UPSTREAM_TASK_PRIO` (18) |
| netmgr + console | `select_reactor` | CPU1 | `NETMGR_TASK_PRIO` (6) |
| `wifi_tx` | pinned `run()` loop (not select) | CPU0 | `WIFI_RADIO_TASK_PRIO` (20) |
| `lc_rx` | `task_reactor` | CPU1 | `UPSTREAM_TASK_PRIO` (18) |
| `channel_info_endpoint` | `task_reactor` | CPU1 | `NETMGR_TASK_PRIO` (6) |

#### TX buffer / zero-copy

PDU metadata lives in Addr1/Addr2, so headroom is MAC only:

```
WIFI_TX_HEADROOM    = WIFI_HDR_LEN                      // 24
WIFI_TX_PACKET_CAP  = WIFI_TX_HEADROOM + WIFI_PAYLOAD_MAX // 1500
```

Layout of a pooled TX buffer:

```
[0, HEADROOM)              reserved (802.11 header)
[HEADROOM, HEADROOM+size)  LCP payload — UDP recv writes here
```

- `lc_tx_endpoint`: `packet p = packet_allocator::tx().allocate()` (invalid → don’t recv), `set_packet_offset(WIFI_TX_HEADROOM)`, recv into `data()`, `set_packet_size(n)`, move `p` onto `lc_tx`. No memcpy of the datagram.
- Solo TX: write 802.11 at offset 0 (headroom), pack one present slot into Addr1/Addr2, `set_packet_offset(0)` / `set_packet_size(24+payload)`, inject `data()`/`size()`. Payload bytes stay put. Packet dtor returns the buffer after inject.
- Merge: first packet is the output; memcpy later LCP payloads onto its tail; stamp Addr1/Addr2/Addr3 in headroom (first payload still does not move). Donor `packet`s leave scope → allocator. First packet dtor after inject.
- Recv cap: `WIFI_PAYLOAD_MAX` (1476). Combine enforces `sum(sizes) ≤ WIFI_PAYLOAD_MAX` and present slots ≤ 5.

RX uses `packet_allocator::rx()` (same `WIFI_TX_PACKET_CAP` `packet`). Copy from the ESP promiscuous buffer is unavoidable — that buffer is recycled when the CB returns. `wifi_rx` does that copy; `lc_rx` unpacks (Pass 6).

### `lc_tx_endpoint`

UDP → air (inject). Console **`sut` / `set_upstream_tx`** (host TX onto the bus). One bound UDP socket per bus. Owns a `select_reactor` (pinnable). Datagram body is one LCP; this class never writes 802.11 or Addr packing.

```cpp
struct lc_tx_bind_s {
    bus_t bus;           // target lcid stamped on air
    uint16_t udp_port;
    bool socket_open;
};

class lc_tx_endpoint {
public:
    static lc_tx_endpoint& instance();
    lc_tx_endpoint(const lc_tx_endpoint&) = delete;
    lc_tx_endpoint& operator=(const lc_tx_endpoint&) = delete;

    bool init(lc_tx& tx);
    bool start(BaseType_t core = APP_TASK_CORE,
               UBaseType_t prio = UPSTREAM_TASK_PRIO,
               uint32_t stack_bytes = 6144);
    // reactor_.start_pinned("lc_tx_ep", core, prio, stack_bytes)

    bool add_endpoint(bus_t bus, uint16_t udp_port);
    bool rem_endpoint(bus_t bus);
    bool load(const lc_tx_bind_s* binds, uint8_t count);
    void fill_status(lc_tx_bind_s* out, uint8_t* count);

private:
    using reactor_t = bfc::select_reactor<>;

    lc_tx_endpoint() = default;

    struct entry_s {
        bool used = false;
        bus_t bus = 0;
        uint16_t udp_port = 0;
        bfc::socket sock;
    };

    void on_readable(entry_s& e);
    // packet p = packet_allocator::tx().allocate();
    // if (!p.is_valid()) return;          // leave UDP queued; do not recv
    // p.set_packet_offset(WIFI_TX_HEADROOM);
    // n = e.sock.recv(p.data(), p.capacity() - p.offset());
    // if (n <= 0 || n > WIFI_PAYLOAD_MAX) return;  // p dtor → pool
    // p.set_packet_size(n);
    // tx_.tx(e.bus, std::move(p));

    entry_s ep_[WIFI_AIRPORT_MAX];  // max bus binds (name kept)
    lc_tx* tx_ = nullptr;
    reactor_t reactor_;
    bfc::semaphore lock_;  // ep_[] vs console; not held across recv/tx
};
```

Bind is the **local** UDP listen port only (`sut bus=<lcid> <udp_port>`). Socket binds `INADDR_ANY`. Same bus replaces and rebinds. Same UDP port on another bus steals the port (today’s inject path).

No `ethernet&`. Today `upstream_rx` uses it only for `connected()` and `local_ipv4()` so inject could bind the Ethernet unicast address. That coupling is unnecessary: `0.0.0.0` still receives unicast to the device, and `set_ip` does not require a rebind. Link down → recv fails / no datagrams; `add_endpoint` can fail if lwIP bind fails.

**Allocate, then recv.** If the pool is empty, do not `recv` — the datagram stays in the socket. Avoids the current “consume UDP then drop” copy. Oversized LCP: drop the packet (dtor → pool), do not enqueue.

Never block. `add_endpoint` false = table full / bind fail.

`lc_tx::tx(bus_t, packet&&)` is the only downstream call; its shape is Pass 3. It must be wait-free (this reactor vs `wifi_tx` on another core/task).

#### Thread

Each module has its own reactor; this one is a `select_reactor`. Pin is a **start argument**, not a hard-coded core:

```
start();                              // default CPU1 / prio 18 / 6144
start(WIFI_RADIO_TASK_CORE, 20);      // allowed — pin wherever it fits
```

| | |
|--|--|
| Reactor | owned `select_reactor` |
| Default pin | CPU1 (`APP_TASK_CORE`), `UPSTREAM_TASK_PRIO` (18), stack 6144 |
| I/O | `add_read_rdy(fd, on_readable)` / `rem_read_rdy` on add/rem |
| Table | `lock_` for `ep_[]` (console `add`/`rem`/`fill_status` vs reactor). Do not hold it across `recv` or `lc_tx.tx`. |
| Backpressure | empty TX pool → skip `recv`. `packet_allocator::tx().set_on_space` wakes this reactor. |

`on_readable` runs on this reactor thread only. Console (netmgr reactor) calls `add`/`rem`/`load`/`fill_status` under `lock_`.

Not chosen: share `manager::reactor()`. Not chosen: hand-rolled `select` like `upstream_rx::run()`.

### `lc_tx`

UDP→air queue. No reactor — wait-free producer (`lc_tx_endpoint` on its reactor) and blocking consumer (`wifi_tx` on its task). Does **not** own the allocator; uses global `packet_allocator::tx()`. Owns a ring of `{bus, packet}` of depth `k_queue_cap`.

```cpp
class lc_tx {
public:
    static constexpr uint8_t k_queue_cap = WIFI_RADIO_TX_QUEUE;  // 16

    static lc_tx& instance();
    lc_tx(const lc_tx&) = delete;
    lc_tx& operator=(const lc_tx&) = delete;

    bool init();                 // ring of invalid packets; allocator already inited
    void set_channel_info(channel_info_endpoint& ci);  // optional; Pass 8

    // Wait-free. Move pkt onto the queue. false = drop (queue full): pkt dtor
    // returns the buffer to packet_allocator::tx(); drop_count_++.
    // Still on_flow_ctrl_info if queue_size > k_queue_cap/2.
    bool tx(bus_t bus, packet&& pkt);

    // wifi_tx only. peek is non-blocking (bus + payload size, no move).
    // pop moves the packet out; wait may block. Timeout → invalid packet.
    bool peek(bus_t* bus, uint16_t* payload_size) const;
    packet pop(bus_t* bus, TickType_t wait);

    uint8_t queue_size() const;
    uint8_t queue_capacity() const { return k_queue_cap; }
    uint32_t drop_count() const;

private:
    lc_tx() = default;

    void publish_flow_ctrl();
    // if (!ci_ || queue_size() <= k_queue_cap / 2) return;
    // ci_->on_flow_ctrl_info(queue_size(), k_queue_cap);

    struct slot_s {
        bus_t bus = 0;
        packet pkt;              // lc_tx is friend: default = invalid
    };

    slot_s q_[k_queue_cap];
    // + FreeRTOS index queue (or equivalent) for filled slots
    channel_info_endpoint* ci_ = nullptr;
};
```

TX allocator depth **equals** queue cap (16), inited once at boot (`packet_allocator::tx().init(16)`). Then “pool empty” and “queue full” are the same backpressure: `allocate()` fails, endpoint does not `recv`. A larger TX pool would let the endpoint recv into a packet it then fails to `tx()` — consume-then-drop, which we are avoiding.

No per-bus queues. Occupancy is global; `on_flow_ctrl_info` (Pass 8) runs only when `queue_size > k_queue_cap / 2`, and fans out to all CI subscribers. On CI `sendto` failure, drop and wait for the next qualifying update.


`peek` exists so `wifi_tx` can combine: pop head (output packet), while present slots `< 5` and body still fits, `pop` donor (any `bus`), memcpy onto first, fill next Addr slot, donor leaves scope → TX pool. `pop` may block (`portMAX_DELAY`); `tx()` never does.

`lc_tx` is a `friend` of `packet` only so the ring can hold move-only packets and `pop` can return invalid on timeout. Callers still cannot write `packet p;`.

#### Thread

None. `tx` runs on the `lc_tx_endpoint` reactor. `peek` / `pop` run on `wifi_tx`. Cross-core: wait-free post, blocking take (same spirit as today’s `packet_queue`). Endpoint `allocate()` is `packet_allocator::tx().allocate()` on that same reactor.

### `wifi_tx`

Still a `wifi` member (PHY + inject). No MPDU pool and no UDP `take`/`post`/`inject` — the first LCP packet **is** the inject buffer. Consumes `lc_tx` only.

```cpp
class wifi_tx {
    friend class wifi;

public:
    wifi_tx(const wifi_tx&) = delete;
    wifi_tx& operator=(const wifi_tx&) = delete;

private:
    explicit wifi_tx(wifi& radio);

    bool init(lc_tx& tx);
    bool start(BaseType_t core = WIFI_RADIO_TASK_CORE,
               UBaseType_t prio = WIFI_RADIO_TASK_PRIO,
               uint32_t stack_bytes = 6144);
    // xTaskCreatePinnedToCore → run()  (same pin knobs as reactor.start_pinned)

    void run();
    // bus_t bus;
    // packet out = tx_.pop(&bus, portMAX_DELAY);
    // if (!out.is_valid()) continue;
    // pdu_slot_t slots[WIFI_PDU_SLOTS] = {};
    // slots[0] = {bus, (uint16_t)out.size()}; n=1;
    // while (n<WIFI_PDU_SLOTS && tx_.peek(&next, &nsz)
    //        && out.size()+nsz <= WIFI_PAYLOAD_MAX) {
    //     packet donor = tx_.pop(&dbus, 0);
    //     memcpy(out.data()+out.size(), donor.data(), donor.size());
    //     out.set_packet_size(out.size()+donor.size());
    //     slots[n] = {dbus, (uint16_t)donor.size()}; n++;
    // }  // donor dtor → packet_allocator::tx()
    // stamp 802.11 at offset 0; pack slots into Addr1/Addr2; Addr3=domain;
    // out.set_packet_offset(0); out.set_packet_size(24+payloads);
    // inject_retry(out.data(), out.size());  // ENOMEM: keep out, delay, retry
    // out dtor → packet_allocator::tx()

    bool inject_retry(const uint8_t* frame, size_t len);
    void stamp(packet& out, const pdu_slot_t slots[WIFI_PDU_SLOTS]);
    // Addr3 = mode prefix + domain_; domain_==0 → do not inject (drop / skip)

    // Console `set_domain` / `unset` (0). Atomic — run()/stamp reads with no lock.
    bool set_domain(uint16_t domain);  // 0 = unset; non-zero = 1…65535
    uint16_t domain() const;

    // PHY — unchanged from today
    bool apply_power();
    bool apply_cca();
    bool apply_tx_done_cb();
    void fill_status(wifi_status_s* status);  // tx_queue from lc_tx.queue_size()
    bool set_cca_enabled(bool enabled);
    bool set_tx_power(int8_t dbm);

    wifi& radio_;
    lc_tx* tx_ = nullptr;
    std::atomic<uint16_t> domain_{0};  // 0 = unset
};
```

No `packet_pool` / `packet_queue` on this class. `wifi::take_tx` / `post_tx` / `inject` go away.

`set_domain` / `domain`: same contract as `wifi_rx`. Each side keeps its own atomic. Console / settings (or a thin `wifi::set_domain` facade) **must set both** `wifi_tx` and `wifi_rx` so Addr3 TX stamp and RX accept stay in lockstep. No shared `frame` domain store.

`stamp`: pack `slots[0..4]` into Addr1||Addr2 (I/G=1 then five bus/size fields); Addr3 = mode prefix + `domain_`; sequence firmware-owned. Domain unset (`0`) → do not inject that packet (dtor → pool; count). 802.11 header sits in the 24-byte headroom immediately before the first payload (which never moves). Inject from `out.data()` after lowering offset.

`inject_retry`: same as today’s send loop — `esp_wifi_80211_tx`, `ESP_ERR_NO_MEM` keeps the packet and retries (do not hold `radio_.lock()` across `vTaskDelay`). Other errors: limited retries then count `udp_tx_failed` and drop the packet (dtor → pool). Yield occasionally so the CPU0 idle task can pet the WDT (today’s MCS7 note).

#### Thread

Pinned **send loop**, not a `select_reactor` (no sockets; it blocks on `lc_tx.pop`). Same pin API as reactors:

```
start();                                 // default CPU0 / prio 20 / 6144
start(APP_TASK_CORE, UPSTREAM_TASK_PRIO); // allowed
```

`run()` is the only context that `peek`/`pop`s. Combine is opportunistic: not enough tailroom or already 5 present slots → send as-is (one present slot is zero-copy). Different `bus` values **do** share an MPDU.

### `wifi_rx`

Still a `wifi` member (PHY + promiscuous). No owned pool/queue and no `pop` memcpy to a caller buffer — CB allocates from `packet_allocator::rx()`, copies the MPDU once, and wait-free posts into `lc_rx`. Slot unpack / bus demux / UDP are **not** here (Pass 6 `lc_rx` / endpoint).

```cpp
class wifi_rx {
    friend class wifi;

public:
    wifi_rx(const wifi_rx&) = delete;
    wifi_rx& operator=(const wifi_rx&) = delete;

private:
    explicit wifi_rx(wifi& radio);

    bool init(lc_rx& rx);
    bool apply_monitor();  // promiscuous filter + rx_cb; same FCSFAIL/MISC as today
    void fill_status(wifi_status_s* status);
    bool set_allow_failed_crc(bool allow);
    void note_udp_fwd_pkt();  // status counter; called from forward path

    // Console `set_domain` / `unset` (0). Atomic — CB reads with no lock.
    bool set_domain(uint16_t domain);  // 0 = unset; non-zero = 1…65535
    uint16_t domain() const;           // 0 = unset

    static void promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type);
    void on_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type);
    // if type not DATA/MISC → return
    // strip FCS; reject ampdu_cnt>1 / oversized / too short (today)
    // if (!accept_mpdu(payload, len)) return;   // Addr3 prefix+domain only
    // count rx; note_air; CRC-fail → count; drop unless allow_failed_crc
    // packet p = packet_allocator::rx().allocate();
    // if (!p.is_valid()) { drop++; return; }
    // p.set_packet_offset(0);
    // memcpy(p.data(), payload, len); p.set_packet_size(len);
    // if (!rx_->rx(std::move(p))) drop++;   // queue full; pkt dtor → pool

    bool accept_mpdu(const uint8_t* mpdu, size_t len) const;
    // Addr3[0..3] == mode prefix; Addr3[4..5] BE == domain_ (0 → false).
    // No Addr1/Addr2 unpack here.

    void note_air(const wifi_pkt_rx_ctrl_t& ctrl);
    // also channel_info_endpoint::instance().on_rx_air_info(rssi, snr); // atomics only

    wifi& radio_;
    lc_rx* rx_ = nullptr;
    std::atomic<uint16_t> domain_{0};  // 0 = unset
    // counters / CRC / air stats — same spirit as today
};
```

No `packet_pool` / `packet_queue` on this class. `wifi::pop_rx` / `take_rx` go away. RX allocator depth **equals** `lc_rx` queue cap (16), inited once at boot (`packet_allocator::rx().init(16)`).

`set_domain` / `domain`: CB filter input. Same shape as `wifi_tx::set_domain`. `0` = unset → `accept_mpdu` always false (no allocate). Console `sdom` / `udom` and settings `use` set **both** tx and rx (facade ok). Store is atomic so the CB never takes `radio_.lock()`. Mode still selects the Addr3 **prefix** via `set_mode` / `frame`; prefix **values** change to `CA:FE:BA:BE` / `BA:DD:CA:FE` (see Addr3). Last two octets are this domain.

`accept_mpdu` is the CB’s only winject filter: mode Addr3 prefix + `domain_`. Foreign / wrong-domain frames never take a pool slot. Slot integrity (`sum(sizes) == body_len`, per-slot size caps) and bus match are Pass 6 — keep the CB short.

Layout of an RX `packet`: full MPDU at offset 0 (`[0,24)` = 802.11 header, rest = concatenated payloads). No TX-style headroom. `WIFI_TX_PACKET_CAP` (1500) is enough after FCS strip; our inject ceiling is the same.

`lc_rx::rx(packet&&)` is the only downstream call; its shape is Pass 6. It must be wait-free (promiscuous CB on CPU0 vs `lc_rx` reactor).

#### Thread

None owned. `on_promiscuous` runs on the ESP promiscuous callback (CPU0). No `start` / `run`. Do not block, do not `vTaskDelay`, do not take console locks.

| | |
|--|--|
| Context | ESP promiscuous CB (CPU0) |
| Work | Addr3 accept → allocate → memcpy MPDU → `lc_rx.rx` |
| Backpressure | empty RX pool or `lc_rx.rx` false → drop MPDU, count |

### `lc_rx`

Air→UDP queue + demux. Wait-free producer (`wifi_rx` CB) and `task_reactor` consumer. Does **not** own the allocator; uses global `packet_allocator::rx()`. Owns a ring of MPDU `packet`s of depth `k_queue_cap`. Unpacks Addr1||Addr2, validates body, demuxes into `lc_rx_endpoint` by **bus** (filter). No sockets here.

```cpp
class lc_rx {
public:
    static constexpr uint8_t k_queue_cap = WIFI_RADIO_RX_QUEUE;  // 16

    static lc_rx& instance();
    lc_rx(const lc_rx&) = delete;
    lc_rx& operator=(const lc_rx&) = delete;

    bool init();  // ring of invalid packets; allocator already inited
    bool start(BaseType_t core = APP_TASK_CORE,
               UBaseType_t prio = UPSTREAM_TASK_PRIO,
               uint32_t stack_bytes = 6144);
    // reactor_.start_pinned("lc_rx", core, prio, stack_bytes)

    void set_endpoint(lc_rx_endpoint& ep);  // required before traffic

    // Wait-free. From wifi_rx CB. Move MPDU onto the queue. false = full:
    // pkt dtor → drop ref (last → pool); drop_count_++.
    bool rx(packet&& pkt);

    uint8_t queue_size() const;
    uint8_t queue_capacity() const { return k_queue_cap; }
    uint32_t drop_count() const;     // queue-full drops (CB path)
    uint32_t bad_mpdu_count() const; // unpack / size-sum failures

private:
    using reactor_t = bfc::task_reactor<>;

    lc_rx() = default;

    void on_wake();
    // while ((packet mpdu = pop(0)).is_valid()) handle_mpdu(std::move(mpdu));

    void handle_mpdu(packet&& mpdu);
    // if size < WIFI_HDR_LEN → bad++; return
    // unpack five pdu_slot_t from Addr1||Addr2 (skip I/G bit)
    // body_len = mpdu.size()-24
    // if sum(sizes) != body_len or any size > WIFI_PAYLOAD_MAX → bad++; return
    // off = 24
    // for slot in present (size>0):
    //   packet pdu = mpdu.share();
    //   pdu.set_packet_offset(off);
    //   pdu.set_packet_size(slot.size);
    //   ep_->forward(slot.bus, std::move(pdu));  // lookup + UDP inside
    //   off += slot.size
    // // mpdu leaves scope → drop ref; PDUs keep buf until their last dtor

    packet pop(TickType_t wait);  // friend-invalid on timeout; reactor only

    struct slot_s {
        packet pkt;  // lc_rx is friend: default = invalid
    };

    slot_s q_[k_queue_cap];
    // + FreeRTOS index queue (or equivalent) for filled slots
    lc_rx_endpoint* ep_ = nullptr;
    reactor_t reactor_;
};
```

RX allocator depth **equals** queue cap (16). Same backpressure story as TX: empty pool and full queue are one limit. `wifi_rx` never copies unless `allocate()` succeeds and `rx()` will be attempted; if `rx()` fails the packet returns to the pool in the CB.

**Unpack in the reactor, not the CB.** Addr3 already accepted. Here: five `(bus,size)` from Addr1||Addr2; `sum(sizes) == body_len`; each `size ≤ WIFI_PAYLOAD_MAX`. Fail → `bad_mpdu_count_++`, drop MPDU.

**Shared PDU windows.** For each present slot, `mpdu.share()` then tighten offset/size to that LCP. `forward(slot.bus, packet&&)` (Pass 7) looks up `sur` filters and UDP-sends. Last outstanding handle returns the buffer to `packet_allocator::rx()`.

**Demux key is `bus` (lcid).** `lc_rx` does not own the bind table — it passes the air slot’s `bus` into the endpoint. No match → `forward` no-ops (packet dtor drops the share).

`lc_rx` is a `friend` of `packet` only so the ring can hold packets and `pop` can return invalid. Callers still cannot write `packet p;`.

`lc_rx_endpoint::forward(bus_t bus, packet&& pdu)` is the only downstream call; its shape is Pass 7.

#### Thread

Owns a `task_reactor` (no sockets — wake-driven drain). Pin is a start argument:

```
start();                              // default CPU1 / prio 18 / 6144
start(WIFI_RADIO_TASK_CORE, 20);      // allowed
```

| | |
|--|--|
| Reactor | owned `task_reactor` |
| Default pin | CPU1 (`APP_TASK_CORE`), `UPSTREAM_TASK_PRIO` (18), stack 6144 |
| Produce | `wifi_rx` CB: `rx(packet&&)` wait-free, then `reactor_.wake_up()` |
| Consume | `on_wake` / drain: non-blocking `pop` until empty; `handle_mpdu` |
| Table | no endpoint table here — `lc_rx_endpoint` owns binds (console vs forward locking is Pass 7) |

Not chosen: pinned blocking `run()` like `wifi_tx` (works, but doc standardizes reactors for non-PHY modules). Not chosen: demux inside the promiscuous CB.

### `lc_rx_endpoint`

Air → UDP (forward). Console **`sur` / `set_upstream_rx`**. Filter table keyed by `bus` (lcid) → host UDP dest. No reactor — `forward` runs on the `lc_rx` reactor thread. One shared UDP send socket. Never writes 802.11.

```cpp
struct lc_rx_bind_s {
    bus_t bus;           // filter lcid
    ip_port_t dest;      // host = IPv4 network order; port = host UDP
    bool active;
};

class lc_rx_endpoint {
public:
    static lc_rx_endpoint& instance();
    lc_rx_endpoint(const lc_rx_endpoint&) = delete;
    lc_rx_endpoint& operator=(const lc_rx_endpoint&) = delete;

    bool init();
    // no start() — invoked via forward() on lc_rx's reactor

    bool add_endpoint(bus_t bus, ip_port_t dest);
    bool rem_endpoint(bus_t bus);
    bool load(const lc_rx_bind_s* binds, uint8_t count);
    void fill_status(lc_rx_bind_s* out, uint8_t* count);

    // From lc_rx only. Match sur binds with exact bus equality
    // (air bus N → bind.bus==N; air bus 0 → bind.bus==0 only).
    // For each match, sendto(dest, pdu.data(), pdu.size()).
    // No match → no-op. pdu dtor (last ref) → packet_allocator::rx().
    // Never blocks on allocate; may briefly block in sendto (non-blocking socket
    // → EAGAIN drops that dest, count, keep going).
    void forward(bus_t bus, packet&& pdu);

private:
    lc_rx_endpoint() = default;

    struct entry_s {
        bool used = false;
        bus_t bus = 0;
        ip_port_t dest{};
    };

    bool ensure_socket();
    void send_one(const ip_port_t& dest, const uint8_t* data, size_t len);

    entry_s ep_[WIFI_AIRPORT_MAX];
    bfc::socket send_sock_;      // one UDP socket, INADDR_ANY ephemeral
    bfc::semaphore lock_;        // ep_[] vs console; not held across sendto
};
```

Bind is **filter bus + UDP dest** (`sur bus=<lcid> <host> <udp_port>`). Same bus replaces. Two rows with the same bus and different dests → `forward` fan-outs to both.

No `ethernet&`. Open one non-blocking UDP send socket on first use (`INADDR_ANY`, port 0). Link down → `sendto` fails; count and drop that dest’s send (packet still released).

**`forward` contract**

1. Under `lock_`, collect matching dests into a small stack array; unlock.
2. `sendto` each (same `pdu.data()` / `size()`); note fwd counter on success (`wifi_rx::note_udp_fwd_pkt` or local).
3. `pdu` leaves scope → drop share.

Sync send is the default (same spirit as today’s `upstream_tx`). Shared `packet` allows a later Pass to queue PDUs for write-ready without copying payload; not required now.

Never call `forward` from the promiscuous CB. Console `add`/`rem`/`load`/`fill_status` take `lock_`.

#### Thread

None owned. Runs on `lc_rx`’s `task_reactor`.

| | |
|--|--|
| Caller | `lc_rx::handle_mpdu` only |
| I/O | one shared non-blocking UDP send socket |
| Table | `lock_` for `ep_[]` (console vs `forward`). Do not hold across `sendto`. |

Not chosen: own `select_reactor` just for send (no listen fds). Not chosen: per-bind sockets.

### `channel_info_endpoint`

UDP out-of-band **channel info** to the host. Console **`suc` / `set_upstream_ci`**. **Subscriber** list of `ip_port_t` (not keyed by bus) — every CI datagram goes to every subscriber. Owns a **`task_reactor`** (timer for periodic RX air). One shared non-blocking UDP send socket. Not on the air.

```cpp
enum channel_info_type_e : uint8_t {
    E_CHANNEL_INFO_TYPE_FLOW_CTRL = 1,
    E_CHANNEL_INFO_TYPE_RX_AIR    = 2,
};

struct tx_flow_ctrl_s {
    uint8_t info_type;          // E_CHANNEL_INFO_TYPE_FLOW_CTRL
    uint8_t tx_queue_size;      // global lc_tx occupancy
    uint8_t tx_queue_capacity;  // lc_tx::k_queue_cap
} __attribute__((packed));

struct rx_air_info_s {
    uint8_t info_type;          // E_CHANNEL_INFO_TYPE_RX_AIR
    int8_t  rssi;               // dBm
    int8_t  snr;                // rssi - noise_floor, clamped int8
} __attribute__((packed));

class channel_info_endpoint {
public:
    static constexpr uint8_t k_subscriber_max = WIFI_AIRPORT_MAX;
    static constexpr uint32_t k_rx_air_interval_ms = 100;  // fixed; not configurable

    static channel_info_endpoint& instance();
    channel_info_endpoint(const channel_info_endpoint&) = delete;
    channel_info_endpoint& operator=(const channel_info_endpoint&) = delete;

    bool init();
    bool start(BaseType_t core = APP_TASK_CORE,
               UBaseType_t prio = NETMGR_TASK_PRIO,
               uint32_t stack_bytes = 6144);
    // reactor_.start_pinned("ci_ep", core, prio, stack_bytes)
    // + get_timer().wait_ms(k_rx_air_interval_ms, on_rx_air_timer) reschedule loop

    // Same subscriber (host+port) replaces / no-op if already present.
    // false = table full (no change). true = added or already present.
    bool add_subscriber(ip_port_t subscriber);  // suc to=<host>:<port>
    bool rem_subscriber(ip_port_t subscriber);  // usuc; false = not found
    bool load(const ip_port_t* subs, uint8_t count);
    void fill_status(ip_port_t* out, uint8_t* count);

    // wifi_rx::note_air (promiscuous CB): store sample only — no send.
    void on_rx_air_info(int8_t rssi, int8_t snr);

    // lc_tx when queue_size > k_queue_cap/2: build tx_flow_ctrl_s, fan-out
    // to all subscribers. Send failure per dest → drop that dest’s datagram.
    void on_flow_ctrl_info(uint8_t queue_size, uint8_t queue_cap);

private:
    using reactor_t = bfc::task_reactor<>;

    channel_info_endpoint() = default;

    void on_rx_air_timer();
    // if (!air_valid_) { reschedule; return; }
    // rx_air_info_s sample{RX_AIR, rssi_.load(), snr_.load()};
    // fanout(&sample, sizeof(sample));
    // reschedule wait_ms(k_rx_air_interval_ms, on_rx_air_timer)

    void fanout(const void* data, size_t len);
    // under lock_: copy subscriber list; unlock
    // for each: send_one; failure → drop (no retry)

    bool ensure_socket();
    bool send_one(const ip_port_t& dest, const void* data, size_t len);

    ip_port_t subs_[k_subscriber_max]{};
    uint8_t n_subs_ = 0;
    bfc::socket send_sock_;
    bfc::semaphore lock_;       // subs_[] vs console; not held across sendto
    reactor_t reactor_;
    std::atomic<int8_t> rssi_{0};
    std::atomic<int8_t> snr_{0};
    std::atomic<bool> air_valid_{false};
};
```

Same `ip_port_t` subscriber replaces (or no-op if already present). Full table → `add_subscriber` returns `false` (console prints error).

**Who calls what**

| API | Caller | When | Wire | Dest |
|-----|--------|------|------|------|
| `on_flow_ctrl_info` | `lc_tx` | size changed **and** `queue_size > k_queue_cap/2` | `tx_flow_ctrl_s` | all subscribers |
| `on_rx_air_info` | `wifi_rx::note_air` | each accepted MPDU (atomics only) | — | — |
| `on_rx_air_timer` | CI reactor | fixed **100 ms** (`k_rx_air_interval_ms`) | `rx_air_info_s` | all subscribers |

**Half-capacity gate (TX):** `<= half` → `on_flow_ctrl_info` not called. Above half → fan-out on each size-changing event.

**Send failure:** drop that subscriber’s datagram; continue others; wait for next tick / next `on_flow_ctrl_info`. No pending retry.

**Threading**

| | |
|--|--|
| Reactor | owned `task_reactor` + timer |
| Default pin | CPU1, `NETMGR_TASK_PRIO` (6), stack 6144 |
| `on_rx_air_info` | promiscuous CB (CPU0) — atomics only |
| `on_flow_ctrl_info` | `lc_tx_endpoint` reactor + `wifi_tx`; lock only to copy `subs_[]` |
| `on_rx_air_timer` | CI reactor only |
| Table | `lock_` for `subs_[]` vs console / fanout copy |

No `ethernet&`. No `packet` pool. No bus key on CI.

Not chosen: per-bus CI dests. Not chosen: publish RX air from `lc_rx`. Not chosen: share another module’s reactor.

## Console commands

| Command | Aliases | Arguments |
|---------|---------|-----------|
| `set_domain` | `sdom` | `<domain>` |
| `unset_domain` | `udom` | (none) |
| `set_upstream_rx` | `sur` | `bus=<lcid> <host> <udp_port>` — air→UDP **filter** |
| `unset_upstream_rx` | `uur` | `bus=<lcid>` |
| `set_upstream_tx` | `sut` | `bus=<lcid> <udp_port>` — UDP→air **target** |
| `unset_upstream_tx` | `uut` | `bus=<lcid>` |
| `set_upstream_ci` | `suc` | `to=<host>:<port>` — add CI subscriber |
| `unset_upstream_ci` | `usuc` | `to=<host>:<port>` — rem CI subscriber |

No `set_winject_id`. No `port=` key — use **`bus=`**. Today’s `sd` remains **`set_modulation`** (do not reuse for domain).

Examples:

```
set_domain 1234
set_upstream_tx bus=b2 9000
set_upstream_rx bus=a1 192.168.32.10 9001
set_upstream_ci to=192.168.32.10:9100
```

Unchanged in role: `set_mode`, `set_channel`, `set_modulation` (`sd`), `set_network`, `set_ip`, DHCP/CCA/power/CRC, `save` / `use`, `status`, `reset`, `help`.

`status` reports domain, mode, bus binds (`sut` / `sur`), CI subscribers, queues, drops.

`set_mode` still selects Addr3 prefix immediately; existing endpoints remain, next TX/RX use the new BSSID. Domain octets are unchanged by `set_mode`.

---

## Thread model

| Context | Core (intent) | Work |
|---------|---------------|------|
| Console / netmgr `select_reactor` | pin at start (default CPU1 prio 6) | TCP console, DHCP |
| `lc_tx_endpoint` `select_reactor` | pin at start (default CPU1 prio 18) | UDP inject sockets |
| `wifi_tx` pinned `run()` | pin at start (default CPU0 prio 20) | Pop/combine `lc_tx`, inject |
| WiFi promiscuous / `wifi_rx` CB (ESP) | CPU0 | Addr3 accept, copy MPDU, `lc_rx.rx` |
| `lc_rx` `task_reactor` | pin at start (default CPU1 prio 18) | Pop MPDU, unpack slots, demux by bus → endpoint |
| `channel_info_endpoint` `task_reactor` | pin at start (default CPU1 prio 6) | RX air timer; FLOW_CTRL sendto from callers |

Drop contracts:

- `lc_tx.tx` full → drop LCP, count; still `on_flow_ctrl_info` if `queue_size > k_queue_cap/2` and CI has subscribers.
- RX pool empty or `lc_rx.rx` full → drop MPDU, count (`wifi_rx`).
- Addr3 prefix / domain mismatch (or domain unset) → drop in CB, no allocate.
- Invalid slot unpack / `sum(sizes) != body_len` → drop in `lc_rx`, count.
- No `sur` filter whose `bus` matches the present slot → ignore that slot.

Flow control: `on_flow_ctrl_info` only when `queue_size > capacity/2`, fan-out to all CI subscribers. RX air: `on_rx_air_info` stores sample; timer sends `{rssi,snr}` every **fixed 100 ms** to all subscribers. CI `sendto` failure → drop; wait for next update/tick.

---

## Settings / NVS

Bump blob to **version 3**. `k_blob_version_min = 3` after cutover — v1/v2 airport blobs fail `use` / `load_current` (treat as empty slot; keep radio/net defaults). Document in release notes.

### Snapshot fields (live + blob)

```cpp
struct snapshot_s {
    // unchanged radio / net
    WinjectMode mode;
    uint8_t channel;
    char modulation[SETTINGS_MODULATION_MAX];
    bool cca_enabled;
    bool allow_failed_crc;
    int8_t tx_power_dbm;
    uint32_t fallback_ip;
    NetmgrMode network_mode;
    bool dhcp_server_enabled;

    uint16_t domain;              // 0 = unset

    uint8_t sut_count;            // UDP→air (lc_tx_endpoint)
    lc_tx_bind_s sut[WIFI_AIRPORT_MAX];   // bus + udp_port; socket_open not packed

    uint8_t sur_count;            // air→UDP (lc_rx_endpoint)
    lc_rx_bind_s sur[WIFI_AIRPORT_MAX];   // bus + dest; active not packed

    uint8_t ci_count;
    ip_port_t ci[WIFI_AIRPORT_MAX];       // host + port
};
```

### Blob layout (little-endian multi-byte, same spirit as today’s pack)

| Order | Type | Field |
|-------|------|-------|
| 1 | u8 | `version` = **3** |
| 2 | u8 | `mode` |
| 3 | u8 | `channel` |
| 4 | u8 | `cca_enabled` |
| 5 | u8 | `allow_failed_crc` |
| 6 | u8 | `tx_power_dbm` (as today) |
| 7 | 16 B | `modulation` |
| 8 | u32 | `fallback_ip` |
| 9 | u8 | `network_mode` |
| 10 | u8 | `dhcp_server_enabled` |
| 11 | u16 | `domain` (`0` = unset) |
| 12 | u8 | `sut_count` |
| 13 | ×N | `bus` u8 + `udp_port` u16 |
| 14 | u8 | `sur_count` |
| 15 | ×N | `bus` u8 + `host` u32 + `port` u16 |
| 16 | u8 | `ci_count` |
| 17 | ×N | `host` u32 + `port` u16 |

Max packed size with `WIFI_AIRPORT_MAX=128`: well under today’s `k_blob_max` (2600). Keep 2600 unless counts grow.

`apply_snapshot`: `wifi::set_domain(domain)` (fans out to tx+rx), `lc_tx_endpoint::load(sut)`, `lc_rx_endpoint::load(sur)`, `channel_info_endpoint::load(ci)`. Capture clears `socket_open` / `active` flags before pack (same as today’s rx `socket_open=false`).

---

## Breaking changes vs current winject.md

| Current | Target |
|---------|--------|
| Addr1 always broadcast | Addr1\|\|Addr2 = five `(bus8,size11)` PDU slots |
| Addr2 = airport SA or STA by mode | (same packing; bus/lcid per slot, no SA) |
| Airport = one 6-byte SA / `src:dst` pair | **`bus=<lcid>`** only (no src) |
| Standalone Addr3 `DE:AD:CA:FE:BA:BE` | **`CA:FE:BA:BE:DH:DL`** (prefix change + domain) |
| Tunnel Addr3 `BA:DD:CA:FE:BA:BE` | **`BA:DD:CA:FE:DH:DL`** (same prefix; last two = domain) |
| One UDP datagram = one MPDU | wifi_tx may combine up to 5 PDUs (any bus) |
| `sur` = UDP→air, `sut` = air→UDP | **`sur` = air→UDP filter**, **`sut` = UDP→air target** |
| `port=<…>` | **`bus=<lcid>`** |
| `set_winject_id` + optional domain | **`set_domain` / `sdom` only** |
| NVS blob v1/v2 (airport rows) | **blob v3** (bus + domain + CI); old slots rejected |
| No channel-info UDP | `suc` / `usuc` + `tx_flow_ctrl_s` / `rx_air_info_s` |

Manager [`docs/manager.md`](manager.md) `upstream-N.airport` strings and host tests that assume the old wire format need a follow-up change when firmware implements this doc; out of scope for firmware Passes 0–9.

---

## File tree

New and replaced paths under `src/winject-esp32/` (register every new `.cpp` in `src/CMakeLists.txt`):

```
radio/
  packet.h / packet.cpp          # packet + packet_allocator (replaces packet_pool.h usage)
  frame.h / frame.cpp            # retarget: Addr pack/unpack, mode prefix, domain helpers
  wifi.h / wifi.cpp              # façade: drop take_tx/post_tx/pop_rx; add set_domain
  wifi_tx.h / wifi_tx.cpp        # combine + stamp + inject from lc_tx
  wifi_rx.h / wifi_rx.cpp        # Addr3 accept + allocate + lc_rx.rx
  packet_pool.h / packet_queue.h # delete once callers gone

logical_channel/
  lc_tx.h / lc_tx.cpp
  lc_tx_endpoint.h / lc_tx_endpoint.cpp   # replaces upstream_rx.*
  lc_rx.h / lc_rx.cpp
  lc_rx_endpoint.h / lc_rx_endpoint.cpp   # replaces upstream_tx.*

endpoint/
  channel_info_endpoint.h / channel_info_endpoint.cpp
  upstream_rx.* / upstream_tx.*           # delete after cutover

bfc-esp32/
  select_reactor.hpp / task_reactor.hpp   # add start_pinned()

config.h          # WIFI_TX_HEADROOM, WIFI_TX_PACKET_CAP; BSSID prefixes
console.cpp/.h    # commands + status
settings.cpp/.h   # blob v3
main.cpp          # boot wiring
```

Host: update `src/test/frame_test.cpp` (and any pack helpers) in the same PR as Pass 2, or immediately after — do not leave golden vectors untested.

---

## `wifi` façade (after cutover)

```cpp
class wifi {
public:
    static wifi& instance();
    bool initialize();           // PHY bring-up; constructs tx_/rx_ members; no lc wiring / no start
    bool ready() const;

    bool set_channel(uint8_t channel);
    bool set_modulation(const char* name);
    bool set_cca_enabled(bool enabled);
    bool set_tx_power(int8_t dbm);
    bool set_allow_failed_crc(bool allow);

    // Fans out to wifi_tx + wifi_rx atomics. 0 = unset.
    bool set_domain(uint16_t domain);
    uint16_t domain() const;     // read tx side (must match rx after fan-out)

    void get_status(wifi_status_s* status);
    // status.tx_queue / rx_queue from lc_tx / lc_rx occupancy

    wifi_tx& tx();
    wifi_rx& rx();

    // REMOVED: take_tx, post_tx, release_tx, inject, pop_rx
    // Inject lives only in wifi_tx::run. RX path is CB → lc_rx.
};
```

`wifi::initialize()` still owns PHY bring-up only. `wifi_tx::init(lc_tx)` / `wifi_rx::init(lc_rx)` / `wifi_tx::start(...)` are called from `main` per the boot order below (do not bury them inside `initialize`).

---

## Boot / `main.cpp` wiring

Order after NVS + netif + event loop:

```
1. settings::load_current()                  // may leave domain unset / empty binds
2. netmgr.start()
3. packet_allocator::tx().init(WIFI_RADIO_TX_QUEUE)
4. packet_allocator::rx().init(WIFI_RADIO_RX_QUEUE)
5. lc_tx::instance().init()
6. lc_rx::instance().init()
7. lc_tx_endpoint::instance().init(lc_tx)
8. lc_rx_endpoint::instance().init()
9. lc_rx.set_endpoint(lc_rx_endpoint)
10. channel_info_endpoint::instance().init()
11. lc_tx.set_channel_info(channel_info_endpoint)
12. wifi.initialize()                        // PHY + wifi_tx/rx members
13. frameBegin() / mode prefix tables
14. wifi.tx().init(lc_tx); wifi.rx().init(lc_rx)
15. settings::apply_live()                   // domain + binds + CI + radio knobs
16. lc_tx_endpoint.start()                   // default CPU1 / prio 18
17. lc_rx.start()
18. channel_info_endpoint.start()            // default CPU1 / prio 6
19. wifi.tx().start()                        // default CPU0 / prio 20
20. console.init(...); otaBegin(...)
```

No `ethernet&` into LC endpoints. Console holds references to `lc_tx_endpoint`, `lc_rx_endpoint`, `channel_info_endpoint`, `wifi`, `manager` (not `upstream_*`).

---

## Implementation passes (firmware)

Do these **in order**. Each pass leaves the tree buildable (or behind a short-lived `#if` only if unavoidable — prefer vertical slices that replace callers in the same pass).

| Pass | Scope | Done when |
|------|-------|-----------|
| **0** | `bfc-esp32`: `select_reactor` / `task_reactor` `start_pinned(name, core, prio, stack)` | Existing netmgr/console still start; new API unit-smokeable |
| **1** | `packet` + `packet_allocator` (`radio/packet.*`); `WIFI_TX_HEADROOM` / `WIFI_TX_PACKET_CAP` in `config.h` | allocate / share / move / dtor return-to-pool; tx+rx pools init |
| **2** | `frame.*` Addr1\|\|Addr2 pack/unpack + Addr3 mode prefix (`CA:FE:BA:BE` / `BA:DD:CA:FE`) + domain octets; golden vectors above; host `frame_test` | Round-trip pack/unpack matches examples; old airport SA tables gone |
| **3** | `lc_tx` wait-free `tx` / blocking `pop` / `peek` | Cross-core post/take; drop on full; no CI yet |
| **4** | `wifi_tx` combine + `stamp` + `inject_retry`; remove `take_tx`/`post_tx`/`inject` callers | Solo + multi-PDU inject; domain unset drops; status queue from `lc_tx` |
| **5** | `lc_tx_endpoint` + console `sut`/`uut` (`bus=`); delete inject path of `upstream_rx` | UDP→air zero-copy allocate-then-recv; backpressure via `set_on_space` |
| **6** | `lc_rx` + `wifi_rx` CB path; remove `pop_rx` | Addr3 accept → pool → queue; unpack + `bad_mpdu`; no UDP yet (forward stub ok) |
| **7** | `lc_rx_endpoint` + console `sur`/`uur`; delete `upstream_tx` | Bus demux + `sendto`; exact bus match table |
| **8** | `channel_info_endpoint` + console `suc`/`usuc`; wire `lc_tx` flow-ctrl + `wifi_rx` air sample | FLOW_CTRL above half; RX air every 100 ms |
| **9** | `set_domain`/`sdom`/`udom`, settings blob **v3**, `main` wiring, `wifi::set_domain`, status, CMake, delete `packet_pool`/`packet_queue`/`upstream_*` | Boot path above; old NVS rejected; `status` shows domain/binds/CI/queues |

**Pass references in module sections above** map as: Pass 3 = `lc_tx`, Pass 6 = `lc_rx`, Pass 7 = `lc_rx_endpoint`, Pass 8 = CI.

### Out of scope (follow-up docs / PRs)

- Host manager `upstream-N.airport` → bus/`lcid` ([manager.md](manager.md))
- Manager FEC / PDCP / AM (unchanged non-goal)
- Async write-ready PDU queue on `lc_rx_endpoint` (optional later; sync `sendto` is the default)

### Implementation map (quick)

| Pass | New / retargeted | Replaces / touches |
|------|------------------|--------------------|
| 0 | `start_pinned` | `bfc-esp32/*reactor*` |
| 1 | `packet` / `packet_allocator` | `packet_pool.h` callers |
| 2 | Addr slots + domain Addr3 | `radio/frame.*`, `config.h` BSSIDs, tests |
| 3–5 | `lc_tx*` → `wifi_tx` | `upstream_rx.*`, `wifi_tx.*`, `wifi` TX API |
| 6–7 | `wifi_rx` → `lc_rx*` | `upstream_tx.*`, `wifi_rx.*`, `wifi` RX API |
| 8 | `channel_info_endpoint` | new; `lc_tx` / `wifi_rx` hooks |
| 9 | console + settings v3 + `main` | `console.*`, `settings.*`, `CMakeLists.txt` |
