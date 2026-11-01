#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include "AltEngine.hpp"

using namespace geode::prelude;

// ------------------------------------------------------------------ shared state
static alt::Engine g_engine;
static alt::Settings g_cfg;
static bool g_enabled = false;
static bool g_wasActive = false;
static int g_slot = 1;
static int g_sinceRefresh = 0;
static float g_tapAge[2] = {9.f, 9.f};
static bool g_showTaps = true;
static float g_tapSize = 35.f;

static void refreshCfg() {
    auto m = Mod::get();
    auto& c = g_cfg;
    c.cps = m->getSettingValue<double>("cps");
    c.intensity = m->getSettingValue<double>("intensity");
    c.speed = m->getSettingValue<double>("speed");
    c.continuous = m->getSettingValue<bool>("continuous");
    c.burstSize = static_cast<int>(m->getSettingValue<int64_t>("burst-size"));
    c.frequency = m->getSettingValue<double>("frequency");
    c.continueAlt = m->getSettingValue<bool>("continue-alt");
    c.startLane = m->getSettingValue<std::string>("start-lane") == "Right" ? 1 : 0;
    c.rampMs = m->getSettingValue<double>("ramp-ms");
    c.holdRatio = m->getSettingValue<double>("hold-ratio");
    c.splitDual = m->getSettingValue<bool>("split-dual");
    c.inputDelay = m->getSettingValue<double>("input-delay");
    c.laneOffset = m->getSettingValue<double>("lane-offset");
    c.bufferMs = m->getSettingValue<double>("buffer-ms");
    c.jitter = m->getSettingValue<double>("jitter");
    c.countdownMs = static_cast<double>(m->getSettingValue<int64_t>("countdown-ms"));
    c.autoStopSec = static_cast<double>(m->getSettingValue<int64_t>("auto-stop-sec"));
    g_showTaps = m->getSettingValue<bool>("show-taps");
    g_tapSize = static_cast<float>(m->getSettingValue<double>("tap-size"));
}

static void setEnabled(bool on) {
    if (on && !g_enabled) { refreshCfg(); g_engine.reset(); }
    g_enabled = on;
}

// Mode selector: 5 clicks <-> 7 clicks <-> nonstop
static void cycleMode(int dir) {
    auto m = Mod::get();
    int burst = static_cast<int>(m->getSettingValue<int64_t>("burst-size"));
    bool cont = m->getSettingValue<bool>("continuous");
    int idx = cont ? 2 : (burst == 7 ? 1 : 0);
    idx = (idx + dir + 3) % 3;
    m->setSettingValue<bool>("continuous", idx == 2);
    if (idx != 2) m->setSettingValue<int64_t>("burst-size", idx == 1 ? 7 : 5);
    refreshCfg();
    if (g_enabled) g_engine.reset();
}
static void adjust(char const* id, double d, double lo, double hi) {
    auto m = Mod::get();
    m->setSettingValue<double>(id, std::min(hi, std::max(lo, m->getSettingValue<double>(id) + d)));
    refreshCfg();
}

// ------------------------------------------------------------------ presets (5 slots)
namespace {
struct PS { const char* id; char t; };
const PS kPreset[] = {
    {"cps", 'd'}, {"intensity", 'd'}, {"speed", 'd'}, {"continuous", 'b'}, {"burst-size", 'i'},
    {"frequency", 'd'}, {"continue-alt", 'b'}, {"start-lane", 's'}, {"ramp-ms", 'd'}, {"hold-ratio", 'd'},
    {"split-dual", 'b'}, {"input-delay", 'd'}, {"lane-offset", 'd'}, {"buffer-ms", 'd'}, {"jitter", 'd'},
    {"countdown-ms", 'i'}, {"auto-stop-sec", 'i'},
};
std::string pkey(int slot, char const* id) { return fmt::format("p{}.{}", slot, id); }
}

static bool presetUsed(int slot) { return Mod::get()->getSavedValue<bool>(pkey(slot, "used"), false); }

static void presetSave(int slot) {
    auto m = Mod::get();
    for (auto const& p : kPreset) {
        auto k = pkey(slot, p.id);
        switch (p.t) {
            case 'd': m->setSavedValue<double>(k, m->getSettingValue<double>(p.id)); break;
            case 'i': m->setSavedValue<int64_t>(k, m->getSettingValue<int64_t>(p.id)); break;
            case 'b': m->setSavedValue<bool>(k, m->getSettingValue<bool>(p.id)); break;
            case 's': m->setSavedValue<std::string>(k, m->getSettingValue<std::string>(p.id)); break;
        }
    }
    m->setSavedValue<bool>(pkey(slot, "used"), true);
}

static bool presetLoad(int slot) {
    if (!presetUsed(slot)) return false;
    auto m = Mod::get();
    for (auto const& p : kPreset) {
        auto k = pkey(slot, p.id);
        switch (p.t) {
            case 'd': m->setSettingValue<double>(p.id, m->getSavedValue<double>(k, m->getSettingValue<double>(p.id))); break;
            case 'i': m->setSettingValue<int64_t>(p.id, m->getSavedValue<int64_t>(k, m->getSettingValue<int64_t>(p.id))); break;
            case 'b': m->setSettingValue<bool>(p.id, m->getSavedValue<bool>(k, m->getSettingValue<bool>(p.id))); break;
            case 's': m->setSettingValue<std::string>(p.id, m->getSavedValue<std::string>(k, m->getSettingValue<std::string>(p.id))); break;
        }
    }
    refreshCfg();
    if (g_enabled) g_engine.reset();
    return true;
}

static void presetClear(int slot) { Mod::get()->setSavedValue<bool>(pkey(slot, "used"), false); }

// ------------------------------------------------------------------ the actual input injection
class $modify(AltGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            auto send = [this](bool down, int lane) {
                bool p1 = !(g_cfg.splitDual && m_gameState.m_isDualMode && lane == 1);
                if (down) g_tapAge[lane] = 0.f;
                this->handleButton(down, 1, p1);
            };
            if (g_enabled && m_player1 && !m_player1->m_isDead) {
                if (++g_sinceRefresh >= 60) { g_sinceRefresh = 0; refreshCfg(); }
                g_engine.step(static_cast<double>(dt) * 1000.0, g_cfg, send);
                g_wasActive = true;
            } else if (g_wasActive) {
                g_engine.releaseAll(send); // never leave a button stuck down
                g_wasActive = false;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ------------------------------------------------------------------ HUD
class $modify(AltPlayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* taps = nullptr;
        CCLabelBMFont* mode = nullptr;
        CCLabelBMFont* cps = nullptr;
        CCLabelBMFont* inten = nullptr;
        CCLabelBMFont* slot = nullptr;
        CCLabelBMFont* stats = nullptr;
        ButtonSprite* tog = nullptr;
        CCNode* box = nullptr;
        float acc = 0.f;
        float fps = 60.f;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        refreshCfg();
        if (Mod::get()->getSettingValue<bool>("auto-arm")) setEnabled(true);
        if (Mod::get()->getSettingValue<bool>("hud")) this->buildHud();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_engine.reset();
        g_wasActive = false;
        refreshCfg();
        if (Mod::get()->getSettingValue<bool>("auto-arm")) g_enabled = true;
        if (auto h = m_uiLayer->getChildByID("altspam-hud")) h->setVisible(true);
    }

    void buildHud() {
        auto f = m_fields.self();
        auto ws = CCDirector::sharedDirector()->getWinSize();
        float sc = static_cast<float>(Mod::get()->getSettingValue<double>("hud-scale"));
        bool right = Mod::get()->getSettingValue<std::string>("hud-side") == "Right";
        float x0 = right ? ws.width - 70.f * sc : 70.f * sc;
        float step = 36.f * sc;
        float y0 = ws.height - 26.f * sc;

        auto root = CCNode::create();
        root->setID("altspam-hud");
        root->setZOrder(300);
        static_cast<CCNode*>(m_uiLayer)->addChild(root);

        f->taps = CCDrawNode::create();
        root->addChild(f->taps, 0);

        // always-visible button that opens/closes the panel
        auto top = CCMenu::create();
        top->setPosition({0.f, 0.f});
        root->addChild(top, 2);
        auto open = CCMenuItemSpriteExtra::create(ButtonSprite::create("AS"), this, menu_selector(AltPlayLayer::onOpen));
        open->setPosition({x0, y0});
        open->setScale(0.8f * sc);
        top->addChild(open);

        // the panel
        f->box = CCNode::create();
        f->box->setVisible(false);
        root->addChild(f->box, 2);
        auto pm = CCMenu::create();
        pm->setPosition({0.f, 0.f});
        f->box->addChild(pm, 1);

        auto btn = [&](CCNode* spr, SEL_MenuHandler cb, float x, float y, float s) {
            auto b = CCMenuItemSpriteExtra::create(spr, this, cb);
            b->setPosition({x, y}); b->setScale(s * sc); pm->addChild(b);
        };
        auto arrow = [](bool flip) { auto a = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"); a->setFlipX(flip); return a; };
        auto label = [&](float x, float y, float s) {
            auto l = CCLabelBMFont::create("", "bigFont.fnt");
            l->setPosition({x, y}); l->setScale(s * sc); f->box->addChild(l, 2); return l;
        };
        float dx = 58.f * sc;

        // row 1: ON / OFF
        f->tog = ButtonSprite::create("OFF");
        btn(f->tog, menu_selector(AltPlayLayer::onToggle), x0, y0 - step, 0.85f);
        // row 2: mode  < 5 CLICKS >
        btn(arrow(false), menu_selector(AltPlayLayer::onModePrev), x0 - dx - 14.f * sc, y0 - 2 * step, 0.8f);
        btn(arrow(true), menu_selector(AltPlayLayer::onModeNext), x0 + dx + 14.f * sc, y0 - 2 * step, 0.8f);
        f->mode = label(x0, y0 - 2 * step, 0.45f);
        // row 3: CPS  -  20  +
        btn(ButtonSprite::create("-"), menu_selector(AltPlayLayer::onCpsDown), x0 - dx, y0 - 3 * step, 0.7f);
        btn(ButtonSprite::create("+"), menu_selector(AltPlayLayer::onCpsUp), x0 + dx, y0 - 3 * step, 0.7f);
        f->cps = label(x0, y0 - 3 * step, 0.4f);
        // row 4: intensity
        btn(ButtonSprite::create("-"), menu_selector(AltPlayLayer::onIntDown), x0 - dx, y0 - 4 * step, 0.7f);
        btn(ButtonSprite::create("+"), menu_selector(AltPlayLayer::onIntUp), x0 + dx, y0 - 4 * step, 0.7f);
        f->inten = label(x0, y0 - 4 * step, 0.4f);
        // row 5: preset slot
        btn(arrow(false), menu_selector(AltPlayLayer::onSlotPrev), x0 - dx - 14.f * sc, y0 - 5 * step, 0.7f);
        btn(arrow(true), menu_selector(AltPlayLayer::onSlotNext), x0 + dx + 14.f * sc, y0 - 5 * step, 0.7f);
        f->slot = label(x0, y0 - 5 * step, 0.4f);
        // row 6: preset actions
        btn(ButtonSprite::create("Load"), menu_selector(AltPlayLayer::onLoad), x0 - 62.f * sc, y0 - 6 * step, 0.55f);
        btn(ButtonSprite::create("Save"), menu_selector(AltPlayLayer::onSave), x0, y0 - 6 * step, 0.55f);
        btn(ButtonSprite::create("Del"), menu_selector(AltPlayLayer::onClear), x0 + 62.f * sc, y0 - 6 * step, 0.55f);
        // stats
        f->stats = CCLabelBMFont::create("", "chatFont.fnt");
        f->stats->setPosition({x0, y0 - 7 * step + 6.f * sc});
        f->stats->setScale(0.6f * sc);
        f->box->addChild(f->stats, 2);
    }

    void onOpen(CCObject*) { auto f = m_fields.self(); f->box->setVisible(!f->box->isVisible()); }
    void onToggle(CCObject*) { setEnabled(!g_enabled); }
    void onModePrev(CCObject*) { cycleMode(-1); }
    void onModeNext(CCObject*) { cycleMode(1); }
    void onCpsDown(CCObject*) { adjust("cps", -1.0, 1.0, 60.0); }
    void onCpsUp(CCObject*) { adjust("cps", 1.0, 1.0, 60.0); }
    void onIntDown(CCObject*) { adjust("intensity", -5.0, 5.0, 100.0); }
    void onIntUp(CCObject*) { adjust("intensity", 5.0, 5.0, 100.0); }
    void onSlotPrev(CCObject*) { g_slot = g_slot <= 1 ? 5 : g_slot - 1; }
    void onSlotNext(CCObject*) { g_slot = g_slot >= 5 ? 1 : g_slot + 1; }
    void onLoad(CCObject*) {
        if (presetLoad(g_slot)) Notification::create(fmt::format("Loaded slot {}", g_slot), NotificationIcon::Success, 1.f)->show();
        else Notification::create(fmt::format("Slot {} is empty", g_slot), NotificationIcon::Warning, 1.f)->show();
    }
    void onSave(CCObject*) {
        presetSave(g_slot);
        Notification::create(fmt::format("Saved slot {}", g_slot), NotificationIcon::Success, 1.f)->show();
    }
    void onClear(CCObject*) {
        presetClear(g_slot);
        Notification::create(fmt::format("Deleted slot {}", g_slot), NotificationIcon::Info, 1.f)->show();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();
        if (!f->taps) return;

        // tap circles (left = cyan, right = pink), fade out quickly
        for (int l = 0; l < 2; l++) g_tapAge[l] += dt;
        f->taps->clear();
        if (g_showTaps) {
            auto ws = CCDirector::sharedDirector()->getWinSize();
            for (int l = 0; l < 2; l++) {
                float age = g_tapAge[l];
                if (age > 0.2f) continue;
                float a = 1.f - age / 0.2f;
                CCPoint pos = {ws.width * (l ? 0.75f : 0.25f), ws.height * 0.5f};
                f->taps->drawDot(pos, g_tapSize * (1.f + age), l ? ccc4f(1.f, 0.4f, 0.7f, 0.75f * a) : ccc4f(0.2f, 0.9f, 0.85f, 0.75f * a));
            }
        }

        if (!f->box->isVisible()) return;
        if (dt > 0.f) f->fps = f->fps * 0.9f + (1.f / dt) * 0.1f;
        f->acc += dt;
        if (f->acc < 0.1f) return;
        f->acc = 0.f;
        f->tog->setString(g_enabled ? "ON" : "OFF");
        f->mode->setString(g_cfg.continuous ? "NONSTOP" : fmt::format("{} CLICKS", g_cfg.burstSize).c_str());
        f->cps->setString(fmt::format("{:.0f} CPS", Mod::get()->getSettingValue<double>("cps")).c_str());
        f->inten->setString(fmt::format("{:.0f}% POWER", Mod::get()->getSettingValue<double>("intensity")).c_str());
        f->slot->setString(fmt::format("SLOT {}{}", g_slot, presetUsed(g_slot) ? " *" : "").c_str());
        f->stats->setString(fmt::format("live {:.1f}  FPS {:.0f}  err {:.1f}ms{}",
            g_engine.liveCps(), f->fps, g_engine.avgErrMs(),
            g_engine.counting() ? "  (starting)" : (g_engine.isFinished() ? "  (auto-stopped)" : "")).c_str());
    }
};

// Hide the panel + taps while the game is paused
#include <Geode/modify/PauseLayer.hpp>
class $modify(AltPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (auto pl = PlayLayer::get()) if (auto h = pl->m_uiLayer->getChildByID("altspam-hud")) h->setVisible(false);
    }
    void onResume(CCObject* s) {
        PauseLayer::onResume(s);
        if (auto pl = PlayLayer::get()) if (auto h = pl->m_uiLayer->getChildByID("altspam-hud")) h->setVisible(true);
    }
};

// ------------------------------------------------------------------ desktop keybinds (no effect on mobile)
#ifdef GEODE_IS_DESKTOP
#include <Geode/modify/CCKeyboardDispatcher.hpp>
static enumKeyCodes keyFromName(std::string const& n) {
    static const std::unordered_map<std::string, enumKeyCodes> k = {
        {"F6", KEY_F6}, {"F7", KEY_F7}, {"F8", KEY_F8}, {"F9", KEY_F9}, {"Q", KEY_Q}, {"E", KEY_E}, {"R", KEY_R},
        {"Z", KEY_Z}, {"X", KEY_X}, {"C", KEY_C}, {"V", KEY_V}, {"Space", KEY_Space}, {"Left", KEY_Left}, {"Right", KEY_Right},
    };
    auto it = k.find(n);
    return it == k.end() ? KEY_None : it->second;
}
class $modify(AltKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat && PlayLayer::get()) {
            auto m = Mod::get();
            if (key == keyFromName(m->getSettingValue<std::string>("key-toggle"))) setEnabled(!g_enabled);
            else if (key == keyFromName(m->getSettingValue<std::string>("key-prev"))) cycleMode(-1);
            else if (key == keyFromName(m->getSettingValue<std::string>("key-next"))) cycleMode(1);
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};
#endif
