// Sources that deliver in every awkward way, against SourceTiming and the sender's
// numbering. Each refresh should get exactly one new picture: none is a repeat, two
// is a collision where one picture replaces another. A steady source must pay no
// extra latency. Needs no hardware.
#include "source_timing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <vector>

using groovy::SourceTiming;

namespace {

constexpr double kPeriod = 1000.0 / 60.0;  // ms, source and raster
constexpr double kPrep = 1.0;              // ms from handover to sending, like scaling and LZ4
constexpr double kNumberMargin = 1.0;      // ms, as in the sender's numbering
constexpr double kLandMargin = 2.0;        // ms before the next frame starts, where the lock puts pictures
// As in the sender: a field sent this soon after the last one waits while Groovy
// unpacks that one, so it is numbered with this much more margin.
constexpr double kBackToBack = 2.0;
constexpr double kBackToBackMargin = 1.5;

struct Picture {
    double stamp = 0.0;    // ms
    double arrival = 0.0;  // ms
};

struct Result {
    int frames = 0, repeats = 0, collisions = 0;
    long long lead_max = 0; // refreshes ahead a field went, 1 normally
    int lead2 = 0, sent = 0; // fields sent two refreshes ahead, and all fields
    double latency_avg = 0.0, latency_max = 0.0, wait_max = 0.0;
};

// The raster is placed the way the lock settles it: a picture arriving at the usual
// worst delay is sent kLandMargin before a frame starts. Frame k starts at
// phase + k * period. `lag_ms` moves the raster from there, as while the trim is
// still catching up after the source's timing changed.
Result run(const std::vector<Picture>& pictures, bool use_model, size_t warmup, double lag_ms = 0.0) {
    SourceTiming timing;
    double phase = 0.0;
    bool placed = false;
    std::map<long long, int> per_frame;
    Result r;
    double latency_sum = 0.0;
    int latency_n = 0;
    double last_send = -1e9;
    bool have_last = false;
    long long last_frame = 0;
    for (size_t i = 0; i < pictures.size(); ++i) {
        const Picture& p = pictures[i];
        const double delay = p.arrival - p.stamp;
        timing.note_arrival(delay);
        const double ready = p.arrival + kPrep;
        const double early = use_model ? timing.early_ms(delay) : 0.0;
        if (!placed && i + 1 == warmup) {
            const double belongs = ready + std::max(0.0, early);
            phase = belongs + kLandMargin - std::ceil((belongs + kLandMargin) / kPeriod) * kPeriod + lag_ms;
            placed = true;
        }
        if (!placed) continue;
        // Numbered for the next frame the raster can still take at that moment.
        const auto number_at = [&](double t) {
            const double margin = kNumberMargin + (t - last_send < kBackToBack ? kBackToBackMargin : 0.0);
            return static_cast<long long>(std::floor((t - phase) / kPeriod + margin / kPeriod)) + 1;
        };
        // As in the sender: wait only if this refresh is already taken by the
        // previous picture and the model says this one belongs at the next.
        double send = ready;
        if (use_model && have_last && early > 0.0 && number_at(ready) <= last_frame &&
            number_at(ready + early) >= last_frame + 1) {
            const double earliest = phase + (static_cast<double>(last_frame + 1) - 1.0 - kNumberMargin / kPeriod) * kPeriod;
            // Never longer than the picture came early.
            if (earliest <= ready + early) send = std::max(ready, earliest);
        }
        r.wait_max = std::max(r.wait_max, send - ready);
        const long long frame = number_at(send);
        const long long lead = frame - static_cast<long long>(std::floor((send - phase) / kPeriod));
        r.lead_max = std::max(r.lead_max, lead);
        if (lead >= 2) ++r.lead2;
        ++r.sent;
        last_send = send;
        last_frame = frame;
        have_last = true;
        ++per_frame[frame];
        const double scan = phase + static_cast<double>(frame) * kPeriod;
        latency_sum += scan - p.arrival;
        ++latency_n;
        r.latency_max = std::max(r.latency_max, scan - p.arrival);
    }
    if (per_frame.size() > 2) {
        const long long first = per_frame.begin()->first + 1, last = per_frame.rbegin()->first - 1;
        for (long long k = first; k <= last; ++k) {
            const auto it = per_frame.find(k);
            const int n = it == per_frame.end() ? 0 : it->second;
            ++r.frames;
            if (n == 0) ++r.repeats;
            if (n > 1) r.collisions += n - 1;
        }
    }
    r.latency_avg = latency_n ? latency_sum / latency_n : 0.0;
    return r;
}

std::vector<Picture> stream(int n, const std::function<double(int)>& delay, const std::function<bool(int)>& dropped = {}) {
    std::vector<Picture> out;
    double last_arrival = -1e9;
    for (int i = 0; i < n; ++i) {
        if (dropped && dropped(i)) continue;
        Picture p;
        p.stamp = i * kPeriod;
        // Pictures arrive in order, however late the one before was.
        p.arrival = std::max(p.stamp + delay(i), last_arrival + 0.05);
        last_arrival = p.arrival;
        out.push_back(p);
    }
    return out;
}

int failures = 0;

void check(const char* name, bool ok, const Result& r) {
    // A field sent close to a refresh boundary goes two ahead, as in 1.0, and the
    // core keeps it until then. Never further.
    const double lead2_share = r.sent ? 100.0 * r.lead2 / r.sent : 0.0;
    ok = ok && r.lead_max <= 2;
    std::printf("%-46s %s  frames %5d  repeats %3d  collisions %3d  lead2 %4.1f%% max %lld  latency %5.2f ms (max %5.2f)  wait max %5.2f\n",
                name, ok ? "ok  " : "FAIL", r.frames, r.repeats, r.collisions, lead2_share, r.lead_max, r.latency_avg, r.latency_max,
                r.wait_max);
    if (!ok) ++failures;
}

} // namespace

int main() {
    std::mt19937 rng(7);
    const int n = 3600; // a minute at 60 fps
    const size_t warmup = 200;

    // A capture card: stamped on arrival, a fraction of a millisecond of noise.
    std::uniform_real_distribution<double> card_noise(0.0, 0.5);
    const auto card = stream(n, [&](int) { return card_noise(rng); });
    const Result steady = run(card, true, warmup);
    const Result steady_plain = run(card, false, warmup);
    check("capture card", steady.repeats == 0 && steady.collisions == 0 && steady.lead2 == 0, steady);
    check("capture card pays nothing for the model",
          std::abs(steady.latency_avg - steady_plain.latency_avg) < 0.5 && steady.wait_max < 0.6, steady);

    // A virtual camera handing pictures over in pairs, 2 ms apart.
    const auto clumped = stream(n, [](int i) { return i % 2 == 0 ? 14.7 : 0.0; });
    check("clumped pairs", [&] { const Result r = run(clumped, true, warmup); return r.repeats == 0 && r.collisions == 0; }(),
          run(clumped, true, warmup));

    // Random jitter over 14 ms, as OBS can be.
    std::uniform_real_distribution<double> wide(0.0, 14.0);
    const auto jittery = stream(n, [&](int) { return wide(rng); });
    const Result jit = run(jittery, true, warmup);
    check("random 14 ms jitter", jit.repeats <= 5 && jit.collisions <= 5, jit);
    // Without the model, the same stream must go wrong, or this test proves nothing.
    const Result jit_plain = run(jittery, false, warmup);
    check("random jitter without the model fails (sanity)", jit_plain.repeats + jit_plain.collisions > 100, jit_plain);

    // Dropped capture frames: their refresh repeats, and nothing else goes wrong.
    const auto drops = stream(n, [&](int) { return card_noise(rng); }, [](int i) { return i % 300 == 150; });
    const Result dr = run(drops, true, warmup);
    check("a dropped frame every 5 s", dr.collisions == 0 && dr.repeats <= 12 + 1, dr);

    // A lasting step: the delay goes up by 5 ms for good. A few refreshes may suffer,
    // then it must be clean again.
    const auto step = stream(n, [&](int i) { return card_noise(rng) + (i >= 1500 ? 5.0 : 0.0); });
    const Result st = run(step, true, warmup);
    check("delay steps up 5 ms for good", st.repeats + st.collisions <= 8, st);

    // One stray slow picture in a steady stream costs at most one refresh.
    const auto stray = stream(n, [&](int i) { return card_noise(rng) + (i == 2000 ? 12.0 : 0.0); });
    const Result sr = run(stray, true, warmup);
    check("one stray picture 12 ms late", sr.repeats <= 1 && sr.collisions <= 1, sr);

    // A slow drift of the delay, 1 ms over the minute.
    const auto drift = stream(n, [&](int i) { return card_noise(rng) + i / 3600.0; });
    const Result dt = run(drift, true, warmup);
    check("delay drifts 1 ms over a minute", dt.repeats == 0 && dt.collisions == 0, dt);

    // The raster away from where the model wants it, as for the 20 s or so the trim
    // needs after the source's timing changes. Pictures must still never go two
    // refreshes ahead, and a capture card must stay clean.
    for (const double lag : {-6.0, -3.0, 3.0, 6.0}) {
        char name[64];
        std::snprintf(name, sizeof(name), "random jitter, raster %+.0f ms off", lag);
        const Result lj = run(jittery, true, warmup, lag);
        check(name, lj.collisions <= 120, lj);
        std::snprintf(name, sizeof(name), "clumped pairs, raster %+.0f ms off", lag);
        const Result lc = run(clumped, true, warmup, lag);
        check(name, lc.collisions <= 120, lc);
        std::snprintf(name, sizeof(name), "capture card, raster %+.0f ms off", lag);
        const Result lk = run(card, true, warmup, lag);
        check(name, lk.repeats == 0 && lk.collisions == 0 && lk.lead2 == 0, lk);
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
