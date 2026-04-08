# Phase 2: Zero-Allocation Parsing Pipeline

**Goal:** Eliminate the dominant source of hot-path latency -- JSON parsing and order book updates. The current `nlohmann::json::parse()` call creates a full DOM tree with hundreds of heap allocations per message. This phase replaces the entire parsing pipeline with zero-allocation alternatives.

**Target improvement:** ~50-100+ us off the hot path per market data message. Eliminates hundreds of heap allocations per tick.

**Dependencies:** Phase 1 (foundation fixes should be in place first so measurements are clean).

---

## Change 1: Replace nlohmann::json with simdjson On-Demand API

**Files:**
- `CMakeLists.txt` -- add simdjson dependency
- `src/data/websocket_client.cpp` lines 214-240 -- message handler
- `src/data/websocket_client.h` line 14 -- callback signature
- `src/data/market_data_feed.cpp` lines 29-122 -- L2 data parsing
- `include/data/market_data.h` -- remove nlohmann::json dependency

**Problem:** `nlohmann::json::parse()` at `websocket_client.cpp:218` is the single most expensive operation on the hot path. For a typical L2 order book update (~2-5KB), it:
- Performs a full recursive-descent parse
- Allocates a tree of `json` nodes (each containing `std::map<string, json>` internally)
- Creates dozens of `std::string` copies for field names and values
- Total cost: **100+ microseconds**, hundreds of heap allocations

`simdjson` uses SIMD instructions to parse JSON at near-memcpy speed. Its on-demand API (`simdjson::ondemand`) is lazy -- it validates structure but only materializes values when you access them, with zero heap allocations.

**Approach:**

### Step 1: Add simdjson to CMakeLists.txt

simdjson is a single-header or single-source library. The simplest integration:

```cmake
include(FetchContent)
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.12.2
)
FetchContent_MakeAvailable(simdjson)

target_link_libraries(crypto_hft_engine PRIVATE simdjson)
```

### Step 2: Change the callback signature

The WebSocket client should pass raw string data to the callback instead of a parsed JSON object. This avoids parsing the JSON twice (once in WebSocket, once in the feed) and eliminates the nlohmann dependency from the hot path entirely.

**Before (`websocket_client.h`):**
```cpp
using MessageCallback = std::function<void(const nlohmann::json&)>;
```

**After:**
```cpp
using MessageCallback = void(*)(const char* data, size_t len, void* context);

void setMessageCallback(MessageCallback callback, void* context);
```

This also eliminates the `std::function` virtual dispatch overhead (see Change 7 below). The callback receives a raw pointer to the WebSocket receive buffer -- zero copies.

### Step 3: Rewrite handleMessage to pass raw data

**Before (`websocket_client.cpp:214-240`):**
```cpp
void WebSocketClient::handleMessage(const std::string& message) {
    message_count_++;
    try {
        nlohmann::json json_msg = nlohmann::json::parse(message);
        if (message.find("error") != std::string::npos) { ... }
        if (message_callback_) {
            message_callback_(json_msg);
        }
    } catch (...) { ... }
}
```

**After:**
```cpp
void WebSocketClient::handleMessage(const char* data, size_t len) {
    message_count_.fetch_add(1, std::memory_order_relaxed);
    if (message_callback_) {
        message_callback_(data, len, callback_context_);
    }
}
```

Error checking moves to the callback layer (the feed handler can check for error messages when parsing fails or when the message isn't L2 data).

### Step 4: Rewrite MarketDataFeed to use simdjson on-demand

**Before (`market_data_feed.cpp:29-122`):** Multiple nested `message["field"]` lookups, each doing a hash-map search in the JSON DOM, plus `std::stod()` for price parsing.

**After (conceptual structure):**
```cpp
#include <simdjson.h>

// Per-thread parser (reuses internal buffers, zero allocation after warmup)
static thread_local simdjson::ondemand::parser parser;

void MarketDataFeed::processMessage(const char* data, size_t len) {
    auto doc = parser.iterate(data, len, len + simdjson::SIMDJSON_PADDING);

    std::string_view channel;
    if (doc["channel"].get_string().get(channel) || channel != "l2_data") return;

    auto events = doc["events"].get_array();
    for (auto event : events) {
        std::string_view product_id;
        if (event["product_id"].get_string().get(product_id)) continue;
        if (product_id != trading_symbol_) continue;

        std::string_view type;
        if (event["type"].get_string().get(type)) continue;

        bool is_snapshot = (type == "snapshot");
        if (!is_snapshot && type != "update") continue;

        if (is_snapshot) {
            bid_count_ = 0;
            ask_count_ = 0;
        }

        for (auto update : event["updates"]) {
            std::string_view side, price_str, qty_str;
            if (update["side"].get_string().get(side)) continue;
            if (update["price_level"].get_string().get(price_str)) continue;
            if (update["new_quantity"].get_string().get(qty_str)) continue;

            double price, qty;
            // std::from_chars -- see Change 2
            auto [p1, ec1] = std::from_chars(price_str.data(),
                price_str.data() + price_str.size(), price);
            auto [p2, ec2] = std::from_chars(qty_str.data(),
                qty_str.data() + qty_str.size(), qty);
            if (ec1 != std::errc{} || ec2 != std::errc{}) continue;

            if (side == "bid") {
                update_book_level(bid_levels_, bid_count_, price, qty);
            } else if (side == "offer") {
                update_book_level(ask_levels_, ask_count_, price, qty);
            }
        }

        // ... compute BBO and push to queue (same as before)
    }
}
```

Key differences:
- `parser.iterate()` reuses internal buffers -- zero allocations after the first call
- Field access is via `doc["field"]` but this is positional iteration, not hash-map lookup
- String values are `std::string_view` into the original buffer -- zero copies
- All parsing happens in a single pass over the data

**Padding requirement:** simdjson requires `SIMDJSON_PADDING` (typically 64) bytes of extra space beyond the document. The `rx_buffer_` should be allocated with this padding (see Change 5).

**Risk:** Medium. This is the largest change in the phase. simdjson's on-demand API has different error semantics (errors are returned as error codes, not exceptions). Testing must verify all Coinbase message formats are handled correctly.

---

## Change 2: Replace std::stod with std::from_chars

**File:** `src/data/market_data_feed.cpp` lines 60-61

**Problem:** `std::stod()` is locale-aware and may internally grab a locale mutex. It also may allocate. `std::from_chars` (C++17) is locale-independent, allocation-free, and typically 2-5x faster.

**Before:**
```cpp
double price = std::stod(update["price_level"].get_ref<const std::string&>());
double qty = std::stod(update["new_quantity"].get_ref<const std::string&>());
```

**After (integrated with simdjson string_view):**
```cpp
#include <charconv>

double price, qty;
auto [p1, ec1] = std::from_chars(price_str.data(),
    price_str.data() + price_str.size(), price);
auto [p2, ec2] = std::from_chars(qty_str.data(),
    qty_str.data() + qty_str.size(), qty);
```

**Note on compiler support:** `std::from_chars` for floating-point is fully supported in GCC 11+, Clang 16+ (with libc++), and MSVC 19.24+. On older Apple Clang (pre-16), `from_chars` for `double` may not be available -- in that case, use a fast float parsing library like `fast_float`:

```cmake
FetchContent_Declare(fast_float
    GIT_REPOSITORY https://github.com/fastfloat/fast_float.git
    GIT_TAG v8.0.0)
FetchContent_MakeAvailable(fast_float)
```

```cpp
#include <fast_float/fast_float.h>
double price;
fast_float::from_chars(price_str.data(),
    price_str.data() + price_str.size(), price);
```

**Risk:** Low. Pure performance improvement with identical semantics for well-formed numeric strings (which exchange data always is).

---

## Change 3: Replace std::map Order Book with Flat Sorted Array

**Files:**
- `include/data/market_data.h` lines 42-43 (data members)
- `src/data/market_data_feed.cpp` lines 16-23 (trimBook), 50-77 (updates), 81-86 (BBO access)

**Problem:** `std::map<double, double>` is a red-black tree. Each node is a separate heap allocation. Every insert/erase does malloc/free. Lookups are O(log n) with pointer-chasing (each node is a cache miss). For a 25-level book, this is ~5 pointer dereferences per access.

**After:** Use a flat sorted array. With only 25 levels, binary search on a contiguous array is faster than a tree, and there are zero heap allocations.

**Data structure:**
```cpp
struct PriceLevel {
    double price = 0.0;
    double qty = 0.0;
};

static constexpr size_t MAX_BOOK_LEVELS = 25;

// Bids: sorted descending by price (best bid first)
std::array<PriceLevel, MAX_BOOK_LEVELS> bid_levels_{};
size_t bid_count_ = 0;

// Asks: sorted ascending by price (best ask first)
std::array<PriceLevel, MAX_BOOK_LEVELS> ask_levels_{};
size_t ask_count_ = 0;
```

**Helper functions:**
```cpp
void update_book_level(std::array<PriceLevel, MAX_BOOK_LEVELS>& book,
                       size_t& count, double price, double qty, bool descending) {
    // Find existing level
    for (size_t i = 0; i < count; ++i) {
        if (book[i].price == price) {
            if (qty > 0.0) {
                book[i].qty = qty;  // Update quantity
            } else {
                // Remove: shift remaining elements left
                for (size_t j = i; j + 1 < count; ++j) {
                    book[j] = book[j + 1];
                }
                --count;
            }
            return;
        }
    }

    // Not found -- insert if qty > 0
    if (qty <= 0.0 || count >= MAX_BOOK_LEVELS) return;

    // Find insertion point (maintain sort order)
    size_t pos = count;
    for (size_t i = 0; i < count; ++i) {
        bool should_insert = descending
            ? (price > book[i].price)
            : (price < book[i].price);
        if (should_insert) {
            pos = i;
            break;
        }
    }

    // Shift right to make room
    if (count < MAX_BOOK_LEVELS) {
        for (size_t j = count; j > pos; --j) {
            book[j] = book[j - 1];
        }
        book[pos] = {price, qty};
        ++count;
    }
}
```

For 25 levels, the linear scan is ~25 comparisons on contiguous memory (fits in a single cache line pair). This is significantly faster than the tree's pointer-chasing.

**BBO access becomes trivial:**
```cpp
double best_bid = (bid_count_ > 0) ? bid_levels_[0].price : 0.0;
double best_ask = (ask_count_ > 0) ? ask_levels_[0].price : 0.0;
```

**trimBook is no longer needed** -- the array has a fixed max size. If insertion would exceed `MAX_BOOK_LEVELS`, just don't insert (the worst price levels naturally get pushed out).

**Risk:** Medium. The insert/remove logic with shifts is straightforward but must be thoroughly tested with snapshot and incremental update sequences. Edge cases: book overflow, removing levels that don't exist, snapshots clearing the book.

---

## Change 4: Pre-Reserve rx_buffer_ and Eliminate Intermediate String

**File:** `src/data/websocket_client.cpp` lines 291-296

**Problem:** On every WebSocket fragment:
1. A temporary `std::string fragment(...)` is heap-allocated (line 291)
2. `rx_buffer_ += fragment` may trigger reallocation if `rx_buffer_` capacity is exceeded (line 292)

The intermediate string is unnecessary -- we can append directly from the raw buffer.

**Before:**
```cpp
case LWS_CALLBACK_CLIENT_RECEIVE:
    if (in && len > 0) {
        std::string fragment(static_cast<char*>(in), len);
        client_instance->rx_buffer_ += fragment;

        if (lws_is_final_fragment(wsi)) {
            client_instance->handleMessage(client_instance->rx_buffer_);
            client_instance->rx_buffer_.clear();
        }
    }
    break;
```

**After:**
```cpp
case LWS_CALLBACK_CLIENT_RECEIVE:
    if (in && len > 0) {
        client_instance->rx_buffer_.append(
            static_cast<const char*>(in), len);

        if (lws_is_final_fragment(wsi)) {
            // Ensure simdjson padding
            size_t msg_len = client_instance->rx_buffer_.size();
            client_instance->rx_buffer_.resize(
                msg_len + simdjson::SIMDJSON_PADDING, '\0');
            client_instance->handleMessage(
                client_instance->rx_buffer_.data(), msg_len);
            client_instance->rx_buffer_.clear();
        }
    }
    break;
```

Also pre-reserve in the constructor or `connect()`:
```cpp
rx_buffer_.reserve(65536);  // 64KB -- larger than any expected L2 message
```

This eliminates the intermediate `std::string` allocation and prevents `rx_buffer_` reallocation after the first large message (since `clear()` preserves capacity).

**Risk:** Low. Direct append is strictly better than constructing a temporary and concatenating.

---

## Change 5: Pre-Allocate WebSocket Send Buffer

**File:** `src/data/websocket_client.cpp` lines 337-339, `include/data/websocket_client.h`

**Problem:** Every outbound WebSocket message allocates a `std::vector<unsigned char>` for the LWS write buffer:
```cpp
std::vector<unsigned char> buffer(LWS_PRE + msg_len);
```

This is a heap allocation on every send. Outbound messages (subscriptions, heartbeats) are infrequent, so the impact is lower than receive-path issues, but it's still unnecessary.

**After:** Add a pre-allocated member buffer to `WebSocketClient`:

```cpp
// In websocket_client.h:
std::vector<unsigned char> tx_buffer_;

// In constructor:
tx_buffer_.resize(LWS_PRE + 4096);  // Pre-allocate for typical message sizes

// In flushTxQueue:
size_t required = LWS_PRE + msg_len;
if (tx_buffer_.size() < required) {
    tx_buffer_.resize(required);  // Grow monotonically, never shrinks
}
memcpy(&tx_buffer_[LWS_PRE], message.c_str(), msg_len);
int n = lws_write(wsi_, &tx_buffer_[LWS_PRE], msg_len, LWS_WRITE_TEXT);
```

**Risk:** Low. The buffer grows monotonically and is reused across sends.

---

## Change 6: Replace std::function Callback with Raw Function Pointer

**File:** `include/data/websocket_client.h` line 14

**Problem:** `std::function<void(const nlohmann::json&)>` involves:
1. Type-erased storage (possible heap allocation if the lambda exceeds the small-buffer optimization threshold of ~16-32 bytes)
2. Virtual dispatch on every invocation (~5-15ns indirect call)
3. Prevents the compiler from inlining the callback

Since the callback is set once during initialization and never changes, `std::function`'s flexibility is unnecessary overhead.

**Before:**
```cpp
using MessageCallback = std::function<void(const nlohmann::json&)>;
MessageCallback message_callback_;
```

**After:**
```cpp
using MessageCallback = void(*)(const char* data, size_t len, void* context);
MessageCallback message_callback_ = nullptr;
void* callback_context_ = nullptr;
```

The `context` parameter carries the `MarketDataFeed*` pointer (or any other state the callback needs). This is the classic C callback pattern -- zero overhead, no heap allocation, direct function call.

**Setting the callback in MarketDataFeed:**
```cpp
// Static callback function
static void on_ws_message(const char* data, size_t len, void* ctx) {
    auto* self = static_cast<MarketDataFeed*>(ctx);
    self->processMessage(data, len);
}

void MarketDataFeed::start(const std::string& trading_symbol,
                           SPSCQueue<HFTMarketData, 1024>& queue) {
    trading_symbol_ = trading_symbol;
    queue_ = &queue;
    ws_client_.setMessageCallback(&on_ws_message, this);
}
```

**Risk:** Low-medium. This changes the callback API. All callers of `setMessageCallback` must be updated. The trade-off is losing the ergonomics of lambdas for a direct function pointer. Worth it for the hot path.

---

## Verification

After applying all Phase 2 changes:

1. **Build:** Add simdjson to CMake, verify the build succeeds.
2. **Smoke test:** Update `tests/smoke_test.cpp` if it touches JSON parsing or order book structures.
3. **Integration test:** Connect to Coinbase WS feed for 60s and verify:
   - L2 snapshots are correctly parsed (book builds from scratch)
   - L2 incremental updates correctly insert/update/remove levels
   - BBO (best bid/ask) is accurate and updates in real-time
   - Crossed-book detection still works
4. **Latency measurement:** The `avg_order_latency_ns` metric should show a significant drop. More importantly, observe the difference in `market_data_updates` count -- with faster parsing, more updates per second can be processed.

---

## Complexity Summary

| Change | Effort | Risk | Impact |
|--------|--------|------|--------|
| 1. simdjson replacement | High | Medium | Very High (~100+ us/msg) |
| 2. from_chars for doubles | Low | Low | Medium (~50-200ns/parse) |
| 3. Flat array order book | Medium | Medium | High (~1-5 us/update) |
| 4. Pre-reserve rx_buffer | Low | Low | Low-Medium (~50ns/frag) |
| 5. Pre-allocate tx_buffer | Low | Low | Low (~50ns/send) |
| 6. Raw function pointer | Low-Med | Low-Med | Low (~5-15ns/call) |
