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

// Raster rate. Samples are kept so the fit can drop outliers (an ACK read
// late, a frame-boundary misread) instead of letting a few skew the rate.
struct RobustFit {
    std::vector<std::pair<double, double>> samples;
    double rms = 0.0, worst = 0.0; // frames, after outlier removal
    size_t dropped = 0;
    void clear() { samples.clear(); }
    void add(double x, double y) {
        if (samples.size() >= 40000) {
            // Keep every other sample: same span, half the cost.
            size_t w = 0;
            for (size_t r = 0; r < samples.size(); r += 2) samples[w++] = samples[r];
            samples.resize(w);
        }
        samples.emplace_back(x, y);
    }
    static bool fit(const std::vector<std::pair<double, double>>& s,
                    const std::vector<bool>* keep, double& a, double& b) {
        LineFit f;
        for (size_t i = 0; i < s.size(); ++i) {
            if (!keep || (*keep)[i]) f.add(s[i].first, s[i].second);
        }
        if (!f.slope(b)) return false;
        a = (f.sy - b * f.sx) / f.n;
        return true;
    }
    bool slope(double& out) {
        double a = 0.0, b = 0.0;
        if (!fit(samples, nullptr, a, b)) return false;
        // Outliers skew the line they are measured from, so one pass misses some.
        std::vector<bool> keep(samples.size(), true);
        for (int pass = 0; pass < 3; ++pass) {
            std::vector<double> dev;
            dev.reserve(samples.size());
            for (size_t i = 0; i < samples.size(); ++i) {
                if (keep[i]) dev.push_back(std::abs(samples[i].second - (a + b * samples[i].first)));
            }
            std::nth_element(dev.begin(), dev.begin() + dev.size() / 2, dev.end());
            const double limit = std::max(5.0 * dev[dev.size() / 2], 0.01);
            for (size_t i = 0; i < samples.size(); ++i) {
                if (std::abs(samples[i].second - (a + b * samples[i].first)) > limit) keep[i] = false;
            }
            if (!fit(samples, &keep, a, b)) return false;
        }
        double sum = 0.0;
        size_t n = 0;
        worst = 0.0;
        dropped = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            if (!keep[i]) {
                ++dropped;
                continue;
            }
            const double r = samples[i].second - (a + b * samples[i].first);
            sum += r * r;
            worst = std::max(worst, std::abs(r));
            ++n;
        }
        rms = n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
        out = b;
        return true;
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
