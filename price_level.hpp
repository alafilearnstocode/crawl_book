#pragma once

#include <iterator>
#include <list>

#include "order.hpp"

struct PriceLevel {
    std::list<Order> orders;

    std::list<Order>::iterator add(const Order& o) {
        orders.push_back(o);
        return std::prev(orders.end());
    }

    bool remove(std::list<Order>::iterator it) {
        orders.erase(it);
        return orders.empty();
    }
};
