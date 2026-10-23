#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "order_book.hpp"

using Clock = std::chrono::steady_clock;

enum class Kind : uint8_t { Passive, Marketable, Cancel };

struct Op {
    Kind  kind;
    Order order;
};

struct Workload {
    std::vector<Op> prefill;
    std::vector<Op> ops;
};

static Workload generate(std::size_t n_ops, uint64_t seed) {
    constexpr std::size_t kPrefill = 20'000;
    constexpr int64_t     kMid     = 100'000;
    constexpr int64_t     kDepth   = 50;

    std::mt19937_64 rng(seed);
    auto uni = [&](int64_t lo, int64_t hi) { return std::uniform_int_distribution<int64_t>(lo, hi)(rng); };

    OrderBook shadow;
    std::vector<uint64_t> live;
    uint64_t next_id = 1;
    Workload w;

    auto best_bid = [&] { return shadow.bids.empty() ? kMid - 1 : shadow.bids.rbegin()->first; };
    auto best_ask = [&] { return shadow.asks.empty() ? kMid + 1 : shadow.asks.begin()->first; };

    auto passive = [&]() -> Op {
        const bool buy = uni(0, 1) == 1;
        const int64_t px = buy ? best_ask() - uni(1, kDepth) : best_bid() + uni(1, kDepth);
        return {Kind::Passive, {next_id++, buy, px, static_cast<uint64_t>(uni(1, 100))}};
    };
    auto marketable = [&]() -> Op {
        const bool buy = uni(0, 1) == 1;
        const int64_t px = buy ? best_ask() + uni(0, 3) : best_bid() - uni(0, 3);
        return {Kind::Marketable, {next_id++, buy, px, static_cast<uint64_t>(uni(1, 300))}};
    };
    auto apply = [&](std::vector<Op>& out, const Op& op) {
        if (op.kind == Kind::Cancel) {
            shadow.cancel(op.order.id);
        } else {
            shadow.submit(op.order);
            if (shadow.order_index.count(op.order.id)) live.push_back(op.order.id);
        }
        shadow.fills.clear();
        out.push_back(op);
    };

    w.prefill.reserve(kPrefill);
    for (std::size_t i = 0; i < kPrefill; ++i) apply(w.prefill, passive());

    w.ops.reserve(n_ops);
    while (w.ops.size() < n_ops) {
        const int64_t r = uni(1, 100);
        const int64_t cancel_pct = shadow.order_index.size() > kPrefill ? 45 : 30;
        if (r <= cancel_pct && !shadow.order_index.empty()) {
            uint64_t id = 0;
            do {
                const std::size_t i = static_cast<std::size_t>(uni(0, static_cast<int64_t>(live.size()) - 1));
                id = live[i];
                live[i] = live.back();
                live.pop_back();
            } while (!shadow.order_index.count(id));
            apply(w.ops, {Kind::Cancel, {id, false, 0, 0}});
        } else if (r <= 90) {
            apply(w.ops, passive());
        } else {
            apply(w.ops, marketable());
        }
    }
    return w;
}

static void prefill(OrderBook& b, const Workload& w) {
    for (const Op& op : w.prefill) b.submit(op.order);
    b.fills.clear();
}

static std::pair<double, uint64_t> run_throughput(const Workload& w) {
    OrderBook b;
    prefill(b, w);
    uint64_t fills = 0;
    const auto t0 = Clock::now();
    for (const Op& op : w.ops) {
        if (op.kind == Kind::Cancel) b.cancel(op.order.id);
        else                         b.submit(op.order);
        fills += b.fills.size();
        b.fills.clear();
    }
    const auto t1 = Clock::now();
    if (b.order_index.empty()) std::puts("");
    return {static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()), fills};
}

static double pct(std::vector<uint32_t>& v, double p) {
    if (v.empty()) return 0;
    const std::size_t k = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(v.size()))) - 1;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

int main(int argc, char** argv) {
    const std::size_t n_ops = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    const int         runs  = argc > 2 ? std::atoi(argv[2]) : 5;
    const uint64_t    seed  = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 42;

    const Workload w = generate(n_ops, seed);
    std::size_t counts[3] = {};
    for (const Op& op : w.ops) ++counts[static_cast<int>(op.kind)];

    std::size_t depth = 0, levels = 0, min_depth = SIZE_MAX, max_depth = 0;
    {
        OrderBook b;
        prefill(b, w);
        for (const Op& op : w.ops) {
            if (op.kind == Kind::Cancel) b.cancel(op.order.id); else b.submit(op.order);
            b.fills.clear();
            min_depth = std::min(min_depth, b.order_index.size());
            max_depth = std::max(max_depth, b.order_index.size());
        }
        depth = b.order_index.size();
        levels = b.bids.size() + b.asks.size();
    }

    std::printf("workload: %zu prefill orders, then %zu ops (seed %llu)\n", w.prefill.size(), w.ops.size(),
                static_cast<unsigned long long>(seed));
    std::printf("  passive %zu (%.0f%%), cancel %zu (%.0f%%), marketable %zu (%.0f%%)\n", counts[0],
                100.0 * static_cast<double>(counts[0]) / static_cast<double>(n_ops), counts[2],
                100.0 * static_cast<double>(counts[2]) / static_cast<double>(n_ops), counts[1],
                100.0 * static_cast<double>(counts[1]) / static_cast<double>(n_ops));
    std::printf("  book depth during run: %zu..%zu resting orders; at end %zu across %zu price levels\n\n",
                min_depth, max_depth, depth, levels);

    std::vector<double> ns;
    uint64_t fills = 0;
    for (int r = 0; r < runs; ++r) {
        auto [t, f] = run_throughput(w);
        ns.push_back(t);
        fills = f;
    }
    std::sort(ns.begin(), ns.end());
    const double med = ns[ns.size() / 2];
    std::printf("throughput (%d runs, no per-op timers):\n", runs);
    std::printf("  median %.2f M ops/s  (%.1f ns/op)   best %.2f M ops/s   worst %.2f M ops/s\n",
                static_cast<double>(n_ops) / med * 1e3, med / static_cast<double>(n_ops),
                static_cast<double>(n_ops) / ns.front() * 1e3, static_cast<double>(n_ops) / ns.back() * 1e3);
    std::printf("  %llu fills per run\n\n", static_cast<unsigned long long>(fills));

    uint64_t tick = UINT64_MAX;
    for (int i = 0; i < 100'000; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        const auto d = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        if (d > 0) tick = std::min(tick, d);
    }
    const auto c0 = Clock::now();
    for (int i = 0; i < 1'000'000; ++i) (void)Clock::now();
    const double now_cost = static_cast<double>((Clock::now() - c0).count()) / 1e6;

    std::vector<uint32_t> lat[3];
    for (auto& v : lat) v.reserve(n_ops);
    {
        OrderBook b;
        prefill(b, w);
        for (const Op& op : w.ops) {
            const auto t0 = Clock::now();
            if (op.kind == Kind::Cancel) b.cancel(op.order.id); else b.submit(op.order);
            const auto t1 = Clock::now();
            b.fills.clear();
            lat[static_cast<int>(op.kind)].push_back(static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }
    }
    std::vector<uint32_t> all;
    for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());

    std::printf("latency per op, ns (single run; clock now() costs ~%.0f ns, clock resolution ~%llu ns):\n",
                now_cost, static_cast<unsigned long long>(tick));
    std::printf("  %-11s %8s %8s %8s %8s %9s\n", "op", "p50", "p90", "p99", "p99.9", "max");
    auto row = [&](const char* name, std::vector<uint32_t>& v) {
        const double mx = v.empty() ? 0 : *std::max_element(v.begin(), v.end());
        std::printf("  %-11s %8.0f %8.0f %8.0f %8.0f %9.0f\n", name, pct(v, 50), pct(v, 90), pct(v, 99),
                    pct(v, 99.9), mx);
    };
    row("passive", lat[0]);
    row("cancel", lat[2]);
    row("marketable", lat[1]);
    row("all", all);
    return 0;
}
