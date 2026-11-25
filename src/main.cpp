#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/Popup.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr double REF_FPS = 60.0;
constexpr double PHYS_TPS = 240.0;
constexpr float TICK_DT_60 = 0.25f; // 1 tick a 240 TPS equivale a 0.25 frames a 60 FPS
constexpr int TICKS_PER_FRAME60 = 4;
constexpr double SEARCH_BUDGET_MS = 8.0;

// Estado global de simulacion del jugador fantasma (solo corre en el hilo principal de Cocos2d)
bool g_simulatingGhost = false;
bool g_ghostDied = false;
PlayerObject* g_ghostPrefix = nullptr;
PlayerObject* g_ghostProbe = nullptr;

inline bool isGhostPlayer(PlayerObject* p) {
    return p != nullptr && (p == g_ghostPrefix || p == g_ghostProbe);
}

enum class VehicleMode {
    Cube,
    Ship,
    Ball,
    UFO,
    Wave,
    Robot,
    Spider,
    Swing
};

struct PendingAction {
    bool down = false;
    int button = 1;
    bool isPlayer1 = true;
    double createdTime = 0.0;
    double targetTime = 0.0;
    double expireTime = 0.0;
    double lastRescanTime = 0.0;
    int targetFrame60 = 0;
    bool quickTapRelease = false;
};

struct ProbeOutcome {
    bool survivedAll = true;
    bool effective = true;
    int survivalTicks = 0;
    float movedX = 0.f;
};

struct DelayDecision {
    int delayTicks = 0;
    bool fireNow = true;
    int d0SurvivalTicks = 0;
    int d0MovedX = 0;
    const char* reason = "OK";
};

struct ActivationSnapshot {
    GameObject* obj = nullptr;
    EffectGameObject* effectObj = nullptr;
    bool isActivated = false;
    bool activatedP1 = false;
    bool activatedP2 = false;
};

template <typename E>
void saveEffectActivation(E* e, bool& outP1, bool& outP2) {
    if (!e) return;
    if constexpr (requires { e->m_hasBeenActivated; }) {
        outP1 = e->m_hasBeenActivated;
    }
    if constexpr (requires { e->m_hasBeenActivatedP2; }) {
        outP2 = e->m_hasBeenActivatedP2;
    }
}

template <typename E>
void restoreEffectActivation(E* e, bool p1, bool p2) {
    if (!e) return;
    if constexpr (requires { e->m_hasBeenActivated; }) {
        e->m_hasBeenActivated = p1;
    }
    if constexpr (requires { e->m_hasBeenActivatedP2; }) {
        e->m_hasBeenActivatedP2 = p2;
    }
}

template <typename P>
void dirtyGhostRects(P* ghost) {
    if (!ghost) return;
    if constexpr (requires { ghost->setObjectRectDirty(true); }) {
        ghost->setObjectRectDirty(true);
    } else if constexpr (requires { ghost->m_objectRectDirty = true; }) {
        ghost->m_objectRectDirty = true;
    }
    if constexpr (requires { ghost->setOrientedRectDirty(true); }) {
        ghost->setOrientedRectDirty(true);
    } else if constexpr (requires { ghost->m_orientedRectDirty = true; }) {
        ghost->m_orientedRectDirty = true;
    }
}

template <typename P>
void copyFullPhysicsState(P* dst, P* src) {
    if (!dst || !src) return;

    // Campos base confirmados en Geode v6.0.0
    dst->m_isShip = src->m_isShip;
    dst->m_isBall = src->m_isBall;
    dst->m_isBird = src->m_isBird;
    dst->m_isDart = src->m_isDart;
    dst->m_isRobot = src->m_isRobot;
    dst->m_isSpider = src->m_isSpider;
    dst->m_isSwing = src->m_isSwing;
    dst->m_isUpsideDown = src->m_isUpsideDown;
    dst->m_isOnGround = src->m_isOnGround;
    dst->m_playerSpeed = src->m_playerSpeed;
    dst->m_vehicleSize = src->m_vehicleSize;
    dst->m_yVelocity = src->m_yVelocity;
    dst->m_isDead = false;

    // Campos fisicos adicionales copiados de forma segura si existen en el binding actual
    if constexpr (requires { dst->m_gravityMod = src->m_gravityMod; }) dst->m_gravityMod = src->m_gravityMod;
    if constexpr (requires { dst->m_isLocked = false; }) dst->m_isLocked = false;
    if constexpr (requires { dst->m_isHidden = false; }) dst->m_isHidden = false;
    if constexpr (requires { dst->m_isGoingLeft = src->m_isGoingLeft; }) dst->m_isGoingLeft = src->m_isGoingLeft;
    if constexpr (requires { dst->m_isSideways = src->m_isSideways; }) dst->m_isSideways = src->m_isSideways;
    if constexpr (requires { dst->m_isPlatformer = src->m_isPlatformer; }) dst->m_isPlatformer = src->m_isPlatformer;
    if constexpr (requires { dst->m_isDashing = src->m_isDashing; }) dst->m_isDashing = src->m_isDashing;
    if constexpr (requires { dst->m_dashX = src->m_dashX; }) dst->m_dashX = src->m_dashX;
    if constexpr (requires { dst->m_dashY = src->m_dashY; }) dst->m_dashY = src->m_dashY;
    if constexpr (requires { dst->m_dashAngle = src->m_dashAngle; }) dst->m_dashAngle = src->m_dashAngle;
    if constexpr (requires { dst->m_dashStartTime = src->m_dashStartTime; }) dst->m_dashStartTime = src->m_dashStartTime;
    if constexpr (requires { dst->m_isOnSlope = src->m_isOnSlope; }) dst->m_isOnSlope = src->m_isOnSlope;
    if constexpr (requires { dst->m_wasOnSlope = src->m_wasOnSlope; }) dst->m_wasOnSlope = src->m_wasOnSlope;
    if constexpr (requires { dst->m_isOnSlope2 = src->m_isOnSlope2; }) dst->m_isOnSlope2 = src->m_isOnSlope2;
    if constexpr (requires { dst->m_slopeAngle = src->m_slopeAngle; }) dst->m_slopeAngle = src->m_slopeAngle;
    if constexpr (requires { dst->m_slopeStartTime = src->m_slopeStartTime; }) dst->m_slopeStartTime = src->m_slopeStartTime;
    if constexpr (requires { dst->m_currentSlope = src->m_currentSlope; }) dst->m_currentSlope = src->m_currentSlope;
    if constexpr (requires { dst->m_currentSlope2 = src->m_currentSlope2; }) dst->m_currentSlope2 = src->m_currentSlope2;
    if constexpr (requires { dst->m_lastPosition = src->m_lastPosition; }) dst->m_lastPosition = src->m_lastPosition;
    if constexpr (requires { dst->m_prevPosition = src->m_prevPosition; }) dst->m_prevPosition = src->m_prevPosition;
    if constexpr (requires { dst->m_lastGroundedPos = src->m_lastGroundedPos; }) dst->m_lastGroundedPos = src->m_lastGroundedPos;
    if constexpr (requires { dst->m_stateOnGround = src->m_stateOnGround; }) dst->m_stateOnGround = src->m_stateOnGround;
    if constexpr (requires { dst->m_isOnGround2 = src->m_isOnGround2; }) dst->m_isOnGround2 = src->m_isOnGround2;
    if constexpr (requires { dst->m_isOnGround3 = src->m_isOnGround3; }) dst->m_isOnGround3 = src->m_isOnGround3;
    if constexpr (requires { dst->m_isOnGround4 = src->m_isOnGround4; }) dst->m_isOnGround4 = src->m_isOnGround4;
    if constexpr (requires { dst->m_wasRobotJump = src->m_wasRobotJump; }) dst->m_wasRobotJump = src->m_wasRobotJump;
    if constexpr (requires { dst->m_robotBurstTimer = src->m_robotBurstTimer; }) dst->m_robotBurstTimer = src->m_robotBurstTimer;
    if constexpr (requires { dst->m_jumpBuffered = src->m_jumpBuffered; }) dst->m_jumpBuffered = src->m_jumpBuffered;
    if constexpr (requires { dst->m_stateRingJump = src->m_stateRingJump; }) dst->m_stateRingJump = src->m_stateRingJump;
    if constexpr (requires { dst->m_hasJustHeld = src->m_hasJustHeld; }) dst->m_hasJustHeld = src->m_hasJustHeld;
    if constexpr (requires { dst->m_hasJustHeld2 = src->m_hasJustHeld2; }) dst->m_hasJustHeld2 = src->m_hasJustHeld2;
    if constexpr (requires { dst->m_holdingButtons = src->m_holdingButtons; }) dst->m_holdingButtons = src->m_holdingButtons;
    if constexpr (requires { dst->m_holdingRight = src->m_holdingRight; }) dst->m_holdingRight = src->m_holdingRight;
    if constexpr (requires { dst->m_holdingLeft = src->m_holdingLeft; }) dst->m_holdingLeft = src->m_holdingLeft;
    if constexpr (requires { dst->m_xVelocity = src->m_xVelocity; }) dst->m_xVelocity = src->m_xVelocity;
    if constexpr (requires { dst->m_platformerXVelocity = src->m_platformerXVelocity; }) dst->m_platformerXVelocity = src->m_platformerXVelocity;
    if constexpr (requires { dst->m_maybeReverseSpeed = src->m_maybeReverseSpeed; }) dst->m_maybeReverseSpeed = src->m_maybeReverseSpeed;
    if constexpr (requires { dst->m_maybeIsFalling = src->m_maybeIsFalling; }) dst->m_maybeIsFalling = src->m_maybeIsFalling;
    if constexpr (requires { dst->m_scaleX = src->m_scaleX; }) dst->m_scaleX = src->m_scaleX;
    if constexpr (requires { dst->m_scaleY = src->m_scaleY; }) dst->m_scaleY = src->m_scaleY;

    dst->setScale(src->getScale());
}

VehicleMode detectVehicle(PlayerObject* player) {
    if (!player) return VehicleMode::Cube;
    if (player->m_isShip) return VehicleMode::Ship;
    if (player->m_isBall) return VehicleMode::Ball;
    if (player->m_isBird) return VehicleMode::UFO;
    if (player->m_isDart) return VehicleMode::Wave;
    if (player->m_isRobot) return VehicleMode::Robot;
    if (player->m_isSpider) return VehicleMode::Spider;
    if (player->m_isSwing) return VehicleMode::Swing;
    return VehicleMode::Cube;
}

bool isTapVehicle(VehicleMode mode) {
    return mode == VehicleMode::Cube ||
           mode == VehicleMode::Ball ||
           mode == VehicleMode::UFO ||
           mode == VehicleMode::Spider ||
           mode == VehicleMode::Swing;
}

bool isGroundVehicle(VehicleMode mode) {
    return mode == VehicleMode::Cube ||
           mode == VehicleMode::Ball ||
           mode == VehicleMode::Spider ||
           mode == VehicleMode::Robot;
}

const char* vehicleName(VehicleMode mode) {
    switch (mode) {
        case VehicleMode::Cube:   return "Cube";
        case VehicleMode::Ship:   return "Ship";
        case VehicleMode::Ball:   return "Ball";
        case VehicleMode::UFO:    return "UFO";
        case VehicleMode::Wave:   return "Wave";
        case VehicleMode::Robot:  return "Robot";
        case VehicleMode::Spider: return "Spider";
        case VehicleMode::Swing:  return "Swing";
    }
    return "Cube";
}

bool isSpeedPortalID(int id) {
    return id == 200 || id == 201 || id == 202 || id == 203 || id == 1334;
}

// Objetos de fisica que el fantasma SI puede activar (pads, orbes, portales de vehiculo/gravedad/tamano/velocidad)
// Excluye triggers de nivel (Move, Toggle, Alpha, Spawn, SFX, End, Coins, Dual, Mirror, Teleport)
bool isGhostInteractivePhysicsObject(GameObject* obj) {
    if (!obj) return false;
    auto type = obj->m_objectType;
    int rawType = static_cast<int>(type);

    // SpiderPad (44) y GravityTogglePortal (42) por valor entero para maxima compatibilidad entre bindings
    if (rawType == 42 || rawType == 44) return true;

    switch (type) {
        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
        case GameObjectType::CubePortal:
        case GameObjectType::ShipPortal:
        case GameObjectType::BallPortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::SpiderPortal:
        case GameObjectType::SwingPortal:
        case GameObjectType::RegularSizePortal:
        case GameObjectType::MiniSizePortal:
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::SpiderOrb:
            return true;
        case GameObjectType::Modifier:
            return isSpeedPortalID(obj->m_objectID);
        default:
            return false;
    }
}

ActivationSnapshot captureObjectSnapshot(GameObject* obj) {
    ActivationSnapshot snap;
    snap.obj = obj;
    if (!obj) return snap;
    snap.isActivated = obj->m_isActivated;
    snap.effectObj = typeinfo_cast<EffectGameObject*>(obj);
    saveEffectActivation(snap.effectObj, snap.activatedP1, snap.activatedP2);
    return snap;
}

void updateObjectSnapshot(ActivationSnapshot& snap) {
    if (!snap.obj) return;
    snap.isActivated = snap.obj->m_isActivated;
    saveEffectActivation(snap.effectObj, snap.activatedP1, snap.activatedP2);
}

void restoreSnapshots(std::vector<ActivationSnapshot> const& snapshots) {
    for (auto const& snap : snapshots) {
        if (!snap.obj) continue;
        snap.obj->m_isActivated = snap.isActivated;
        restoreEffectActivation(snap.effectObj, snap.activatedP1, snap.activatedP2);
    }
}

} // namespace

// ============================================================================
// 1. VENTANA DE CONFIGURACION (Popup estilo Geometry Dash para Android)
// ============================================================================
class ModConfigPopup : public geode::Popup {
protected:
    std::function<void()> m_onSettingsChanged;

    CCMenuItemToggler* m_enabledToggle = nullptr;
    CCMenuItemToggler* m_safeModeToggle = nullptr;
    CCMenuItemToggler* m_pressToggle = nullptr;
    CCMenuItemToggler* m_releaseToggle = nullptr;
    CCMenuItemToggler* m_hudToggle = nullptr;
    CCMenuItemToggler* m_practiceToggle = nullptr;

    Slider* m_marginSlider = nullptr;
    Slider* m_lookaheadSlider = nullptr;
    Slider* m_scaleSlider = nullptr;

    CCLabelBMFont* m_marginValLabel = nullptr;
    CCLabelBMFont* m_lookaheadValLabel = nullptr;
    CCLabelBMFont* m_scaleValLabel = nullptr;

    static constexpr int MIN_MARGIN = 1;
    static constexpr int MAX_MARGIN = 30;
    static constexpr int MIN_LOOKAHEAD = 8;
    static constexpr int MAX_LOOKAHEAD = 45;
    static constexpr float MIN_SCALE = 0.7f;
    static constexpr float MAX_SCALE = 1.4f;

    bool init(std::function<void()> onSettingsChanged) {
        if (!Popup::init(415.f, 285.f)) {
            return false;
        }

        m_onSettingsChanged = std::move(onSettingsChanged);
        this->setTitle("Frame Assist (Ghost) - Config");

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(menu);

        // --- Columna izquierda de interruptores ---
        m_enabledToggle = addCompactToggle(
            menu, "Activar Mod", "mod-enabled", 22.f, 185.f, 224.f,
            menu_selector(ModConfigPopup::onToggleEnabled)
        );
        m_pressToggle = addCompactToggle(
            menu, "Asistir Clic (Press)", "buffer-press", 22.f, 185.f, 194.f,
            menu_selector(ModConfigPopup::onTogglePress)
        );
        m_hudToggle = addCompactToggle(
            menu, "HUD en Nivel", "show-hud", 22.f, 185.f, 164.f,
            menu_selector(ModConfigPopup::onToggleHud)
        );

        // --- Columna derecha de interruptores ---
        m_safeModeToggle = addCompactToggle(
            menu, "Modo Seguro", "safe-mode", 214.f, 386.f, 224.f,
            menu_selector(ModConfigPopup::onToggleSafeMode)
        );
        m_releaseToggle = addCompactToggle(
            menu, "Asistir Soltar", "buffer-release", 214.f, 386.f, 194.f,
            menu_selector(ModConfigPopup::onToggleRelease)
        );
        m_practiceToggle = addCompactToggle(
            menu, "Modo Practica", "enable-in-practice", 214.f, 386.f, 164.f,
            menu_selector(ModConfigPopup::onTogglePractice)
        );

        // --- Sliders principales ---
        int margin = static_cast<int>(Mod::get()->getSettingValue<int64_t>("margin-frames"));
        addSliderRow(
            "Margen (60 FPS)",
            static_cast<float>(margin),
            static_cast<float>(MIN_MARGIN),
            static_cast<float>(MAX_MARGIN),
            126.f,
            fmt::format("{}f", margin),
            menu_selector(ModConfigPopup::onMarginChanged),
            m_marginSlider,
            m_marginValLabel
        );

        int lookahead = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead-frames"));
        addSliderRow(
            "Prediccion",
            static_cast<float>(lookahead),
            static_cast<float>(MIN_LOOKAHEAD),
            static_cast<float>(MAX_LOOKAHEAD),
            94.f,
            fmt::format("{}f", lookahead),
            menu_selector(ModConfigPopup::onLookaheadChanged),
            m_lookaheadSlider,
            m_lookaheadValLabel
        );

        float scale = static_cast<float>(Mod::get()->getSettingValue<double>("ui-scale"));
        addSliderRow(
            "Escala Panel",
            scale,
            MIN_SCALE,
            MAX_SCALE,
            62.f,
            fmt::format("{:.0f}%", scale * 100.f),
            menu_selector(ModConfigPopup::onScaleChanged),
            m_scaleSlider,
            m_scaleValLabel
        );

        // --- Boton Restablecer ---
        auto resetSpr = ButtonSprite::create("Restablecer", "goldFont.fnt", "GJ_button_06.png", 0.75f);
        resetSpr->setScale(0.62f);
        auto resetBtn = CCMenuItemSpriteExtra::create(
            resetSpr, this, menu_selector(ModConfigPopup::onResetDefaults)
        );
        resetBtn->setPosition({m_size.width / 2.f, 25.f});
        menu->addChild(resetBtn);

        return true;
    }

    CCMenuItemToggler* addCompactToggle(
        CCMenu* menu,
        const char* labelText,
        const char* settingKey,
        float labelX,
        float toggleX,
        float posY,
        SEL_MenuHandler callback
    ) {
        auto label = CCLabelBMFont::create(labelText, "bigFont.fnt");
        label->setAnchorPoint({0.f, 0.5f});
        label->setScale(0.36f);
        label->setPosition({labelX, posY});
        m_mainLayer->addChild(label);

        auto toggler = CCMenuItemToggler::createWithStandardSprites(this, callback, 0.65f);
        toggler->setPosition({toggleX, posY});
        toggler->toggle(Mod::get()->getSettingValue<bool>(settingKey));
        menu->addChild(toggler);

        return toggler;
    }

    void addSliderRow(
        const char* labelText,
        float currentValue,
        float minVal,
        float maxVal,
        float posY,
        std::string const& initialText,
        SEL_MenuHandler callback,
        Slider*& outSlider,
        CCLabelBMFont*& outValueLabel
    ) {
        auto label = CCLabelBMFont::create(labelText, "bigFont.fnt");
        label->setAnchorPoint({0.f, 0.5f});
        label->setScale(0.39f);
        label->setPosition({22.f, posY});
        m_mainLayer->addChild(label);

        outSlider = Slider::create(this, callback, 0.66f);
        outSlider->setPosition({252.f, posY});
        float norm = std::clamp((currentValue - minVal) / (maxVal - minVal), 0.f, 1.f);
        outSlider->setValue(norm);
        outSlider->updateBar();
        m_mainLayer->addChild(outSlider);

        outValueLabel = CCLabelBMFont::create(initialText.c_str(), "goldFont.fnt");
        outValueLabel->setAnchorPoint({1.f, 0.5f});
        outValueLabel->setScale(0.45f);
        outValueLabel->setPosition({394.f, posY});
        m_mainLayer->addChild(outValueLabel);
    }

    void notifyChange() {
        if (m_onSettingsChanged) m_onSettingsChanged();
    }

    void toggleSetting(CCObject* sender, const char* key) {
        auto toggler = static_cast<CCMenuItemToggler*>(sender);
        Mod::get()->setSettingValue<bool>(key, !toggler->isToggled());
        notifyChange();
    }

    void onToggleEnabled(CCObject* s)  { toggleSetting(s, "mod-enabled"); }
    void onToggleSafeMode(CCObject* s) { toggleSetting(s, "safe-mode"); }
    void onTogglePress(CCObject* s)    { toggleSetting(s, "buffer-press"); }
    void onToggleRelease(CCObject* s)  { toggleSetting(s, "buffer-release"); }
    void onToggleHud(CCObject* s)      { toggleSetting(s, "show-hud"); }
    void onTogglePractice(CCObject* s) { toggleSetting(s, "enable-in-practice"); }

    void onMarginChanged(CCObject*) {
        int val = static_cast<int>(std::round(
            MIN_MARGIN + m_marginSlider->getValue() * (MAX_MARGIN - MIN_MARGIN)
        ));
        val = std::clamp(val, MIN_MARGIN, MAX_MARGIN);
        Mod::get()->setSettingValue<int64_t>("margin-frames", val);
        m_marginValLabel->setString(fmt::format("{}f", val).c_str());
        notifyChange();
    }

    void onLookaheadChanged(CCObject*) {
        int val = static_cast<int>(std::round(
            MIN_LOOKAHEAD + m_lookaheadSlider->getValue() * (MAX_LOOKAHEAD - MIN_LOOKAHEAD)
        ));
        val = std::clamp(val, MIN_LOOKAHEAD, MAX_LOOKAHEAD);
        Mod::get()->setSettingValue<int64_t>("lookahead-frames", val);
        m_lookaheadValLabel->setString(fmt::format("{}f", val).c_str());
        notifyChange();
    }

    void onScaleChanged(CCObject*) {
        float val = MIN_SCALE + m_scaleSlider->getValue() * (MAX_SCALE - MIN_SCALE);
        val = std::clamp(val, MIN_SCALE, MAX_SCALE);
        Mod::get()->setSettingValue<double>("ui-scale", static_cast<double>(val));
        m_scaleValLabel->setString(fmt::format("{:.0f}%", val * 100.f).c_str());
        notifyChange();
    }

    void onResetDefaults(CCObject*) {
        Mod::get()->setSettingValue<bool>("mod-enabled", true);
        Mod::get()->setSettingValue<bool>("safe-mode", true);
        Mod::get()->setSettingValue<bool>("buffer-press", true);
        Mod::get()->setSettingValue<bool>("buffer-release", true);
        Mod::get()->setSettingValue<bool>("show-hud", true);
        Mod::get()->setSettingValue<bool>("enable-in-practice", true);
        Mod::get()->setSettingValue<int64_t>("margin-frames", 16);
        Mod::get()->setSettingValue<int64_t>("lookahead-frames", 24);
        Mod::get()->setSettingValue<double>("ui-scale", 1.0);

        m_enabledToggle->toggle(true);
        m_safeModeToggle->toggle(true);
        m_pressToggle->toggle(true);
        m_releaseToggle->toggle(true);
        m_hudToggle->toggle(true);
        m_practiceToggle->toggle(true);

        m_marginSlider->setValue(static_cast<float>(16 - MIN_MARGIN) / (MAX_MARGIN - MIN_MARGIN));
        m_marginSlider->updateBar();
        m_marginValLabel->setString("16f");

        m_lookaheadSlider->setValue(static_cast<float>(24 - MIN_LOOKAHEAD) / (MAX_LOOKAHEAD - MIN_LOOKAHEAD));
        m_lookaheadSlider->updateBar();
        m_lookaheadValLabel->setString("24f");

        m_scaleSlider->setValue((1.0f - MIN_SCALE) / (MAX_SCALE - MIN_SCALE));
        m_scaleSlider->updateBar();
        m_scaleValLabel->setString("100%");

        notifyChange();
    }

public:
    static ModConfigPopup* create(std::function<void()> onSettingsChanged) {
        auto ret = new ModConfigPopup();
        if (ret && ret->init(std::move(onSettingsChanged))) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// ============================================================================
// 2. PANEL VISUAL EN EL MENU PRINCIPAL (MenuLayer)
// ============================================================================
class $modify(FrameAssistMenuLayer, MenuLayer) {
    struct Fields {
        CCNode* panelContainer = nullptr;
        CCLabelBMFont* statusLabel = nullptr;
        CCLabelBMFont* infoLabel = nullptr;
        CCMenuItemToggler* quickToggle = nullptr;
    };

    bool init() {
        if (!MenuLayer::init()) return false;

        auto winSize = CCDirector::get()->getWinSize();

        auto panel = CCNode::create();
        panel->setID("frame-assist-panel"_spr);
        panel->setContentSize({168.f, 46.f});
        panel->setAnchorPoint({0.f, 1.f});
        panel->setPosition({12.f, winSize.height - 12.f});
        this->addChild(panel, 50);
        m_fields->panelContainer = panel;

        auto bg = CCScale9Sprite::create("square02_001.png");
        bg->setContentSize(panel->getContentSize());
        bg->setPosition(panel->getContentSize() / 2.f);
        bg->setOpacity(210);
        panel->addChild(bg);

        auto panelMenu = CCMenu::create();
        panelMenu->setPosition({0.f, 0.f});
        panel->addChild(panelMenu);

        auto quickToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(FrameAssistMenuLayer::onQuickToggle), 0.65f
        );
        quickToggle->setPosition({22.f, 23.f});
        panelMenu->addChild(quickToggle);
        m_fields->quickToggle = quickToggle;

        auto clickArea = CCNode::create();
        clickArea->setContentSize({120.f, 38.f});

        auto statusLabel = CCLabelBMFont::create("Assist: ON", "bigFont.fnt");
        statusLabel->setAnchorPoint({0.f, 0.5f});
        statusLabel->setScale(0.36f);
        statusLabel->setPosition({2.f, 26.f});
        clickArea->addChild(statusLabel);
        m_fields->statusLabel = statusLabel;

        auto infoLabel = CCLabelBMFont::create("Ghost 16f | Config", "goldFont.fnt");
        infoLabel->setAnchorPoint({0.f, 0.5f});
        infoLabel->setScale(0.31f);
        infoLabel->setPosition({2.f, 11.f});
        clickArea->addChild(infoLabel);
        m_fields->infoLabel = infoLabel;

        auto gearIcon = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        gearIcon->setScale(0.4f);
        gearIcon->setPosition({108.f, 19.f});
        clickArea->addChild(gearIcon);

        auto openBtn = CCMenuItemSpriteExtra::create(
            clickArea, this, menu_selector(FrameAssistMenuLayer::onOpenConfig)
        );
        openBtn->setPosition({102.f, 23.f});
        panelMenu->addChild(openBtn);

        refreshPanelVisuals();
        return true;
    }

    void refreshPanelVisuals() {
        bool enabled = Mod::get()->getSettingValue<bool>("mod-enabled");
        int margin = static_cast<int>(Mod::get()->getSettingValue<int64_t>("margin-frames"));
        float scale = static_cast<float>(Mod::get()->getSettingValue<double>("ui-scale"));

        if (m_fields->quickToggle) {
            m_fields->quickToggle->toggle(enabled);
        }
        if (m_fields->statusLabel) {
            m_fields->statusLabel->setString(enabled ? "Assist: ON" : "Assist: OFF");
            m_fields->statusLabel->setColor(enabled ? ccColor3B{120, 255, 120} : ccColor3B{255, 110, 110});
        }
        if (m_fields->infoLabel) {
            m_fields->infoLabel->setString(fmt::format("Ghost {}f | Config", margin).c_str());
        }
        if (m_fields->panelContainer) {
            m_fields->panelContainer->setScale(std::clamp(scale, 0.7f, 1.4f));
        }
    }

    void onQuickToggle(CCObject* sender) {
        auto toggler = static_cast<CCMenuItemToggler*>(sender);
        Mod::get()->setSettingValue<bool>("mod-enabled", !toggler->isToggled());
        refreshPanelVisuals();
    }

    void onOpenConfig(CCObject*) {
        auto popup = ModConfigPopup::create([this]() {
            this->refreshPanelVisuals();
        });
        if (popup) popup->show();
    }
};

// ============================================================================
// 3. MOTOR FANTASMA CON FISICA REAL DEL JUEGO (GJBaseGameLayer)
// Usa 2 PlayerObject invisibles (prefijo + sonda) ejecutando PlayerObject::update
// y GJBaseGameLayer::checkCollisions a 240 TPS reales con horizonte comun.
// ============================================================================
class $modify(FrameAssistGameLayer, GJBaseGameLayer) {
    struct Fields {
        double elapsedTime = 0.0;
        int currentFrame60 = 0;
        bool actualHolding = false;
        bool isDispatchingBuffered = false;
        int savedInputsCount = 0;

        CCLayer* ghostLayer = nullptr;
        PlayerObject* prefixGhost = nullptr;
        PlayerObject* probeGhost = nullptr;

        size_t cachedObjectCount = 0;
        std::vector<GameObject*> interactiveObjects;

        std::deque<PendingAction> pendingQueue;
        int lastDelayTicks = 0;
        int lastD0Survival = 0;
        int lastD0MovedX = 0;
        std::string lastReason = "LISTO";

        ~Fields() {
            if (g_ghostPrefix == prefixGhost) g_ghostPrefix = nullptr;
            if (g_ghostProbe == probeGhost) g_ghostProbe = nullptr;
            g_simulatingGhost = false;
            g_ghostDied = false;
        }
    };

    // Unico hook de aislamiento necesario en GJBaseGameLayer: impide que el fantasma
    // active triggers de nivel (Move, Toggle, SFX, End, Coins, Checkpoints)
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* obj) {
        if (g_simulatingGhost || isGhostPlayer(player)) {
            return isGhostInteractivePhysicsObject(obj);
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, obj);
    }

    bool isAssistActive() {
        auto playLayer = PlayLayer::get();
        if (!playLayer || static_cast<GJBaseGameLayer*>(playLayer) != this) return false;
        if (!Mod::get()->getSettingValue<bool>("mod-enabled")) return false;
        if (m_isPracticeMode && !Mod::get()->getSettingValue<bool>("enable-in-practice")) return false;
        if (!m_player1 || m_player1->m_isDead) return false;
        return true;
    }

    void ensureInteractiveCache() {
        if (!m_objects) return;
        size_t count = static_cast<size_t>(m_objects->count());
        if (count == 0 || count == m_fields->cachedObjectCount) return;

        m_fields->interactiveObjects.clear();
        for (auto* obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            if (isGhostInteractivePhysicsObject(obj)) {
                m_fields->interactiveObjects.push_back(obj);
            }
        }
        std::sort(
            m_fields->interactiveObjects.begin(),
            m_fields->interactiveObjects.end(),
            [](GameObject* a, GameObject* b) {
                return a->getPositionX() < b->getPositionX();
            }
        );
        m_fields->cachedObjectCount = count;
    }

    bool ensureGhosts() {
        if (m_fields->prefixGhost && m_fields->probeGhost) {
            g_ghostPrefix = m_fields->prefixGhost;
            g_ghostProbe = m_fields->probeGhost;
            return true;
        }
        if (!m_player1 || !m_objectLayer || m_fields->elapsedTime < 0.05) return false;

        g_simulatingGhost = true;

        if (!m_fields->ghostLayer) {
            auto layer = CCLayer::create();
            layer->setID("frame-assist-ghost-layer"_spr);
            layer->setVisible(false);
            m_objectLayer->addChild(layer, -100);
            m_fields->ghostLayer = layer;
        }

        if (!m_fields->prefixGhost) {
            auto g1 = PlayerObject::create(1, 1, this, m_fields->ghostLayer, false);
            if (g1) {
                g1->setVisible(false);
                g1->setOpacity(0);
                m_fields->ghostLayer->addChild(g1);
                m_fields->prefixGhost = g1;
            }
        }

        if (!m_fields->probeGhost) {
            auto g2 = PlayerObject::create(1, 1, this, m_fields->ghostLayer, false);
            if (g2) {
                g2->setVisible(false);
                g2->setOpacity(0);
                m_fields->ghostLayer->addChild(g2);
                m_fields->probeGhost = g2;
            }
        }

        g_ghostPrefix = m_fields->prefixGhost;
        g_ghostProbe = m_fields->probeGhost;
        g_simulatingGhost = false;

        return m_fields->prefixGhost != nullptr && m_fields->probeGhost != nullptr;
    }

    void resetAssistState() {
        m_fields->elapsedTime = 0.0;
        m_fields->currentFrame60 = 0;
        m_fields->actualHolding = false;
        m_fields->savedInputsCount = 0;
        m_fields->pendingQueue.clear();
        m_fields->lastDelayTicks = 0;
        m_fields->lastD0Survival = 0;
        m_fields->lastD0MovedX = 0;
        m_fields->lastReason = "LISTO";

        if (m_fields->prefixGhost && m_fields->probeGhost) {
            g_simulatingGhost = true;
            m_fields->prefixGhost->resetObject();
            m_fields->probeGhost->resetObject();
            m_fields->prefixGhost->setVisible(false);
            m_fields->probeGhost->setVisible(false);
            g_simulatingGhost = false;
        }
    }

    void snapshotNearbyInteractive(float minX, float maxX, std::vector<ActivationSnapshot>& out) {
        out.clear();
        auto it = std::lower_bound(
            m_fields->interactiveObjects.begin(),
            m_fields->interactiveObjects.end(),
            minX,
            [](GameObject* obj, float x) { return obj->getPositionX() < x; }
        );
        for (; it != m_fields->interactiveObjects.end(); ++it) {
            GameObject* obj = *it;
            if (obj->getPositionX() > maxX) break;
            out.push_back(captureObjectSnapshot(obj));
        }
    }

    void syncGhostFrom(PlayerObject* dst, PlayerObject* src) {
        dst->resetTouchedRings(true);
        dst->copyAttributes(src);
        copyFullPhysicsState(dst, src);

        CCPoint pos = src->m_position;
        if (pos.x == 0.f && pos.y == 0.f) {
            pos = src->getPosition();
        }
        dst->m_position = pos;
        dst->setPosition(pos);
        dirtyGhostRects(dst);
        dst->setVisible(false);
        dst->setOpacity(0);
    }

    // Avanza un PlayerObject fantasma exactamente 1 tick real de GD (1/240 s),
    // sincronizando su posicion y ensuciando su hitbox antes y despues de checkCollisions.
    bool stepGhostOneTick(PlayerObject* ghost) {
        g_ghostDied = false;

        CCPoint beforePos = ghost->m_position;
        CCPoint beforeNode = ghost->getPosition();

        ghost->update(TICK_DT_60);
        if (g_ghostDied || ghost->m_isDead) return false;

        // Sincronizar m_position <-> CCNode::position y marcar hitbox sucia para checkCollisions
        CCPoint afterPos = ghost->m_position;
        CCPoint afterNode = ghost->getPosition();
        if (afterPos.x != beforePos.x || afterPos.y != beforePos.y) {
            ghost->setPosition(afterPos);
        } else if (afterNode.x != beforeNode.x || afterNode.y != beforeNode.y) {
            ghost->m_position = afterNode;
        }
        dirtyGhostRects(ghost);

        this->checkCollisions(ghost, TICK_DT_60, false);
        if (g_ghostDied || ghost->m_isDead) return false;

        // Reflejar cualquier ajuste de aterrizaje o teletransporte aplicado por checkCollisions
        ghost->setPosition(ghost->m_position);
        dirtyGhostRects(ghost);
        return true;
    }

    // Simula una sonda desde `baseGhost` aplicando `actionDown` y corriendo exactamente `remainingTicks`
    ProbeOutcome runProbeFromBase(
        PlayerObject* baseGhost,
        bool actionDown,
        int remainingTicks,
        std::vector<ActivationSnapshot> const& snapshots
    ) {
        auto* probe = m_fields->probeGhost;
        ProbeOutcome out;
        out.survivedAll = true;
        out.effective = true;
        out.survivalTicks = remainingTicks;
        out.movedX = 0.f;

        syncGhostFrom(probe, baseGhost);
        float startX = probe->m_position.x;

        VehicleMode modeBefore = detectVehicle(probe);
        bool onGroundBefore = probe->m_isOnGround;
        bool upsideBefore = probe->m_isUpsideDown;
        double vyBefore = probe->m_yVelocity;

        if (actionDown) {
            // Sincronizar orbes en contacto justo antes de pulsar el boton
            g_ghostDied = false;
            dirtyGhostRects(probe);
            this->checkCollisions(probe, 0.0f, false);
            if (g_ghostDied || probe->m_isDead) {
                restoreSnapshots(snapshots);
                return {false, false, 0, 0.f};
            }

            probe->releaseButton(PlayerButton::Jump);
            probe->pushButton(PlayerButton::Jump);
        } else {
            probe->releaseButton(PlayerButton::Jump);
        }

        // En vehiculos de suelo en el aire, verificar si el clic activo un orbe o cambio estado
        if (actionDown && isGroundVehicle(modeBefore) && !onGroundBefore) {
            bool vyChanged = std::fabs(probe->m_yVelocity - vyBefore) > 1.0;
            bool gravChanged = (probe->m_isUpsideDown != upsideBefore);
            if (!vyChanged && !gravChanged) {
                out.effective = false;
            }
        }

        bool simulateQuickTapRelease = actionDown && isTapVehicle(modeBefore);

        for (int t = 1; t <= remainingTicks; ++t) {
            // Para vehiculos de toque, soltar 4 ticks despues para evitar saltos automaticos falsos
            if (simulateQuickTapRelease && t == 5) {
                probe->releaseButton(PlayerButton::Jump);
            }

            if (!stepGhostOneTick(probe)) {
                out.survivedAll = false;
                out.survivalTicks = t;
                out.movedX = probe->m_position.x - startX;
                restoreSnapshots(snapshots);
                return out;
            }

            // Detectar si un orbe o salto se disparo en los primeros 3 ticks
            if (actionDown && !out.effective && t <= 3) {
                if (std::fabs(probe->m_yVelocity - vyBefore) > 2.5 ||
                    probe->m_isUpsideDown != upsideBefore) {
                    out.effective = true;
                }
            }
        }

        out.movedX = probe->m_position.x - startX;
        restoreSnapshots(snapshots);
        return out;
    }

    int computeBaseLookaheadTicks(VehicleMode mode, int lookaheadFramesSetting) {
        int ticks = std::clamp(lookaheadFramesSetting, 8, 45) * TICKS_PER_FRAME60;
        if (mode == VehicleMode::Cube || mode == VehicleMode::Robot) {
            return std::max(ticks, 108); // ~27 frames a 60 FPS para cubrir un salto completo + aterrizaje
        }
        if (mode == VehicleMode::Wave || mode == VehicleMode::Ship) {
            return std::clamp(ticks, 60, 120);
        }
        return std::max(ticks, 84);
    }

    // Evaluacion rapida en vivo mientras una accion espera en la cola (solo 2 sondas: d=0 y d=2)
    bool evaluateLiveTrigger(
        bool actionDown,
        int remainingMarginTicks,
        int lookaheadFramesSetting,
        bool& outNowSafe,
        bool& outNextSafe
    ) {
        outNowSafe = false;
        outNextSafe = false;
        if (!ensureGhosts()) return false;
        ensureInteractiveCache();

        VehicleMode mode = detectVehicle(m_player1);
        int baseLookahead = computeBaseLookaheadTicks(mode, lookaheadFramesSetting);
        int commonHorizon = std::max(0, remainingMarginTicks) + baseLookahead;

        float px = m_player1->m_position.x;
        std::vector<ActivationSnapshot> snapshots;
        snapshotNearbyInteractive(px - 90.f, px + 950.f, snapshots);

        g_simulatingGhost = true;

        auto* prefix = m_fields->prefixGhost;
        syncGhostFrom(prefix, m_player1);
        if (actionDown) {
            prefix->releaseButton(PlayerButton::Jump);
        } else {
            prefix->pushButton(PlayerButton::Jump);
        }

        // Sonda 1: disparar YA (d = 0) sobre todo el horizonte comun
        ProbeOutcome nowOut = runProbeFromBase(prefix, actionDown, commonHorizon, snapshots);
        outNowSafe = nowOut.survivedAll && nowOut.effective;

        // Sonda 2: esperar 2 ticks mas (1 frame @ 120fps) sobre el mismo horizonte comun
        bool prefixAlive = true;
        for (int s = 0; s < 2; ++s) {
            if (!stepGhostOneTick(prefix)) {
                prefixAlive = false;
                break;
            }
        }
        if (prefixAlive) {
            std::vector<ActivationSnapshot> afterPrefixSnaps = snapshots;
            for (auto& snap : afterPrefixSnaps) updateObjectSnapshot(snap);
            ProbeOutcome nextOut = runProbeFromBase(
                prefix, actionDown, std::max(16, commonHorizon - 2), afterPrefixSnaps
            );
            outNextSafe = nextOut.survivedAll && nextOut.effective;
        } else {
            outNextSafe = false;
        }

        restoreSnapshots(snapshots);
        g_simulatingGhost = false;
        return true;
    }

    // Busqueda completa con horizonte COMUN (maxDelayTicks + baseLookahead) para que d=0
    // y d=maxDelay se comparen exactamente hasta el mismo punto futuro del nivel.
    DelayDecision findBestGhostDelay(
        bool actionDown,
        int maxDelayTicks,
        int lookaheadFramesSetting,
        bool isAlreadyBuffered
    ) {
        if (!ensureGhosts()) {
            return {0, true, 0, 0, "NO_GHOST"};
        }
        ensureInteractiveCache();

        VehicleMode mode = detectVehicle(m_player1);
        int baseLookahead = computeBaseLookaheadTicks(mode, lookaheadFramesSetting);
        int clampedMax = std::clamp(maxDelayTicks, 0, 120);
        int commonHorizon = clampedMax + baseLookahead;

        float px = m_player1->m_position.x;
        std::vector<ActivationSnapshot> snapshots;
        snapshotNearbyInteractive(px - 90.f, px + 1100.f, snapshots);

        auto startClock = std::chrono::steady_clock::now();
        g_simulatingGhost = true;

        auto* prefix = m_fields->prefixGhost;
        syncGhostFrom(prefix, m_player1);

        // Configurar estado de retencion del prefijo mientras espera
        if (actionDown) {
            prefix->releaseButton(PlayerButton::Jump);
        } else {
            prefix->pushButton(PlayerButton::Jump);
        }

        // 1. Evaluar d = 0 sobre todo el horizonte comun
        ProbeOutcome nowOut = runProbeFromBase(prefix, actionDown, commonHorizon, snapshots);
        int d0Surv = nowOut.survivalTicks;
        int d0Dx = static_cast<int>(std::round(nowOut.movedX));

        if (!isAlreadyBuffered && nowOut.survivedAll && nowOut.effective) {
            restoreSnapshots(snapshots);
            g_simulatingGhost = false;
            return {0, true, d0Surv, d0Dx, "SAFE0"};
        }

        struct CandResult {
            int delayTicks;
            ProbeOutcome out;
            int totalSurvivalTicks;
        };
        std::vector<CandResult> results;
        results.reserve(static_cast<size_t>(clampedMax / 4 + 4));
        results.push_back({0, nowOut, nowOut.survivalTicks});

        // Escaneo rapido cada 4 ticks (60 FPS -> maximo 16 sondas para margen de 16f)
        constexpr int STEP_TICKS = 4;
        std::vector<ActivationSnapshot> prefixStateSnaps = snapshots;

        for (int d = STEP_TICKS; d <= clampedMax; d += STEP_TICKS) {
            auto nowClock = std::chrono::steady_clock::now();
            double elapsedMs = std::chrono::duration<double, std::milli>(nowClock - startClock).count();
            if (elapsedMs >= SEARCH_BUDGET_MS) {
                break;
            }

            restoreSnapshots(prefixStateSnaps);

            bool prefixAlive = true;
            for (int s = 0; s < STEP_TICKS; ++s) {
                if (!stepGhostOneTick(prefix)) {
                    prefixAlive = false;
                    break;
                }
            }
            if (!prefixAlive) {
                break;
            }

            for (auto& snap : prefixStateSnaps) {
                updateObjectSnapshot(snap);
            }

            int remainingHorizon = std::max(baseLookahead, commonHorizon - d);
            ProbeOutcome probeOut = runProbeFromBase(prefix, actionDown, remainingHorizon, prefixStateSnaps);
            int totalSurv = probeOut.survivedAll ? commonHorizon : (d + probeOut.survivalTicks);
            results.push_back({d, probeOut, totalSurv});
        }

        // Restaurar SIEMPRE el estado original de todos los pads/orbes/portales del nivel real
        restoreSnapshots(snapshots);
        g_simulatingGhost = false;

        // Buscar la ventana contigua mas amplia de ticks seguros y efectivos
        int bestStartIdx = -1;
        int bestEndIdx = -1;
        int bestWinLen = 0;

        int n = static_cast<int>(results.size());
        int i = 0;
        while (i < n) {
            if (results[i].out.survivedAll && results[i].out.effective) {
                int start = i;
                while (i + 1 < n && results[i + 1].out.survivedAll && results[i + 1].out.effective) {
                    i++;
                }
                int end = i;
                int len = end - start + 1;
                if (len > bestWinLen) {
                    bestWinLen = len;
                    bestStartIdx = start;
                    bestEndIdx = end;
                }
            }
            i++;
        }

        if (bestStartIdx >= 0) {
            int chosenIdx = bestStartIdx;
            if (actionDown && isTapVehicle(mode)) {
                chosenIdx = (bestStartIdx + bestEndIdx) / 2;
            } else {
                chosenIdx = (bestEndIdx - bestStartIdx >= 2) ? (bestStartIdx + 1) : bestStartIdx;
            }

            int chosenDelay = results[chosenIdx].delayTicks;
            if (isAlreadyBuffered && nowOut.survivedAll && nowOut.effective) {
                bool nextUnsafe = (n > 1 && !results[1].out.survivedAll);
                if (chosenDelay == 0 || nextUnsafe) {
                    return {0, true, d0Surv, d0Dx, "GHOST_OK"};
                }
            }
            return {chosenDelay, chosenDelay == 0, d0Surv, d0Dx, "GHOST_WIN"};
        }

        // Respaldo 1: algun retraso sobrevive todo el horizonte comun
        for (int idx = 1; idx < n; ++idx) {
            if (results[idx].out.survivedAll) {
                return {results[idx].delayTicks, false, d0Surv, d0Dx, "ESQUIVA"};
            }
        }

        // Respaldo 2: hay otro obstaculo mas adelante; retrasar si gana >= 8 ticks reales de supervivencia
        int bestSurvDelay = 0;
        int bestSurvTotal = results[0].totalSurvivalTicks;
        bool bestSurvEff = results[0].out.effective;
        for (int idx = 1; idx < n; ++idx) {
            auto const& cand = results[idx];
            bool better = (cand.totalSurvivalTicks > bestSurvTotal) ||
                          (cand.totalSurvivalTicks == bestSurvTotal &&
                           cand.out.effective && !bestSurvEff);
            if (better) {
                bestSurvTotal = cand.totalSurvivalTicks;
                bestSurvDelay = cand.delayTicks;
                bestSurvEff = cand.out.effective;
            }
        }

        if (bestSurvDelay > 0 && bestSurvTotal >= results[0].totalSurvivalTicks + 8) {
            return {bestSurvDelay, false, d0Surv, d0Dx, "SUPERV"};
        }

        return {0, true, d0Surv, d0Dx, "SIN_ALT"};
    }

    void dispatchButton(bool down, int button, bool isPlayer1) {
        if (button == 1 || button == 0) {
            m_fields->actualHolding = down;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (m_fields->isDispatchingBuffered || g_simulatingGhost) {
            dispatchButton(down, button, isPlayer1);
            return;
        }

        if ((button != 1 && button != 0) || !isAssistActive()) {
            dispatchButton(down, button, isPlayer1);
            return;
        }

        bool assistPress = Mod::get()->getSettingValue<bool>("buffer-press");
        bool assistRelease = Mod::get()->getSettingValue<bool>("buffer-release");
        int marginFrames = static_cast<int>(Mod::get()->getSettingValue<int64_t>("margin-frames"));
        int lookaheadFrames = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead-frames"));
        int maxDelayTicks = marginFrames * TICKS_PER_FRAME60;
        double marginSec = static_cast<double>(maxDelayTicks) / PHYS_TPS;

        // Toque rapido en Android: encolar SOLTAR detras del CLIC pendiente para no cancelarlo
        if (!down && !m_fields->pendingQueue.empty() && m_fields->pendingQueue.front().down) {
            if (m_fields->pendingQueue.back().down) {
                PendingAction rel;
                rel.down = false;
                rel.button = button;
                rel.isPlayer1 = isPlayer1;
                rel.createdTime = m_fields->elapsedTime;
                rel.targetTime = m_fields->pendingQueue.front().targetTime + (4.0 / PHYS_TPS);
                rel.expireTime = m_fields->pendingQueue.front().expireTime + marginSec;
                rel.lastRescanTime = m_fields->elapsedTime;
                rel.targetFrame60 = static_cast<int>(std::round(rel.targetTime * REF_FPS));
                rel.quickTapRelease = isTapVehicle(detectVehicle(m_player1));
                m_fields->pendingQueue.push_back(rel);
            }
            return;
        }

        // Si habia un SOLTAR esperando y el jugador vuelve a tocar, cancelar el soltar pendiente
        if (down && !m_fields->pendingQueue.empty() && !m_fields->pendingQueue.front().down) {
            m_fields->pendingQueue.clear();
            if (m_fields->actualHolding) {
                return;
            }
        }

        if ((down && !assistPress) || (!down && !assistRelease)) {
            m_fields->pendingQueue.clear();
            dispatchButton(down, button, isPlayer1);
            return;
        }

        DelayDecision decision = findBestGhostDelay(down, maxDelayTicks, lookaheadFrames, false);
        m_fields->lastDelayTicks = decision.delayTicks;
        m_fields->lastD0Survival = decision.d0SurvivalTicks;
        m_fields->lastD0MovedX = decision.d0MovedX;
        m_fields->lastReason = decision.reason;

        if (decision.fireNow || decision.delayTicks <= 0) {
            m_fields->pendingQueue.clear();
            dispatchButton(down, button, isPlayer1);
            return;
        }

        m_fields->pendingQueue.clear();
        PendingAction act;
        act.down = down;
        act.button = button;
        act.isPlayer1 = isPlayer1;
        act.createdTime = m_fields->elapsedTime;
        act.targetTime = m_fields->elapsedTime + (static_cast<double>(decision.delayTicks) / PHYS_TPS);
        act.expireTime = m_fields->elapsedTime + marginSec;
        act.lastRescanTime = m_fields->elapsedTime;
        act.targetFrame60 = static_cast<int>(std::round(act.targetTime * REF_FPS));
        act.quickTapRelease = false;
        m_fields->pendingQueue.push_back(act);
    }

    void tickFrameAssist(float dt) {
        if (dt <= 0.f || dt > 0.25f) {
            m_fields->pendingQueue.clear();
            return;
        }
        ensureInteractiveCache();

        m_fields->elapsedTime += static_cast<double>(dt);
        m_fields->currentFrame60 = static_cast<int>(m_fields->elapsedTime * REF_FPS);

        if (m_fields->pendingQueue.empty()) return;

        if (!isAssistActive()) {
            m_fields->pendingQueue.clear();
            return;
        }

        auto& front = m_fields->pendingQueue.front();
        int lookaheadFrames = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead-frames"));

        bool shouldTrigger = false;
        bool wasSaved = false;

        if (front.quickTapRelease) {
            if (m_fields->elapsedTime >= front.targetTime) {
                shouldTrigger = true;
            }
        } else if (m_fields->elapsedTime >= front.expireTime) {
            shouldTrigger = true;
            m_fields->lastReason = "LIMITE";
        } else {
            double remainingSec = std::max(0.0, front.expireTime - m_fields->elapsedTime);
            int remainingTicks = static_cast<int>(std::ceil(remainingSec * PHYS_TPS));

            bool nowSafe = false;
            bool nextSafe = false;
            evaluateLiveTrigger(front.down, remainingTicks, lookaheadFrames, nowSafe, nextSafe);

            if (nowSafe && (m_fields->elapsedTime >= front.targetTime || !nextSafe)) {
                shouldTrigger = true;
                wasSaved = true;
                m_fields->lastDelayTicks = 0;
                m_fields->lastReason = "GHOST_OK";
            } else if (m_fields->elapsedTime >= front.targetTime ||
                       (m_fields->elapsedTime - front.lastRescanTime) >= (1.0 / REF_FPS)) {
                DelayDecision live = findBestGhostDelay(front.down, remainingTicks, lookaheadFrames, true);
                front.lastRescanTime = m_fields->elapsedTime;
                m_fields->lastDelayTicks = live.delayTicks;
                m_fields->lastD0Survival = live.d0SurvivalTicks;
                m_fields->lastD0MovedX = live.d0MovedX;
                m_fields->lastReason = live.reason;

                if (live.fireNow) {
                    shouldTrigger = true;
                    wasSaved = true;
                } else {
                    front.targetTime = m_fields->elapsedTime + (static_cast<double>(live.delayTicks) / PHYS_TPS);
                    front.targetFrame60 = static_cast<int>(std::round(front.targetTime * REF_FPS));
                }
            }
        }

        if (shouldTrigger) {
            PendingAction action = front;
            m_fields->pendingQueue.pop_front();
            if (wasSaved) {
                m_fields->savedInputsCount++;
            }

            if (!m_fields->pendingQueue.empty() && action.down && !m_fields->pendingQueue.front().down) {
                int marginFrames = static_cast<int>(Mod::get()->getSettingValue<int64_t>("margin-frames"));
                double marginSec = static_cast<double>(marginFrames * TICKS_PER_FRAME60) / PHYS_TPS;
                auto& nextRel = m_fields->pendingQueue.front();
                if (nextRel.quickTapRelease) {
                    nextRel.targetTime = m_fields->elapsedTime + (4.0 / PHYS_TPS);
                    nextRel.expireTime = nextRel.targetTime;
                    nextRel.targetFrame60 = static_cast<int>(std::round(nextRel.targetTime * REF_FPS));
                } else {
                    nextRel.createdTime = m_fields->elapsedTime;
                    nextRel.targetTime = m_fields->elapsedTime + (2.0 / PHYS_TPS);
                    nextRel.expireTime = m_fields->elapsedTime + marginSec;
                    nextRel.lastRescanTime = m_fields->elapsedTime;
                    nextRel.targetFrame60 = static_cast<int>(std::round(nextRel.targetTime * REF_FPS));
                }
            }

            m_fields->isDispatchingBuffered = true;
            dispatchButton(action.down, action.button, action.isPlayer1);
            m_fields->isDispatchingBuffered = false;
        }
    }

    int getCurrentFrame60() { return m_fields->currentFrame60; }
    int getSavedInputsCount() { return m_fields->savedInputsCount; }
    int getLastDelayTicks() { return m_fields->lastDelayTicks; }
    int getLastD0Survival() { return m_fields->lastD0Survival; }
    int getLastD0MovedX() { return m_fields->lastD0MovedX; }
    std::string getLastReason() { return m_fields->lastReason; }

    bool hasPendingAction() {
        return !m_fields->pendingQueue.empty();
    }

    PendingAction getFrontPendingAction() {
        if (m_fields->pendingQueue.empty()) return PendingAction{};
        return m_fields->pendingQueue.front();
    }
};

// ============================================================================
// 4. INTEGRACION CON EL NIVEL Y HUD (PlayLayer)
// ============================================================================
class $modify(FrameAssistPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* hudLabel = nullptr;
    };

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;

        auto base = static_cast<FrameAssistGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        base->resetAssistState();
        base->ensureInteractiveCache();

        applySafeModeIfNeeded();

        if (Mod::get()->getSettingValue<bool>("mod-enabled") &&
            Mod::get()->getSettingValue<bool>("show-hud")) {
            auto winSize = CCDirector::get()->getWinSize();
            auto hud = CCLabelBMFont::create("F:0 Cube S:0 GHOST", "bigFont.fnt");
            hud->setID("frame-assist-hud"_spr);
            hud->setAnchorPoint({0.f, 1.f});
            hud->setScale(0.25f);
            hud->setOpacity(195);
            hud->setPosition({8.f, winSize.height - 6.f});
            this->addChild(hud, 100);
            m_fields->hudLabel = hud;
        }

        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        // Interceptar la muerte del jugador fantasma sin afectar al jugador real
        if (g_simulatingGhost || isGhostPlayer(player)) {
            g_ghostDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto base = static_cast<FrameAssistGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        base->resetAssistState();
        base->ensureInteractiveCache();
        applySafeModeIfNeeded();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto base = static_cast<FrameAssistGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        base->tickFrameAssist(dt);

        if (m_fields->hudLabel && m_player1) {
            auto mode = detectVehicle(m_player1);
            bool safeMode = Mod::get()->getSettingValue<bool>("safe-mode");

            std::string stateStr;
            if (base->hasPendingAction()) {
                auto pending = base->getFrontPendingAction();
                stateStr = fmt::format(
                    "ESP {}->F:{}",
                    pending.down ? "CLIC" : "SOLTAR",
                    pending.targetFrame60
                );
                m_fields->hudLabel->setColor({255, 220, 90});
            } else {
                stateStr = fmt::format(
                    "dx:{} s0:{} T:{}({})",
                    base->getLastD0MovedX(),
                    base->getLastD0Survival(),
                    base->getLastDelayTicks(),
                    base->getLastReason()
                );
                m_fields->hudLabel->setColor({150, 255, 160});
            }

            m_fields->hudLabel->setString(fmt::format(
                "F:{} {} S:{} {}{}",
                base->getCurrentFrame60(),
                vehicleName(mode),
                base->getSavedInputsCount(),
                stateStr,
                safeMode ? " [S]" : ""
            ).c_str());
        }
    }

    void applySafeModeIfNeeded() {
        if (Mod::get()->getSettingValue<bool>("mod-enabled") &&
            Mod::get()->getSettingValue<bool>("safe-mode")) {
            m_isTestMode = true;
        }
    }
};
