#include "async_sender.h"

#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

namespace groovy {

namespace {

using clock = std::chrono::steady_clock;

// Timed waits on Windows overshoot by 1-2 ms, enough to miss a repeat deadline:
// sleep until this long before it, then yield-spin.
constexpr auto kSpinBeforeDeadline = std::chrono::microseconds(2500);

constexpr auto kFirstSampleLimit = std::chrono::milliseconds(250);

constexpr size_t kMaxQueuedAudioBytes = 48000u * 4u / 25u; // 40 ms of 48 kHz stereo s16

// Free-running with a usable capture rate: each window is timed afresh, as a source that
// has come on starts a new timing, and two in a row have to agree.
constexpr auto kFreeWindow = std::chrono::seconds(5);
constexpr double kFreeAgreement = 0.001;
// More relocks than this in the time below means the source won't hold still: it stays
// free-running for that long, so the Calibrating image doesn't keep coming back.
constexpr size_t kMaxRelocks = 3;
constexpr auto kRelockWindow = std::chrono::minutes(2);

} // namespace

bool AsyncSender::send_at_deadline(const PreparedField& field) {
    const auto deadline = sender_.next_frame_deadline(true);
    std::this_thread::sleep_until(deadline - kSpinBeforeDeadline);
    while (clock::now() < deadline) std::this_thread::yield();
    double elapsed_ms = 0.0;
    const bool sent = sender_.send_prepared(field, elapsed_ms);
    add_log(sender_.take_log());
    return sent;
}

bool AsyncSender::open(const std::string& ip, VideoMode mode, uint32_t audio_rate_hz) {
    stop_session();
    if (cancel_open_.load()) return false;
    relocks_.clear();
    free_until_ = {};
    if (!sender_.open(ip, mode, audio_rate_hz, 1.0)) {
        report_sender_error();
        return false;
    }
    return calibrate_and_start(mode);
}

void AsyncSender::start_open(const std::string& ip, VideoMode mode, uint32_t audio_rate_hz) {
    if (connected_.load()) return;
    bool expected = false;
    if (!opening_.compare_exchange_strong(expected, true)) return;
    cancel_open_.store(false);
    // The last attempt's error stays up until one succeeds.
    if (open_thread_.joinable()) open_thread_.join();
    open_thread_ = std::thread([this, ip, mode, audio_rate_hz] {
        open(ip, mode, audio_rate_hz);
        opening_.store(false);
    });
}

void AsyncSender::start_recalibrate() {
    bool expected = false;
    if (!connected_.load() || !opening_.compare_exchange_strong(expected, true)) return;
    cancel_open_.store(false);
    connected_.store(false);
    set_error("");
    stop_workers();
    if (open_thread_.joinable()) open_thread_.join();
    open_thread_ = std::thread([this, mode = sender_.mode()] {
        calibrate_and_start(mode);
        opening_.store(false);
    });
}

void AsyncSender::close() {
    cancel_open_.store(true);
    if (opening_.load() && !connected_.load()) sender_.interrupt();
    if (open_thread_.joinable()) open_thread_.join();
    opening_.store(false);
    stop_session();
    set_error("");
}

void AsyncSender::stop_session() {
    connected_.store(false);
    stop_workers();
    sender_.close();
    std::lock_guard lock(mutex_);
    pending_ready_ = false;
    pending_.clear();
    std::lock_guard audio_lock(audio_mutex_);
    audio_.clear();
}

bool AsyncSender::set_mode(VideoMode mode) {
    stop_workers();
    mode = plan_start(mode);
    if (!sender_.set_mode(mode, start_plan_.trim > 0.0 ? start_plan_.trim : 1.0)) {
        connected_.store(false);
        report_sender_error();
        return false;
    }
    start_workers();
    return true;
}

bool AsyncSender::set_audio(uint32_t audio_rate_hz) {
    stop_workers();
    sender_.set_audio_rate(audio_rate_hz);
    return set_mode(sender_.mode());
}

void AsyncSender::set_view(const ViewSettings& view) {
    letterbox_.store(view.letterbox);
    crop_h_.store(view.crop_h);
    crop_v_.store(view.crop_v);
}

// Caller holds mutex_ and has filled pending_.
void AsyncSender::publish_pending(PendingKind kind, unsigned width, unsigned height) {
    pending_at_ = clock::now();
    pending_w_ = width;
    pending_h_ = height;
    pending_kind_ = kind;
    if (pending_ready_) replaced_.fetch_add(1);
    pending_ready_ = true;
    pending_cv_.notify_one();
}

void AsyncSender::submit_packed422(const uint8_t* source, long pitch, unsigned width,
                                   unsigned height, bool uyvy) {
    capture_rate_.add(clock::now());
    if (!connected_.load() || !source || width < 2 || height < 1) return;
    const size_t row = static_cast<size_t>(width) * 2u;
    incoming_.resize(row * height);
    for (unsigned y = 0; y < height; ++y) {
        std::memcpy(incoming_.data() + y * row, source + static_cast<ptrdiff_t>(y) * pitch, row);
    }
    std::lock_guard lock(mutex_);
    pending_.swap(incoming_);
    publish_pending(uyvy ? PendingKind::Uyvy : PendingKind::Yuy2, width, height);
}

// Both 4:2:0 layouts are height * 1.5 rows of `pitch` bytes: two half-pitch I420
// chroma rows fill one. Stored tightly packed.
void AsyncSender::submit_planar420(const uint8_t* source, long pitch, unsigned width,
                                   unsigned height, bool i420) {
    capture_rate_.add(clock::now());
    if (!connected_.load() || !source || width < 2 || height < 2 ||
        pitch < static_cast<long>(width)) {
        return;
    }
    const size_t row = width;
    const unsigned rows = height + height / 2u;
    incoming_.resize(row * rows);
    for (unsigned y = 0; y < rows; ++y) {
        std::memcpy(incoming_.data() + y * row, source + static_cast<ptrdiff_t>(y) * pitch, row);
    }
    std::lock_guard lock(mutex_);
    pending_.swap(incoming_);
    publish_pending(i420 ? PendingKind::I420 : PendingKind::Nv12, width, height);
}

void AsyncSender::submit_mjpeg(const uint8_t* jpeg, size_t size, unsigned width, unsigned height) {
    capture_rate_.add(clock::now());
    if (!connected_.load() || !jpeg || size < 4 || width < 2 || height < 1) return;
    incoming_.assign(jpeg, jpeg + size);
    std::lock_guard lock(mutex_);
    pending_.swap(incoming_);
    publish_pending(PendingKind::Mjpeg, width, height);
}

void AsyncSender::submit_pcm(const uint8_t* pcm, size_t bytes) {
    if (!connected_.load() || !pcm || !bytes) return;
    std::lock_guard lock(audio_mutex_);
    audio_.insert(audio_.end(), pcm, pcm + bytes);
    if (audio_.size() > kMaxQueuedAudioBytes) {
        audio_.erase(audio_.begin(), audio_.begin() + static_cast<ptrdiff_t>(audio_.size() - kMaxQueuedAudioBytes));
    }
    audio_cv_.notify_one();
}

bool AsyncSender::copy_preview(std::vector<uint8_t>& dest, unsigned& width, unsigned& height,
                               uint64_t& seen) {
    std::lock_guard lock(preview_mutex_);
    if (preview_.empty() || preview_seq_ == seen) return false;
    dest.assign(preview_.begin(), preview_.end());
    width = preview_w_;
    height = preview_h_;
    seen = preview_seq_;
    return true;
}

void AsyncSender::publish_preview(const uint8_t* rgb, unsigned width, unsigned height) {
    std::lock_guard lock(preview_mutex_);
    preview_.assign(rgb, rgb + static_cast<size_t>(width) * height * 3u);
    preview_w_ = width;
    preview_h_ = height;
    ++preview_seq_;
}

AsyncSender::Statistics AsyncSender::take_statistics() {
    Statistics stats;
    stats.send_ms = static_cast<double>(send_microseconds_.exchange(0)) / 1000.0;
    stats.sent = sent_.exchange(0);
    stats.replaced = replaced_.exchange(0);
    stats.errors = errors_.exchange(0);
    return stats;
}

double AsyncSender::take_decode_ms() {
    const uint64_t micros = decode_microseconds_.exchange(0);
    const uint64_t count = decoded_frames_.exchange(0);
    return count ? static_cast<double>(micros) / 1000.0 / static_cast<double>(count) : 0.0;
}

std::string AsyncSender::take_probe_line() {
    std::lock_guard lock(probe_mutex_);
    return std::exchange(probe_line_, std::string());
}

std::string AsyncSender::take_log() {
    std::lock_guard lock(probe_mutex_);
    return std::exchange(log_, std::string());
}

std::string AsyncSender::error() const {
    std::lock_guard lock(error_mutex_);
    return error_;
}

void AsyncSender::set_error(std::string value) {
    std::lock_guard lock(error_mutex_);
    error_ = std::move(value);
}

void AsyncSender::report_sender_error() {
    set_error(sender_.error());
    if (!cancel_open_.load()) add_log("MiSTer: " + sender_.error_detail() + "\n");
}

void AsyncSender::add_log(const std::string& text) {
    if (text.empty()) return;
    std::lock_guard lock(probe_mutex_);
    log_ += text;
}

void AsyncSender::restart_rate_lock() {
    capture_rate_.reset();
    recalibrate_.store(true);
}

bool AsyncSender::measured_capture_rate(double& fps) {
    return capture_rate_.rate(kTrimCaptureSeconds, fps);
}

// Counts a relock about to happen. False if there have been too many lately.
bool AsyncSender::relock_allowed() {
    const auto now = clock::now();
    if (now < free_until_) return false;
    relocks_.erase(std::remove_if(relocks_.begin(), relocks_.end(),
                                  [&](clock::time_point at) { return now - at > kRelockWindow; }),
                   relocks_.end());
    if (relocks_.size() >= kMaxRelocks) {
        free_until_ = now + kRelockWindow;
        relocks_.clear();
        return false;
    }
    relocks_.push_back(now);
    return true;
}

// Free-running: true once capture has settled on a rate that some plan fits, as when
// the source has been turned on.
bool AsyncSender::free_run_can_lock(clock::time_point now, FreeWatch& watch) {
    if (now < watch.next) return false;
    const bool timed = watch.next != clock::time_point{};
    watch.next = now + kFreeWindow;
    double fps = 0.0;
    const bool measured = timed && capture_rate_.rate(kCaptureMeasureSeconds - 1.0, fps);
    capture_rate_.reset();
    const double previous = watch.fps;
    watch.fps = measured ? fps : 0.0;
    if (!measured || previous <= 0.0 || std::abs(fps / previous - 1.0) > kFreeAgreement) return false;
    const VideoMode mode = sender_.mode();
    const VideoMode other = video_mode_for(!is_pal_mode(mode), !is_interlaced(mode));
    return plan_rate(fps, Sender::nominal_mode_rate(mode) * start_clock_error()).trim > 0.0 ||
           plan_rate(fps, Sender::nominal_mode_rate(other) * start_clock_error()).trim > 0.0;
}

void AsyncSender::start_workers() {
    {
        std::lock_guard lock(mutex_);
        running_ = true;
        pending_ready_ = false;
        prep_new_ = false;
        prep_write_ = 0;
        prep_ready_ = 1;
        prep_front_ = 2;
    }
    audio_run_.store(sender_.pcm_bytes_per_chunk() != 0);
    // The mode is fixed for the threads' lifetime (set_mode restarts them); the
    // prep thread gets a copy because the sender thread can re-INIT.
    const VideoMode mode = sender_.mode();
    have_black_ = prepare_black(mode, black_);
    sender_thread_ = std::thread([this] { sender_loop(); });
    prep_thread_ = std::thread([this, mode] { prep_loop(mode); });
    if (audio_run_.load()) audio_thread_ = std::thread([this] { audio_loop(); });
}

void AsyncSender::stop_workers() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        pending_ready_ = false;
    }
    audio_run_.store(false);
    pending_cv_.notify_all();
    prepared_cv_.notify_all();
    audio_cv_.notify_all();
    if (sender_thread_.joinable()) sender_thread_.join();
    if (prep_thread_.joinable()) prep_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();
}

void AsyncSender::stop_on_send_error() {
    report_sender_error();
    errors_.fetch_add(1);
    connected_.store(false);
    audio_run_.store(false);
    audio_cv_.notify_all();
    std::lock_guard lock(mutex_);
    running_ = false;
    pending_ready_ = false;
    pending_cv_.notify_all();
    prepared_cv_.notify_all();
}

bool AsyncSender::take_pcm(std::vector<uint8_t>& out, size_t want) {
    std::unique_lock lock(audio_mutex_);
    audio_cv_.wait_for(lock, std::chrono::milliseconds(8), [this, want] {
        return !audio_run_.load() || audio_.size() >= want;
    });
    if (!audio_run_.load()) return false;
    size_t take = std::min(want, audio_.size());
    take -= take % 4;
    if (take < 4) return false;
    out.assign(audio_.begin(), audio_.begin() + static_cast<ptrdiff_t>(take));
    audio_.erase(audio_.begin(), audio_.begin() + static_cast<ptrdiff_t>(take));
    return true;
}

void AsyncSender::audio_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); // shares the socket lock
    const uint16_t chunk = sender_.pcm_bytes_per_chunk();
    std::vector<uint8_t> pcm;
    auto next_send = clock::now();
    while (audio_run_.load()) {
        if (!take_pcm(pcm, chunk)) continue;
        const auto chunk_ns = std::chrono::nanoseconds(
            (static_cast<long long>(pcm.size()) * 10'000'000ll) / chunk);
        auto now = clock::now();
        if (now < next_send) {
            std::this_thread::sleep_until(next_send);
            now = clock::now();
        }
        if (next_send + std::chrono::milliseconds(20) < now) next_send = now;
        next_send += chunk_ns;
        if (!sender_.send_audio(pcm.data(), static_cast<uint16_t>(pcm.size()))) {
            stop_on_send_error();
            break;
        }
    }
}

bool AsyncSender::decode_mjpeg(const std::vector<uint8_t>& jpeg, unsigned width, unsigned height,
                               std::vector<uint8_t>& rgb, double& decode_ms) {
    std::string error;
    const auto started = clock::now();
    if (!jpeg_decoder_.decompress(jpeg.data(), jpeg.size(), static_cast<int>(width),
                                  static_cast<int>(height), rgb, &error)) {
        add_log("MJPEG decode failed: " + error + "\n");
        return false;
    }
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - started).count();
    decode_microseconds_.fetch_add(static_cast<uint64_t>(micros));
    decoded_frames_.fetch_add(1);
    decode_ms = static_cast<double>(micros) / 1000.0;
    return true;
}

// Turns every capture sample into ready-to-send LZ4 fields as soon as it arrives,
// so the sender thread never does image work and can always send on time. Writes
// prepared_[prep_write_] and publishes it as prep_ready_; the sender owns
// prep_front_.
void AsyncSender::prep_loop(VideoMode mode) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); // shares a lock with the sender
    std::vector<uint8_t> frame;
    std::vector<uint8_t> rgb;
    for (;;) {
        unsigned width = 0, height = 0;
        PendingKind kind = PendingKind::Yuy2;
        clock::time_point captured{};
        {
            std::unique_lock lock(mutex_);
            pending_cv_.wait(lock, [this] { return !running_ || pending_ready_; });
            if (!running_) break;
            width = pending_w_;
            height = pending_h_;
            kind = pending_kind_;
            captured = pending_at_;
            frame.swap(pending_);
            pending_ready_ = false;
        }
        if (frame.empty()) continue;
        PreparedField& out = prepared_[prep_write_];
        out.captured = captured;
        out.picked = clock::now();
        out.decode_ms = 0.0;
        out.convert_ms = out.lz4_ms = 0.0;
        out.ready[0].store(false);
        out.ready[1].store(false);

        Sample sample;
        sample.width = width;
        sample.height = height;
        sample.data = frame.data();
        switch (kind) {
        case PendingKind::Mjpeg:
            if (!decode_mjpeg(frame, width, height, rgb, out.decode_ms)) continue;
            sample.data = rgb.data();
            sample.pitch = static_cast<long>(width) * 3;
            sample.pixels = SamplePixels::Rgb;
            break;
        case PendingKind::Nv12:
            sample.pitch = static_cast<long>(width);
            sample.pixels = SamplePixels::Nv12;
            break;
        case PendingKind::I420:
            sample.pitch = static_cast<long>(width);
            sample.pixels = SamplePixels::I420;
            break;
        case PendingKind::Yuy2:
            sample.pitch = static_cast<long>(width) * 2;
            sample.pixels = SamplePixels::Yuy2;
            break;
        case PendingKind::Uyvy:
            sample.pitch = static_cast<long>(width) * 2;
            sample.pixels = SamplePixels::Uyvy;
            break;
        }
        const ViewSettings view{letterbox_.load(), crop_h_.load(), crop_v_.load()};
        const bool interlaced = is_interlaced(mode);
        const uint8_t parity = interlaced ? next_parity_.load() : uint8_t{0};
        if (!field_prep_.prepare_field(sample, mode, view, parity, true, out)) continue;
        {
            std::lock_guard lock(mutex_);
            std::swap(prep_write_, prep_ready_);
            prep_new_ = true;
        }
        prepared_cv_.notify_one();
        // For a blit that turns out to need the other parity, as a repeat does. `out` stays
        // this thread's to fill until it comes round again, since nothing else writes to it.
        if (interlaced) field_prep_.prepare_field(sample, mode, view, parity ^ 1u, false, out);
        // After the field is out of the way, so the preview never delays it.
        if (kind == PendingKind::Mjpeg) publish_preview(rgb.data(), width, height);
    }
}

// Sends prepared fields on the raster. Locked, a sample goes out as it arrives, with
// a repeat at the deadline if it is late or the plan calls for a duplicate. Otherwise
// the newest sample goes out once per frame.
void AsyncSender::sender_loop() {
    // Normal thread priorities can still lose a few ms to other work, and the
    // repeat deadline only has about 2 ms to spare.
    DWORD mmcss_index = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_index);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    struct RevertMmcss {
        HANDLE handle;
        ~RevertMmcss() { if (handle) AvRevertMmThreadCharacteristics(handle); }
    } revert_mmcss{mmcss};

    bool have_frame = false;
    LatencyProbe probe;
    FreeWatch free_watch;
    capture_unusual_.store(false);
    const bool can_lock = rate_lock_.load();
    RateLock rate;
    rate.use_trim = can_lock && sender_.has_trim();
    rate.trim_ppm = trim_ppm_;
    if (rate.use_trim && trim_ppm_ != 0.0) sender_.set_trim(trim_ppm_); // INIT cleared it
    if (!can_lock) {
        rate.go_free("turned off");
    } else if (start_plan_.trim > 0.0) {
        rate.start_parked(start_plan_, start_fps_);
    } else if (start_fps_ > 0.0) {
        char why[64];
        std::snprintf(why, sizeof(why), "capture %.4f fps fits no plan", start_fps_);
        rate.go_free(why);
        capture_unusual_.store(true);
    }
    const auto publish_rate_log = [&] { add_log(std::exchange(rate.log, std::string())); };
    publish_rate_log();
    // A clock change failing means the session is gone: the send path reports it.
    const auto retune = [&] {
        if (!sender_.retune(rate.scale)) {
            report_sender_error();
            errors_.fetch_add(1);
        }
    };

    for (;;) {
        bool fresh = false;
        bool at_deadline = false;
        bool repeat_planned = false;
        bool send_black = false;
        {
            std::unique_lock lock(mutex_);
            if (!have_frame) {
                // The raster stays parked until a field arrives, and on stock Groovy
                // that stops line sync. Don't leave a CRT there if no sample comes.
                const bool got = prepared_cv_.wait_until(lock, clock::now() + kFirstSampleLimit,
                                                         [&] { return !running_ || prep_new_; });
                send_black = !got && have_black_;
            } else {
                const bool wait_for_sample = rate.capture_clocked();
                repeat_planned = wait_for_sample && rate.repeat_planned();
                const auto wake = [&] { return !running_ || (wait_for_sample && prep_new_); };
                const auto deadline = sender_.next_frame_deadline(!wait_for_sample || repeat_planned);
                if (!prepared_cv_.wait_until(lock, deadline - kSpinBeforeDeadline, wake)) {
                    while (!wake() && clock::now() < deadline) {
                        lock.unlock();
                        std::this_thread::yield();
                        lock.lock();
                    }
                    at_deadline = !wake();
                }
            }
            if (!running_) break;
            if (prep_new_) {
                std::swap(prep_ready_, prep_front_);
                prep_new_ = false;
                fresh = true;
                have_frame = true;
            }
        }
        if (send_black) {
            if (!send_at_deadline(black_)) {
                stop_on_send_error();
                break;
            }
            // The next wait starts over, so black keeps coming a frame apart.
            continue;
        }
        if (!have_frame) continue;
        if (fresh && !rate.accept_sample()) fresh = false;
        if (!fresh && !at_deadline) continue;
        const PreparedField& field = prepared_[prep_front_];
        const double pickup_ms =
            std::chrono::duration<double, std::milli>(field.picked - field.captured).count();
        if (fresh && rate.take_release()) {
            const auto until = clock::now() + std::chrono::duration_cast<clock::duration>(
                                                  std::chrono::duration<double>(RateLock::kLandMarginSeconds));
            while (clock::now() < until) std::this_thread::yield();
        }
        double elapsed_ms = 0.0;
        const bool sent = sender_.send_prepared(field, elapsed_ms);
        add_log(sender_.take_log());
        if (!sent) {
            stop_on_send_error();
            break;
        }
        next_parity_.store(sender_.last_parity() ^ 1u);
        send_microseconds_.fetch_add(static_cast<uint64_t>(elapsed_ms * 1000.0 + 0.5));
        sent_.fetch_add(1);
        rate.note_send(fresh);
        if (!fresh && rate.capture_clocked() && !repeat_planned) ++rate.repeats;
        if (timing_log_.load()) {
            record_probe(probe, fresh, field.captured, pickup_ms, field.decode_ms, rate.repeats, rate.trim_ppm);
        }
        if (!can_lock) continue;

        if (rate.state == RateLock::State::Free) {
            if (free_run_can_lock(clock::now(), free_watch) && relock_allowed()) restart_rate_lock();
            continue;
        }
        if (rate.state == RateLock::State::Measure) {
            double fps = 0.0;
            if (capture_rate_.rate(kCaptureMeasureSeconds, fps)) recalibrate_.store(true);
            continue;
        }
        if (!fresh) continue;
        const RateLock::Action action = rate.after_send(sender_, field.captured);
        publish_rate_log();
        if (action == RateLock::Action::Restart) {
            // Free-run on the current clock until the main thread calibrates again.
            const double scale = rate.scale;
            const double trim = rate.trim_ppm;
            rate = RateLock{};
            rate.scale = scale;
            rate.trim_ppm = trim;
            if (relock_allowed()) {
                restart_rate_lock();
            } else {
                rate.go_free("capture keeps changing rate");
                publish_rate_log();
            }
        } else if (action == RateLock::Action::Trim) {
            trim_ppm_ = rate.trim_ppm;
            if (!sender_.set_trim(trim_ppm_)) {
                stop_on_send_error();
                break;
            }
        } else if (action == RateLock::Action::Retune) {
            retune();
        }
    }
}

void AsyncSender::record_probe(LatencyProbe& probe, bool fresh, clock::time_point captured,
                               double pickup_ms, double decode_ms, unsigned& repeats, double trim_ppm) {
    std::string line;
    if (!probe.add(sender_, fresh, captured, pickup_ms, decode_ms, line)) return;
    line += " | repeats " + std::to_string(repeats);
    if (sender_.has_trim()) line += " | trim " + std::to_string(trim_ppm);
    repeats = 0;
    std::lock_guard lock(probe_mutex_);
    probe_line_ = std::move(line);
}

} // namespace groovy
