#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr double REF_FPS = 60.0;

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

struct SimState {
    float x = 0.f;
    float y = 0.f;
    float vxPerFrame = 5.193f; // px por frame a 60 FPS (~311.58 px/s a 1x)
    float vyPerFrame = 0.f;
    float halfW = 13.5f;
    float halfH = 13.5f;
    float floorY = 0.f;
    bool upsideDown = false;
    bool onGround = false;
    bool holding = false;
    bool mini = false;
    VehicleMode mode = VehicleMode::Cube;
};

struct PendingAction {
    bool down = false;
    int button = 1;
    bool isPlayer1 = true;
    int createdFrame = 0;
    int targetFrame = 0;
    int expireFrame = 0;
    bool quickTapRelease = false;
};

struct SimOutcome {
    bool survivedAll = true;
    bool actionEffective = true;
    int survivalFrames = 0;
};

struct DelayDecision {
    int delay = 0;
    bool delayZeroSafe = true;
    const char* reason = "OK";
};

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

bool isHazardObject(GameObject* obj) {
    if (!obj) return false;
    auto type = obj->m_objectType;
    return type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard;
}

bool isOrbObject(GameObject* obj) {
    if (!obj) return false;
    switch (obj->m_objectType) {
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::DropRing:
        case GameObjectType::SpiderOrb:
            return true;
        default:
            return false;
    }
}

bool isSolidObject(GameObject* obj) {
    if (!obj) return false;
    return obj->m_objectType == GameObjectType::Solid;
}

CCRect safeObjectRect(GameObject* obj, bool asHazard) {
    CCRect r = obj->getObjectRect();
    if (r.size.width <= 0.5f || r.size.height <= 0.5f) {
        auto pos = obj->getPosition();
        auto sz = obj->getScaledContentSize();
        float w = sz.width > 1.f ? sz.width : 30.f;
        float h = sz.height > 1.f ? sz.height : 30.f;
        r = CCRect(pos.x - w * 0.5f, pos.y - h * 0.5f, w, h);
    }

    if (asHazard) {
        // Solo recortar si vino como tile completo de 30x30; no encoger hitboxes ya exactas
        if (r.size.width > 24.f && r.size.height > 24.f) {
            float padX = r.size.width * 0.12f;
            float padY = r.size.height * 0.12f;
            r = CCRect(
                r.origin.x + padX,
                r.origin.y + padY,
                std::max(6.f, r.size.width - padX * 2.f),
                std::max(6.f, r.size.height - padY * 2.f)
            );
        }
    }
    return r;
}

float estimateVxFromSpeed(float playerSpeed) {
    // Velocidades reales de GD en px/frame a 60 FPS
    if (playerSpeed <= 0.75f) return 251.16f / 60.f;      // 0.5x
    if (playerSpeed <= 0.98f) return 311.58f / 60.f;      // 1x
    if (playerSpeed <= 1.20f) return 387.42f / 60.f;      // 2x
    if (playerSpeed <= 1.45f) return 468.00f / 60.f;      // 3x
    return 576.00f / 60.f;                                // 4x
}

SimState captureState(PlayerObject* p, bool isHolding, float measuredVx) {
    SimState s;
    auto pos = p->getPosition();
    auto rect = p->getObjectRect();
    s.x = pos.x;
    s.y = pos.y;
    s.mini = p->m_vehicleSize < 0.8f;

    float baseW = rect.size.width > 2.f ? rect.size.width : (s.mini ? 18.f : 30.f);
    float baseH = rect.size.height > 2.f ? rect.size.height : (s.mini ? 18.f : 30.f);
    s.halfW = std::clamp(baseW * 0.48f, 4.f, 15.f);
    s.halfH = std::clamp(baseH * 0.48f, 4.f, 15.f);

    if (measuredVx > 1.5f && measuredVx < 18.f) {
        s.vxPerFrame = measuredVx;
    } else {
        float speedMult = p->m_playerSpeed > 0.1f ? p->m_playerSpeed : 0.9f;
        s.vxPerFrame = estimateVxFromSpeed(speedMult);
    }

    s.vyPerFrame = static_cast<float>(p->m_yVelocity);
    s.upsideDown = p->m_isUpsideDown;
    s.onGround = p->m_isOnGround;
    s.holding = isHolding;
    s.mode = detectVehicle(p);
    s.floorY = 0.f;
    return s;
}

// Avanza 1 frame (1/60 s) usando 2 subpasos con la escala fisica real de GD (0.9 por frame a 60 FPS).
// Devuelve true si choco fatalmente contra un bloque solido.
bool stepVehicleOneFrame(
    SimState& s,
    bool justPressed,
    bool justReleased,
    bool touchingOrb,
    const std::vector<CCRect>& solids
) {
    float gravDir = s.upsideDown ? 1.f : -1.f;
    float sizeFactor = s.mini ? 0.8f : 1.0f;

    if (justPressed) s.holding = true;
    if (justReleased) s.holding = false;

    // Interaccion con orbe al presionar
    if (justPressed && touchingOrb) {
        s.vyPerFrame = -gravDir * (11.18f * sizeFactor);
        s.onGround = false;
    }

    // 2 subpasos por frame de 60 FPS -> dt_sub = 0.9 / 2 = 0.45
    constexpr float DT_SUB = 0.45f;

    for (int sub = 0; sub < 2; ++sub) {
        bool subPressed = justPressed && (sub == 0);
        bool subReleased = justReleased && (sub == 0);
        float dy = 0.f;

        switch (s.mode) {
            case VehicleMode::Cube: {
                if ((subPressed || s.holding) && s.onGround) {
                    s.vyPerFrame = -gravDir * (11.180032f * sizeFactor);
                    s.onGround = false;
                }
                s.vyPerFrame += gravDir * (0.958199f * sizeFactor * DT_SUB);
                s.vyPerFrame = std::clamp(s.vyPerFrame, -15.f, 15.f);
                dy = s.vyPerFrame * DT_SUB;
                break;
            }
            case VehicleMode::Ship: {
                float accel = s.holding ? (-gravDir * 0.48f) : (gravDir * 0.40f);
                s.vyPerFrame += accel * sizeFactor * DT_SUB;
                s.vyPerFrame = std::clamp(s.vyPerFrame, -8.5f, 8.5f);
                dy = s.vyPerFrame * DT_SUB;
                break;
            }
            case VehicleMode::Ball: {
                if (subPressed && s.onGround) {
                    s.upsideDown = !s.upsideDown;
                    gravDir = s.upsideDown ? 1.f : -1.f;
                    s.vyPerFrame = gravDir * 2.5f;
                    s.onGround = false;
                }
                s.vyPerFrame += gravDir * (0.78f * sizeFactor * DT_SUB);
                s.vyPerFrame = std::clamp(s.vyPerFrame, -12.f, 12.f);
                dy = s.vyPerFrame * DT_SUB;
                break;
            }
            case VehicleMode::UFO: {
                if (subPressed) {
                    s.vyPerFrame = -gravDir * (9.2f * sizeFactor);
                    s.onGround = false;
                }
                s.vyPerFrame += gravDir * (0.78f * sizeFactor * DT_SUB);
                s.vyPerFrame = std::clamp(s.vyPerFrame, -11.5f, 11.5f);
                dy = s.vyPerFrame * DT_SUB;
                break;
            }
            case VehicleMode::Wave: {
                float slope = s.mini ? 2.0f : 1.0f;
                float dir = s.holding ? -gravDir : gravDir;
                dy = dir * (s.vxPerFrame * 0.5f) * slope;
                s.vyPerFrame = dy / DT_SUB;
                break;
            }
            case VehicleMode::Robot: {
                if (subPressed && s.onGround) {
                    s.vyPerFrame = -gravDir * (8.8f * sizeFactor);
                    s.onGround = false;
                } else if (s.holding && (-gravDir * s.vyPerFrame) > 0.f) {
                    s.vyPerFrame += -gravDir * (0.45f * sizeFactor * DT_SUB);
                }
                if (subReleased && (-gravDir * s.vyPerFrame) > 3.f) {
                    s.vyPerFrame *= 0.55f;
                }
                s.vyPerFrame += gravDir * (0.92f * sizeFactor * DT_SUB);
                s.vyPerFrame = std::clamp(s.vyPerFrame, -15.f, 15.f);
                dy = s.vyPerFrame * DT_SUB;
                break;
            }
            case VehicleMode::Spider: {
                if (subPressed && s.onGround) {
                    s.upsideDown = !s.upsideDown;
                    gravDir = s.upsideDown ? 1.f : -1.f;
                    float searchDir = gravDir;
                    float bestY = s.y + searchDir * 120.f;
                    float bestDist = 9999.f;
                    for (auto const& box : solids) {
                        if (s.x + s.halfW < box.getMinX() || s.x - s.halfW > box.getMaxX()) continue;
                        if (searchDir > 0.f && box.getMinY() >= s.y) {
                            float d = box.getMinY() - s.y;
                            if (d < bestDist) {
                                bestDist = d;
                                bestY = box.getMinY() - s.halfH;
                            }
                        } else if (searchDir < 0.f && box.getMaxY() <= s.y) {
                            float d = s.y - box.getMaxY();
                            if (d < bestDist) {
                                bestDist = d;
                                bestY = box.getMaxY() + s.halfH;
                            }
                        }
                    }
                    s.y = bestY;
                    s.vyPerFrame = 0.f;
                    s.onGround = true;
                    dy = 0.f;
                } else {
                    s.vyPerFrame += gravDir * (0.958199f * sizeFactor * DT_SUB);
                    s.vyPerFrame = std::clamp(s.vyPerFrame, -15.f, 15.f);
                    dy = s.vyPerFrame * DT_SUB;
                }
                break;
            }
            case VehicleMode::Swing: {
                if (subPressed) {
                    s.upsideDown = !s.upsideDown;
                    gravDir = s.upsideDown ? 1.f : -1.f;
                }
                s.vyPerFrame += gravDir * (0.48f * sizeFactor * DT_SUB);
                s.vyPerFrame = std::clamp(s.vyPerFrame, -9.f, 9.f);
                dy = s.vyPerFrame * DT_SUB;
                break;
            }
        }

        float prevX = s.x;
        float prevY = s.y;
        s.x += s.vxPerFrame * 0.5f;
        s.y += dy;
        s.onGround = false;

        // Piso base del nivel (y = 0)
        if (!s.upsideDown && s.y - s.halfH <= s.floorY) {
            s.y = s.floorY + s.halfH;
            s.vyPerFrame = 0.f;
            s.onGround = true;
        }

        // Colisiones con bloques solidos
        CCRect pRect(s.x - s.halfW, s.y - s.halfH, s.halfW * 2.f, s.halfH * 2.f);
        for (auto const& box : solids) {
            if (!pRect.intersectsRect(box)) continue;

            if (s.mode == VehicleMode::Wave) {
                return true;
            }

            if (!s.upsideDown && prevY - s.halfH >= box.getMaxY() - 10.f && dy <= 0.f) {
                s.y = box.getMaxY() + s.halfH;
                s.vyPerFrame = 0.f;
                s.onGround = true;
                continue;
            }
            if (s.upsideDown && prevY + s.halfH <= box.getMinY() + 10.f && dy >= 0.f) {
                s.y = box.getMinY() - s.halfH;
                s.vyPerFrame = 0.f;
                s.onGround = true;
                continue;
            }

            bool canSlideCeiling = (
                s.mode == VehicleMode::Ship ||
                s.mode == VehicleMode::UFO ||
                s.mode == VehicleMode::Ball ||
                s.mode == VehicleMode::Swing
            );
            if (canSlideCeiling) {
                if (!s.upsideDown && prevY + s.halfH <= box.getMinY() + 10.f && dy >= 0.f) {
                    s.y = box.getMinY() - s.halfH;
                    s.vyPerFrame = 0.f;
                    continue;
                }
                if (s.upsideDown && prevY - s.halfH >= box.getMaxY() - 10.f && dy <= 0.f) {
                    s.y = box.getMaxY() + s.halfH;
                    s.vyPerFrame = 0.f;
                    continue;
                }
            }

            float innerHalfH = s.halfH * 0.65f;
            bool verticalCoreOverlap = (s.y + innerHalfH > box.getMinY() + 2.f) &&
                                       (s.y - innerHalfH < box.getMaxY() - 2.f);
            bool hitFrontWall = (prevX + s.halfW <= box.getMinX() + s.vxPerFrame + 5.f);
            if (verticalCoreOverlap || hitFrontWall || !canSlideCeiling) {
                return true;
            }
        }
    }

    return false;
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
        this->setTitle("Frame Assist - Configuracion");

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
        Mod::get()->setSettingValue<int64_t>("lookahead-frames", 28);
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

        m_lookaheadSlider->setValue(static_cast<float>(28 - MIN_LOOKAHEAD) / (MAX_LOOKAHEAD - MIN_LOOKAHEAD));
        m_lookaheadSlider->updateBar();
        m_lookaheadValLabel->setString("28f");

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

        // Interruptor rapido ON / OFF
        auto quickToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(FrameAssistMenuLayer::onQuickToggle), 0.65f
        );
        quickToggle->setPosition({22.f, 23.f});
        panelMenu->addChild(quickToggle);
        m_fields->quickToggle = quickToggle;

        // Area clicable para abrir ventana de configuracion
        auto clickArea = CCNode::create();
        clickArea->setContentSize({120.f, 38.f});

        auto statusLabel = CCLabelBMFont::create("Assist: ON", "bigFont.fnt");
        statusLabel->setAnchorPoint({0.f, 0.5f});
        statusLabel->setScale(0.36f);
        statusLabel->setPosition({2.f, 26.f});
        clickArea->addChild(statusLabel);
        m_fields->statusLabel = statusLabel;

        auto infoLabel = CCLabelBMFont::create("Margen: 16f | Config", "goldFont.fnt");
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
            m_fields->infoLabel->setString(fmt::format("Margen: {}f | Config", margin).c_str());
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
// 3. MOTOR DE LECTURA DE FRAMES Y PREDICCION (GJBaseGameLayer + PlayLayer)
// ============================================================================
class $modify(FrameAssistGameLayer, GJBaseGameLayer) {
    struct Fields {
        double elapsedTime = 0.0;
        int currentFrame60 = 0;
        bool actualHolding = false;
        bool isDispatchingBuffered = false;
        size_t cachedObjectCount = 0;
        std::deque<PendingAction> pendingQueue;

        // Medicion real de velocidad horizontal por frame (60 FPS)
        float lastPlayerX = 0.f;
        bool hasLastX = false;
        float measuredVxPerFrame = 0.f;

        // Diagnostico en vivo para HUD
        int inputHookCount = 0;
        int lastButtonArg = -1;
        bool lastIsP1Arg = true;
        int lastDecisionDelay = 0;
        std::string lastDecisionReason = "OK";

        std::vector<GameObject*> hazards;
        std::vector<GameObject*> solids;
        std::vector<GameObject*> orbs;
    };

    bool isAssistActive() {
        auto playLayer = PlayLayer::get();
        if (!playLayer || static_cast<GJBaseGameLayer*>(playLayer) != this) return false;
        if (!Mod::get()->getSettingValue<bool>("mod-enabled")) return false;
        if (m_isPracticeMode && !Mod::get()->getSettingValue<bool>("enable-in-practice")) return false;
        if (!m_player1 || m_player1->m_isDead) return false;
        return true;
    }

    void ensureSpatialCache() {
        if (!m_objects) return;
        size_t count = static_cast<size_t>(m_objects->count());
        if (count == 0 || count == m_fields->cachedObjectCount) return;

        m_fields->hazards.clear();
        m_fields->solids.clear();
        m_fields->orbs.clear();

        for (auto* obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            if (isHazardObject(obj)) {
                m_fields->hazards.push_back(obj);
            } else if (isSolidObject(obj)) {
                m_fields->solids.push_back(obj);
            } else if (isOrbObject(obj)) {
                m_fields->orbs.push_back(obj);
            }
        }

        auto sortByX = [](GameObject* a, GameObject* b) {
            return a->getPositionX() < b->getPositionX();
        };
        std::sort(m_fields->hazards.begin(), m_fields->hazards.end(), sortByX);
        std::sort(m_fields->solids.begin(), m_fields->solids.end(), sortByX);
        std::sort(m_fields->orbs.begin(), m_fields->orbs.end(), sortByX);

        m_fields->cachedObjectCount = count;
    }

    void resetAssistState() {
        m_fields->elapsedTime = 0.0;
        m_fields->currentFrame60 = 0;
        m_fields->actualHolding = false;
        m_fields->pendingQueue.clear();
        m_fields->hasLastX = false;
        m_fields->measuredVxPerFrame = 0.f;
        m_fields->lastDecisionDelay = 0;
        m_fields->lastDecisionReason = "OK";
    }

    void gatherNearbyRects(
        std::vector<GameObject*> const& list,
        float minX,
        float maxX,
        std::vector<CCRect>& out,
        bool asHazard
    ) {
        out.clear();
        auto it = std::lower_bound(
            list.begin(), list.end(), minX,
            [](GameObject* obj, float x) { return obj->getPositionX() < x; }
        );
        for (; it != list.end(); ++it) {
            GameObject* obj = *it;
            if (obj->getPositionX() > maxX) break;
            if (obj->m_isDisabled) continue;
            out.push_back(safeObjectRect(obj, asHazard));
        }
    }

    // Evalua cuantos frames sobrevive el jugador si ejecuta `actionDown` exactamente en `delayFrames`
    SimOutcome simulateTrajectory(
        SimState initial,
        bool actionDown,
        int delayFrames,
        int commonHorizon,
        std::vector<CCRect> const& nearHazards,
        std::vector<CCRect> const& nearSolids,
        std::vector<CCRect> const& nearOrbs
    ) {
        SimState s = initial;
        SimOutcome result;
        result.survivedAll = true;
        result.actionEffective = true;
        result.survivalFrames = commonHorizon;

        bool tapMode = actionDown && isTapVehicle(s.mode);

        // Colchon de seguridad alrededor del jugador para evitar roces mortales en espinas
        constexpr float SAFETY_PAD = 3.5f;

        for (int f = 0; f <= commonHorizon; ++f) {
            bool justPressed = (f == delayFrames && actionDown);
            bool justReleased = (f == delayFrames && !actionDown);

            // Para vehiculos de toque, simular liberacion limpia 2 frames despues
            if (tapMode && f == delayFrames + 2) {
                justReleased = true;
            }

            CCRect pRect(s.x - s.halfW, s.y - s.halfH, s.halfW * 2.f, s.halfH * 2.f);
            bool touchingOrb = false;
            for (auto const& orb : nearOrbs) {
                if (pRect.intersectsRect(orb)) {
                    touchingOrb = true;
                    break;
                }
            }

            // En vehiculos de suelo, presionar en el aire sin orbe no tiene efecto inmediato
            if (justPressed && !touchingOrb && !s.onGround && isGroundVehicle(s.mode)) {
                result.actionEffective = false;
            }

            bool crashedSolid = stepVehicleOneFrame(s, justPressed, justReleased, touchingOrb, nearSolids);
            if (crashedSolid) {
                result.survivedAll = false;
                result.survivalFrames = f;
                return result;
            }

            float hx = s.halfW + SAFETY_PAD;
            float hy = s.halfH + SAFETY_PAD;
            CCRect hazardCheckRect(s.x - hx, s.y - hy, hx * 2.f, hy * 2.f);
            for (auto const& hz : nearHazards) {
                if (hazardCheckRect.intersectsRect(hz)) {
                    result.survivedAll = false;
                    result.survivalFrames = f;
                    return result;
                }
            }
        }
        return result;
    }

    // Busca el mejor frame dentro de [0 .. marginFrames] comparando todos sobre el mismo horizonte
    DelayDecision findBestDelayFrame(bool actionDown, int marginFrames, int lookaheadFrames) {
        ensureSpatialCache();

        SimState initial = captureState(m_player1, m_fields->actualHolding, m_fields->measuredVxPerFrame);
        int commonHorizon = marginFrames + std::max(lookaheadFrames, 26);
        float maxDist = initial.vxPerFrame * static_cast<float>(commonHorizon + 14);
        float minX = initial.x - 60.f;
        float maxX = initial.x + maxDist;

        std::vector<CCRect> nearHazards;
        std::vector<CCRect> nearSolids;
        std::vector<CCRect> nearOrbs;
        gatherNearbyRects(m_fields->hazards, minX, maxX, nearHazards, true);
        gatherNearbyRects(m_fields->solids, minX, maxX, nearSolids, false);
        gatherNearbyRects(m_fields->orbs, minX, maxX, nearOrbs, false);

        if (nearHazards.empty() && nearSolids.empty() && nearOrbs.empty()) {
            return {0, true, "LIBRE"};
        }

        // 1. Evaluar delay = 0 (ejecutar ahora mismo)
        SimOutcome nowOut = simulateTrajectory(
            initial, actionDown, 0, commonHorizon,
            nearHazards, nearSolids, nearOrbs
        );
        if (nowOut.survivedAll && nowOut.actionEffective) {
            return {0, true, "SAFE0"};
        }

        // 2. Evaluar todos los delays dentro del margen [1 .. marginFrames]
        int firstSafeEffective = -1;
        int lastSafeEffective = -1;
        int firstSafeAny = -1;

        int bestSurviveDelay = 0;
        int bestSurviveFrames = nowOut.survivalFrames;
        bool bestSurviveEffective = nowOut.actionEffective;

        for (int delay = 1; delay <= marginFrames; ++delay) {
            SimOutcome out = simulateTrajectory(
                initial, actionDown, delay, commonHorizon,
                nearHazards, nearSolids, nearOrbs
            );

            if (out.survivedAll && out.actionEffective) {
                if (firstSafeEffective < 0) firstSafeEffective = delay;
                lastSafeEffective = delay;
            } else if (out.survivedAll && firstSafeAny < 0) {
                firstSafeAny = delay;
            }

            bool betterSurvival = (out.survivalFrames > bestSurviveFrames) ||
                                  (out.survivalFrames == bestSurviveFrames &&
                                   out.actionEffective && !bestSurviveEffective);
            if (betterSurvival) {
                bestSurviveFrames = out.survivalFrames;
                bestSurviveDelay = delay;
                bestSurviveEffective = out.actionEffective;
            }
        }

        // Elegir el centro/inicio seguro de la ventana valida
        if (firstSafeEffective >= 0) {
            int chosen = firstSafeEffective;
            if (lastSafeEffective - firstSafeEffective >= 2) {
                chosen = firstSafeEffective + 1;
            }
            return {chosen, false, "VENTANA"};
        }

        if (firstSafeAny >= 0) {
            return {firstSafeAny, false, "ESQUIVA"};
        }

        if (bestSurviveDelay > 0 && bestSurviveFrames >= nowOut.survivalFrames + 2) {
            return {bestSurviveDelay, false, "SUPERV"};
        }

        return {0, nowOut.survivedAll, "SIN_ALT"};
    }

    void dispatchButton(bool down, int button, bool isPlayer1) {
        if (button == 1 || button == 0) {
            m_fields->actualHolding = down;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // Evitar recursividad al disparar acciones retenidas
        if (m_fields->isDispatchingBuffered) {
            dispatchButton(down, button, isPlayer1);
            return;
        }

        // Registrar diagnostico de cada entrada real que llega a handleButton
        m_fields->inputHookCount++;
        m_fields->lastButtonArg = button;
        m_fields->lastIsP1Arg = isPlayer1;

        // NOTA ANDROID: No filtrar por !isPlayer1 porque en pantalla tactil
        // tocar la mitad derecha puede enviar isPlayer1 = false incluso en modo 1 jugador.
        if ((button != 1 && button != 0) || !isAssistActive()) {
            dispatchButton(down, button, isPlayer1);
            return;
        }

        bool assistPress = Mod::get()->getSettingValue<bool>("buffer-press");
        bool assistRelease = Mod::get()->getSettingValue<bool>("buffer-release");
        int margin = static_cast<int>(Mod::get()->getSettingValue<int64_t>("margin-frames"));
        int lookahead = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead-frames"));

        // Toque rapido en Android: si ya hay un CLIC esperando y el dedo se levanta antes de disparar,
        // encolar el SOLTAR detras para que no cancele el salto pendiente.
        if (!down && !m_fields->pendingQueue.empty() && m_fields->pendingQueue.front().down) {
            if (m_fields->pendingQueue.back().down) {
                PendingAction rel;
                rel.down = false;
                rel.button = button;
                rel.isPlayer1 = isPlayer1;
                rel.createdFrame = m_fields->currentFrame60;
                rel.targetFrame = m_fields->pendingQueue.front().targetFrame + 2;
                rel.expireFrame = m_fields->pendingQueue.front().expireFrame + margin;
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

        DelayDecision decision = findBestDelayFrame(down, margin, lookahead);
        m_fields->lastDecisionDelay = decision.delay;
        m_fields->lastDecisionReason = decision.reason;

        if (decision.delay <= 0) {
            m_fields->pendingQueue.clear();
            dispatchButton(down, button, isPlayer1);
            return;
        }

        // Encolar la accion para ejecutarla en el frame seguro
        m_fields->pendingQueue.clear();
        PendingAction act;
        act.down = down;
        act.button = button;
        act.isPlayer1 = isPlayer1;
        act.createdFrame = m_fields->currentFrame60;
        act.targetFrame = m_fields->currentFrame60 + decision.delay;
        act.expireFrame = m_fields->currentFrame60 + margin;
        act.quickTapRelease = false;
        m_fields->pendingQueue.push_back(act);
    }

    void tickFrameAssist(float dt) {
        ensureSpatialCache();

        // Medir velocidad horizontal real del jugador en px/frame (60 FPS)
        if (m_player1 && dt > 0.0005f && dt < 0.15f) {
            float curX = m_player1->getPositionX();
            if (m_fields->hasLastX) {
                float dx = curX - m_fields->lastPlayerX;
                if (dx > 0.01f && dx < 120.f) {
                    float instVx60 = (dx / dt) / static_cast<float>(REF_FPS);
                    if (instVx60 > 1.5f && instVx60 < 18.f) {
                        if (m_fields->measuredVxPerFrame <= 0.f) {
                            m_fields->measuredVxPerFrame = instVx60;
                        } else {
                            m_fields->measuredVxPerFrame =
                                m_fields->measuredVxPerFrame * 0.7f + instVx60 * 0.3f;
                        }
                    }
                }
            }
            m_fields->lastPlayerX = curX;
            m_fields->hasLastX = true;
        }

        m_fields->elapsedTime += static_cast<double>(dt);
        int newFrame = static_cast<int>(m_fields->elapsedTime * REF_FPS);

        // Evaluar cada avance de frame (60 FPS)
        if (newFrame == m_fields->currentFrame60) return;
        m_fields->currentFrame60 = newFrame;

        if (m_fields->pendingQueue.empty()) return;

        if (!isAssistActive()) {
            m_fields->pendingQueue.clear();
            return;
        }

        auto& front = m_fields->pendingQueue.front();
        int lookahead = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead-frames"));
        int remainingMargin = std::max(0, front.expireFrame - newFrame);

        bool shouldTrigger = false;

        if (front.quickTapRelease) {
            if (newFrame >= front.targetFrame) {
                shouldTrigger = true;
            }
        } else if (newFrame >= front.expireFrame) {
            shouldTrigger = true;
        } else {
            DelayDecision live = findBestDelayFrame(front.down, remainingMargin, lookahead);
            if (live.delay == 0 && (live.delayZeroSafe || newFrame >= front.targetFrame)) {
                shouldTrigger = true;
            } else if (live.delay > 0) {
                front.targetFrame = newFrame + live.delay;
            }
        }

        if (shouldTrigger) {
            PendingAction action = front;
            m_fields->pendingQueue.pop_front();

            if (!m_fields->pendingQueue.empty() && action.down && !m_fields->pendingQueue.front().down) {
                int margin = static_cast<int>(Mod::get()->getSettingValue<int64_t>("margin-frames"));
                auto& nextRel = m_fields->pendingQueue.front();
                if (nextRel.quickTapRelease) {
                    nextRel.targetFrame = newFrame + 2;
                    nextRel.expireFrame = newFrame + 2;
                } else {
                    nextRel.createdFrame = newFrame;
                    nextRel.targetFrame = newFrame + 1;
                    nextRel.expireFrame = newFrame + margin;
                }
            }

            m_fields->isDispatchingBuffered = true;
            dispatchButton(action.down, action.button, action.isPlayer1);
            m_fields->isDispatchingBuffered = false;
        }
    }

    int getCurrentFrame60() { return m_fields->currentFrame60; }
    size_t getHazardCount() { return m_fields->hazards.size(); }
    int getInputHookCount() { return m_fields->inputHookCount; }
    int getLastButtonArg() { return m_fields->lastButtonArg; }
    bool getLastIsP1Arg() { return m_fields->lastIsP1Arg; }
    int getLastDecisionDelay() { return m_fields->lastDecisionDelay; }
    std::string getLastDecisionReason() { return m_fields->lastDecisionReason; }

    bool hasPendingAction() {
        return !m_fields->pendingQueue.empty();
    }

    PendingAction getFrontPendingAction() {
        if (m_fields->pendingQueue.empty()) return PendingAction{};
        return m_fields->pendingQueue.front();
    }
};

class $modify(FrameAssistPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* hudLabel = nullptr;
    };

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;

        auto base = static_cast<FrameAssistGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        base->resetAssistState();
        base->ensureSpatialCache();

        applySafeModeIfNeeded();

        if (Mod::get()->getSettingValue<bool>("mod-enabled") &&
            Mod::get()->getSettingValue<bool>("show-hud")) {
            auto winSize = CCDirector::get()->getWinSize();
            auto hud = CCLabelBMFont::create("F:0 | Cube | H:0 | I:0 | LISTO", "bigFont.fnt");
            hud->setID("frame-assist-hud"_spr);
            hud->setAnchorPoint({0.f, 1.f});
            hud->setScale(0.26f);
            hud->setOpacity(195);
            hud->setPosition({8.f, winSize.height - 6.f});
            this->addChild(hud, 100);
            m_fields->hudLabel = hud;
        }

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto base = static_cast<FrameAssistGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        base->resetAssistState();
        base->ensureSpatialCache();
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
                    pending.targetFrame
                );
                m_fields->hudLabel->setColor({255, 220, 90});
            } else {
                stateStr = fmt::format(
                    "D:{}({})",
                    base->getLastDecisionDelay(),
                    base->getLastDecisionReason()
                );
                m_fields->hudLabel->setColor({150, 255, 160});
            }

            m_fields->hudLabel->setString(fmt::format(
                "F:{} {} H:{} I:{} B:{} P:{} {}{}",
                base->getCurrentFrame60(),
                vehicleName(mode),
                base->getHazardCount(),
                base->getInputHookCount(),
                base->getLastButtonArg(),
                base->getLastIsP1Arg() ? 1 : 0,
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
