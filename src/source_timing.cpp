#include "source_timing.h"

#include <algorithm>

namespace groovy {

void SourceTiming::reset() {
    count_ = next_ = 0;
    allowance_ms_ = 0.0;
}

// The second longest delay of the window, so one stray picture doesn't count.
double SourceTiming::worst_in_window() const {
    std::array<double, kWindow> sorted = delays_;
    const auto end = sorted.begin() + static_cast<ptrdiff_t>(count_);
    const auto pick = count_ >= 2 ? end - 2 : end - 1;
    std::nth_element(sorted.begin(), pick, end);
    return *pick;
}

void SourceTiming::note_arrival(double delay_ms) {
    delays_[next_] = delay_ms;
    next_ = (next_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;
    const double worst = worst_in_window();
    if (count_ <= kMinPictures) {
        allowance_ms_ = worst;
        return;
    }
    allowance_ms_ += (worst - allowance_ms_) * (worst > allowance_ms_ ? kRise : kFall);
}

} // namespace groovy
