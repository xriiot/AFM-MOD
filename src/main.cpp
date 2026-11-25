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
        bool isDispatchingBuffered = false;
        bool indexBuilt = false;
        PendingAction pending;

        std::vector<GameObject*> hazards;
        std::vector<GameObject*> solids;
        std::vector<GameObject*> orbs;
    };

    bool isAssistActive() {
        auto playLayer = typeinfo_cast<PlayLayer*>(this);
        if (!playLayer) return false;
        if (!Mod::get()->getSettingValue<bool>("mod-enabled")) return false;
        if (m_isPracticeMode && !Mod::get()->getSettingValue<bool>("enable-in-practice")) return false;
        if (!m_player1 || m_player1->m_isDead) return false;
        return true;
    }

    void buildSpatialCache() {
        m_fields->hazards.clear();
        m_fields->solids.clear();
        m_fields->orbs.clear();

        if (!m_objects) return;

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

        m_fields->indexBuilt = true;
    }

    void resetAssistState() {
        m_fields->elapsedTime = 0.0;
        m_fields->currentFrame60 = 0;
        m_fields->pending = PendingAction{};
    }

    void gatherNearbyRects(
        std::vector<GameObject*> const& list,
        float minX,
        float maxX,
        std::vector<CCRect>& out,
        bool shrinkAsHazard
    ) {
        out.clear();
        auto it = std::lower_bound(
            list.begin(), list.end(), minX,
            [](GameObject* obj, float x) { return obj->getPositionX() < x; }
        );
        for (; it != list.end(); ++it) {
            GameObject* obj = *it;
            if (obj->getPositionX() > maxX) break;
            if (obj->m_isDisabled || !obj->isVisible()) continue;
            CCRect r = obj->getObjectRect();
            out.push_back(shrinkAsHazard ? shrinkHazardRect(r) : r);
        }
    }

    // Evalua si ejecutar `actionDown` exactamente tras `delayFrames` mantiene vivo al jugador
    bool simulateTrajectorySafe(
        SimState initial,
        bool actionDown,
        int delayFrames,
        int lookaheadFrames,
        std::vector<CCRect> const& nearHazards,
        std::vector<CCRect> const& nearSolids,
        std::vector<CCRect> const& nearOrbs,
        bool& outActionWasEffective
    ) {
        SimState s = initial;
        outActionWasEffective = true;
        int totalFrames = delayFrames + lookaheadFrames;

        for (int f = 0; f <= totalFrames; ++f) {
            bool justPressed = (f == delayFrames && actionDown);
            bool justReleased = (f == delayFrames && !actionDown);

            CCRect pRect(s.x - s.halfW, s.y - s.halfH, s.halfW * 2.f, s.halfH * 2.f);
            bool touchingOrb = false;
            for (auto const& orb : nearOrbs) {
                if (pRect.intersectsRect(orb)) {
                    touchingOrb = true;
                    break;
                }
            }

            // En vehiculos de suelo (Cube, Ball, Spider, Robot), presionar en el aire sin orbe no tiene efecto inmediato
            if (justPressed && !touchingOrb && !s.onGround) {
                if (s.mode == VehicleMode::Cube ||
                    s.mode == VehicleMode::Ball ||
                    s.mode == VehicleMode::Spider ||
                    s.mode == VehicleMode::Robot) {
                    outActionWasEffective = false;
                }
            }

            stepVehicleOneFrame(s, justPressed, justReleased, touchingOrb, nearSolids);

            CCRect nextRect(s.x - s.halfW, s.y - s.halfH, s.halfW * 2.f, s.halfH * 2.f);
            for (auto const& hz : nearHazards) {
                if (nextRect.intersectsRect(hz)) {
                    return false; // Muerte predicha en el frame f
                }
            }
        }
        return true;
    }

    // Busca el mejor frame dentro de [0 .. marginFrames] para ejecutar la accion
    int findBestDelayFrame(bool actionDown, int marginFrames, int lookaheadFrames) {
        if (!m_fields->indexBuilt) buildSpatialCache();

        SimState initial = captureState(m_player1);
        float maxDist = initial.vxPerFrame * static_cast<float>(marginFrames + lookaheadFrames + 8);
        float minX = initial.x - 45.f;
        float maxX = initial.x + maxDist;

        std::vector<CCRect> nearHazards;
        std::vector<CCRect> nearSolids;
        std::vector<CCRect> nearOrbs;
        gatherNearbyRects(m_fields->hazards, minX, maxX, nearHazards, true);
        gatherNearbyRects(m_fields->solids, minX, maxX, nearSolids, false);
        gatherNearbyRects(m_fields->orbs, minX, maxX, nearOrbs, false);

        // 1. Verificar si ejecutar YA MISMO (delay 0) es seguro y efectivo
        bool effectiveNow = true;
        bool safeNow = simulateTrajectorySafe(
            initial, actionDown, 0, lookaheadFrames,
            nearHazards, nearSolids, nearOrbs, effectiveNow
        );
        if (safeNow && effectiveNow) {
            return 0;
        }

        // 2. Buscar el primer frame seguro y efectivo dentro del margen (1 .. marginFrames)
        int fallbackSafeDelay = -1;
        for (int delay = 1; delay <= marginFrames; ++delay) {
            bool effective = true;
            bool safe = simulateTrajectorySafe(
                initial, actionDown, delay, lookaheadFrames,
                nearHazards, nearSolids, nearOrbs, effective
            );
            if (safe && effective) {
                return delay;
            }
            if (safe && fallbackSafeDelay < 0) {
                fallbackSafeDelay = delay;
            }
        }

        // Si hubo algun frame seguro aunque no efectivo (ej. esperar a esquivar), usarlo
        if (fallbackSafeDelay >= 0) {
            return fallbackSafeDelay;
        }

        // Si ningun retraso mejora la situacion, ejecutar de inmediato para no bloquear control
        return 0;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // Evitar recursividad al disparar nuestras acciones retenidas
        if (m_fields->isDispatchingBuffered || !isPlayer1 || button != 1 || !isAssistActive()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        bool assistPress = Mod::get()->getSettingValue<bool>("buffer-press");
        bool assistRelease = Mod::get()->getSettingValue<bool>("buffer-release");

        if ((down && !assistPress) || (!down && !assistRelease)) {
            m_fields->pending.active = false;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        int margin = static_cast<int>(Mod::get()->getSettingValue<int64_t>("margin-frames"));
        int lookahead = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead-frames"));

        int bestDelay = findBestDelayFrame(down, margin, lookahead);

        if (bestDelay <= 0) {
            m_fields->pending.active = false;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Guardar la accion en el bufer para ejecutarla en el frame seguro
        m_fields->pending.active = true;
        m_fields->pending.down = down;
        m_fields->pending.button = button;
        m_fields->pending.isPlayer1 = isPlayer1;
        m_fields->pending.createdFrame = m_fields->currentFrame60;
        m_fields->pending.targetFrame = m_fields->currentFrame60 + bestDelay;
        m_fields->pending.expireFrame = m_fields->currentFrame60 + margin;
    }

    void tickFrameAssist(float dt) {
        m_fields->elapsedTime += static_cast<double>(dt);
        int newFrame = static_cast<int>(m_fields->elapsedTime * REF_FPS);

        // Ejecutar evaluacion cada avance de frame (60 FPS)
        if (newFrame == m_fields->currentFrame60) return;
        m_fields->currentFrame60 = newFrame;

        if (!m_fields->pending.active) return;

        if (!isAssistActive()) {
            m_fields->pending.active = false;
            return;
        }

        int lookahead = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead-frames"));
        int remainingMargin = std::max(0, m_fields->pending.expireFrame - newFrame);

        bool shouldTrigger = false;

        if (newFrame >= m_fields->pending.expireFrame) {
            // Se cumplio el margen maximo (ej. 16 frames): ejecutar la accion ahora
            shouldTrigger = true;
        } else {
            // Reevaluar en tiempo real con la posicion real del jugador
            int liveDelay = findBestDelayFrame(m_fields->pending.down, remainingMargin, lookahead);
            m_fields->pending.targetFrame = newFrame + liveDelay;
            if (liveDelay == 0) {
                shouldTrigger = true;
            }
        }

        if (shouldTrigger) {
            auto action = m_fields->pending;
            m_fields->pending.active = false;

            m_fields->isDispatchingBuffered = true;
            GJBaseGameLayer::handleButton(action.down, action.button, action.isPlayer1);
            m_fields->isDispatchingBuffered = false;
        }
    }

    int getCurrentFrame60() {
        return m_fields->currentFrame60;
    }

    PendingAction getPendingAction() {
        return m_fields->pending;
    }
};

class $modify(FrameAssistPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* hudLabel = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto base = static_cast<FrameAssistGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        base->resetAssistState();
        base->buildSpatialCache();

        applySafeModeIfNeeded();

        if (Mod::get()->getSettingValue<bool>("mod-enabled") &&
            Mod::get()->getSettingValue<bool>("show-hud")) {
            auto winSize = CCDirector::get()->getWinSize();
            auto hud = CCLabelBMFont::create("F: 0 | Cube | LISTO", "bigFont.fnt");
            hud->setID("frame-assist-hud"_spr);
            hud->setAnchorPoint({0.f, 1.f});
            hud->setScale(0.30f);
            hud->setOpacity(175);
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
        applySafeModeIfNeeded();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto base = static_cast<FrameAssistGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        base->tickFrameAssist(dt);

        if (m_fields->hudLabel && m_player1) {
            auto mode = detectVehicle(m_player1);
            bool safeMode = Mod::get()->getSettingValue<bool>("safe-mode");
            auto pending = base->getPendingAction();

            std::string stateStr;
            if (pending.active) {
                stateStr = fmt::format(
                    "ESPERANDO {} -> F:{}",
                    pending.down ? "CLIC" : "SOLTAR",
                    pending.targetFrame
                );
                m_fields->hudLabel->setColor({255, 220, 90});
            } else {
                stateStr = "LISTO";
                m_fields->hudLabel->setColor({150, 255, 160});
            }

            m_fields->hudLabel->setString(fmt::format(
                "F:{} | {} | {}{}",
                base->getCurrentFrame60(),
                vehicleName(mode),
                stateStr,
                safeMode ? " [SAFE]" : ""
            ).c_str());
        }
    }

    void applySafeModeIfNeeded() {
        if (Mod::get()->getSettingValue<bool>("mod-enabled") &&
            Mod::get()->getSettingValue<bool>("safe-mode")) {
            // m_isTestMode evita que GD registre progreso o estrellas mientras pruebas el mod
            m_isTestMode = true;
        }
    }
};
