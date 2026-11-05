#include "groovy_sender.h"

#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace groovy {

namespace {

constexpr uint16_t kGroovyPort = 32100;
constexpr size_t kMaxPayload = 1472;

enum Command : uint8_t {
    kCmdClose = 1,
    kCmdInit = 2,
    kCmdSwitchres = 3,
    kCmdAudio = 4,
    kCmdBlitFieldVsync = 7,
    kCmdGetVersion = 8,
    kCmdTrim = 9,
};

// What the GroovyGrab core's server reports. The stock server reports 1.
constexpr uint8_t kTrimVersion = 0;

constexpr int kAckLength = 13;
constexpr int kVersionLength = 1;

// Margins for numbering a field for the next frame Groovy can still scan out.
// They cover Groovy handling back-to-back blits (it inflates one before reading
// the next) and ACK/network jitter. kRepeatLead must exceed kNumberMargin so a
// repeat is numbered for the frame it covers, and normal arrivals must land
// well before kRepeatLead (see RateLock::kLandMarginSeconds).
constexpr auto kNumberMargin = std::chrono::microseconds(1000);
constexpr auto kRepeatLead = std::chrono::microseconds(1800);
// For a frame with no sample to wait for, there is no reason to cut it close.
constexpr auto kEarlyLead = std::chrono::microseconds(4000);
// A blit right behind another waits while Groovy inflates the first.
constexpr auto kBackToBack = std::chrono::microseconds(2000);
constexpr auto kBackToBackMargin = std::chrono::microseconds(1500);

constexpr uint32_t kMailboxWarmup = 120; // ~2 s at 60 Hz
constexpr uint32_t kMailboxFail = 60;    // ~1 s of confirmed stall
// A lost packet leaves the core waiting for the rest of that field: video stays
// black until something resets it, with no error flag. An empty video queue is
// normal now and then, but not this many ACKs in a row.
constexpr uint32_t kVideoStuck = 12;     // ~0.2 s
// Fields can run one further ahead for several seconds after the source's timing
// steps, while the trim moves the raster back.
constexpr uint32_t kVideoStuckAhead = 180; // ~3 s
// A lost packet can also leave the core replaying old frames with its frame skip flag
// up, when its volatile framebuffer option is off. Nothing else looks wrong, and it
// doesn't end by itself. Only INIT clears it.
constexpr uint32_t kDecoderStuck = 12;   // ~0.2 s
// Each restart shows on screen, so no more often than this if packets keep being lost.
constexpr auto kDecoderRestartGap = std::chrono::seconds(5);
// The core's frame counter is 24 bits and its comparisons don't survive wrapping, which
// would come after about 3.2 days: the session restarts shortly before.
constexpr uint32_t kLastFrame = 0xFFF000;
constexpr auto kParityWait = std::chrono::milliseconds(10);

struct ModeTiming {
    double clock;
    uint16_t h_act, h_beg, h_end, h_tot;
    uint16_t v_act, v_beg, v_end, v_tot;
    uint8_t interlace;
};

// The progressive clocks give exactly 59.94 and 50 Hz; a plain 13.5 MHz would
// run them at 60.054 and 50.080 Hz.
ModeTiming mode_timing(VideoMode mode) {
    switch (mode) {
    case VideoMode::p240: return {13.474285714, 720, 736, 798, 858, 240, 244, 247, 262, 0};
    case VideoMode::p288: return {13.4784, 720, 732, 796, 864, 288, 290, 293, 312, 0};
    case VideoMode::i576: return {13.5, 720, 732, 796, 864, 576, 581, 584, 625, 1};
    case VideoMode::i480: break;
    }
    return {13.5, 720, 736, 798, 858, 480, 489, 495, 525, 1};
}

template <size_t N>
void put_u16(std::array<uint8_t, N>& packet, size_t offset, uint16_t value) {
    packet[offset] = static_cast<uint8_t>(value);
    packet[offset + 1] = static_cast<uint8_t>(value >> 8);
}

template <size_t N>
void put_u32(std::array<uint8_t, N>& packet, size_t offset, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        packet[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

double ms_between(Sender::clock::time_point a, Sender::clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

} // namespace

bool Sender::open(const std::string& ip, VideoMode mode, uint32_t audio_rate_hz, double clock_scale) {
    close();
    interrupted_ = false;
    clock_scale_ = clock_scale;
    if (ip.empty()) return give_up("No address set", "Enter the MiSTer's IP address");
    if (WSAStartup(MAKEWORD(2, 2), &wsa_) != 0) {
        return give_up("WSAStartup failed", "Windows networking is unavailable");
    }
    wsa_started_ = true;
    {
        std::lock_guard lock(socket_mutex_);
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
    if (socket_ == INVALID_SOCKET) return fail("UDP socket failed");

    int send_buffer = 1024 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&send_buffer),
               sizeof(send_buffer));

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(kGroovyPort);
    if (InetPtonA(AF_INET, ip.c_str(), &endpoint.sin_addr) != 1) {
        return give_up("Address is not an IPv4 address", "Invalid address. Use an IP like 192.168.0.10");
    }
    if (::connect(socket_, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) ==
        SOCKET_ERROR) {
        return fail("UDP connect failed");
    }
    endpoint_ = endpoint;
    session_open_ = false;
    has_trim_ = false;
    wrong_core_ = false;
    audio_rate_hz_ = audio_rate_hz == 44100 || audio_rate_hz == 48000 ? audio_rate_hz : 0;

    if (!handshake(mode)) return false;
    if (!timer_period_set_) {
        timeBeginPeriod(1);
        timer_period_set_ = true;
    }
    return true;
}

void Sender::close() {
    if (socket_ != INVALID_SOCKET) {
        if (session_open_) send_close();
        session_open_ = false;
        std::lock_guard lock(socket_mutex_);
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    if (wsa_started_) {
        WSACleanup();
        wsa_started_ = false;
    }
    frame_ = 0;
    ack_ = {};
    clock_scale_ = 1.0;
    raster_valid_ = false;
    if (timer_period_set_) {
        timeEndPeriod(1);
        timer_period_set_ = false;
    }
    error_.clear();
    error_detail_.clear();
}

void Sender::interrupt() {
    std::lock_guard lock(socket_mutex_);
    if (socket_ != INVALID_SOCKET) {
        interrupted_ = true;
        shutdown(socket_, SD_BOTH);
    }
}

double Sender::nominal_mode_rate(VideoMode mode) {
    const ModeTiming timing = mode_timing(mode);
    return timing.clock * 1e6 / (static_cast<double>(timing.h_tot) * timing.v_tot) *
           (is_interlaced(mode) ? 2.0 : 1.0);
}

bool Sender::set_mode(VideoMode mode, double clock_scale) {
    if (socket_ == INVALID_SOCKET) return false;
    clock_scale_ = clock_scale;
    return reinit(mode);
}

bool Sender::retune(double scale) {
    if (socket_ == INVALID_SOCKET) return false;
    std::lock_guard lock(wire_mutex_);
    clock_scale_ = scale;
    if (!apply_mode(mode_)) return false;
    field_reset_frame_ = frame_ + 1; // frame counting carries on
    return true;
}

// In hundredths of a ppm. The server works out what that means for the current mode.
bool Sender::set_trim(double ppm) {
    if (socket_ == INVALID_SOCKET || !has_trim_) return false;
    std::array<uint8_t, 3> packet{kCmdTrim};
    put_u16(packet, 1, static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(ppm, -300.0, 300.0) * 100.0))));
    std::lock_guard lock(wire_mutex_);
    if (!send_packet(packet.data(), packet.size())) return fail("TRIM send failed");
    trim_ppm_ = ppm;
    return true;
}

std::chrono::nanoseconds Sender::frame_duration() const {
    return std::chrono::nanoseconds((static_cast<long long>(line_time_ns_) * v_total_) >>
                                    (is_interlaced(mode_) ? 1 : 0));
}

double Sender::field_ms() const {
    return std::chrono::duration<double, std::milli>(frame_duration()).count();
}

double Sender::frame_seconds() const {
    return std::chrono::duration<double>(frame_duration()).count();
}

double Sender::nominal_rate() const {
    if (!h_total_) return 0.0;
    return pixel_clock_mhz_ * 1e6 / (static_cast<double>(h_total_) * v_total_) *
           (is_interlaced(mode_) ? 2.0 : 1.0);
}

double Sender::release_fraction() const {
    const bool interlaced = is_interlaced(mode_);
    const double active = output_height(mode_) * (interlaced ? 2.0 : 1.0);
    return (active + 1.0 + (interlaced ? 1.0 : 0.0)) / v_total_;
}

Sender::clock::time_point Sender::next_frame_deadline(bool early) const {
    const auto now = clock::now();
    // No raster position right after a clock change, and the raster parks until the
    // next blit: keep the frame's usual spacing, whether or not a sample is due.
    if (!raster_valid_) return last_blit_at_ + frame_duration();
    const double lead = std::chrono::duration<double>(early ? kEarlyLead : kRepeatLead).count() / frame_seconds();
    const double frames_left = static_cast<double>(frame_) + 1.0 - lead - raster_at_time(now);
    return now + std::chrono::duration_cast<clock::duration>(
                     std::chrono::duration<double>(frames_left * frame_seconds()));
}

uint32_t Sender::frame_number_at(clock::time_point t) const {
    if (!raster_valid_ || raster_count_ == 0) return frame_ + 1u;
    const double margin = std::chrono::duration<double>(kNumberMargin).count() / frame_seconds();
    return static_cast<uint32_t>(std::floor(raster_at_time(t) + margin)) + 1u;
}

// The inverse of advance_frame(): a field is numbered floor(raster + margin) + 1, so
// it becomes `frame` once the raster reaches frame - 1 - margin.
Sender::clock::time_point Sender::earliest_send_for(uint32_t frame) const {
    if (!raster_valid_ || raster_count_ == 0) return {};
    const double period = frame_seconds();
    if (period <= 0.0) return {};
    const double margin = std::chrono::duration<double>(kNumberMargin).count() / period;
    const double best = *std::max_element(raster_offsets_.begin(),
                                          raster_offsets_.begin() + static_cast<ptrdiff_t>(raster_count_));
    const double frames = static_cast<double>(frame) - 1.0 - margin - best;
    return raster_epoch_ + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(frames * period));
}

bool Sender::raster_sample(double& position, clock::time_point& at) const {
    if (!raster_valid_) return false;
    position = raster_position_;
    at = raster_at_;
    return true;
}

// A repeat sends the same image, in interlaced modes as whichever parity the new
// field number needs.
bool Sender::send_prepared(const PreparedField& field, double& elapsed_ms) {
    if (socket_ == INVALID_SOCKET) return false;
    const auto started = clock::now();
    advance_frame();
    uint8_t udp_field = 0;
    uint8_t source_parity = 0;
    if (is_interlaced(mode_)) {
        if (ack_.gpu_frame < field_reset_frame_) {
            // The raster has not reached the retune yet, so ACKs still show the
            // old field. The core restarts its field there.
            udp_field = static_cast<uint8_t>((frame_ - field_reset_frame_) % 2u);
        } else {
            // Fields alternate, and Groovy's numbering of them is the opposite of vga_f1.
            const bool odd_distance = (frame_ - ack_.gpu_frame) % 2u == 1u;
            udp_field = ack_.vga_f1 == odd_distance ? 1u : 0u;
        }
        source_parity = udp_field == 0 ? 1u : 0u;
    }
    // The prep thread is on the other parity right now, and takes a few ms. Should it
    // ever fail, the wrong parity is better than no field.
    if (!field.ready[source_parity].load()) {
        const auto limit = clock::now() + kParityWait;
        while (!field.ready[source_parity].load() && clock::now() < limit) std::this_thread::yield();
        if (!field.ready[source_parity].load()) source_parity ^= 1u;
    }
    last_parity_ = source_parity;
    const int size = field.size[source_parity];
    timing_.convert_ms = field.convert_ms;
    timing_.lz4_ms = field.lz4_ms;
    timing_.bytes = size > 0 ? static_cast<unsigned>(size) : 0u;
    return transmit_field(field.data[source_parity].data(), size, udp_field, started, elapsed_ms);
}

bool Sender::transmit_field(const char* data, int compressed_size, uint8_t udp_field,
                            clock::time_point started, double& elapsed_ms) {
    if (compressed_size <= 0) return fail("LZ4 compress failed", "Couldn't send the picture to MiSTer");

    std::array<uint8_t, 12> command{};
    command[0] = kCmdBlitFieldVsync;
    put_u32(command, 1, frame_);
    command[5] = udp_field;
    put_u16(command, 6, 0); // the vsync line: Groovy only echoes it back
    put_u32(command, 8, static_cast<uint32_t>(compressed_size));

    const auto stream_started = clock::now();
    {
        std::lock_guard lock(wire_mutex_);
        if (!send_packet(command.data(), command.size())) {
            if (!out_of_buffers()) return fail("BLIT send failed");
            elapsed_ms = ms_between(started, clock::now());
            return true; // skip the field: its data is no use without the command
        }
        if (!send_chunks(reinterpret_cast<const uint8_t*>(data),
                         static_cast<size_t>(compressed_size))) {
            return fail("Frame packet send failed");
        }
    }
    last_blit_at_ = clock::now();
    timing_.stream_ms = ms_between(stream_started, last_blit_at_);
    timing_.sent_at = last_blit_at_;
    wait_ack(std::chrono::milliseconds(4));
    // The core ACKs a field as its command arrives, and normally that field is two
    // frames after the one scanning. One further ahead is already in the core,
    // waiting: its queue reads empty until the field before it has scanned. Only an
    // empty queue with nothing waiting means the core is stuck. Should a packet be
    // lost while fields are that far ahead, the longer limit still catches it.
    const bool waiting_ahead = ack_.echo > ack_.gpu_frame + 2u;
    empty_queue_streak_ = (ack_.vram_queued || waiting_ahead) ? 0 : empty_queue_streak_ + 1;
    empty_queue_any_ = ack_.vram_queued ? 0 : empty_queue_any_ + 1;
    // Each fires once per stretch, and the longer one only if the shorter didn't.
    if (empty_queue_streak_ == kVideoStuck ||
        (empty_queue_any_ == kVideoStuckAhead && empty_queue_streak_ < kVideoStuck)) {
        char line[64];
        std::snprintf(line, sizeof(line), "MiSTer video stuck (frame %u), resetting it\n", frame_);
        log_ += line;
        if (!retune(clock_scale_)) return fail("Video reset failed", "MiSTer stopped responding");
    }
    frame_skip_streak_ = ack_.frame_skip ? frame_skip_streak_ + 1 : 0;
    if (frame_skip_streak_ >= kDecoderStuck && clock::now() - last_decoder_restart_ >= kDecoderRestartGap) {
        last_decoder_restart_ = clock::now();
        char line[64];
        std::snprintf(line, sizeof(line), "MiSTer decoder stuck (frame %u), sending INIT again\n", frame_);
        log_ += line;
        if (!restart_session()) return fail("Decoder reset failed", "MiSTer stopped responding");
    } else if (mailbox_lost()) {
        char line[96];
        std::snprintf(line, sizeof(line), "MiSTer mailbox stalled (synced=%d echo=%u frame=%u), sending INIT again\n",
                      ack_.vram_synced ? 1 : 0, ack_.echo, frame_);
        log_ += line;
        if (!restart_session()) return fail("Mailbox stalled", "MiSTer stopped responding");
    } else if (frame_ >= kLastFrame) {
        log_ += "MiSTer frame counter nearly full, starting it again\n";
        if (!restart_session()) return fail("Frame counter restart failed", "MiSTer stopped responding");
    }
    elapsed_ms = ms_between(started, clock::now());
    return true;
}

uint16_t Sender::pcm_bytes_per_chunk() const {
    return static_cast<uint16_t>((audio_rate_hz_ / 100u) * 4u);
}

bool Sender::send_audio(const uint8_t* pcm, uint16_t bytes) {
    if (socket_ == INVALID_SOCKET || !pcm || bytes < 4) return true;
    std::lock_guard lock(wire_mutex_);
    std::array<uint8_t, 3> command{kCmdAudio};
    put_u16(command, 1, bytes);
    if (!send_packet(command.data(), command.size())) {
        return out_of_buffers() || fail("AUDIO send failed");
    }
    if (!send_chunks(pcm, bytes)) return fail("Audio packet send failed");
    return true;
}

// INIT, wait for the ACK, then SWITCHRES.
bool Sender::handshake(VideoMode mode) {
    const uint8_t rate_code = audio_rate_hz_ == 48000 ? 3 : audio_rate_hz_ == 44100 ? 2 : 0;
    const uint8_t channels = audio_rate_hz_ ? 2 : 0;
    constexpr uint8_t kLz4 = 1, kRgb565 = 2;
    const std::array<uint8_t, 5> init{kCmdInit, kLz4, rate_code, channels, kRgb565};
    if (!send_packet(init.data(), init.size())) return fail("INIT send failed");
    std::array<uint8_t, 64> reply{};
    if (!await_reply(reply, kAckLength, std::chrono::milliseconds(1000))) {
        set_recv_timeout(1);
        return give_up("No reply to INIT within 1 s", "No reply from MiSTer. Is the GroovyGrab core running?");
    }
    session_open_ = true;
    // Asked a few times: one lost reply must not make GroovyGrab's own core look like
    // another. Stock Groovy answers with another number, or not at all if it's older.
    const uint8_t get_version = kCmdGetVersion;
    for (int attempt = 0; attempt < 3 && !has_trim_; ++attempt) {
        if (send_packet(&get_version, 1) && await_reply(reply, kVersionLength, std::chrono::milliseconds(200))) {
            has_trim_ = reply[0] == kTrimVersion;
            if (!has_trim_) break; // an answer, just not ours
        }
    }
    set_recv_timeout(1);
    // Stock Groovy answers too, but only the GroovyGrab core can trim its pixel rate,
    // which the lock needs.
    if (!has_trim_) {
        wrong_core_ = true;
        return give_up("Not the GroovyGrab core: no pixel rate trim",
                       "This MiSTer runs another core, such as stock Groovy. Start GroovyGrab from its Utility menu.");
    }
    // INIT reallocates the session, restarts the core's frame counter and clears its trim.
    ack_ = {};
    frame_ = 0;
    field_reset_frame_ = 0;
    trim_ppm_ = 0.0;
    return apply_mode(mode);
}

// Waits for a reply of this length. ACKs for earlier fields can still be queued on a
// session that is running: skips them, as INIT's own ACK has a frame number of 0.
bool Sender::await_reply(std::array<uint8_t, 64>& reply, int length, std::chrono::milliseconds limit) {
    const auto deadline = clock::now() + limit;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (left <= 0) return false;
        set_recv_timeout(static_cast<DWORD>(left));
        const int received =
            recv(socket_, reinterpret_cast<char*>(reply.data()), static_cast<int>(reply.size()), 0);
        if (received == SOCKET_ERROR) return false;
        if (received == length && (length != kAckLength || get_u32(reply.data()) == 0)) return true;
    }
}

bool Sender::reinit(VideoMode mode) {
    std::lock_guard lock(wire_mutex_);
    return handshake(mode);
}

// INIT again on a running stream, with the trim it clears.
bool Sender::restart_session() {
    const double trim = trim_ppm_;
    if (!reinit(mode_)) return false;
    return trim == 0.0 || set_trim(trim);
}

bool Sender::apply_mode(VideoMode mode) {
    ModeTiming timing = mode_timing(mode);
    timing.clock *= clock_scale_;
    pixel_clock_mhz_ = timing.clock;
    h_total_ = timing.h_tot;
    raster_valid_ = false;
    std::array<uint8_t, 26> packet{};
    packet[0] = kCmdSwitchres;
    std::memcpy(packet.data() + 1, &timing.clock, sizeof(timing.clock));
    put_u16(packet, 9, timing.h_act);
    put_u16(packet, 11, timing.h_beg);
    put_u16(packet, 13, timing.h_end);
    put_u16(packet, 15, timing.h_tot);
    put_u16(packet, 17, timing.v_act);
    put_u16(packet, 19, timing.v_beg);
    put_u16(packet, 21, timing.v_end);
    put_u16(packet, 23, timing.v_tot);
    packet[25] = timing.interlace;
    if (!send_packet(packet.data(), packet.size())) return fail("SWITCHRES send failed");
    mode_ = mode;
    v_total_ = timing.v_tot;
    line_time_ns_ = static_cast<uint32_t>(timing.h_tot * 1000.0 / timing.clock + 0.5);
    reset_mailbox_watch();
    error_.clear();
    error_detail_.clear();
    return true;
}

bool Sender::mailbox_lost() {
    frames_since_reset_ += 1;
    if (frames_since_reset_ < kMailboxWarmup) {
        unsynced_streak_ = 0;
        no_ack_streak_ = 0;
        return false;
    }
    if (ack_.vram_synced) unsynced_streak_ = 0;
    else unsynced_streak_ += 1;
    if (ack_.echo >= frame_) no_ack_streak_ = 0;
    else no_ack_streak_ += 1;
    return unsynced_streak_ >= kMailboxFail || no_ack_streak_ >= kMailboxFail;
}

void Sender::reset_mailbox_watch() {
    frames_since_reset_ = 0;
    empty_queue_streak_ = 0;
    empty_queue_any_ = 0;
    frame_skip_streak_ = 0;
    unsynced_streak_ = 0;
    no_ack_streak_ = 0;
}

void Sender::set_recv_timeout(DWORD timeout_ms) {
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
               sizeof(timeout_ms));
}

// If the main socket was shut down by interrupt(), CLOSE goes out on a fresh one.
void Sender::send_close() {
    const uint8_t close_command = kCmdClose;
    std::lock_guard lock(wire_mutex_);
    if (!interrupted_ && send_packet(&close_command, 1)) return;
    SOCKET once = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (once == INVALID_SOCKET) return;
    sendto(once, reinterpret_cast<const char*>(&close_command), 1, 0,
           reinterpret_cast<const sockaddr*>(&endpoint_), sizeof(endpoint_));
    closesocket(once);
}

bool Sender::send_packet(const uint8_t* data, size_t size) {
    return send(socket_, reinterpret_cast<const char*>(data), static_cast<int>(size), 0) ==
           static_cast<int>(size);
}

// Windows being briefly out of buffers only costs what the failed send carried, like a
// packet lost on the network. Only for picture and sound: a lost command does more harm.
bool Sender::out_of_buffers() {
    if (WSAGetLastError() != WSAENOBUFS) return false;
    full_buffer_sends_.fetch_add(1);
    return true;
}

std::string Sender::take_log() {
    if (const uint32_t count = full_buffer_sends_.exchange(0)) {
        log_ += "Windows was out of network buffers: dropped " + std::to_string(count) + " packet(s)\n";
    }
    return std::exchange(log_, std::string());
}

bool Sender::send_chunks(const uint8_t* data, size_t size) {
    while (size) {
        const size_t chunk = std::min(kMaxPayload, size);
        if (!send_packet(data, chunk) && !out_of_buffers()) return false;
        data += chunk;
        size -= chunk;
    }
    return true;
}

void Sender::parse_ack(const uint8_t* reply, int length) {
    if (length != kAckLength) return;
    const uint32_t echo = get_u32(reply);
    if (echo < ack_.echo && echo != 0) return;
    ack_.echo = echo;
    ack_.gpu_frame = get_u32(reply + 6);
    ack_.vcount = get_u16(reply + 10);
    ack_.vga_f1 = (reply[12] & (1u << 5)) != 0;
    ack_.vram_synced = (reply[12] & (1u << 2)) != 0;
    ack_.vram_queued = (reply[12] & (1u << 7)) != 0;
    ack_.frame_skip = (reply[12] & (1u << 3)) != 0;
}

void Sender::poll_ack() {
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval no_wait{};
        if (select(0, &readable, nullptr, nullptr, &no_wait) <= 0) break;
        std::array<uint8_t, 64> reply{};
        const int length =
            recv(socket_, reinterpret_cast<char*>(reply.data()), static_cast<int>(reply.size()), 0);
        if (length == SOCKET_ERROR) break;
        parse_ack(reply.data(), length);
    }
}

// Blocks in select until this field's ACK or `limit`. An ACK now and then takes longer,
// and is read with the next field: raster_at_time() doesn't need every one.
void Sender::wait_ack(std::chrono::milliseconds limit) {
    const auto t0 = clock::now();
    const auto deadline = t0 + limit;
    timing_.acked = false;
    for (;;) {
        poll_ack();
        if (ack_.echo == frame_) {
            note_ack();
            break;
        }
        const auto now = clock::now();
        if (now >= deadline) break;
        const auto remain = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval wait{0, static_cast<long>(remain.count())};
        select(0, &readable, nullptr, nullptr, &wait);
    }
    timing_.sleep_ms = ms_between(t0, clock::now());
}

void Sender::note_ack() {
    const auto now = clock::now();
    timing_.acked = true;
    timing_.ack_at = now;
    timing_.lead = static_cast<int>(frame_ - ack_.gpu_frame);
    timing_.vcount = ack_.vcount;
    raster_position_ = static_cast<double>(ack_.gpu_frame) + static_cast<double>(ack_.vcount) / v_total_;
    raster_at_ = now;
    if (!raster_valid_) {
        raster_epoch_ = now;
        raster_count_ = 0;
        raster_next_ = 0;
    }
    raster_offsets_[raster_next_] =
        raster_position_ - std::chrono::duration<double>(now - raster_epoch_).count() / frame_seconds();
    raster_next_ = (raster_next_ + 1) % raster_offsets_.size();
    raster_count_ = std::min(raster_count_ + 1, raster_offsets_.size());
    raster_valid_ = true;
}

// Each ACK gives a phase offset. A delayed ACK can only make that offset smaller,
// so the largest of the last ~second is the best estimate.
double Sender::raster_at_time(clock::time_point t) const {
    const double best = *std::max_element(raster_offsets_.begin(),
                                          raster_offsets_.begin() + static_cast<ptrdiff_t>(raster_count_));
    return best + std::chrono::duration<double>(t - raster_epoch_).count() / frame_seconds();
}

// Number for the next frame Groovy can still scan out once the field has
// arrived, from the raster position. Counting sends instead would drift by one on
// every missing capture sample and leave Groovy's VRAM empty (a frame skip).
void Sender::advance_frame() {
    if (raster_valid_) {
        const auto now = clock::now();
        auto margin_time = std::chrono::duration<double>(kNumberMargin);
        if (now - last_blit_at_ < kBackToBack) margin_time += kBackToBackMargin;
        const double margin = margin_time.count() / frame_seconds();
        frame_ = static_cast<uint32_t>(std::floor(raster_at_time(now) + margin)) + 1u;
        return;
    }
    frame_ = std::max(frame_, ack_.gpu_frame) + 1u;
}

bool Sender::fail(const char* detail, const char* message) {
    error_ = message;
    error_detail_ = std::string(detail) + " (Winsock error " + std::to_string(WSAGetLastError()) + ")";
    return false;
}

// For failures that Winsock's error code says nothing about.
bool Sender::give_up(const char* detail, const char* message) {
    error_ = message;
    error_detail_ = detail;
    return false;
}

} // namespace groovy
