#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
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

inline bool getShowTrajectorySetting() {
    return Mod::get()->getSavedValue<bool>("show-trajectory", false);
}

inline void setShowTrajectorySetting(bool val) {
    Mod::get()->setSavedValue<bool>("show-trajectory", val);
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
};

struct DelayDecision {
    int delayTicks = 0;
    bool fireNow = true;
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
bool isGhostInteractivePhysicsObject(GameObject* obj) {
    if (!obj) return false;
    auto type = obj->m_objectType;
    int rawType = static_cast<int>(type);

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
// 1. VENTANA DE CONFIGURACION (Se abre desde la tuerquita en PauseLayer)
// ============================================================================
class ModConfigPopup : public geode::Popup {
protected:
    std::function<void()> m_onSettingsChanged;

    CCMenuItemToggler* m_enabledToggle = nullptr;
    CCMenuItemToggler* m_safeModeToggle = nullptr;
    CCMenuItemToggler* m_pressToggle = nullptr;
    CCMenuItemToggler* m_releaseToggle = nullptr;
    CCMenuItemToggler* m_hudToggle = nullptr;
    CCMenuItemToggler* m_trajToggle = nullptr;
    CCMenuItemToggler* m_practiceToggle = nullptr;

    Slider* m_marginSlider = nullptr;
    Slider* m_lookaheadSlider = nullptr;

    CCLabelBMFont* m_marginValLabel = nullptr;
    CCLabelBMFont* m_lookaheadValLabel = nullptr;

    static constexpr int MIN_MARGIN = 1;
    static constexpr int MAX_MARGIN = 30;
    static constexpr int MIN_LOOKAHEAD = 8;
    static constexpr int MAX_LOOKAHEAD = 45;

    bool init(std::function<void()> onSettingsChanged) {
        if (!Popup::init(420.f, 286.f)) {
            return false;
        }

        m_onSettingsChanged = std::move(onSettingsChanged);
        this->setTitle("Frame Assist - Configuracion");

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(menu);

        // --- Columna izquierda de interruptores ---
        m_enabledToggle = addCompactToggle(
            menu, "Activar Mod", "mod-enabled", 22.f, 188.f, 228.f,
            menu_selector(ModConfigPopup::onToggleEnabled)
        );
        m_pressToggle = addCompactToggle(
            menu, "Asistir Clic", "buffer-press", 22.f, 188.f, 200.f,
            menu_selector(ModConfigPopup::onTogglePress)
        );
        m_hudToggle = addCompactToggle(
            menu, "Mostrar HUD", "show-hud", 22.f, 188.f, 172.f,
            menu_selector(ModConfigPopup::onToggleHud)
        );
        m_practiceToggle = addCompactToggle(
            menu, "Modo Practica", "enable-in-practice", 22.f, 188.f, 144.f,
            menu_selector(ModConfigPopup::onTogglePractice)
        );

        // --- Columna derecha de interruptores ---
        m_safeModeToggle = addCompactToggle(
            menu, "Modo Seguro", "safe-mode", 216.f, 390.f, 228.f,
            menu_selector(ModConfigPopup::onToggleSafeMode)
        );
        m_releaseToggle = addCompactToggle(
            menu, "Asistir Soltar", "buffer-release", 216.f, 390.f, 200.f,
            menu_selector(ModConfigPopup::onToggleRelease)
        );
        m_trajToggle = addSavedToggle(
            menu, "Ver Recorrido", getShowTrajectorySetting(), 216.f, 390.f, 172.f,
            menu_selector(ModConfigPopup::onToggleTrajectory)
        );

        // --- Sliders principales ---
        int margin = static_cast<int>(Mod::get()->getSettingValue<int64_t>("margin-frames"));
        addSliderRow(
            "Margen Max",
            static_cast<float>(margin),
            static_cast<float>(MIN_MARGIN),
            static_cast<float>(MAX_MARGIN),
            104.f,
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
            70.f,
            fmt::format("{}f", lookahead),
            menu_selector(ModConfigPopup::onLookaheadChanged),
            m_lookaheadSlider,
            m_lookaheadValLabel
        );

        // --- Boton Restablecer ---
        auto resetSpr = ButtonSprite::create("Restablecer", "goldFont.fnt", "GJ_button_06.png", 0.75f);
        resetSpr->setScale(0.62f);
        auto resetBtn = CCMenuItemSpriteExtra::create(
            resetSpr, this, menu_selector(ModConfigPopup::onResetDefaults)
        );
        resetBtn->setPosition({m_size.width / 2.f, 26.f});
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
        return addSavedToggle(
            menu, labelText, Mod::get()->getSettingValue<bool>(settingKey),
            labelX, toggleX, posY, callback
        );
    }

    CCMenuItemToggler* addSavedToggle(
        CCMenu* menu,
        const char* labelText,
        bool initialState,
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

        auto toggler = CCMenuItemToggler::createWithStandardSprites(this, callback, 0.64f);
        toggler->setPosition({toggleX, posY});
        toggler->toggle(initialState);
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
        label->setScale(0.40f);
        label->setPosition({22.f, posY});
        m_mainLayer->addChild(label);

        outSlider = Slider::create(this, callback, 0.72f);
        outSlider->setPosition({250.f, posY});
        float norm = std::clamp((currentValue - minVal) / (maxVal - minVal), 0.f, 1.f);
        outSlider->setValue(norm);
        outSlider->updateBar();
        m_mainLayer->addChild(outSlider);

        outValueLabel = CCLabelBMFont::create(initialText.c_str(), "goldFont.fnt");
        outValueLabel->setAnchorPoint({1.f, 0.5f});
        outValueLabel->setScale(0.48f);
        outValueLabel->setPosition({398.f, posY});
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

    void onToggleTrajectory(CCObject* sender) {
        auto toggler = static_cast<CCMenuItemToggler*>(sender);
        setShowTrajectorySetting(!toggler->isToggled());
        notifyChange();
    }

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

    void onResetDefaults(CCObject*) {
        Mod::get()->setSettingValue<bool>("mod-enabled", true);
        Mod::get()->setSettingValue<bool>("safe-mode", true);
        Mod::get()->setSettingValue<bool>("buffer-press", true);
        Mod::get()->setSettingValue<bool>("buffer-release", true);
        Mod::get()->setSettingValue<bool>("show-hud", true);
        Mod::get()->setSettingValue<bool>("enable-in-practice", true);
        Mod::get()->setSettingValue<int64_t>("margin-frames", 16);
        Mod::get()->setSettingValue<int64_t>("lookahead-frames", 24);
        setShowTrajectorySetting(false);

        m_enabledToggle->toggle(true);
        m_safeModeToggle->toggle(true);
        m_pressToggle->toggle(true);
        m_releaseToggle->toggle(true);
        m_hudToggle->toggle(true);
        m_practiceToggle->toggle(true);
        m_trajToggle->toggle(false);

        m_marginSlider->setValue(static_cast<float>(16 - MIN_MARGIN) / (MAX_MARGIN - MIN_MARGIN));
        m_marginSlider->updateBar();
        m_marginValLabel->setString("16f");

        m_lookaheadSlider->setValue(static_cast<float>(24 - MIN_LOOKAHEAD) / (MAX_LOOKAHEAD - MIN_LOOKAHEAD));
        m_lookaheadSlider->updateBar();
        m_lookaheadValLabel->setString("24f");

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
// 2. MOTOR FANTASMA CON FISICA REAL DEL JUEGO (GJBaseGameLayer)
// Activacion ESTRICTA solo dentro de [1 .. marginFrames] (ej. 16 frames hacia abajo)
// + renderizado de trayectoria en tiempo real con CCDrawNode.
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
        std::string lastReason = "LISTO";

        ~Fields() {
            if (g_ghostPrefix == prefixGhost) g_ghostPrefix = nullptr;
            if (g_ghostProbe == probeGhost) g_ghostProbe = nullptr;
            g_simulatingGhost = false;
            g_ghostDied = false;
        }
    };

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

    void clearPendingQueue() {
        m_fields->pendingQueue.clear();
    }

    void resetAssistState() {
        m_fields->elapsedTime = 0.0;
        m_fields->currentFrame60 = 0;
        m_fields->actualHolding = false;
        m_fields->savedInputsCount = 0;
        m_fields->pendingQueue.clear();
        m_fields->lastDelayTicks = 0;
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

    // Avanza un PlayerObject fantasma exactamente 1 tick real de GD (1/240 s)
    bool stepGhostOneTick(PlayerObject* ghost) {
        g_ghostDied = false;

        CCPoint beforePos = ghost->m_position;
        CCPoint beforeNode = ghost->getPosition();

        ghost->update(TICK_DT_60);
        if (g_ghostDied || ghost->m_isDead) return false;

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

        ghost->setPosition(ghost->m_position);
        dirtyGhostRects(ghost);
        return true;
    }

    // Simula la accion `actionDown` desde `baseGhost` durante `lookaheadTicks`.
    // En vehiculos de suelo (Cube/Robot/Ball/Spider), si el salto aterriza de forma segura
    // y sobrevive 16 ticks (4 frames @ 60 FPS) en el suelo, el salto concluyo con exito
    // y no culpa falsamente a obstaculos lejanos (> 16 frames despues).
    ProbeOutcome runProbeFromBase(
        PlayerObject* baseGhost,
        bool actionDown,
        int lookaheadTicks,
        std::vector<ActivationSnapshot> const& snapshots
    ) {
        auto* probe = m_fields->probeGhost;
        ProbeOutcome out;
        out.survivedAll = true;
        out.effective = true;
        out.survivalTicks = lookaheadTicks;

        syncGhostFrom(probe, baseGhost);

        VehicleMode modeBefore = detectVehicle(probe);
        bool onGroundBefore = probe->m_isOnGround;
        bool upsideBefore = probe->m_isUpsideDown;
        double vyBefore = probe->m_yVelocity;

        if (actionDown) {
            g_ghostDied = false;
            dirtyGhostRects(probe);
            this->checkCollisions(probe, 0.0f, false);
            if (g_ghostDied || probe->m_isDead) {
                restoreSnapshots(snapshots);
                return {false, false, 0};
            }

            probe->releaseButton(PlayerButton::Jump);
            probe->pushButton(PlayerButton::Jump);
        } else {
            probe->releaseButton(PlayerButton::Jump);
        }

        if (actionDown && isGroundVehicle(modeBefore) && !onGroundBefore) {
            bool vyChanged = std::fabs(probe->m_yVelocity - vyBefore) > 1.0;
            bool gravChanged = (probe->m_isUpsideDown != upsideBefore);
            if (!vyChanged && !gravChanged) {
                out.effective = false;
            }
        }

        bool simulateQuickTapRelease = actionDown && isTapVehicle(modeBefore);
        bool checkLandingComplete = actionDown && isGroundVehicle(modeBefore);
        bool leftGround = !probe->m_isOnGround;
        int safeLandTicks = 0;

        for (int t = 1; t <= lookaheadTicks; ++t) {
            if (simulateQuickTapRelease && t == 5) {
                probe->releaseButton(PlayerButton::Jump);
            }

            if (!stepGhostOneTick(probe)) {
                out.survivedAll = false;
                out.survivalTicks = t;
                restoreSnapshots(snapshots);
                return out;
            }

            if (actionDown && !out.effective && t <= 3) {
                if (std::fabs(probe->m_yVelocity - vyBefore) > 2.5 ||
                    probe->m_isUpsideDown != upsideBefore) {
                    out.effective = true;
                }
            }

            // Salida temprana limpia al aterrizar tras un salto de suelo (solo si cambio de vehiculo no paso)
            if (checkLandingComplete && detectVehicle(probe) == modeBefore) {
                if (!probe->m_isOnGround) {
                    leftGround = true;
                    safeLandTicks = 0;
                } else if (leftGround && t >= 12) {
                    safeLandTicks++;
                    // 16 ticks = 4 frames @ 60 FPS seguros en suelo tras aterrizar -> salto completado
                    if (safeLandTicks >= 16) {
                        out.survivedAll = true;
                        out.survivalTicks = lookaheadTicks;
                        restoreSnapshots(snapshots);
                        return out;
                    }
                }
            }
        }

        restoreSnapshots(snapshots);
        return out;
    }

    int computeLookaheadTicks(VehicleMode mode, int lookaheadFramesSetting) {
        int ticks = std::clamp(lookaheadFramesSetting, 8, 45) * TICKS_PER_FRAME60;
        if (mode == VehicleMode::Cube || mode == VehicleMode::Robot) {
            // Cubre 1 salto completo (~100 ticks) + margen de aterrizaje (~16 ticks)
            return std::max(ticks, 116);
        }
        if (mode == VehicleMode::Wave || mode == VehicleMode::Ship) {
            return std::clamp(ticks, 56, 112);
        }
        return std::max(ticks, 84);
    }

    // Evaluacion rapida en vivo mientras una accion espera en la cola (solo 2 sondas: d=0 y d=2)
    bool evaluateLiveTrigger(
        bool actionDown,
        int lookaheadFramesSetting,
        bool& outNowSafe,
        bool& outNextSafe
    ) {
        outNowSafe = false;
        outNextSafe = false;
        if (!ensureGhosts()) return false;
        ensureInteractiveCache();

        VehicleMode mode = detectVehicle(m_player1);
        int lookaheadTicks = computeLookaheadTicks(mode, lookaheadFramesSetting);

        float px = m_player1->m_position.x;
        std::vector<ActivationSnapshot> snapshots;
        snapshotNearbyInteractive(px - 90.f, px + 850.f, snapshots);

        g_simulatingGhost = true;

        auto* prefix = m_fields->prefixGhost;
        syncGhostFrom(prefix, m_player1);
        if (actionDown) {
            prefix->releaseButton(PlayerButton::Jump);
        } else {
            prefix->pushButton(PlayerButton::Jump);
        }

        ProbeOutcome nowOut = runProbeFromBase(prefix, actionDown, lookaheadTicks, snapshots);
        outNowSafe = nowOut.survivedAll && nowOut.effective;

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
            ProbeOutcome nextOut = runProbeFromBase(prefix, actionDown, lookaheadTicks, afterPrefixSnaps);
            outNextSafe = nextOut.survivedAll && nextOut.effective;
        } else {
            outNextSafe = false;
        }

        restoreSnapshots(snapshots);
        g_simulatingGhost = false;
        return true;
    }

    // Busqueda ESTRICTA dentro de [0 .. maxDelayTicks] (ej. 16 frames hacia abajo).
    // Si d = 0 ya es seguro -> ejecuta al instante.
    // Si NO existe una ventana realmente segura y efectiva dentro de [1 .. maxDelayTicks]
    // -> NO retiene el input (evita activarse antes de los 16 frames).
    DelayDecision findBestGhostDelay(
        bool actionDown,
        int maxDelayTicks,
        int lookaheadFramesSetting,
        bool isAlreadyBuffered
    ) {
        if (!ensureGhosts()) {
            return {0, true, "NO_GHOST"};
        }
        ensureInteractiveCache();

        VehicleMode mode = detectVehicle(m_player1);
        int lookaheadTicks = computeLookaheadTicks(mode, lookaheadFramesSetting);
        int clampedMax = std::clamp(maxDelayTicks, 0, 120);

        float px = m_player1->m_position.x;
        std::vector<ActivationSnapshot> snapshots;
        snapshotNearbyInteractive(px - 90.f, px + 950.f, snapshots);

        auto startClock = std::chrono::steady_clock::now();
        g_simulatingGhost = true;

        auto* prefix = m_fields->prefixGhost;
        syncGhostFrom(prefix, m_player1);

        if (actionDown) {
            prefix->releaseButton(PlayerButton::Jump);
        } else {
            prefix->pushButton(PlayerButton::Jump);
        }

        // 1. Evaluar d = 0 (ejecutar ahora mismo)
        ProbeOutcome nowOut = runProbeFromBase(prefix, actionDown, lookaheadTicks, snapshots);
        if (!isAlreadyBuffered && nowOut.survivedAll && nowOut.effective) {
            restoreSnapshots(snapshots);
            g_simulatingGhost = false;
            return {0, true, "SAFE0"};
        }

        struct CandResult {
            int delayTicks;
            ProbeOutcome out;
        };
        std::vector<CandResult> results;
        results.reserve(static_cast<size_t>(clampedMax / 4 + 4));
        results.push_back({0, nowOut});

        // Escaneo en pasos de 4 ticks (1 frame @ 60 FPS) estrictamente hasta clampedMax (<= 16f)
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

            ProbeOutcome probeOut = runProbeFromBase(prefix, actionDown, lookaheadTicks, prefixStateSnaps);
            results.push_back({d, probeOut});
        }

        restoreSnapshots(snapshots);
        g_simulatingGhost = false;

        // Buscar la ventana contigua mas amplia de frames realmente SEGUROS Y EFECTIVOS dentro de [0 .. clampedMax]
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

            int chosenDelay = std::min(results[chosenIdx].delayTicks, clampedMax);
            if (isAlreadyBuffered && nowOut.survivedAll && nowOut.effective) {
                bool nextUnsafe = (n > 1 && !results[1].out.survivedAll);
                if (chosenDelay == 0 || nextUnsafe) {
                    return {0, true, "GHOST_OK"};
                }
            }
            return {chosenDelay, chosenDelay == 0, "GHOST_WIN"};
        }

        // ESTRICTO: si no existe un frame seguro dentro del margen (ej. <= 16 frames),
        // NO retener el clic ni el soltado; ejecutar inmediatamente.
        return {0, true, "FUERA_MARGEN"};
    }

    // Dibuja en vivo el recorrido futuro exacto del jugador en coordenadas del nivel
    void renderPlayerTrajectory(CCDrawNode* drawNode) {
        if (!drawNode) return;

        bool enabled = Mod::get()->getSettingValue<bool>("mod-enabled");
        bool showTraj = getShowTrajectorySetting();
        if (!enabled || !showTraj || !isAssistActive() || !ensureGhosts()) {
            drawNode->clear();
            drawNode->setVisible(false);
            return;
        }

        ensureInteractiveCache();
        drawNode->setVisible(true);
        drawNode->clear();

        int lookaheadFrames = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead-frames"));
        int trajTicks = std::clamp(lookaheadFrames * 3, 48, 96);

        float px = m_player1->m_position.x;
        std::vector<ActivationSnapshot> snapshots;
        snapshotNearbyInteractive(px - 90.f, px + 850.f, snapshots);

        g_simulatingGhost = true;

        auto* probe = m_fields->probeGhost;
        syncGhostFrom(probe, m_player1);

        if (m_fields->actualHolding) {
            probe->pushButton(PlayerButton::Jump);
        } else {
            probe->releaseButton(PlayerButton::Jump);
        }

        bool hasPending = !m_fields->pendingQueue.empty();
        PendingAction frontPending;
        int ticksToPending = -1;
        bool pendingTapRelease = false;
        if (hasPending) {
            frontPending = m_fields->pendingQueue.front();
            double dtToAct = std::max(0.0, frontPending.targetTime - m_fields->elapsedTime);
            ticksToPending = std::clamp(static_cast<int>(std::round(dtToAct * PHYS_TPS)), 0, trajTicks);
            pendingTapRelease = frontPending.down && isTapVehicle(detectVehicle(probe));
        }

        std::vector<CCPoint> pts;
        pts.reserve(static_cast<size_t>(trajTicks / 2 + 4));
        pts.push_back(probe->m_position);

        bool died = false;
        CCPoint deathPos = probe->m_position;
        bool hasActionPos = false;
        CCPoint actionPos = probe->m_position;

        for (int t = 0; t < trajTicks; ++t) {
            if (hasPending && t == ticksToPending) {
                actionPos = probe->m_position;
                hasActionPos = true;
                if (frontPending.down) {
                    dirtyGhostRects(probe);
                    this->checkCollisions(probe, 0.0f, false);
                    probe->releaseButton(PlayerButton::Jump);
                    probe->pushButton(PlayerButton::Jump);
                } else {
                    probe->releaseButton(PlayerButton::Jump);
                }
            }
            if (hasPending && pendingTapRelease && t == ticksToPending + 4) {
                probe->releaseButton(PlayerButton::Jump);
            }

            if (!stepGhostOneTick(probe)) {
                died = true;
                deathPos = probe->m_position;
                pts.push_back(deathPos);
                break;
            }

            if ((t % 2) == 1 || t == trajTicks - 1) {
                pts.push_back(probe->m_position);
            }
        }

        restoreSnapshots(snapshots);
        g_simulatingGhost = false;

        if (pts.size() < 2) return;

        ccColor4F lineColor = hasPending
            ? ccc4f(1.0f, 0.88f, 0.18f, 0.92f)   // Amarillo: trayectoria asistida en espera
            : (died
                ? ccc4f(1.0f, 0.25f, 0.25f, 0.92f) // Rojo: trayectoria actual va a chocar
                : ccc4f(0.22f, 1.0f, 0.55f, 0.88f)); // Verde: trayectoria actual segura

        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            drawNode->drawSegment(pts[i], pts[i + 1], 1.6f, lineColor);
        }

        if (hasActionPos) {
            drawNode->drawDot(actionPos, 4.0f, ccc4f(0.2f, 0.95f, 1.0f, 0.95f));
        }
        if (died) {
            drawNode->drawDot(deathPos, 4.5f, ccc4f(1.0f, 0.15f, 0.15f, 0.95f));
        }
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
        m_fields->lastReason = decision.reason;

        if (decision.fireNow || decision.delayTicks <= 0 || decision.delayTicks > maxDelayTicks) {
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
        act.expireTime = m_fields->elapsedTime + marginSec;
        act.targetTime = std::min(
            m_fields->elapsedTime + (static_cast<double>(decision.delayTicks) / PHYS_TPS),
            act.expireTime
        );
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
            bool nowSafe = false;
            bool nextSafe = false;
            evaluateLiveTrigger(front.down, lookaheadFrames, nowSafe, nextSafe);

            if (nowSafe && (m_fields->elapsedTime >= front.targetTime || !nextSafe)) {
                shouldTrigger = true;
                wasSaved = true;
                m_fields->lastDelayTicks = 0;
                m_fields->lastReason = "GHOST_OK";
            } else if (m_fields->elapsedTime >= front.targetTime ||
                       (m_fields->elapsedTime - front.lastRescanTime) >= (1.0 / REF_FPS)) {
                double remainingSec = std::max(0.0, front.expireTime - m_fields->elapsedTime);
                int remainingTicks = static_cast<int>(std::floor(remainingSec * PHYS_TPS));
                DelayDecision live = findBestGhostDelay(front.down, remainingTicks, lookaheadFrames, true);
                front.lastRescanTime = m_fields->elapsedTime;
                m_fields->lastDelayTicks = live.delayTicks;
                m_fields->lastReason = live.reason;

                if (live.fireNow) {
                    shouldTrigger = true;
                    wasSaved = (std::string(live.reason) == "GHOST_OK");
                } else {
                    front.targetTime = std::min(
                        m_fields->elapsedTime + (static_cast<double>(live.delayTicks) / PHYS_TPS),
                        front.expireTime
                    );
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
                    nextRel.expireTime = m_fields->elapsedTime + marginSec;
                    nextRel.targetTime = std::min(m_fields->elapsedTime + (2.0 / PHYS_TPS), nextRel.expireTime);
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
// 3. INTEGRACION CON EL NIVEL, HUD Y RECORRIDO (PlayLayer)
// ============================================================================
class $modify(FrameAssistPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* hudLabel = nullptr;
        CCDrawNode* trajDraw = nullptr;
    };

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;

        auto base = static_cast<FrameAssistGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        base->resetAssistState();
        base->ensureInteractiveCache();

        auto winSize = CCDirector::get()->getWinSize();
        auto hud = CCLabelBMFont::create("F:0 Cube S:0 LISTO", "bigFont.fnt");
        hud->setID("frame-assist-hud"_spr);
        hud->setAnchorPoint({0.f, 1.f});
        hud->setScale(0.26f);
        hud->setOpacity(195);
        hud->setPosition({8.f, winSize.height - 6.f});
        this->addChild(hud, 100);
        m_fields->hudLabel = hud;

        ensureTrajectoryNode();
        refreshVisualSettings();

        return true;
    }

    void ensureTrajectoryNode() {
        if (m_fields->trajDraw || !m_objectLayer) return;
        auto draw = CCDrawNode::create();
        draw->setID("frame-assist-trajectory"_spr);
        m_objectLayer->addChild(draw, 1500);
        m_fields->trajDraw = draw;
    }

    void refreshVisualSettings() {
        bool enabled = Mod::get()->getSettingValue<bool>("mod-enabled");
        bool showHud = Mod::get()->getSettingValue<bool>("show-hud");
        bool showTraj = getShowTrajectorySetting();
        bool safeMode = Mod::get()->getSettingValue<bool>("safe-mode");

        if (m_fields->hudLabel) {
            m_fields->hudLabel->setVisible(enabled && showHud);
        }

        ensureTrajectoryNode();
        if (m_fields->trajDraw) {
            if (!enabled || !showTraj) {
                m_fields->trajDraw->clear();
                m_fields->trajDraw->setVisible(false);
            } else {
                m_fields->trajDraw->setVisible(true);
            }
        }

        if (!enabled) {
            auto base = static_cast<FrameAssistGameLayer*>(static_cast<GJBaseGameLayer*>(this));
            base->clearPendingQueue();
        }

        if (enabled && safeMode) {
            m_isTestMode = true;
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
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
        if (m_fields->trajDraw) {
            m_fields->trajDraw->clear();
        }
        refreshVisualSettings();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto base = static_cast<FrameAssistGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        base->tickFrameAssist(dt);

        bool enabled = Mod::get()->getSettingValue<bool>("mod-enabled");
        bool showHud = Mod::get()->getSettingValue<bool>("show-hud");
        bool showTraj = getShowTrajectorySetting();

        ensureTrajectoryNode();
        if (m_fields->trajDraw) {
            if (enabled && showTraj) {
                base->renderPlayerTrajectory(m_fields->trajDraw);
            } else if (m_fields->trajDraw->isVisible()) {
                m_fields->trajDraw->clear();
                m_fields->trajDraw->setVisible(false);
            }
        }

        if (m_fields->hudLabel) {
            bool hudVisible = enabled && showHud;
            m_fields->hudLabel->setVisible(hudVisible);

            if (hudVisible && m_player1) {
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
                        "T:{}({})",
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
    }
};

// ============================================================================
// 4. TUERQUITA DE CONFIGURACION EN EL MENU DE PAUSA DEL NIVEL (PauseLayer)
// ============================================================================
class $modify(FrameAssistPauseLayer, PauseLayer) {
    struct Fields {
        CCSprite* gearSprite = nullptr;
        CCLabelBMFont* tagLabel = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::get()->getWinSize();

        auto menu = CCMenu::create();
        menu->setID("frame-assist-pause-menu"_spr);
        menu->setPosition({0.f, 0.f});
        this->addChild(menu, 100);

        auto btnNode = CCNode::create();
        btnNode->setContentSize({44.f, 48.f});

        auto gear = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        gear->setScale(0.64f);
        gear->setPosition({22.f, 29.f});
        btnNode->addChild(gear);
        m_fields->gearSprite = gear;

        auto tag = CCLabelBMFont::create("Assist", "goldFont.fnt");
        tag->setScale(0.34f);
        tag->setPosition({22.f, 6.f});
        btnNode->addChild(tag);
        m_fields->tagLabel = tag;

        auto gearBtn = CCMenuItemSpriteExtra::create(
            btnNode, this, menu_selector(FrameAssistPauseLayer::onOpenModConfig)
        );
        gearBtn->setID("frame-assist-config-btn"_spr);
        gearBtn->setPosition({34.f, winSize.height - 34.f});
        menu->addChild(gearBtn);

        refreshGearVisual();
    }

    void refreshGearVisual() {
        bool enabled = Mod::get()->getSettingValue<bool>("mod-enabled");
        if (m_fields->gearSprite) {
            m_fields->gearSprite->setColor(enabled ? ccColor3B{135, 255, 215} : ccColor3B{255, 130, 130});
        }
        if (m_fields->tagLabel) {
            m_fields->tagLabel->setString(enabled ? "Assist" : "OFF");
        }
    }

    void onOpenModConfig(CCObject*) {
        auto popup = ModConfigPopup::create([this]() {
            this->refreshGearVisual();
            if (auto* pl = PlayLayer::get()) {
                auto assistPL = static_cast<FrameAssistPlayLayer*>(pl);
                assistPL->refreshVisualSettings();
            }
        });
        if (popup) popup->show();
    }
};
