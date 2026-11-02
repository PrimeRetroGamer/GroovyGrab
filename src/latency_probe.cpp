#include "latency_probe.h"

#include <algorithm>
#include <cstdio>

namespace groovy {

bool LatencyProbe::add(const Sender& sender, bool is_fresh, clock::time_point captured,
                       double pickup_ms, double decode_ms, std::string& out) {
    const auto& t = sender.last_timing();
    if (last_sent != clock::time_point{}) {
        gap_max = std::max(gap_max, std::chrono::duration<double, std::milli>(
                                        t.sent_at - last_sent).count());
    }
    last_sent = t.sent_at;
    ++fields;
    convert += t.convert_ms;
    lz4 += t.lz4_ms;
    stream += t.stream_ms;
    sleep += t.sleep_ms;
    bytes += t.bytes;
    if (is_fresh) {
        ++fresh;
        pickup += pickup_ms;
        pickup_max = std::max(pickup_max, pickup_ms);
        decode += decode_ms;
    }
    if (!t.acked) {
        ++noack;
    } else {
        ++acked;
        const double ack_ms = std::chrono::duration<double, std::milli>(
                                  t.ack_at - t.sent_at).count();
        ack += ack_ms;
        ack_max = std::max(ack_max, ack_ms);
        lead += t.lead;
        lead_min = std::min(lead_min, t.lead);
        lead_max = std::max(lead_max, t.lead);
        vcount += t.vcount;
        // Blit periods until scanout, from vcount/v_total (a field
        // fraction in interlaced modes, where vcount steps by 2).
        const double vt = sender.v_total();
        const double periods =
            t.lead >= 1 ? (t.lead - 1) + (1.0 - t.vcount / vt) : 0.0;
        const double scan_ms = periods * sender.field_ms();
        scan += scan_ms;
        if (is_fresh) {
            const double age_ms = std::chrono::duration<double, std::milli>(
                                      t.ack_at - captured).count() + scan_ms;
            age += age_ms;
            ++fresh_acked;
            age_min = std::min(age_min, age_ms);
            age_max = std::max(age_max, age_ms);
        }
    }
    const auto now = clock::now();
    if (now - start < std::chrono::seconds(1)) return false;
    const double f = fields ? fields : 1, n = fresh ? fresh : 1,
                 a = acked ? acked : 1;
    char line[512];
    std::snprintf(line, sizeof(line),
        "lat fields %u fresh %u noack %u | pickup %.2f (max %.2f) decode %.2f "
        "convert %.2f lz4 %.2f (%.0f KB) stream %.2f | send gap max %.1f "
        "ack %.2f (max %.2f) sleep %.2f | lead %.2f [%d..%d] vcount %.0f "
        "scan %.2f | capture->scanout %.2f [%.2f..%.2f]",
        fields, fresh, noack, pickup / n, pickup_max, decode / n,
        convert / f, lz4 / f, bytes / f / 1024.0, stream / f, gap_max,
        ack / a, ack_max, sleep / f, lead / a, lead_min,
        lead_max, vcount / a, scan / a, age / (fresh_acked ? fresh_acked : 1),
        age_min, age_max);
    out = line;
    const auto keep_last = last_sent;
    *this = LatencyProbe{};
    last_sent = keep_last;
    return true;
}

} // namespace groovy
