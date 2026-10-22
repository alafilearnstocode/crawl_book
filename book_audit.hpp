#pragma once

#include <cstddef>
#include <string>

#include "order_book.hpp"

inline std::string audit(const OrderBook& b) {
    std::size_t resting = 0;
    for (const auto* side : {&b.bids, &b.asks}) {
        const bool side_is_buy = (side == &b.bids);
        for (const auto& [price, lvl] : *side) {
            const std::string at = " at price " + std::to_string(price);
            if (lvl.orders.empty()) return "empty level left in tree" + at;
            for (auto node = lvl.orders.begin(); node != lvl.orders.end(); ++node) {
                ++resting;
                const std::string who = " (id " + std::to_string(node->id) + ")" + at;
                if (node->price != price)        return "order price != level price" + who;
                if (node->quantity == 0)         return "zero-quantity order resting" + who;
                if (node->is_buy != side_is_buy) return "order on wrong side" + who;
                auto idx = b.order_index.find(node->id);
                if (idx == b.order_index.end())  return "resting order missing from index" + who;
                if (idx->second.price != price)  return "index price mismatch" + who;
                if (idx->second.node != node)    return "index node mismatch" + who;
            }
        }
    }
    if (resting != b.order_index.size())
        return "index has " + std::to_string(b.order_index.size()) + " entries but " +
               std::to_string(resting) + " orders rest";
    if (!b.bids.empty() && !b.asks.empty() && b.bids.rbegin()->first >= b.asks.begin()->first)
        return "book left crossed";
    return {};
}
