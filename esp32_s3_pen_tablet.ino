#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include <USB.h>
#include <USBHIDMouse.h>
#include <USBHIDGamepad.h>

using namespace esp_panel::drivers;
using namespace esp_panel::board;

// Relative USB HID motion makes the touchscreen behave like a mousepad.
static constexpr uint16_t TOUCH_POLL_MS = 8;
static constexpr uint16_t TOUCH_UI_REFRESH_MS = 33;
static constexpr uint16_t TOUCH_RELEASE_DEBOUNCE_MS = 40;
static constexpr uint8_t TOUCH_MAX_POINTS = 5;
static constexpr uint8_t TOUCH_DEADZONE_PX = 1;
static constexpr uint8_t CLICK_SLOP_PX = 8;
static constexpr int8_t POINTER_SENSITIVITY = 3;
static constexpr uint16_t FINGER_AREA_RATIO = 2;
static constexpr uint16_t FINGER_AREA_MARGIN = 12;
static constexpr uint8_t MULTITOUCH_REBASE_DELTA_PX = 48;
static constexpr uint16_t MODE_GESTURE_HOLD_MS = 650;
static constexpr int16_t LCD_WIDTH = 800;
static constexpr int16_t LCD_HEIGHT = 480;
static constexpr uint32_t GAMEPAD_BUTTON_MASK = (1UL << BUTTON_A) | (1UL << BUTTON_B) |
    (1UL << BUTTON_X) | (1UL << BUTTON_Y) | (1UL << BUTTON_SELECT) | (1UL << BUTTON_START);

USBHIDMouse tabletMouse;
USBHIDGamepad tabletGamepad;
Board *board = nullptr;
lv_obj_t *statusLabel = nullptr;
lv_obj_t *touchDot = nullptr;
lv_obj_t *mouseScreen = nullptr;
lv_obj_t *gamepadScreen = nullptr;
enum class InputMode : uint8_t { Mouse, Gamepad };
InputMode inputMode = InputMode::Mouse;
bool touchActive = false;
bool touchMoved = false;
bool multipleContactsSeen = false;
int16_t touchStartX = 0;
int16_t touchStartY = 0;
int16_t lastTouchX = 0;
int16_t lastTouchY = 0;
uint16_t activeContactArea = 0;
uint32_t lastTouchPollMs = 0;
uint32_t lastTouchUiMs = 0;
uint32_t neutralReportsUntilMs = 0;
uint32_t touchNoPointSinceMs = 0;
bool touchReleasePending = false;
uint32_t gamepadButtons = 0;
int8_t gamepadX = 0;
int8_t gamepadY = 0;
bool modeGesturePending = false;
bool modeGestureLatched = false;
uint32_t modeGestureStartMs = 0;
bool modeGestureReleasePending = false;
uint32_t modeGestureNoPointSinceMs = 0;

static int8_t scaleDelta(int32_t delta)
{
    const int32_t scaled = delta * POINTER_SENSITIVITY;
    if (scaled < -127) return -127;
    if (scaled > 127) return 127;
    return static_cast<int8_t>(scaled);
}

static void setTouchDisplay(bool touched, int16_t x = 0, int16_t y = 0, int count = 0, int area = 0)
{
    if (statusLabel == nullptr || touchDot == nullptr) return;
    if (!lvgl_port_lock(1000)) return;

    if (touched) {
        lv_label_set_text_fmt(statusLabel, "TRACKING TOUCH\nX: %d    Y: %d\nContacts: %d  Area: %d", x, y, count, area);
        const int16_t dotX = constrain(static_cast<int>(x) - 16, 0, LCD_WIDTH - 32);
        const int16_t dotY = constrain(static_cast<int>(y) - 16, 0, LCD_HEIGHT - 32);
        lv_obj_set_pos(touchDot, dotX, dotY);
        lv_obj_clear_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(statusLabel, "WAITING FOR TOUCH\nTouch the screen to test the GT911 sensor");
        lv_obj_add_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_port_unlock();
}

static void createTabletUi()
{
    lv_theme_t *theme = lv_theme_default_init(
        lv_disp_get_default(),
        lv_palette_main(LV_PALETTE_CYAN),
        lv_palette_main(LV_PALETTE_GREY),
        true,
        LV_FONT_DEFAULT);
    lv_disp_set_theme(lv_disp_get_default(), theme);

    lv_obj_t *screen = lv_scr_act();
    mouseScreen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "ESP32-S3 PEN TABLET");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF3F4F6), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    statusLabel = lv_label_create(screen);
    lv_label_set_text(statusLabel, "WAITING FOR TOUCH\nTouch the screen to test the GT911 sensor");
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(0x67E8F9), LV_PART_MAIN);
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(statusLabel, LV_ALIGN_CENTER, 0, 0);

    touchDot = lv_obj_create(screen);
    lv_obj_set_size(touchDot, 32, 32);
    lv_obj_set_style_radius(touchDot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(touchDot, lv_color_hex(0x34D399), LV_PART_MAIN);
    lv_obj_set_style_border_width(touchDot, 0, LV_PART_MAIN);
    lv_obj_add_flag(touchDot, LV_OBJ_FLAG_HIDDEN);

    gamepadScreen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(gamepadScreen, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gamepadScreen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *gamepadTitle = lv_label_create(gamepadScreen);
    lv_label_set_text(gamepadTitle, "CONTROLLER MODE  |  HOLD FOUR FINGERS TO SWITCH");
    lv_obj_set_style_text_color(gamepadTitle, lv_color_hex(0xF3F4F6), LV_PART_MAIN);
    lv_obj_align(gamepadTitle, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *stick = lv_obj_create(gamepadScreen);
    lv_obj_set_size(stick, 220, 220);
    lv_obj_set_style_radius(stick, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stick, lv_color_hex(0x263449), LV_PART_MAIN);
    lv_obj_set_style_border_color(stick, lv_color_hex(0x67E8F9), LV_PART_MAIN);
    lv_obj_align(stick, LV_ALIGN_LEFT_MID, 50, 24);
    lv_obj_t *stickLabel = lv_label_create(gamepadScreen);
    lv_label_set_text(stickLabel, "LEFT STICK");
    lv_obj_set_style_text_color(stickLabel, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_align_to(stickLabel, stick, LV_ALIGN_CENTER, 0, 0);

    const char *buttonNames[] = {"Y", "X", "B", "A"};
    const uint32_t buttonColors[] = {0x3B82F6, 0x10B981, 0xF59E0B, 0xEF4444};
    const int16_t buttonX[] = {635, 705, 635, 705};
    const int16_t buttonY[] = {145, 215, 285, 355};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *button = lv_obj_create(gamepadScreen);
        lv_obj_set_size(button, 58, 58);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(buttonColors[i]), LV_PART_MAIN);
        lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
        lv_obj_set_pos(button, buttonX[i], buttonY[i]);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, buttonNames[i]);
        lv_obj_center(label);
    }
    lv_obj_t *systemLabel = lv_label_create(gamepadScreen);
    lv_label_set_text(systemLabel, "SELECT                         START");
    lv_obj_set_style_text_color(systemLabel, lv_color_hex(0xD1D5DB), LV_PART_MAIN);
    lv_obj_align(systemLabel, LV_ALIGN_BOTTOM_MID, 0, -24);
}

static void setMode(InputMode mode)
{
    if (inputMode == mode) return;
    inputMode = mode;
    touchActive = false;
    touchReleasePending = false;
    multipleContactsSeen = false;
    activeContactArea = 0;
    gamepadButtons = 0;
    gamepadX = gamepadY = 0;
    tabletMouse.release(MOUSE_LEFT);
    tabletMouse.move(0, 0);
    tabletGamepad.send(0, 0, 0, 0, 0, 0, HAT_CENTER, 0);
    if (lvgl_port_lock(1000)) {
        lv_scr_load(mode == InputMode::Mouse ? mouseScreen : gamepadScreen);
        lvgl_port_unlock();
    }
    Serial.printf("Input mode: %s\n", mode == InputMode::Mouse ? "mouse" : "gamepad");
}

static void updateGamepad(const TouchPoint points[], int count)
{
    int32_t stickX = 0, stickY = 0;
    uint32_t buttons = 0;
    bool hasStick = false;
    for (int i = 0; i < count; ++i) {
        const int16_t x = points[i].x;
        const int16_t y = points[i].y;
        if (x < 400) {
            int32_t dx = x - 160;
            int32_t dy = y - 264;
            const int32_t radius = max(abs(dx), abs(dy));
            if (radius > 14) {
                stickX = constrain(dx * 127 / 110, -127, 127);
                stickY = constrain(dy * 127 / 110, -127, 127);
                hasStick = true;
            }
        } else if (x >= 600 && x < 680 && y >= 110 && y < 190) buttons |= 1UL << BUTTON_Y;
        else if (x >= 680 && x < 760 && y >= 180 && y < 250) buttons |= 1UL << BUTTON_X;
        else if (x >= 600 && x < 680 && y >= 250 && y < 320) buttons |= 1UL << BUTTON_B;
        else if (x >= 680 && x < 760 && y >= 320 && y < 390) buttons |= 1UL << BUTTON_A;
        else if (x >= 400 && y > 410) buttons |= 1UL << (x < 600 ? BUTTON_SELECT : BUTTON_START);
    }
    gamepadX = hasStick ? static_cast<int8_t>(stickX) : 0;
    gamepadY = hasStick ? static_cast<int8_t>(stickY) : 0;
    gamepadButtons = buttons & GAMEPAD_BUTTON_MASK;
    tabletGamepad.send(gamepadX, gamepadY, 0, 0, 0, 0, HAT_CENTER, gamepadButtons);
}

// GT911 has no pen identity. On multi-touch frames, prefer the point with the
// smallest reported contact area, which is usually the narrow capacitive tip.
static bool selectPointerContact(const TouchPoint points[], int count, TouchPoint &selected)
{
    if (count <= 0) return false;

    int bestIndex = 0;
    for (int i = 1; i < count; ++i) {
        if (points[i].strength > 0 &&
            (points[bestIndex].strength <= 0 || points[i].strength < points[bestIndex].strength)) {
            bestIndex = i;
        }
    }

    selected = points[bestIndex];
    if (count > 1) {
        multipleContactsSeen = true;
        if (selected.strength > 0 &&
            (activeContactArea == 0 || selected.strength < activeContactArea)) {
            activeContactArea = selected.strength;
        }
        return true;
    }

    // If multiple contacts were present and only a much larger point remains,
    // treat it as the resting finger after the smaller stylus contact lifted.
    if (multipleContactsSeen && activeContactArea > 0 && selected.strength > 0 &&
        selected.strength > activeContactArea * FINGER_AREA_RATIO + FINGER_AREA_MARGIN) {
        return false;
    }

    if (selected.strength > 0 &&
        (activeContactArea == 0 || selected.strength < activeContactArea)) {
        activeContactArea = selected.strength;
    }
    return true;
}

static void beginTouch(const TouchPoint &point, int pointCount)
{
    touchActive = true;
    touchMoved = false;
    touchReleasePending = false;
    touchStartX = lastTouchX = point.x;
    touchStartY = lastTouchY = point.y;
    setTouchDisplay(true, point.x, point.y, pointCount, point.strength);
    lastTouchUiMs = millis();
    Serial.printf("Touch down: %d,%d area=%d points=%d\n", point.x, point.y, point.strength, pointCount);
}

static void updateTouch(const TouchPoint &point, int pointCount, bool rebase)
{
    int32_t dx = static_cast<int32_t>(point.x) - lastTouchX;
    int32_t dy = static_cast<int32_t>(point.y) - lastTouchY;

    // When a second contact appears, the smallest-area point may be the stylus
    // while the previous point was a finger. Rebase instead of jumping across
    // the distance between the two contacts.
    if (rebase) {
        lastTouchX = touchStartX = point.x;
        lastTouchY = touchStartY = point.y;
        touchMoved = false;
        return;
    }

    lastTouchX = point.x;
    lastTouchY = point.y;
    // The diagnostic label and dot are only for visual feedback. Updating
    // them for every GT911 sample forces excessive LVGL/RGB display work and
    // competes with timely USB mouse reports.
    const uint32_t now = millis();
    if (now - lastTouchUiMs >= TOUCH_UI_REFRESH_MS) {
        setTouchDisplay(true, point.x, point.y, pointCount, point.strength);
        lastTouchUiMs = now;
    }

    if (abs(dx) <= TOUCH_DEADZONE_PX) dx = 0;
    if (abs(dy) <= TOUCH_DEADZONE_PX) dy = 0;

    const int32_t totalX = static_cast<int32_t>(point.x) - touchStartX;
    const int32_t totalY = static_cast<int32_t>(point.y) - touchStartY;
    if (abs(totalX) > CLICK_SLOP_PX || abs(totalY) > CLICK_SLOP_PX) {
        touchMoved = true;
    }

    if (dx != 0 || dy != 0) {
        tabletMouse.move(scaleDelta(dx), scaleDelta(dy));
    }
}

static void endTouch()
{
    if (!touchMoved) {
        tabletMouse.click(MOUSE_LEFT);
    }
    // Swipes never press the left button. Release defensively at touch-up so
    // the host cannot remain in a held-button state.
    tabletMouse.release(MOUSE_LEFT);
    // USBHIDMouse::release() may omit a report if its internal state is
    // already zero. move() always transmits the current button bitmap.
    tabletMouse.move(0, 0);
    touchActive = false;
    setTouchDisplay(false);
    Serial.println("Touch up");
}

void setup()
{
    Serial.begin(115200);
    delay(100);
    Serial.println("Starting ESP32-S3 Pen Tablet");

    // Match the proven Spotify sketch's board/display startup order. Starting
    // USB HID first can interfere with native-USB configurations, so bring up
    // the RGB panel and LVGL before enumerating the mouse.
    Serial.println("Initializing Waveshare 4.3B board");
    board = new Board();
    board->init();

#if LVGL_PORT_AVOID_TEARING_MODE
    auto lcd = board->getLCD();
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    auto lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 20);
    }
#endif
#endif

    if (!board->begin()) {
        Serial.println("ERROR: board initialization failed");
        while (true) delay(1000);
    }
    if (board->getTouch() == nullptr) {
        Serial.println("ERROR: touch controller is unavailable");
        while (true) delay(1000);
    }

    // The main loop polls GT911 directly for low-latency relative mouse input;
    // don't register a second LVGL reader for the same controller.
    if (!lvgl_port_init(board->getLCD(), nullptr)) {
        Serial.println("ERROR: LVGL display initialization failed");
        while (true) delay(1000);
    }
    lvgl_port_lock(-1);
    createTabletUi();
    lvgl_port_unlock();

    // Start USB HID only after the board/display are initialized, as the
    // Spotify project does for its board and LVGL startup.
    tabletMouse.begin();
    tabletGamepad.begin();
    USB.begin();
    // Repeated neutral reports clear a button state the PC may have retained
    // when switching from the previous drag firmware.
    neutralReportsUntilMs = millis() + 1500;
    Serial.println("Ready. Waiting for touch input.");
}

void loop()
{
    const uint32_t now = millis();
    if (now - lastTouchPollMs < TOUCH_POLL_MS) {
        delay(1);
        return;
    }
    lastTouchPollMs = now;

    if (!touchActive && now < neutralReportsUntilMs) {
        tabletMouse.move(0, 0);
    }

    TouchPoint points[TOUCH_MAX_POINTS];
    const int count = board->getTouch()->readPoints(points, TOUCH_MAX_POINTS, 0);

    // Require a deliberate four-contact hold, and consume that entire gesture
    // through release so its contacts cannot activate mouse or gamepad inputs.
    if (count >= 4) {
        modeGestureReleasePending = false;
        if (!modeGestureLatched) {
            if (!modeGesturePending) {
                modeGesturePending = true;
                modeGestureStartMs = now;
            } else if (now - modeGestureStartMs >= MODE_GESTURE_HOLD_MS) {
                modeGestureLatched = true;
                modeGesturePending = false;
                setMode(inputMode == InputMode::Mouse ? InputMode::Gamepad : InputMode::Mouse);
            }
        }
    } else if (count == 0) {
        modeGesturePending = false;
        if (modeGestureLatched) {
            if (!modeGestureReleasePending) {
                modeGestureReleasePending = true;
                modeGestureNoPointSinceMs = now;
            } else if (now - modeGestureNoPointSinceMs >= TOUCH_RELEASE_DEBOUNCE_MS) {
                modeGestureLatched = false;
                modeGestureReleasePending = false;
            }
        }
    } else if (!modeGestureLatched) {
        modeGesturePending = false;
        modeGestureReleasePending = false;
    } else {
        modeGestureReleasePending = false;
    }

    if (modeGestureLatched || count >= 4) {
        if (inputMode == InputMode::Gamepad) {
            gamepadX = gamepadY = 0;
            gamepadButtons = 0;
            tabletGamepad.send(0, 0, 0, 0, 0, 0, HAT_CENTER, 0);
        }
        if (touchActive) endTouch();
        return;
    }

    if (inputMode == InputMode::Gamepad) {
        touchActive = false;
        touchReleasePending = false;
        updateGamepad(points, count);
        return;
    }

    TouchPoint point;
    const bool hasPointerContact = selectPointerContact(points, count, point);
    if (hasPointerContact) {
        touchReleasePending = false;
        if (!touchActive) {
            beginTouch(point, count);
        } else {
            const bool rebase = count > 1 && abs(static_cast<int>(point.x) - lastTouchX) +
                abs(static_cast<int>(point.y) - lastTouchY) > MULTITOUCH_REBASE_DELTA_PX;
            updateTouch(point, count, rebase);
        }
    } else if (touchActive) {
        // The controller reports at about 100 Hz, while this loop polls at
        // 125 Hz. A single empty read can be an inter-report gap, not a lift.
        // Debounce release so those gaps cannot create repeated tap clicks.
        if (!touchReleasePending) {
            touchReleasePending = true;
            touchNoPointSinceMs = now;
        } else if (now - touchNoPointSinceMs >= TOUCH_RELEASE_DEBOUNCE_MS) {
            touchReleasePending = false;
            endTouch();
        }
    } else if (count == 0) {
        multipleContactsSeen = false;
        activeContactArea = 0;
    }
}
