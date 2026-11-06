#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

using namespace geode::prelude;

// ---- settings --------------------------------------------------------
static constexpr int SPAWN_GRACE_TICKS = 16;

static constexpr int MODE_COUNT = 7; // Wave dihapus
static constexpr char const* MODE_KEYS[] = {
    "cube-ticks", "ball-ticks", "robot-ticks", "spider-ticks",
    "ship-ticks", "ufo-ticks", "swing-ticks"
};
static int g_modeTicks[MODE_COUNT] = {-1, -1, -1, -1, -1, -1, -1};

static int g_stage = 4;
static constexpr int TPS_PRESETS[] = {60, 120, 240, 360, 480, 720, 960, 1000};
static constexpr int TPS_COUNT = 8;
static constexpr int TPS_MIN = 1;
static constexpr int TPS_MAX = 10000;
static int g_tps = 240;

static int g_resetEpoch = 0;
// ----------------------------------------------------------------------

static void loadSettings() {
    auto m = Mod::get();
    g_stage = static_cast<int>(m->getSettingValue<int64_t>("stage"));
    g_tps = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("tps-value")), TPS_MIN, TPS_MAX);
    for (int i = 0; i < MODE_COUNT; i++) {
        g_modeTicks[i] = static_cast<int>(m->getSettingValue<int64_t>(MODE_KEYS[i]));
    }
}

static float stepsPerTick() {
    return g_tps / 240.0f;
}

static bool isFlyMode(PlayerObject* p) {
    return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
}

static bool isGroundMode(PlayerObject* p) {
    return !isFlyMode(p);
}

static int modeIndex(PlayerObject* p) {
    if (p->m_isShip) return 4;
    if (p->m_isBird) return 5;
    if (p->m_isSwing) return 6;
    if (p->m_isBall) return 1;
    if (p->m_isRobot) return 2;
    if (p->m_isSpider) return 3;
    return 0;
}

static int ticksForMode(int idx) {
    int c = g_modeTicks[idx];
    if (c < 0) {
        return g_stage;
    }
    return c;
}

static int modeLimitSteps(PlayerObject* p) {
    int ticks = ticksForMode(modeIndex(p));
    if (ticks <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(ticks * stepsPerTick())));
}

static int spawnGraceSteps() {
    return std::max(1, static_cast<int>(std::lround(SPAWN_GRACE_TICKS * stepsPerTick())));
}

static int ringsTouching(PlayerObject* p) {
    return p->m_touchingRings ? static_cast<int>(p->m_touchingRings->count()) : 0;
}

static int stepTps(int cur, bool up) {
    if (up) {
        for (int i = 0; i < TPS_COUNT; i++) {
            if (TPS_PRESETS[i] > cur) return TPS_PRESETS[i];
        }
        return cur;
    }
    for (int i = TPS_COUNT - 1; i >= 0; i--) {
        if (TPS_PRESETS[i] < cur) return TPS_PRESETS[i];
    }
    return cur;
}

static void doHotkey(int action) {
    auto m = Mod::get();
    switch (action) {
        case 0: {
            g_stage = (g_stage + 1) % 17;
            m->setSettingValue<int64_t>("stage", static_cast<int64_t>(g_stage));
            break;
        }
        case 1:
            g_tps = stepTps(g_tps, true);
            m->setSettingValue<int64_t>("tps-value", static_cast<int64_t>(g_tps));
            break;
        case 2:
            g_tps = stepTps(g_tps, false);
            m->setSettingValue<int64_t>("tps-value", static_cast<int64_t>(g_tps));
            break;
    }
}

$on_mod(Loaded) {
    loadSettings();
    listenForSettingChanges<int64_t>("stage", [](int64_t) { loadSettings(); });
    listenForSettingChanges<int64_t>("tps-value", [](int64_t) { loadSettings(); });
    for (auto key : MODE_KEYS) {
        listenForSettingChanges<int64_t>(key, [](int64_t) { loadSettings(); });
    }
}

class $modify(BufferPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = CCMenu::create();
        auto spr = ButtonSprite::create("Buffer");
        spr->setScale(0.7f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BufferPause::onBufferSettings));
        menu->addChild(btn);
        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 60.f, 30.f});
        this->addChild(menu);
    }

    void onBufferSettings(CCObject*) {
        // Buka menu setting bawaan Geode
        openSettingsPopup(Mod::get(), false);
    }
};

class $modify(BufferReset, PlayLayer) {
    void resetLevel() { PlayLayer::resetLevel(); g_resetEpoch++; }
    void resetLevelFromStart() { PlayLayer::resetLevelFromStart(); g_resetEpoch++; }
    void loadFromCheckpoint(CheckpointObject* object) { PlayLayer::loadFromCheckpoint(object); g_resetEpoch++; }
    void destroyPlayer(PlayerObject* player, GameObject* object) { PlayLayer::destroyPlayer(player, object); g_resetEpoch++; }
};

struct PState {
    bool held = false;
    bool pending = false;
    int pendingStart = 0;
    int lastRingCount = 0;
    bool releaseNext = false;
};

class $modify(BufferLayer, GJBaseGameLayer) {
    struct Fields {
        int step = 0;
        int epoch = 0;
        int graceUntil = 0;
        PState ps[2];
    };

    void sendInput(PlayerObject* p, bool down) {
        if (down) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    }

    void syncState() {
        auto f = m_fields.operator->();
        if (f->epoch == g_resetEpoch) return;
        f->epoch = g_resetEpoch;
        for (auto& s : f->ps) s = PState();
        f->graceUntil = f->step + spawnGraceSteps();
    }

    void stepPlayer(PlayerObject* p, PState& s, int idx) {
        int step = m_fields->step;

        if (s.releaseNext) {
            s.releaseNext = false;
            if (!s.held) sendInput(p, false);
        }

        if (!s.pending) return;

        int waited = step - s.pendingStart;
        int limit = modeLimitSteps(p);

        if (limit <= 0 || waited > limit || !isGroundMode(p)) {
            s.pending = false;
            return;
        }

        int rings = ringsTouching(p);
        if (rings != s.lastRingCount) s.lastRingCount = rings;

        bool onGround = p->m_isOnGround;
        bool onRing = rings > 0;

        if (onGround || onRing) {
            s.pending = false;
            if (!s.held) {
                sendInput(p, true);
                s.releaseNext = true;
            }
        }
    }

    bool inputPlayer(PlayerObject* p, PState& s, int idx, bool down) {
        int step = m_fields->step;
        int n = modeLimitSteps(p);

        // Logika Min Hold sederhana untuk mode terbang
        if (!down && isFlyMode(p) && s.held) {
            // Karena kita udah gak pakai sistem antrean rumit,
            // kita biarin game yang urus rilisnya.
        }

        s.held = down;

        if (down) {
            bool early = isGroundMode(p) && !p->m_isOnGround && ringsTouching(p) == 0;
            if (early && n > 0) {
                s.pending = true;
                s.pendingStart = step;
                s.lastRingCount = 0;
            } else {
                s.pending = false;
            }
        }
        return false;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto f = m_fields.operator->();
        f->step++;
        syncState();

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (!PlayLayer::get()) {
            for (auto& s : f->ps) s = PState();
            return;
        }

        if (m_player1) stepPlayer(m_player1, f->ps[0], 0);
        if (m_player2) stepPlayer(m_player2, f->ps[1], 1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto f = m_fields.operator->();
        syncState();

        if (PlayLayer::get() && button == 1) {
            bool asPlayer1 = isPlayer1 || !(m_levelSettings && m_levelSettings->m_twoPlayerMode);
            int idx = asPlayer1 ? 0 : 1;
            auto p = asPlayer1 ? m_player1 : m_player2;
            if (p) {
                if (inputPlayer(p, f->ps[idx], idx, down)) return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
