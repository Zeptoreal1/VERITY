#include "../src/AltEngine.hpp"
#include <cstdio>
#include <vector>
#include <cstdlib>
struct Ev { double t; bool down; int lane; };
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static std::vector<Ev> run(const alt::Settings& s, double ms, double tps = 240) {
    alt::Engine e; e.reset(); std::vector<Ev> out; double dt = 1000.0 / tps, t = 0;
    for (int i = 0; i < (int)(ms / dt); i++) { t += dt; e.step(dt, s, [&](bool d, int l) { out.push_back({t, d, l}); }); }
    return out;
}
int main() {
    { // alternation + CPS at 240 tps
        alt::Settings s; s.continuous = true; s.cps = 30;
        auto ev = run(s, 2000); int downs = 0, last = -1; bool alt_ok = true;
        for (auto& e : ev) if (e.down) { downs++; if (last >= 0 && e.lane == last) alt_ok = false; last = e.lane; }
        CHECK(alt_ok, "lanes alternate"); CHECK(downs >= 58 && downs <= 61, "30cps ~ 60 clicks in 2s");
    }
    { // a release never shares a step with its press; no stuck holds; balanced down/up
        alt::Settings s; s.continuous = true; s.cps = 60; s.holdRatio = 10; 
        auto ev = run(s, 1000); int bal = 0; bool ok = true;
        for (size_t i = 1; i < ev.size(); i++) if (!ev[i].down && ev[i - 1].down && ev[i].lane == ev[i - 1].lane && ev[i].t == ev[i - 1].t) ok = false;
        for (auto& e : ev) bal += e.down ? 1 : -1;
        CHECK(ok, "release not in same step as press"); CHECK(bal == 0 || bal == 1, "balanced");
    }
    { // single-input mode never has two lanes held at once
        alt::Settings s; s.continuous = true; s.cps = 50; s.holdRatio = 90; s.laneOffset = 8; s.jitter = 6;
        auto ev = run(s, 2000); int h = 0; bool ok = true;
        for (auto& e : ev) { h += e.down ? 1 : -1; if (h > 1 || h < 0) ok = false; }
        CHECK(ok, "no overlapping holds");
    }
    { // 5-click bursts at 2 Hz => 10 clicks / s
        alt::Settings s; s.cps = 25; s.burstSize = 5; s.frequency = 2;
        auto ev = run(s, 3000); int downs = 0; for (auto& e : ev) downs += e.down;
        CHECK(downs >= 29 && downs <= 31, "5-bursts @2Hz ~ 30 clicks in 3s");
    }
    { // 7-click bursts, odd burst flips side
        alt::Settings s; s.cps = 28; s.burstSize = 7; s.frequency = 1;
        auto ev = run(s, 2600); std::vector<int> lanes; for (auto& e : ev) if (e.down) lanes.push_back(e.lane);
        CHECK(lanes.size() >= 14, "two bursts"); CHECK(lanes[0] == 0 && lanes[7] == 1, "second burst starts on right");
        s.continueAlt = false; auto ev2 = run(s, 2600); lanes.clear(); for (auto& e : ev2) if (e.down) lanes.push_back(e.lane);
        CHECK(lanes[7] == 0, "reset lane each burst");
    }
    { // input delay + countdown
        alt::Settings s; s.continuous = true; s.cps = 10; s.inputDelay = 100; s.countdownMs = 500;
        auto ev = run(s, 1500); CHECK(!ev.empty() && ev[0].t >= 600 - 4.2, "delay+countdown respected");
    }
    { // auto-stop releases everything
        alt::Settings s; s.continuous = true; s.cps = 20; s.autoStopSec = 1;
        auto ev = run(s, 2000); int bal = 0; for (auto& e : ev) bal += e.down ? 1 : -1;
        CHECK(bal == 0, "auto-stop leaves nothing held"); CHECK(ev.back().t <= 1004.5, "stops at 1s");
    }
    { // timing error is within one physics step
        alt::Settings s; s.continuous = true; s.cps = 17; alt::Engine e; e.reset(); double dt = 1000.0 / 240;
        for (int i = 0; i < 960; i++) e.step(dt, s, [](bool, int) {});
        CHECK(e.avgErrMs() < dt, "avg err < 1 step"); CHECK(std::abs(e.liveCps() - 17) < 2.5, "live cps near target");
    }
    std::printf(fails ? "%d FAILED\n" : "all engine tests passed\n", fails);
    return fails ? 1 : 0;
}
