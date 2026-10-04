#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "order.hpp"
#include "price_level.hpp"

// Where an order lives: the price (key into its side's tree) and its list node.
// The side is not stored: it is read from node->is_buy while the node is alive.
struct OrderLocation {
    int64_t                    price;
    std::list<Order>::iterator node;
};

// One execution. Recorded so callers can see fill prices; not used for matching.
struct Fill {
    uint64_t taker_id;
    uint64_t maker_id;
    int64_t  price;     // always the resting (maker) order's price
    uint64_t quantity;
};

// Single-threaded limit order book. The tree and the index are two views of the
// same orders; this struct is the ONLY place either is mutated, so they cannot
// drift out of sync.
//
// Deviation from a single shared price tree: bids and asks live in separate
// trees of the same type. With one tree, a resting bid sits below the asks, so
// begin() would return a bid level and an incoming buy would "match" against it.
struct OrderBook {
    std::map<int64_t, PriceLevel> bids;  // ascending; best bid = rbegin()
    std::map<int64_t, PriceLevel> asks;  // ascending; best ask = begin()
    std::unordered_map<uint64_t, OrderLocation> order_index;
    std::vector<Fill> fills;             // execution log, appended by match()

    // Public entry: match against the opposite side, then rest any remainder.
    // Taken by value: it's the caller's order and we consume it.
    void submit(Order incoming) {
        match(incoming);
        if (incoming.quantity > 0) rest(incoming);
    }

    // O(1) average: hash lookup + O(1) list erase (+ O(log L) only if the level empties).
    void cancel(uint64_t id) {
        auto it = order_index.find(id);  // find, never operator[] (which would insert)
        if (it == order_index.end()) return;  // unknown / already filled id: no-op

        const OrderLocation& loc = it->second;
        auto& book = loc.node->is_buy ? bids : asks;
        auto lvl = book.find(loc.price);
        assert(lvl != book.end() && "index points to a live level");

        // Everything needed from loc is read above/here, before any erase.
        bool now_empty = lvl->second.remove(loc.node);
        order_index.erase(it);  // loc dangles after this line
        if (now_empty) book.erase(lvl);
    }

private:
    // Fills incoming against the opposite side, best price first, FIFO within a
    // level. Mutates incoming.quantity; on return it is the unfilled remainder.
    void match(Order& incoming) {
        auto& book = incoming.is_buy ? asks : bids;

        while (incoming.quantity > 0 && !book.empty()) {
            // Re-read the best level fresh every iteration (never reuse an erased iterator).
            auto lvl_it = incoming.is_buy ? book.begin() : std::prev(book.end());
            const int64_t best_price = lvl_it->first;
            const bool crosses = incoming.is_buy ? incoming.price >= best_price
                                                 : incoming.price <= best_price;
            if (!crosses) break;  // remainder will rest

            PriceLevel& level = lvl_it->second;
            // Front-only consumption: no iterator is ever advanced past an erased node.
            while (incoming.quantity > 0 && !level.orders.empty()) {
                Order& resting = level.orders.front();
                const uint64_t fill = std::min(incoming.quantity, resting.quantity);
                incoming.quantity -= fill;
                resting.quantity  -= fill;
                fills.push_back({incoming.id, resting.id, best_price, fill});

                if (resting.quantity == 0) {
                    order_index.erase(resting.id);  // read id while the node is still alive
                    level.orders.pop_front();       // `resting` dangles after this
                } else {
                    break;  // resting survived => incoming is exhausted
                }
            }

            if (level.orders.empty()) book.erase(lvl_it);  // no ghost empty levels
        }
    }

    // Places a (remaining) order into its side's tree and the index.
    void rest(const Order& o) {
        assert(o.quantity > 0 && "only a non-empty remainder rests");
        assert(order_index.find(o.id) == order_index.end() && "order ids are unique");
        auto& book = o.is_buy ? bids : asks;
        auto node = book[o.price].add(o);  // operator[] intended: creates the level if absent
        order_index[o.id] = {o.price, node};
    }
};
