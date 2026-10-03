#pragma once

#include <iterator>
#include <list>

#include "order.hpp"

// Dumb FIFO primitive for one price. Knows nothing about order ids or the index.
// std::list: O(1) push_back / pop_front / erase-of-known-node, and iterators to
// other nodes stay valid across insert/erase (a vector's would not).
struct PriceLevel {
    std::list<Order> orders;  // front = oldest = highest time priority

    // Newest order goes to the back. orders.end() is the past-the-end sentinel,
    // so the just-added node is std::prev(orders.end()).
    std::list<Order>::iterator add(const Order& o) {
        orders.push_back(o);
        return std::prev(orders.end());
    }

    // O(1) erase of a known node. Returns true if the level is now empty so the
    // caller can drop it from the tree. Does NOT touch the index or the tree.
    bool remove(std::list<Order>::iterator it) {
        orders.erase(it);
        return orders.empty();
    }
};
