# crawl_book

A single-threaded limit order book and matching engine written in C++20 from scratch. It accepts buy and sell limit orders, matches them by price-time priority, rests any unfilled remainder in the book, and supports O(1) cancel by order id. It aims for correctness and clarity, not production use. There is no networking, persistence or concurrency.

Correctness is checked by a randomized differential test: 4,000,000 random operations, each compared against a deliberately naive reference engine. Its speed is measured by a benchmark: about **11 million operations per second** (about 91 ns each), with a median of 83 ns per operation and a p99 of 292 ns, on an Apple M3. See [Testing](#testing) and [Benchmark](#benchmark) for how these were measured.

## Design: the same orders, reachable two ways

Each resting order can be found by price and by id. Every mutation updates both views together. If one view is updated without the other, the book is corrupt.

| Structure | Type | Why |
|---|---|---|
| **Price trees** | `std::map<int64_t, PriceLevel>`, one for bids and one for asks | Kept sorted by price, so the best ask is `asks.begin()` and the best bid is `bids.rbegin()`. Adding or removing a level is O(log L), where L is the number of price levels. |
| **Per-level FIFO** | `std::list<Order>` inside each `PriceLevel` | Enforces time priority: the front is the oldest order. A list gives O(1) push to the back, O(1) pop from the front, and O(1) erase of a node you already hold. Its iterators also stay valid when other nodes are inserted or erased. A `vector` would invalidate them on reallocation, so the index could not store them. |
| **Order-id index** | `std::unordered_map<uint64_t, OrderLocation>` | Maps an id to its order's price and list iterator, so cancel never searches a list. It stores the price (a key into the tree), not a pointer to the level. |

The id appears twice on purpose. It is the index key, which lets you go from id to node for a cancel. It is also a field on `Order`, which lets you go from node to id during matching.

**Prices are integer ticks (`int64_t`), never `double`.** Floating point cannot represent decimal prices exactly, and the error accumulates over repeated operations. Integer comparisons are also exact.

**Why two trees, not one.** If bids and asks shared a single tree, a resting bid would sit below the asks. `begin()` would then return a bid level, and an incoming buy would trade against another buy. Using separate trees of the same type keeps the best price on each side at a fixed end.

## Operations

### `submit(Order)`
This is the public entry point. It matches the order against the opposite side, then rests any remainder. The order is passed by value because the book takes ownership of it.

### `match(Order&)` (private)
1. Repeatedly takes the best opposite level: the lowest ask for a buy, or the highest bid for a sell. It stops when that level no longer crosses the incoming limit price.
2. Within a level, it consumes only from the front, fills `min(incoming, resting)` at the resting price, and records a `Fill`.
3. When a resting order is fully filled, it erases the order's index entry while the node is still alive, then calls `pop_front()`.
4. When a level becomes empty, it erases the level from the tree. The best level is re-read on every iteration, so an erased iterator is never reused.

It changes `incoming.quantity` in place, so on return the order holds only its unfilled remainder.

Complexity is O(F + K log L), where F is the number of fills and K is the number of levels emptied.

### `cancel(id)`
1. `order_index.find(id)`: if the id is absent, the call does nothing. It uses `find`, never `operator[]`, which would insert an entry.
2. Gets the side from the node, then finds the level by price. It **asserts** the level exists. An index entry pointing at a missing level would mean corruption, so it should fail loudly rather than be skipped silently.
3. Removes the node from the level, erases the index entry by iterator, and erases the level if it is now empty.

Complexity is O(1) average for the hash lookup and list erase, plus O(log L) for the tree lookup.

### `rest(const Order&)` (private)
Creates the level if needed, appends the order to the back of its FIFO, and records `{price, node}` in the index.

## The sync invariant

> Every order in a tree has exactly one index entry pointing at its node and price. Every index entry points at a live node. No level is empty. Bids are always priced strictly below asks.

All mutations happen inside `OrderBook`'s `submit`, `match`, `rest` and `cancel`. `PriceLevel` is a simple helper that never touches the index or the trees, so the invariant only has to hold in one class. The test driver checks it after every scenario.

## Files

- `order.hpp`: the `Order` struct
- `price_level.hpp`: `PriceLevel`, the FIFO for one price
- `order_book.hpp`: `OrderBook` (the trees, the index, submit, match, rest, cancel) plus `OrderLocation` and `Fill`
- `main.cpp`: the scenario driver. It prints the book before and after each scenario and checks expected results with an always-on `CHECK` macro.
- `reference_book.hpp`: `ReferenceBook`, a naive engine used only as the stress test's reference, plus a canonical snapshot function shared by both engines
- `book_audit.hpp`: `audit()`, which checks that the trees and the index agree
- `stress_test.cpp`: the randomized test that compares the two engines
- `benchmark.cpp`: throughput and latency measurement

## Build and run

Requires a C++20 compiler. Every target builds with zero warnings under `-Wall -Wextra`.

```bash
make build   # builds orderbook, stress_test, benchmark (-std=c++20 -Wall -Wextra -O2)
make test    # scenario driver, then the full stress test
make asan    # stress test under AddressSanitizer + UndefinedBehaviorSanitizer
make bench   # benchmark
```

## Testing

### Scenario driver (`./orderbook`)
The driver runs hand-built scenarios and prints the book before and after each one:
- A partial fill within one level
- A buy that sweeps several levels
- A partial fill whose remainder rests as a bid
- A later buy that must not trade against that bid
- A sell that crosses the bids
- Cancels of a resting order, an unknown id, and the last order in a level

It ends with `ALL N CHECKS PASSED` and exits non-zero if any check fails.

### Randomized differential test (`./stress_test [seeds] [ops] [first_seed]`)
`ReferenceBook` is an intentionally simple engine. It keeps all resting orders in one flat vector and finds the best order to match by scanning every order: best price first, then earliest arrival. It shares no matching code with `OrderBook`, so a bug in one is unlikely to be repeated in the other.

The stress test sends the same random stream of submits and cancels to both engines and fails if, after **any** operation:
- the fills differ (taker, maker, price, quantity, or order)
- the books differ (side, price, position in the queue, id, or remaining quantity)
- `audit(OrderBook)` finds the trees and the index out of sync, an empty level, a zero-quantity order, or a crossed book

Each seed randomizes the conditions:
- **Price band (1 to 25 ticks):** narrow bands make orders cross constantly and stack up in the same level.
- **Cancel rate:** varied per seed.
- **Quantity range:** sometimes small, which produces many exact-size fills.
- **Depth cap:** keeps per-operation checks cheap.

Cancels target live orders anywhere in a queue, ids that were already filled or cancelled, and ids that were never issued.

The default run is 200 seeds × 20,000 operations, so **4,000,000 operations are compared with the reference**. It produces about 1.8 million fills and takes about 30 seconds on an M3. A failure prints the seed and operation number, so it can be replayed exactly. `make asan` runs 600,000 operations under the sanitizers and finds no errors.

To confirm the test catches real bugs, six deliberately broken engines were each run against it. Every one failed within the first 31 operations of seed 1:

| Injected bug | Caught by |
|---|---|
| Newest order filled first (LIFO instead of FIFO) | book contents differ |
| Fill priced at the incoming order's limit, not the resting price | fill differs |
| An order priced exactly at the best opposite price doesn't trade (`>` instead of `>=`) | fill count differs |
| Cancel leaves an empty level in the tree | audit failed |
| Filled order left in the id index | audit failed |
| Sell matched against the lowest bid instead of the highest | fill differs |

## Benchmark

`./benchmark [ops] [runs] [seed]` generates the whole workload in advance, so random-number generation and bookkeeping aren't timed. It then replays the workload into a fresh book.

**Workload**
- 20,000 prefilled orders, then 2,000,000 operations.
- **Mix:** about 51% passive limit orders resting 1 to 50 ticks behind the best price, 39% cancels of live orders, and 10% orders that cross and may sweep several levels.
- **Steady size:** the cancel rate adjusts so the book stays at about 20,000 resting orders across about 120 price levels.

**Throughput:** the full operation stream runs with no timing calls inside the loop. The number reported is the median of 5 runs.

**Latency:** a separate run reads the clock before and after every operation.

Results on an Apple M3 (8 GB, macOS), built with Apple clang 21 at `-O2`. The figures were consistent across seeds 7 and 42:

| Metric | Result |
|---|---|
| Throughput (median of 5) | **~11.0 M ops/s**, ~91 ns/op |
| Fills per run | ~437,000 |

| Latency (ns) | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| passive submit | 83 | 125 | 125 | 167 to 208 | 29k to 43k |
| cancel | 83 | 84 | 125 | 167 to 208 | 10k to 40k |
| marketable submit | 166 | 291 | 417 | 542 to 583 | ~7k |
| all ops | 83 | 125 | 292 | 458 | 40k to 43k |

How to read these numbers:
- **Timer resolution:** on Apple Silicon, `steady_clock` advances in steps of about 42 ns. Latencies below about 200 ns are therefore rounded to multiples of 42 ns, and each one includes about 12 ns of clock-reading cost. The throughput figure isn't affected, because it doesn't read the clock per operation.
- **Max values:** the tens-of-microseconds maximums are rare one-off spikes. They are probably the OS or memory allocator pausing the process, or the id index resizing. They don't show up at the p99.9 level.
- **Machine-specific:** these numbers come from one laptop with background processes running. Expect different results on other hardware.

## Scope

- Single-threaded. There is no locking and nothing is lock-free, so use it from one thread only.
- Limit orders only. There are no market, IOC or FOK order types, no order modification, and no self-trade prevention.
- No performance tuning has been done yet. Each resting order is a separately allocated `std::list` node, and each new price level is a `std::map` node, so allocation and pointer-chasing are the obvious next costs to reduce.
