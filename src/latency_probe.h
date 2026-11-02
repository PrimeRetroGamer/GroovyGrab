#pragma once

#include "groovy_sender.h"

#include <chrono>
#include <string>

namespace groovy {

// Aggregates Sender::FieldTiming into one line per second for --stats. The
// sender thread must not print it itself: a console write there stalls it long
// enough to miss Groovy's raster.
// "scan" takes a field N to start scanning out when the ACK's gpu_frame reaches
// N: (lead - 1) whole frames plus the rest of the current one.
struct LatencyProbe {
    using clock = std::chrono::steady_clock;

    // Returns true when `out` holds a finished one-second line.
    bool add(const Sender& sender, bool is_fresh, clock::time_point captured, double pickup_ms,
             double decode_ms, std::string& out);

private:
    clock::time_point start = clock::now();
    unsigned fields = 0, fresh = 0, noack = 0, acked = 0, fresh_acked = 0;
    double pickup = 0, pickup_max = 0, decode = 0, convert = 0, lz4 = 0, stream = 0, ack = 0,
           ack_max = 0, sleep = 0, scan = 0, age = 0, age_min = 1e9, age_max = 0, bytes = 0,
           vcount = 0, lead = 0;
    int lead_min = 1 << 30, lead_max = -(1 << 30);
    // Longest gap between consecutive sends: a stalled sender shows here.
    clock::time_point last_sent{};
    double gap_max = 0.0;
};

} // namespace groovy
