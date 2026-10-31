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
}

static void setEnabled(bool on) {
    if (on && !g_enabled) { refreshCfg(); g_engine.reset(); }
    g_enabled = on;
}

// 5 <-> 7 selector (anything else jumps to 5)
static void stepPattern() {
    auto m = Mod::get();
    int cur = static_cast<int>(m->getSettingValue<int64_t>("burst-size"));
    m->setSettingValue<int64_t>("burst-size", cur == 5 ? 7 : 5);
    m->setSettingValue<bool>("continuous", false);
    refreshCfg();
    if (g_enabled) g_engine.reset();
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
    void processCommands(float dt) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            auto send = [this](bool down, int lane) {
                bool p1 = !(g_cfg.splitDual && m_gameState.m_isDualMode && lane == 1);
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
        GJBaseGameLayer::processCommands(dt);
    }
};

// ------------------------------------------------------------------ HUD
class $modify(AltPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* stats = nullptr;
        CCLabelBMFont* pat = nullptr;
        CCLabelBMFont* slot = nullptr;
        ButtonSprite* tog = nullptr;
        CCNode* panel = nullptr;
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
    }

    void buildHud() {
        auto fields = m_fields.self();
        auto parent = static_cast<CCNode*>(m_uiLayer);
        auto ws = CCDirector::sharedDirector()->getWinSize();
        float top = ws.height - 34.f;

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setZOrder(200);
        parent->addChild(menu);

        auto addBtn = [&](CCNode* spr, SEL_MenuHandler cb, CCPoint pos, float scale) {
            auto b = CCMenuItemSpriteExtra::create(spr, this, cb);
            b->setPosition(pos);
            b->setScale(scale);
            menu->addChild(b);
            return b;
        };

        // 5/7 selector:  <  5  >
        auto l = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        auto r = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        r->setFlipX(true);
        addBtn(l, menu_selector(AltPlayLayer::onPattern), {28.f, top}, 0.75f);
        addBtn(r, menu_selector(AltPlayLayer::onPattern), {104.f, top}, 0.75f);
        fields->pat = CCLabelBMFont::create("5", "bigFont.fnt");
        fields->pat->setPosition({66.f, top});
        fields->pat->setScale(0.9f);
        parent->addChild(fields->pat, 200);

        // ON/OFF + preset panel button
        fields->tog = ButtonSprite::create("OFF");
        addBtn(fields->tog, menu_selector(AltPlayLayer::onToggle), {170.f, top}, 0.6f);
        addBtn(ButtonSprite::create("P"), menu_selector(AltPlayLayer::onPanel), {232.f, top}, 0.6f);

        fields->stats = CCLabelBMFont::create("", "chatFont.fnt");
        fields->stats->setAnchorPoint({0.f, 0.5f});
        fields->stats->setPosition({10.f, top - 28.f});
        fields->stats->setScale(0.7f);
        parent->addChild(fields->stats, 200);

        // preset panel (hidden by default)
        auto panel = CCMenu::create();
        panel->setPosition({0.f, 0.f});
        panel->setVisible(false);
        panel->setZOrder(200);
        parent->addChild(panel);
        fields->panel = panel;
        float py = top - 58.f;
        auto pl = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        auto pr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        pr->setFlipX(true);
        auto mk = [&](CCNode* spr, SEL_MenuHandler cb, CCPoint pos, float sc) {
            auto b = CCMenuItemSpriteExtra::create(spr, this, cb);
            b->setPosition(pos); b->setScale(sc); panel->addChild(b);
        };
        mk(pl, menu_selector(AltPlayLayer::onSlotPrev), {28.f, py}, 0.6f);
        mk(pr, menu_selector(AltPlayLayer::onSlotNext), {118.f, py}, 0.6f);
        fields->slot = CCLabelBMFont::create("Slot 1", "bigFont.fnt");
        fields->slot->setPosition({73.f, py});
        fields->slot->setScale(0.45f);
        parent->addChild(fields->slot, 200);
        fields->slot->setVisible(false);
        mk(ButtonSprite::create("Load"), menu_selector(AltPlayLayer::onLoad), {172.f, py}, 0.5f);
        mk(ButtonSprite::create("Save"), menu_selector(AltPlayLayer::onSave), {228.f, py}, 0.5f);
        mk(ButtonSprite::create("Clear"), menu_selector(AltPlayLayer::onClear), {288.f, py}, 0.5f);
    }

    void onPattern(CCObject*) { stepPattern(); }
    void onToggle(CCObject*) { setEnabled(!g_enabled); }
    void onPanel(CCObject*) {
        auto f = m_fields.self();
        bool v = !f->panel->isVisible();
        f->panel->setVisible(v);
        f->slot->setVisible(v);
    }
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
        Notification::create(fmt::format("Cleared slot {}", g_slot), NotificationIcon::Info, 1.f)->show();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();
        if (!f->stats) return;
        if (dt > 0.f) f->fps = f->fps * 0.9f + (1.f / dt) * 0.1f;
        f->acc += dt;
        if (f->acc < 0.1f) return;
        f->acc = 0.f;
        f->pat->setString(g_cfg.continuous ? "~" : fmt::format("{}", g_cfg.burstSize).c_str());
        f->tog->setString(g_enabled ? "ON" : "OFF");
        f->slot->setString(fmt::format("Slot {}{}", g_slot, presetUsed(g_slot) ? "*" : "").c_str());
        double target = alt::effectiveCps(g_cfg, alt::Engine::kInf);
        f->stats->setString(fmt::format("CPS {:.1f}/{:.1f}  FPS {:.0f}  err {:.1f}ms{}",
            g_engine.liveCps(), target, f->fps, g_engine.avgErrMs(),
            g_engine.counting() ? "  (starting)" : (g_engine.isFinished() ? "  (auto-stopped)" : "")).c_str());
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
            else if (key == keyFromName(m->getSettingValue<std::string>("key-prev"))) stepPattern();
            else if (key == keyFromName(m->getSettingValue<std::string>("key-next"))) stepPattern();
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};
#endif
