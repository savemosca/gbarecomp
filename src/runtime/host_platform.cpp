#include "host_platform.h"

#include <algorithm>

#include <cstdio>
#include <thread>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <timeapi.h>  // timeBeginPeriod/timeEndPeriod (excluded by LEAN_AND_MEAN)
#  if defined(_MSC_VER)
#    pragma comment(lib, "winmm.lib")  // MinGW links winmm via CMake
#  endif
#endif

namespace gbarecomp {

FramePacer::FramePacer(double target_hz) {
    if (target_hz <= 0.0) target_hz = kGbaFrameHz;
    period_ = std::chrono::nanoseconds(
        static_cast<long long>(1e9 / target_hz + 0.5));
#if defined(_WIN32)
    // Raise the scheduler timer resolution to 1 ms so sleep_until is
    // accurate enough for ~16.74 ms frame pacing.
    if (timeBeginPeriod(1) == TIMERR_NOERROR) raised_timer_res_ = true;
    // HP-002: high-resolution waitable timer (Win10 1803+). sleep_until
    // quantizes to the system tick under load; its phase drift against the
    // 16.742 ms frame period measured as a 234 ms beat of 20-24 ms
    // oversleeps. This timer honors sub-millisecond due times. Null on
    // older Windows -> the hybrid sleep below still applies.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    wait_timer_ = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_MANUAL_RESET |
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
#endif
    reset();
}

FramePacer::~FramePacer() {
#if defined(_WIN32)
    if (wait_timer_) CloseHandle(static_cast<HANDLE>(wait_timer_));
    if (raised_timer_res_) timeEndPeriod(1);
#endif
}

void FramePacer::reset() {
    next_ = clock::now() + period_;
    debt_ = std::chrono::nanoseconds(0);  // a deliberate jump owes nothing
}

namespace {
// Late time above this is a hitch (load, pause), not drift: dropped, as
// before. Below it, the time is owed and repaid.
constexpr auto kMaxDebt = std::chrono::milliseconds(100);
// Repayment per frame: 1/16 of the period (~1 ms at 59.73 Hz). A 6% shorter
// frame is not the visible short/long pair the full payback produced, and it
// repays up to ~60 ms of lost time per second.
constexpr int kRepayDivisor = 16;
}  // namespace

void FramePacer::wait_for_next_frame(uint64_t frames) {
    if (uncapped_) {
        next_ = clock::now();  // keep the deadline current for re-cap
        return;
    }
    // Throttle banked frames: a present covering k>1 guest frames (after
    // stale-frame skips) advances the deadline by the banked count before
    // waiting, so this one wait paces all of them. Clamped defensively;
    // a genuine multi-second stall still resyncs below. frames==1 skips
    // the adjustment entirely (bit-identical to the old path).
    if (frames > 1) {
        uint64_t extra = frames - 1u;
        if (extra > 15u) extra = 15u;
        next_ += period_ * static_cast<std::chrono::nanoseconds::rep>(extra);
    }
    clock::time_point now = clock::now();
    // If we've fallen more than one whole frame behind, resync instead
    // of trying to replay the lost time (avoids a catch-up death spiral
    // after a hitch or a breakpoint pause).
    if (now > next_ + period_) {
        debt_ = std::min<std::chrono::nanoseconds>(
            debt_ + std::chrono::duration_cast<std::chrono::nanoseconds>(now - next_),
            kMaxDebt);
        next_ = now + period_;
        return;
    }
    sleep_until_point(next_);

    // Never pay back a late wake with a shortened following frame. That
    // short/long pair is especially visible during steady pixel scrolling.
    // Normal sub-millisecond timer error retains the absolute cadence; a
    // scheduler or present stall above 1.5 ms establishes a fresh full-frame
    // period instead of producing a visibly short recovery frame.
    const clock::time_point woke = clock::now();
    constexpr auto kLateTolerance = std::chrono::microseconds(1500);
    if (woke > next_ + kLateTolerance) {
        // Late: start a fresh full period, but remember the lost time.
        debt_ = std::min<std::chrono::nanoseconds>(
            debt_ + std::chrono::duration_cast<std::chrono::nanoseconds>(woke - next_),
            kMaxDebt);
        next_ = woke + period_;
    } else {
        // On time: repay a slice of the debt with a slightly shorter period.
        const auto repay = std::min<std::chrono::nanoseconds>(debt_, period_ / kRepayDivisor);
        debt_ -= repay;
        next_ = next_ + period_ - repay;
    }
}

FramePacer::clock::time_point FramePacer::smooth_origin() {
    // Presenting interpolated frames as soon as they were ready made the gap
    // to the next one shrink with the emulate time (6 of 8.4 ms), and on a
    // 120 Hz display two landed in one refresh, so only 60 fps showed. The
    // lead jumps up to a slow frame and eases back down 50 us per frame.
    const clock::time_point start = next_ - period_;
    const auto busy = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start);
    lead_ = std::clamp(std::max(busy + std::chrono::milliseconds(2), lead_ - std::chrono::microseconds(50)),
                       std::chrono::nanoseconds(0), period_ / 2);
    return start + lead_;
}

void FramePacer::sleep_until(clock::time_point t) {
    if (!uncapped_ && clock::now() < t) sleep_until_point(t);
}

void FramePacer::sleep_until_point(clock::time_point target) {
    const clock::time_point now = clock::now();
    // A high-resolution waitable timer is already precise enough on modern
    // Windows. Waiting directly on it avoids a 1.2 ms yield/spin tail whose
    // wake time becomes erratic when the game process runs at Idle priority.
#if defined(_WIN32)
    if (target > now) {
        if (wait_timer_) {
            const auto due_ns = std::chrono::duration_cast<
                std::chrono::nanoseconds>(target - now).count();
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(
                due_ns >= 100 ? due_ns / 100 : 1);
            HANDLE t = static_cast<HANDLE>(wait_timer_);
            if (SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(t, INFINITE);
            } else {
                std::this_thread::sleep_until(target);
            }
        } else {
            std::this_thread::sleep_until(target);
        }
    }
#else
    // Hybrid wait: sleep most of the gap (cheap, but coarse), then yield
    // through the final ~1 ms for deadline accuracy.
    constexpr auto kSpinTail = std::chrono::microseconds(1200);
    if (target - now > kSpinTail) {
        std::this_thread::sleep_until(target - kSpinTail);
    }
#endif
    while (clock::now() < target) {
        std::this_thread::yield();
    }
}

std::vector<unsigned char> read_file(const std::string& path, std::string* error) {
    std::vector<unsigned char> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "open failed: " + path;
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        if (error) *error = "ftell failed: " + path;
        std::fclose(f);
        return out;
    }
    out.resize(static_cast<std::size_t>(sz));
    std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        out.clear();
        if (error) *error = "short read: " + path;
    }
    return out;
}

}  // namespace gbarecomp
