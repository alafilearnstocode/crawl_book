#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "order_book.hpp"

static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__,       \
                         __LINE__, #cond);                                   \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

static void dump_side(const char* name, const std::map<int64_t, PriceLevel>& side, bool descending) {
    std::printf("  %s:\n", name);
    if (side.empty()) {
        std::printf("    (empty)\n");
        return;
    }
    auto print_level = [](int64_t price, const PriceLevel& lvl) {
        std::printf("    %5lld |", static_cast<long long>(price));
        for (const Order& o : lvl.orders)
            std::printf("  id=%llu qty=%llu", static_cast<unsigned long long>(o.id),
                        static_cast<unsigned long long>(o.quantity));
        std::printf("\n");
    };
    if (descending)
        for (auto it = side.rbegin(); it != side.rend(); ++it) print_level(it->first, it->second);
    else
        for (const auto& [price, lvl] : side) print_level(price, lvl);
}

static void dump(const char* label, const OrderBook& b) {
    std::printf(" [%s]\n", label);
    dump_side("ASKS (best = lowest, listed first)", b.asks, false);
    dump_side("BIDS (best = highest, listed first)", b.bids, true);
}

static uint64_t total_qty(const std::map<int64_t, PriceLevel>& side) {
    uint64_t t = 0;
    for (const auto& [price, lvl] : side)
        for (const Order& o : lvl.orders) t += o.quantity;
    return t;
}

static uint64_t qty_of(const OrderBook& b, uint64_t id) {
    auto it = b.order_index.find(id);
    return it == b.order_index.end() ? 0 : it->second.node->quantity;
}

static void check_invariants(const OrderBook& b) {
    size_t resting = 0;
    for (const auto* side : {&b.bids, &b.asks}) {
        for (const auto& [price, lvl] : *side) {
            CHECK(!lvl.orders.empty());
            for (auto node = lvl.orders.begin(); node != lvl.orders.end(); ++node) {
                ++resting;
                CHECK(node->price == price);
                CHECK(node->quantity > 0);
                CHECK(node->is_buy == (side == &b.bids));
                auto idx = b.order_index.find(node->id);
                CHECK(idx != b.order_index.end());
                CHECK(idx->second.price == price);
                CHECK(idx->second.node == node);
            }
        }
    }
    CHECK(resting == b.order_index.size());
    if (!b.bids.empty() && !b.asks.empty())
        CHECK(b.bids.rbegin()->first < b.asks.begin()->first);
    std::printf("  invariants OK (%zu resting orders, index in sync)\n", resting);
}

static void print_fills(const OrderBook& b, size_t from) {
    for (size_t i = from; i < b.fills.size(); ++i) {
        const Fill& f = b.fills[i];
        std::printf("  FILL taker=%llu maker=%llu qty=%llu @ %lld\n",
                    static_cast<unsigned long long>(f.taker_id),
                    static_cast<unsigned long long>(f.maker_id),
                    static_cast<unsigned long long>(f.quantity),
                    static_cast<long long>(f.price));
    }
    if (from == b.fills.size()) std::printf("  (no fills)\n");
}

static void submit_and_show(OrderBook& b, Order o) {
    std::printf(" >> submit id=%llu %s %llu @ %lld\n", static_cast<unsigned long long>(o.id),
                o.is_buy ? "BUY" : "SELL", static_cast<unsigned long long>(o.quantity),
                static_cast<long long>(o.price));
    size_t before = b.fills.size();
    b.submit(o);
    print_fills(b, before);
}

static void scenario1() {
    std::printf("\n=== 1. Buy crosses one level partially (resting partly filled, incoming fully filled)\n");
    OrderBook b;
    b.submit({1, false, 101, 100});
    b.submit({2, false, 101, 50});
    b.submit({3, false, 102, 200});
    dump("before", b);

    submit_and_show(b, {10, true, 101, 30});
    dump("after", b);

    CHECK(b.fills.size() == 1);
    CHECK(b.fills[0].maker_id == 1);
    CHECK(qty_of(b, 1) == 70);
    CHECK(qty_of(b, 2) == 50);
    CHECK(b.order_index.count(10) == 0);
    CHECK(b.bids.empty());
    CHECK(total_qty(b.asks) == 320);
    check_invariants(b);
}

static void scenario2() {
    std::printf("\n=== 2. Buy sweeps multiple levels (fills at each resting price)\n");
    OrderBook b;
    b.submit({1, false, 100, 50});
    b.submit({2, false, 101, 50});
    b.submit({3, false, 102, 100});
    dump("before", b);

    submit_and_show(b, {10, true, 105, 120});
    dump("after", b);

    uint64_t notional = 0;
    for (const Fill& f : b.fills) notional += f.quantity * static_cast<uint64_t>(f.price);
    std::printf("  limit 105, filled 120 for %llu ticks total: every fill is at the resting price,\n"
                "  at or better than the limit; later fills cost more as the buy walks the book\n",
                static_cast<unsigned long long>(notional));

    CHECK(b.fills.size() == 3);
    CHECK(b.fills[0].price == 100 && b.fills[0].quantity == 50);
    CHECK(b.fills[1].price == 101 && b.fills[1].quantity == 50);
    CHECK(b.fills[2].price == 102 && b.fills[2].quantity == 20);
    CHECK(notional == 50 * 100 + 50 * 101 + 20 * 102);
    CHECK(b.asks.count(100) == 0 && b.asks.count(101) == 0);
    CHECK(qty_of(b, 3) == 80);
    CHECK(b.bids.empty());
    CHECK(total_qty(b.asks) == 80);
    check_invariants(b);
}

static void scenario3() {
    std::printf("\n=== 3. Buy partially fills, remainder rests as a bid\n");
    OrderBook b;
    b.submit({1, false, 100, 50});
    b.submit({2, false, 103, 50});
    dump("before", b);

    submit_and_show(b, {10, true, 101, 80});
    dump("after", b);

    CHECK(b.fills.size() == 1 && b.fills[0].price == 100 && b.fills[0].quantity == 50);
    CHECK(qty_of(b, 10) == 30);
    CHECK(b.bids.size() == 1 && b.bids.count(101) == 1);
    CHECK(b.asks.size() == 1 && b.asks.count(103) == 1);
    check_invariants(b);

    std::printf("\n --- 3b. A later buy must not trade against the resting bid\n");
    submit_and_show(b, {11, true, 102, 40});
    dump("after", b);
    CHECK(b.fills.size() == 1);
    CHECK(qty_of(b, 11) == 40 && b.bids.size() == 2);
    check_invariants(b);

    std::printf("\n --- 3c. A sell crosses the bids (mirror path): best bid first\n");
    submit_and_show(b, {12, false, 101, 50});
    dump("after", b);
    CHECK(b.fills.size() == 3);
    CHECK(b.fills[1].maker_id == 11 && b.fills[1].price == 102 && b.fills[1].quantity == 40);
    CHECK(b.fills[2].maker_id == 10 && b.fills[2].price == 101 && b.fills[2].quantity == 10);
    CHECK(qty_of(b, 10) == 20);
    CHECK(b.bids.count(102) == 0);
    CHECK(total_qty(b.bids) == 20 && total_qty(b.asks) == 50);
    check_invariants(b);
}

static void scenario4() {
    std::printf("\n=== 4. Cancel\n");
    OrderBook b;
    b.submit({1, false, 100, 10});
    b.submit({2, false, 100, 20});
    b.submit({3, false, 101, 30});
    b.submit({4, true, 98, 40});
    dump("before", b);

    std::printf(" >> cancel id=1 (resting, level 100 keeps id 2)\n");
    b.cancel(1);
    dump("after", b);
    CHECK(b.order_index.count(1) == 0);
    CHECK(b.asks.at(100).orders.size() == 1 && b.asks.at(100).orders.front().id == 2);
    check_invariants(b);

    std::printf(" >> cancel id=999 (unknown id: must be a no-op, no crash)\n");
    const size_t idx_before = b.order_index.size();
    const uint64_t asks_before = total_qty(b.asks), bids_before = total_qty(b.bids);
    b.cancel(999);
    CHECK(b.order_index.size() == idx_before);
    CHECK(b.order_index.count(999) == 0);
    CHECK(total_qty(b.asks) == asks_before && total_qty(b.bids) == bids_before);
    std::printf("  book unchanged\n");
    check_invariants(b);

    std::printf(" >> cancel id=2 (last order at 100: level must leave the tree)\n");
    b.cancel(2);
    dump("after", b);
    CHECK(b.asks.count(100) == 0);
    CHECK(b.asks.begin()->first == 101);
    check_invariants(b);

    std::printf(" >> cancel id=4 (the only bid) then cancel id=4 again (now unknown)\n");
    b.cancel(4);
    b.cancel(4);
    dump("after", b);
    CHECK(b.bids.empty());
    CHECK(total_qty(b.asks) == 30);
    check_invariants(b);
}

int main() {
    scenario1();
    scenario2();
    scenario3();
    scenario4();
    std::printf("\nALL %d CHECKS PASSED\n", g_checks);
    return 0;
}
