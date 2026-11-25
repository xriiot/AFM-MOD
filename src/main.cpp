#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
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
    float vxPerFrame = 5.19f; // px por frame a 60 FPS (~311.58 px/s a velocidad 1x)
    float vyPerFrame = 0.f;
    float halfW = 12.f;
    float halfH = 12.f;
    bool upsideDown = false;
    bool onGround = false;
    bool holding = false;
    bool mini = false;
    VehicleMode mode = VehicleMode::Cube;
};

struct PendingAction {
    bool active = false;
    bool down = false;
    int button = 1;
    bool isPlayer1 = true;
    int createdFrame = 0;
    int targetFrame = 0;
    int expireFrame = 0;
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

CCRect shrinkHazardRect(CCRect r) {
    // Las hitboxes reales de espinas en GD son mas pequenas que su sprite visual
    float padX = r.size.width * 0.28f;
    float padY = r.size.height * 0.28f;
    return CCRect(
        r.origin.x + padX,
        r.origin.y + padY,
        std::max(2.f, r.size.width - padX * 2.f),
        std::max(2.f, r.size.height - padY * 2.f)
    );
}

SimState captureState(PlayerObject* p) {
    SimState s;
    auto pos = p->getPosition();
    auto rect = p->getObjectRect();
    s.x = pos.x;
    s.y = pos.y;
    s.mini = p->m_vehicleSize < 0.8f;
    s.halfW = std::max(4.f, rect.size.width * 0.42f);
    s.halfH = std::max(4.f, rect.size.height * 0.42f);

    // Velocidad horizontal aproximada por frame a 60 FPS segun m_playerSpeed
    float speedMult = p->m_playerSpeed > 0.1f ? p->m_playerSpeed : 0.9f;
    s.vxPerFrame = (346.2f * speedMult) / static_cast<float>(REF_FPS);
    s.vyPerFrame = static_cast<float>(p->m_yVelocity);
    s.upsideDown = p->m_isUpsideDown;
    s.onGround = p->m_isOnGround;
    s.holding = p->m_isHolding;
    s.mode = detectVehicle(p);
    return s;
}

// Avanza 1 frame (1/60 s) la fisica aproximada del vehiculo activo
void stepVehicleOneFrame(
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
        s.vyPerFrame = -gravDir * (11.1f * sizeFactor);
        s.onGround = false;
    }

    switch (s.mode) {
        case VehicleMode::Cube: {
            if ((justPressed || s.holding) && s.onGround) {
                s.vyPerFrame = -gravDir * (11.18f * sizeFactor);
                s.onGround = false;
            }
            s.vyPerFrame += gravDir * (0.958f * sizeFactor);
            s.vyPerFrame = std::clamp(s.vyPerFrame, -15.f, 15.f);
            break;
        }
        case VehicleMode::Ship: {
            float accel = s.holding ? (-gravDir * 0.46f) : (gravDir * 0.38f);
            s.vyPerFrame += accel * sizeFactor;
            s.vyPerFrame = std::clamp(s.vyPerFrame, -8.5f, 8.5f);
            break;
        }
        case VehicleMode::Ball: {
            if (justPressed && s.onGround) {
                s.upsideDown = !s.upsideDown;
                gravDir = s.upsideDown ? 1.f : -1.f;
                s.vyPerFrame = gravDir * 2.5f;
                s.onGround = false;
            }
            s.vyPerFrame += gravDir * (0.78f * sizeFactor);
            s.vyPerFrame = std::clamp(s.vyPerFrame, -12.f, 12.f);
            break;
        }
        case VehicleMode::UFO: {
            if (justPressed) {
                s.vyPerFrame = -gravDir * (9.2f * sizeFactor);
                s.onGround = false;
            }
            s.vyPerFrame += gravDir * (0.78f * sizeFactor);
            s.vyPerFrame = std::clamp(s.vyPerFrame, -11.5f, 11.5f);
            break;
        }
        case VehicleMode::Wave: {
            float slope = s.mini ? 2.0f : 1.0f;
            float dir = s.holding ? -gravDir : gravDir;
            s.vyPerFrame = dir * s.vxPerFrame * slope;
            break;
        }
        case VehicleMode::Robot: {
            if (justPressed && s.onGround) {
                s.vyPerFrame = -gravDir * (8.8f * sizeFactor);
                s.onGround = false;
            } else if (s.holding && (-gravDir * s.vyPerFrame) > 0.f) {
                s.vyPerFrame += -gravDir * (0.45f * sizeFactor);
            }
            if (justReleased && (-gravDir * s.vyPerFrame) > 3.f) {
                s.vyPerFrame *= 0.55f;
            }
            s.vyPerFrame += gravDir * (0.92f * sizeFactor);
            s.vyPerFrame = std::clamp(s.vyPerFrame, -15.f, 15.f);
            break;
        }
        case VehicleMode::Spider: {
            if (justPressed && s.onGround) {
                s.upsideDown = !s.upsideDown;
                float searchDir = s.upsideDown ? 1.f : -1.f;
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
            } else {
                s.vyPerFrame += gravDir * (0.958f * sizeFactor);
                s.vyPerFrame = std::clamp(s.vyPerFrame, -15.f, 15.f);
            }
            break;
        }
        case VehicleMode::Swing: {
            if (justPressed) {
                s.upsideDown = !s.upsideDown;
                gravDir = s.upsideDown ? 1.f : -1.f;
            }
            s.vyPerFrame += gravDir * (0.48f * sizeFactor);
            s.vyPerFrame = std::clamp(s.vyPerFrame, -9.f, 9.f);
            break;
        }
    }

    float prevY = s.y;
    s.x += s.vxPerFrame;
    s.y += s.vyPerFrame;
    s.onGround = false;

    // Piso base del nivel (y = 0)
    if (!s.upsideDown && s.y - s.halfH <= 0.f) {
        s.y = s.halfH;
        s.vyPerFrame = 0.f;
        s.onGround = true;
    }

    // Aterrizaje simple sobre bloques solidos
    CCRect pRect(s.x - s.halfW, s.y - s.halfH, s.halfW * 2.f, s.halfH * 2.f);
    for (auto const& box : solids) {
        if (!pRect.intersectsRect(box)) continue;
        if (!s.upsideDown && prevY - s.halfH >= box.getMaxY() - 8.f && s.vyPerFrame <= 0.f) {
            s.y = box.getMaxY() + s.halfH;
            s.vyPerFrame = 0.f;
            s.onGround = true;
        } else if (s.upsideDown && prevY + s.halfH <= box.getMinY() + 8.f && s.vyPerFrame >= 0.f) {
            s.y = box.getMinY() - s.halfH;
            s.vyPerFrame = 0.f;
            s.onGround = true;
        }
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
        CCMenuIt
