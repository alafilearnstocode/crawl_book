#pragma once

#include <cstdint>

struct Order {
    uint64_t id;        // unique; the key a cancel message carries
    bool     is_buy;    // true = bid, false = ask
    int64_t  price;     // INTEGER TICKS, never double (doubles can't represent decimal money exactly)
    uint64_t quantity;  // remaining shares; shrinks on partial fill
};
