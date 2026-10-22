#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#include "order.hpp"
#include "order_book.hpp"

struct ReferenceBook {
    struct Resting {
        Order    order;
        uint64_t seq;
    };

    std::vector<Resting> resting;
    std::vector<Fill>    fills;
    uint64_t             next_seq = 0;

    void submit(Order in) {
        while (in.quantity > 0) {
            std::size_t best = resting.size();
            for (std::size_t i = 0; i < resting.size(); ++i) {
                const Order& r = resting[i].order;
                if (r.is_buy == in.is_buy) continue;
                const bool crosses = in.is_buy ? in.price >= r.price : in.price <= r.price;
                if (!crosses) continue;
                if (best == resting.size()) { best = i; continue; }
                const Order& b = resting[best].order;
                const bool better_price = in.is_buy ? r.price < b.price : r.price > b.price;
                if (better_price || (r.price == b.price && resting[i].seq < resting[best].seq))
                    best = i;
            }
            if (best == resting.size()) break;

            Order& maker = resting[best].order;
            const uint64_t qty = in.quantity < maker.quantity ? in.quantity : maker.quantity;
            in.quantity    -= qty;
            maker.quantity -= qty;
            fills.push_back({in.id, maker.id, maker.price, qty});
            if (maker.quantity == 0)
                resting.erase(resting.begin() + static_cast<std::ptrdiff_t>(best));
        }
        if (in.quantity > 0) resting.push_back({in, next_seq++});
    }

    void cancel(uint64_t id) {
        for (std::size_t i = 0; i < resting.size(); ++i) {
            if (resting[i].order.id == id) {
                resting.erase(resting.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }
};

using Snapshot = std::vector<std::tuple<bool, int64_t, uint64_t, uint64_t>>;

inline Snapshot snapshot(const OrderBook& b) {
    Snapshot s;
    for (const auto& [price, lvl] : b.asks)
        for (const Order& o : lvl.orders) s.emplace_back(false, price, o.id, o.quantity);
    for (auto it = b.bids.rbegin(); it != b.bids.rend(); ++it)
        for (const Order& o : it->second.orders) s.emplace_back(true, it->first, o.id, o.quantity);
    return s;
}

inline Snapshot snapshot(const ReferenceBook& r) {
    std::vector<const ReferenceBook::Resting*> v;
    for (const auto& x : r.resting) v.push_back(&x);
    auto key = [](const ReferenceBook::Resting* x) {
        return std::make_tuple(x->order.is_buy, x->order.is_buy ? -x->order.price : x->order.price, x->seq);
    };
    std::sort(v.begin(), v.end(), [&](auto* a, auto* b) { return key(a) < key(b); });
    Snapshot s;
    for (const auto* x : v) s.emplace_back(x->order.is_buy, x->order.price, x->order.id, x->order.quantity);
    return s;
}
