#include "rate_lock.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace groovy {

RatePlan plan_rate(double capture_fps, double mode_fps) {
    RatePlan plan;
    const auto try_plan = [&](unsigned repeat, unsigned skip) {
        const double trim = capture_fps * repeat / skip / mode_fps;
        if (std::abs(trim - 1.0) > RateLock::kMaxTrim) return false;
        plan.repeat = repeat;
        plan.skip = skip;
        plan.trim = trim;
        return true;
    };
    for (unsigned repeat = 1; repeat <= 4; ++repeat) {
        if (try_plan(repeat, 1)) return plan;
    }
    try_plan(1, 2);
    return plan;
}

void RateLock::note(const char* format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    log += line;
}

double RateLock::wrap(double x) {
    return x - std::floor(x + 0.5);
}

void RateLock::start_parked(const RatePlan& rate_plan, double capture_fps) {
    plan = rate_plan;
    ref_fps = capture_fps / plan.skip;
    state = State::Locked;
    trim_sum = trim_sent = trim_ppm;
    trim_since = clock::now();
    release_pending = true;
    note("Rate lock: capture %.4f fps, %u blit(s) per %u sample(s), pixel clock x%.6f\n",
         capture_fps, plan.repeat, plan.skip, plan.trim);
}

void RateLock::go_free(const char* why) {
    note("Rate lock off: %s\n", why);
    state = State::Free;
    plan = RatePlan{};
}

bool RateLock::take_release() {
    const bool pending = release_pending;
    release_pending = false;
    return pending;
}

bool RateLock::accept_sample() {
    samples_seen = (samples_seen + 1) % plan.skip;
    return samples_seen == 0;
}

double RateLock::median_gap_ms() const {
    std::array<double, 64> sorted{};
    std::copy_n(gaps.begin(), gap_count, sorted.begin());
    const auto mid = sorted.begin() + static_cast<ptrdiff_t>(gap_count / 2);
    std::nth_element(sorted.begin(), mid, sorted.begin() + static_cast<ptrdiff_t>(gap_count));
    return *mid;
}

RateLock::Action RateLock::update_trim(const Sender& sender) {
    double position = 0.0;
    clock::time_point at{};
    if (!sender.raster_sample(position, at)) return Action::None;
    const auto now = clock::now();
    // Where the raster is when this picture would have arrived at the source's usual
    // worst delay, so a clumpy source doesn't swing the trim.
    position += seconds(now + phase_shift - at) * 1000.0 / sender.field_ms();
    const double want = sender.release_fraction() - kTrimLandMarginSeconds * 1000.0 / sender.field_ms();
    if (trim_error_count < trim_errors.size()) {
        trim_errors[trim_error_count++] = wrap(position - std::floor(position) - want);
    }
    if (now - trim_since < kTrimInterval) return Action::None;
    const double elapsed = seconds(now - trim_since);
    trim_since = now;
    const auto mid = trim_errors.begin() + static_cast<ptrdiff_t>(trim_error_count / 2);
    std::nth_element(trim_errors.begin(), mid, trim_errors.begin() + static_cast<ptrdiff_t>(trim_error_count));
    const double error = *mid; // positive: the raster is further along than wanted
    trim_error_count = 0;

    // A step has to last into a second reading: a burst of late ACKs can move just one.
    const bool jumped = trim_have_error && !trim_stepping && std::abs(wrap(error - trim_last_error)) > kTrimStep;
    if (jumped && trim_step_seen) {
        trim_stepping = true;
        note("Rate lock: capture timing stepped %.2f ms, moving the raster to match\n",
             wrap(error - trim_last_error) * sender.field_ms());
    }
    trim_step_seen = jumped && !trim_stepping;
    if (!trim_step_seen) trim_last_error = error;
    trim_have_error = true;
    if (std::abs(error) < kTrimStepDone) trim_stepping = false;

    const double next_sum = trim_stepping ? trim_sum : trim_sum - kTrimIntegral * error * elapsed;
    const double next = next_sum - (trim_stepping ? kTrimStepGain : kTrimGain) * error;
    // Hold the integral at the limit, or it would overshoot when the error clears.
    if (std::abs(next) <= kTrimLimit) trim_sum = next_sum;
    trim_ppm = std::clamp(next, -kTrimLimit, kTrimLimit);
    if (std::abs(trim_ppm - trim_sent) < kTrimResend) return Action::None;
    trim_sent = trim_ppm;
    return Action::Trim;
}

// Locked: is capture still running at the rate it was locked at?
RateLock::Action RateLock::check_capture_rate(double gap_ms, clock::time_point captured) {
    if (ref_fps <= 0.0 || gap_ms <= 0.0 || gap_count < 32) return Action::None;
    const double ref_ms = 1000.0 / ref_fps;
    const double median = median_gap_ms();
    if (std::abs(median / ref_ms - 1.0) > kIntervalChange) {
        note("Rate lock restart: capture interval %.2f ms, was %.2f ms\n", median, ref_ms);
        return Action::Restart;
    }
    if (!block_rate.valid) {
        block_rate.start(captured, ref_fps);
        return Action::None;
    }
    block_rate.add(captured);
    if (captured - block_rate.t0 < kRateBlock) return Action::None;
    double fps = 0.0;
    block_rate.valid = false;
    if (!block_rate.fit.slope(fps)) return Action::None;
    if (std::abs(fps / ref_fps - 1.0) > kRateChange) {
        note("Rate lock restart: capture %.4f fps, was %.4f fps\n", fps, ref_fps);
        return Action::Restart;
    }
    return Action::None;
}

RateLock::Action RateLock::after_send(const Sender& sender, clock::time_point source, bool track_phase,
                                       clock::duration shift) {
    phase_shift = shift;
    if (state != State::Locked) return Action::None;
    if (sender.field_ms() <= 0.0) return Action::None;

    double gap_ms = 0.0;
    if (have_arrival) {
        gap_ms = std::chrono::duration<double, std::milli>(source - last_arrival).count();
        record_gap(gap_ms);
    }
    last_arrival = source;
    have_arrival = true;
    if (const Action change = check_capture_rate(gap_ms, source); change != Action::None) {
        return change;
    }
    // The rate was recorded above. A frame from a previous graph does not move the phase.
    if (!track_phase) return Action::None;
    return update_trim(sender);
}

void RateLock::record_gap(double gap_ms) {
    if (gap_ms <= 0.0) return;
    gaps[gap_next] = gap_ms;
    gap_next = (gap_next + 1) % gaps.size();
    gap_count = std::min(gap_count + 1, gaps.size());
}

} // namespace groovy
