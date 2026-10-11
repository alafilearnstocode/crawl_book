# crawl_book

A single-threaded limit order book and matching engine written in C++20 from scratch. It accepts buy and sell limit orders, matches them by price-time priority, rests any unfilled remainder in the book, and supports O(1) cancel by order id. It aims for correctness and clarity, not production use. There is no networking, persistence or concurrency, and no performance numbers have been measured.

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
- `main.cpp`: the test driver. It runs each scenario, prints the book before and after, and checks expected results with an always-on `CHECK` macro.

## Build and run

Requires a C++20 compiler. The build produces zero warnings with `-Wall -Wextra`.

```bash
make build   # g++ -std=c++20 -Wall -Wextra -O2 -o orderbook main.cpp
./orderbook  # or: make run
```

The driver covers:
- A partial fill within one level
- A buy that sweeps several levels
- A partial fill whose remainder rests as a bid
- A later buy that must not trade against that bid
- A sell that crosses the bids
- Cancels of a resting order, an unknown id, and the last order in a level

It ends with `ALL N CHECKS PASSED` and exits non-zero if any check fails.

## Scope

- Single-threaded. There is no locking and nothing is lock-free, so use it from one thread only.
- Limit orders only. There are no market, IOC or FOK order types, no order modification, and no self-trade prevention.
- No benchmark harness is included, so this README makes no claims about latency or throughput.
