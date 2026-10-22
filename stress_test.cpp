#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "book_audit.hpp"
#include "order_book.hpp"
#include "reference_book.hpp"

struct Stats {
    uint64_t submits = 0, cancels = 0, fills = 0, rested = 0, max_depth = 0;
};

static void fail(uint64_t seed, uint64_t op, const std::string& what, const std::string& detail) {
    std::fprintf(stderr, "\nFAIL seed=%llu op=%llu: %s\n%s\n", static_cast<unsigned long long>(seed),
                 static_cast<unsigned long long>(op), what.c_str(), detail.c_str());
    std::exit(1);
}

static std::string show(const Fill& f) {
    return "taker=" + std::to_string(f.taker_id) + " maker=" + std::to_string(f.maker_id) +
           " px=" + std::to_string(f.price) + " qty=" + std::to_string(f.quantity);
}

static void run_seed(uint64_t seed, uint64_t ops, Stats& st) {
    std::mt19937_64 rng(seed);
    auto uni = [&](int64_t lo, int64_t hi) { return std::uniform_int_distribution<int64_t>(lo, hi)(rng); };

    const int64_t mid        = 10'000;
    const int64_t band       = uni(1, 25);
    const int64_t cancel_pct = uni(10, 50);
    const int64_t max_qty    = uni(1, 3) == 1 ? 5 : 500;
    const std::size_t depth_cap = static_cast<std::size_t>(uni(20, 400));

    OrderBook fast;
    ReferenceBook ref;
    std::vector<uint64_t> ids;
    uint64_t next_id = 1;

    for (uint64_t op = 0; op < ops; ++op) {
        const std::size_t fills_before = fast.fills.size();

        const bool at_cap = ref.resting.size() >= depth_cap;
        if (!ids.empty() && (at_cap || uni(1, 100) <= cancel_pct)) {
            uint64_t id;
            const int64_t kind = at_cap ? 1 : uni(1, 10);
            if (kind <= 7 && !ref.resting.empty())
                id = ref.resting[static_cast<std::size_t>(uni(0, static_cast<int64_t>(ref.resting.size()) - 1))].order.id;
            else if (kind <= 9)
                id = ids[static_cast<std::size_t>(uni(0, static_cast<int64_t>(ids.size()) - 1))];
            else
                id = next_id + static_cast<uint64_t>(uni(0, 1000));
            fast.cancel(id);
            ref.cancel(id);
            ++st.cancels;
        } else {
            const Order o{next_id++, uni(0, 1) == 1, mid + uni(-band, band),
                          static_cast<uint64_t>(uni(1, max_qty))};
            ids.push_back(o.id);
            fast.submit(o);
            ref.submit(o);
            ++st.submits;
            if (fast.order_index.count(o.id)) ++st.rested;
        }

        if (fast.fills.size() != ref.fills.size())
            fail(seed, op, "fill count differs",
                 "fast=" + std::to_string(fast.fills.size()) + " ref=" + std::to_string(ref.fills.size()));
        for (std::size_t i = fills_before; i < fast.fills.size(); ++i) {
            const Fill& a = fast.fills[i];
            const Fill& b = ref.fills[i];
            if (a.taker_id != b.taker_id || a.maker_id != b.maker_id || a.price != b.price ||
                a.quantity != b.quantity)
                fail(seed, op, "fill differs", "fast: " + show(a) + "\nref:  " + show(b));
        }
        st.fills += fast.fills.size() - fills_before;

        if (snapshot(fast) != snapshot(ref)) fail(seed, op, "book contents differ", "");
        if (std::string why = audit(fast); !why.empty()) fail(seed, op, "audit failed", why);
        if (fast.order_index.size() > st.max_depth) st.max_depth = fast.order_index.size();
    }
}

int main(int argc, char** argv) {
    const uint64_t seeds      = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200;
    const uint64_t ops        = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 20'000;
    const uint64_t first_seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;

    Stats st;
    for (uint64_t s = first_seed; s < first_seed + seeds; ++s) run_seed(s, ops, st);

    std::printf("stress test PASSED: %llu seeds x %llu ops = %llu ops, each checked against the reference\n"
                "  %llu submits (%llu rested), %llu cancels, %llu fills, max resting orders %llu\n",
                static_cast<unsigned long long>(seeds), static_cast<unsigned long long>(ops),
                static_cast<unsigned long long>(seeds * ops), static_cast<unsigned long long>(st.submits),
                static_cast<unsigned long long>(st.rested), static_cast<unsigned long long>(st.cancels),
                static_cast<unsigned long long>(st.fills), static_cast<unsigned long long>(st.max_depth));
    return 0;
}
