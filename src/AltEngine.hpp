// Pure alt-spam timing engine (no Geode / GD includes) so it can be unit-tested on any machine.
// Time is *game time*: the sum of the physics-step deltas, so clicks stay frame-accurate.
#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <random>

namespace alt {

struct Settings {
    double cps = 20, intensity = 100, speed = 1;
    bool continuous = false;
    int burstSize = 5;
    double frequency = 2;          // bursts per second (burst mode)
    bool continueAlt = true;       // keep alternating across bursts
    int startLane = 0;             // 0 = left, 1 = right
    double rampMs = 0, holdRatio = 40;
    double inputDelay = 0, laneOffset = 0, bufferMs = 0, jitter = 0;
    double countdownMs = 0, autoStopSec = 0;
    bool splitDual = false;        // lanes map to P1/P2 instead of both to P1
};

inline double clampd(double v, double a, double b) { return std::min(b, std::max(a, v)); }

inline double effectiveCps(const Settings& s, double elapsedMs) {
    double c = s.cps * (s.intensity / 100.0) * s.speed;
    if (s.rampMs > 0) c *= 0.25 + 0.75 * clampd(elapsedMs / s.rampMs, 0, 1);
    return clampd(c, 0.5, 240);
}

class Engine {
public:
    static constexpr double kInf = std::numeric_limits<double>::infinity();

    void reset(unsigned seed = 12345) {
        now = 0; stepIdx = 0; cursor = countdownStart = 0; index = 0; burstPos = 0; burstNo = 0;
        lane = 0; havePress = false; finished = false; started = false;
        rel[0] = rel[1] = kInf; held[0] = held[1] = false; pressStep[0] = pressStep[1] = -1;
        presses.clear(); errSum = 0; errN = 0; total = 0; rng.seed(seed);
    }

    /// Advance one physics step of dtMs. sink(bool down, int lane) is called for each due event, in order.
    template <class F>
    void step(double dtMs, const Settings& s, F&& sink) {
        if (!started) { started = true; cursor = s.countdownMs; countdownStart = s.countdownMs; lane = s.startLane; }
        now += dtMs; stepIdx++;
        if (!finished && s.autoStopSec > 0 && now >= s.autoStopSec * 1000.0) { finished = true; releaseAll(sink); return; }
        if (finished) return;
        const double limit = now + s.bufferMs;
        for (int guard = 0; guard < 64; guard++) {
            if (!havePress) plan(s);
            // earliest releasable lane (a release can't share a step with its own press)
            int rl = -1; double rt = kInf;
            for (int l = 0; l < 2; l++) if (held[l] && rel[l] < rt && stepIdx > pressStep[l]) { rt = rel[l]; rl = l; }
            const bool relDue = rl >= 0 && rt <= limit;
            const bool pressDue = next.time <= limit;
            if (!relDue && !pressDue) break;
            if (relDue && (!pressDue || rt <= next.time)) {
                held[rl] = false; rel[rl] = kInf; sink(false, rl);
            } else {
                // single-input mode: never overlap two holds on the same player button
                if (!s.splitDual) for (int l = 0; l < 2; l++) if (held[l]) { held[l] = false; rel[l] = kInf; sink(false, l); }
                if (held[next.lane]) { held[next.lane] = false; rel[next.lane] = kInf; sink(false, next.lane); }
                held[next.lane] = true; rel[next.lane] = next.time + next.hold; pressStep[next.lane] = stepIdx;
                const double err = std::max(0.0, now - next.time);
                errSum += err; errN++; total++;
                presses.push_back(now); while (!presses.empty() && presses.front() < now - 1000) presses.pop_front();
                havePress = false;
                sink(true, next.lane);
            }
        }
    }

    template <class F> void releaseAll(F&& sink) {
        for (int l = 0; l < 2; l++) if (held[l]) { held[l] = false; rel[l] = kInf; sink(false, l); }
    }

    double liveCps() const {
        if (presses.size() < 3 || now - presses.back() > 400) return 0;
        size_t k = std::min<size_t>(presses.size() - 1, 6);
        double span = presses.back() - presses[presses.size() - 1 - k];
        return span > 0 ? k * 1000.0 / span : 0;
    }
    double avgErrMs() const { return errN ? errSum / errN : 0; }
    long totalClicks() const { return total; }
    bool isFinished() const { return finished; }
    bool counting() const { return started && now < countdownStart; }

private:
    struct Press { double time, hold; int lane; };
    void plan(const Settings& s) {
        const double interval = 1000.0 / effectiveCps(s, cursor - countdownStart);
        double t = cursor + s.inputDelay + (lane == 1 ? s.laneOffset : 0);
        if (s.jitter > 0) t += (std::uniform_real_distribution<double>(-1.0, 1.0)(rng)) * s.jitter;
        t = std::max(t, countdownStart);
        double hold = std::max(1.0, interval * s.holdRatio / 100.0);
        hold = std::min(hold, interval * 2 - 0.1);
        next = {t, hold, lane}; havePress = true;
        index++; burstPos++; cursor += interval; lane ^= 1;
        if (!s.continuous && burstPos >= s.burstSize) {
            burstPos = 0; burstNo++;
            cursor += std::max(0.0, 1000.0 / s.frequency - s.burstSize * interval);
            if (!s.continueAlt) lane = s.startLane;
        }
    }
    double now = 0, cursor = 0, countdownStart = 0, rel[2] = {kInf, kInf};
    long stepIdx = 0, pressStep[2] = {-1, -1}, index = 0, total = 0;
    int burstPos = 0, burstNo = 0, lane = 0, errN = 0;
    bool held[2] = {false, false}, havePress = false, finished = false, started = false;
    double errSum = 0;
    Press next{0, 0, 0};
    std::deque<double> presses;
    std::mt19937 rng{12345};
};

} // namespace alt
