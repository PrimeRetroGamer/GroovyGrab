#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace groovy {

inline double rate_fit_seconds(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

// Least-squares line y = a + slope * x. Capture: sample index over
// time; raster: ACK position over time. Either slope is a rate in
// frames per second, and one late sample barely moves it.
struct LineFit {
    double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    void add(double x, double y) {
        n += 1.0;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    bool slope(double& out) const {
        const double denom = n * sxx - sx * sx;
        if (n < 3.0 || denom <= 0.0) return false;
        out = (n * sxy - sx * sy) / denom;
        return true;
    }
};

// Capture rate from sample times. Each sample is indexed against the
// fitted line itself (round of elapsed * rate) and samples more than
// a quarter period off the line are left out, so dropped samples
// just leave an index unused and late ones do not count.
struct RateFit {
    bool valid = false;
    std::chrono::steady_clock::time_point t0{};
    double fps = 0.0; // current estimate; seeds the indexing
    LineFit fit;
    void start(std::chrono::steady_clock::time_point t, double seed_fps) {
        valid = true;
        t0 = t;
        fps = seed_fps;
        fit = LineFit{};
        fit.add(0.0, 0.0);
    }
    void add(std::chrono::steady_clock::time_point t) {
        const double x = rate_fit_seconds(t - t0);
        const double predicted = x * fps;
        const double index = std::round(predicted);
        if (std::abs(predicted - index) > 0.25) return;
        fit.add(x, index);
        double slope = 0.0;
        if (fit.n >= 30.0 && fit.slope(slope) && slope > 0.0) fps = slope;
    }
};

// Capture rate measured from sample arrivals, independent of any stream.
class CaptureRateMeter {
public:
    void add(std::chrono::steady_clock::time_point t) {
        std::lock_guard lock(mutex_);
        if (!have_last_) {
            have_last_ = true;
            last_ = t;
            return;
        }
        if (first_gaps_.size() < 16) {
            first_gaps_.push_back(rate_fit_seconds(t - last_));
            last_ = t;
            if (first_gaps_.size() == 16) {
                std::nth_element(first_gaps_.begin(), first_gaps_.begin() + 8, first_gaps_.end());
                if (first_gaps_[8] > 0.0) fit_.start(t, 1.0 / first_gaps_[8]);
            }
            return;
        }
        if (fit_.valid) {
            fit_.add(t);
            last_ = t;
        }
    }

    void reset() {
        std::lock_guard lock(mutex_);
        have_last_ = false;
        first_gaps_.clear();
        fit_ = RateFit{};
    }

    // Frames per second, once `min_seconds` of samples have been fitted.
    bool rate(double min_seconds, double& fps) {
        std::lock_guard lock(mutex_);
        if (!fit_.valid || rate_fit_seconds(last_ - fit_.t0) < min_seconds) return false;
        return fit_.fit.slope(fps) && fps > 0.0;
    }

private:
    std::mutex mutex_;
    bool have_last_ = false;
    std::chrono::steady_clock::time_point last_{};
    std::vector<double> first_gaps_;
    RateFit fit_;
};

} // namespace groovy
