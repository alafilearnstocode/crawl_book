#pragma once

#include <cstdint>

struct Order {
    uint64_t id;
    bool     is_buy;
    int64_t  price;
    uint64_t quantity;
};
