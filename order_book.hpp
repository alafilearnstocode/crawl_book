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

struct OrderLocation {
    int64_t                    price;
    std::list<Order>::iterator node;
};

struct Fill {
    uint64_t taker_id;
    uint64_t maker_id;
    int64_t  price;
    uint64_t quantity;
};

struct OrderBook {
    std::map<int64_t, PriceLevel> bids;
    std::map<int64_t, PriceLevel> asks;
    std::unordered_map<uint64_t, OrderLocation> order_index;
    std::vector<Fill> fills;

    void submit(Order incoming) {
        match(incoming);
        if (incoming.quantity > 0) rest(incoming);
    }

    void cancel(uint64_t id) {
        auto it = order_index.find(id);
        if (it == order_index.end()) return;

        const OrderLocation& loc = it->second;
        auto& book = loc.node->is_buy ? bids : asks;
        auto lvl = book.find(loc.price);
        assert(lvl != book.end() && "index points to a live level");

        bool now_empty = lvl->second.remove(loc.node);
        order_index.erase(it);
        if (now_empty) book.erase(lvl);
    }

private:
    void match(Order& incoming) {
        auto& book = incoming.is_buy ? asks : bids;

        while (incoming.quantity > 0 && !book.empty()) {
            auto lvl_it = incoming.is_buy ? book.begin() : std::prev(book.end());
            const int64_t best_price = lvl_it->first;
            const bool crosses = incoming.is_buy ? incoming.price >= best_price
                                                 : incoming.price <= best_price;
            if (!crosses) break;

            PriceLevel& level = lvl_it->second;
            while (incoming.quantity > 0 && !level.orders.empty()) {
                Order& resting = level.orders.front();
                const uint64_t fill = std::min(incoming.quantity, resting.quantity);
                incoming.quantity -= fill;
                resting.quantity  -= fill;
                fills.push_back({incoming.id, resting.id, best_price, fill});

                if (resting.quantity == 0) {
                    order_index.erase(resting.id);
                    level.orders.pop_front();
                } else {
                    break;
                }
            }

            if (level.orders.empty()) book.erase(lvl_it);
        }
    }

    void rest(const Order& o) {
        assert(o.quantity > 0 && "only a non-empty remainder rests");
        assert(order_index.find(o.id) == order_index.end() && "order ids are unique");
        auto& book = o.is_buy ? bids : asks;
        auto node = book[o.price].add(o);
        order_index[o.id] = {o.price, node};
    }
};
