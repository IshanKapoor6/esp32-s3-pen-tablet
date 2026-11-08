#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include <USB.h>
#include <USBHIDMouse.h>
#include <USBHIDGamepad.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include "wallpaper_image.h"

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
    (1UL << BUTTON_X) | (1UL << BUTTON_Y) | (1UL << BUTTON_TL) | (1UL << BUTTON_TR) |
    (1UL << BUTTON_SELECT) | (1UL << BUTTON_START) | (1UL << BUTTON_THUMBL) |
    (1UL << BUTTON_THUMBR);

USBHIDMouse tabletMouse;
USBHIDGamepad tabletGamepad;
Board *board = nullptr;
lv_obj_t *statusLabel = nullptr;
lv_obj_t *touchDot = nullptr;
lv_obj_t *mouseScreen = nullptr;
lv_obj_t *gamepadScreen = nullptr;
lv_obj_t *gamepadGestureLabel = nullptr;
lv_obj_t *wallpaperObject = nullptr;
static uint16_t *wallpaperPixels = nullptr;
static lv_img_dsc_t wallpaperImage = {};
bool wallpaperActive = false;
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
int8_t gamepadZ = 0;
int8_t gamepadRz = 0;
int8_t gamepadLeftTrigger = 0;
int8_t gamepadRightTrigger = 0;
uint8_t gamepadHat = HAT_CENTER;
bool gamepadReleasePending = false;
uint32_t gamepadNoPointSinceMs = 0;
bool leftStickTouchActive = false;
bool rightStickTouchActive = false;
int16_t leftStickLastX = 0;
int16_t leftStickLastY = 0;
int16_t rightStickLastX = 0;
int16_t rightStickLastY = 0;
bool modeGesturePending = false;
bool modeGestureLatched = false;
uint32_t modeGestureStartMs = 0;
uint32_t modeGestureLastSeenMs = 0;
bool modeGestureReleasePending = false;
uint32_t modeGestureNoPointSinceMs = 0;
uint32_t lastGestureUiMs = 0;

static int8_t scaleDelta(int32_t delta)
{
    const int32_t scaled = delta * POINTER_SENSITIVITY;
    if (scaled < -127) return -127;
    if (scaled > 127) return 127;
    return static_cast<int8_t>(scaled);
}

static bool wallpaperJpegOutput(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t *pixels)
{
    if (wallpaperPixels == nullptr || x < 0 || y < 0 || x + width > LCD_WIDTH || y + height > LCD_HEIGHT) {
        return false;
    }
    for (uint16_t row = 0; row < height; ++row) {
        memcpy(wallpaperPixels + (static_cast<size_t>(y + row) * LCD_WIDTH) + x,
               pixels + (static_cast<size_t>(row) * width),
               static_cast<size_t>(width) * sizeof(uint16_t));
    }
    return true;
}

static bool loadWallpaperImage()
{
    wallpaperPixels = static_cast<uint16_t *>(heap_caps_malloc(
        static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (wallpaperPixels == nullptr) {
        Serial.println("Wallpaper image: PSRAM allocation failed");
        return false;
    }

    TJpgDec.setCallback(wallpaperJpegOutput);
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(false);
    if (TJpgDec.drawJpg(0, 0, wallpaperJpg, wallpaperJpgLength) != JDR_OK) {
        Serial.println("Wallpaper image: JPEG decode failed");
        heap_caps_free(wallpaperPixels);
        wallpaperPixels = nullptr;
        return false;
    }

    wallpaperImage.header.cf = LV_IMG_CF_TRUE_COLOR;
    wallpaperImage.header.always_zero = 0;
    wallpaperImage.header.w = LCD_WIDTH;
    wallpaperImage.header.h = LCD_HEIGHT;
    wallpaperImage.data_size = static_cast<uint32_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t);
    wallpaperImage.data = reinterpret_cast<const uint8_t *>(wallpaperPixels);
    wallpaperActive = true;
    Serial.printf("Wallpaper image loaded (%u bytes JPEG)\n", wallpaperJpgLength);
    return true;
}

static void setTouchDisplay(bool touched, int16_t x = 0, int16_t y = 0, int count = 0, int area = 0)
{
    if (wallpaperActive) return;
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

    if (wallpaperActive) {
        wallpaperObject = lv_img_create(screen);
        lv_img_set_src(wallpaperObject, &wallpaperImage);
        lv_obj_set_pos(wallpaperObject, 0, 0);
    }

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "ESP32-S3 PEN TABLET");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF3F4F6), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);
    if (wallpaperActive) lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);

    statusLabel = lv_label_create(screen);
    lv_label_set_text(statusLabel, "WAITING FOR TOUCH\nTouch the screen to test the GT911 sensor");
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(0x67E8F9), LV_PART_MAIN);
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(statusLabel, LV_ALIGN_CENTER, 0, 0);
    if (wallpaperActive) lv_obj_add_flag(statusLabel, LV_OBJ_FLAG_HIDDEN);

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
    gamepadGestureLabel = lv_label_create(gamepadScreen);
    lv_label_set_text(gamepadGestureLabel, "Touch controls to answer Steam setup prompts");
    lv_obj_set_style_text_color(gamepadGestureLabel, lv_color_hex(0x67E8F9), LV_PART_MAIN);
    lv_obj_align(gamepadGestureLabel, LV_ALIGN_TOP_MID, 0, 58);

    const auto addControl = [](const char *name, int x, int y, int width, int height, uint32_t color, bool round) {
        lv_obj_t *control = lv_obj_create(gamepadScreen);
        lv_obj_set_size(control, width, height);
        lv_obj_set_style_radius(control, round ? LV_RADIUS_CIRCLE : 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(control, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_border_color(control, lv_color_hex(0x64748B), LV_PART_MAIN);
        lv_obj_set_style_border_width(control, 1, LV_PART_MAIN);
        lv_obj_set_pos(control, x, y);
        lv_obj_t *label = lv_label_create(control);
        lv_label_set_text(label, name);
        lv_obj_set_style_text_color(label, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
        lv_obj_center(label);
    };

    // Standard landscape gamepad layout. The touch hit regions below match
    // these screen controls so Steam's setup prompts can be answered directly.
    addControl("LT", 35, 76, 105, 42, 0x334155, false);
    addControl("LB", 146, 76, 105, 42, 0x475569, false);
    addControl("SELECT", 310, 76, 82, 42, 0x334155, false);
    addControl("START", 408, 76, 82, 42, 0x334155, false);
    addControl("RB", 549, 76, 105, 42, 0x475569, false);
    addControl("RT", 660, 76, 105, 42, 0x334155, false);

    addControl("L STICK", 55, 226, 174, 174, 0x263449, true);
    addControl("UP", 304, 216, 52, 44, 0x334155, true);
    addControl("LEFT", 250, 270, 52, 44, 0x334155, true);
    addControl("DOWN", 304, 324, 52, 44, 0x334155, true);
    addControl("RIGHT", 358, 270, 52, 44, 0x334155, true);
    addControl("R STICK", 432, 226, 174, 174, 0x263449, true);

    addControl("Y", 678, 216, 54, 54, 0x3B82F6, true);
    addControl("X", 618, 276, 54, 54, 0x10B981, true);
    addControl("B", 738, 276, 54, 54, 0xF59E0B, true);
    addControl("A", 678, 336, 54, 54, 0xEF4444, true);
}

static void setGestureDisplay(int count, uint32_t heldMs)
{
    const uint32_t now = millis();
    if (lastGestureUiMs != 0 && now - lastGestureUiMs < 100) return;
    lastGestureUiMs = now;
    if (!lvgl_port_lock(1000)) return;

    char message[96];
    snprintf(message, sizeof(message), "TOUCH CONTACTS: %d\nHold four to switch: %.1f / 0.7 sec",
             count, heldMs / 1000.0f);
    if (inputMode == InputMode::Mouse) {
        lv_label_set_text(statusLabel, message);
        lv_obj_add_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
    } else if (gamepadGestureLabel != nullptr) {
        lv_label_set_text(gamepadGestureLabel, message);
    }

    lvgl_port_unlock();
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
    gamepadZ = gamepadRz = 0;
    gamepadLeftTrigger = gamepadRightTrigger = 0;
    gamepadHat = HAT_CENTER;
    gamepadReleasePending = false;
    leftStickTouchActive = false;
    rightStickTouchActive = false;
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
    int32_t stickX = 0, stickY = 0, rightStickX = 0, rightStickY = 0;
    uint32_t buttons = 0;
    bool hasStick = false, hasRightStick = false;
    bool leftTrigger = false, rightTrigger = false;
    uint8_t hat = HAT_CENTER;

    // The GT911 API does not expose contact IDs, so keep each virtual stick
    // attached to the nearest point from its previous sample. A finger that
    // starts on a stick remains captured even after leaving the drawn circle.
    int leftStickPoint = -1;
    int rightStickPoint = -1;
    bool pointUsed[TOUCH_MAX_POINTS] = {};
    const auto findTrackedPoint = [&](int16_t lastX, int16_t lastY) {
        int best = -1;
        int32_t bestDistance = 250 * 250;
        for (int i = 0; i < count; ++i) {
            if (pointUsed[i]) continue;
            const int32_t dx = points[i].x - lastX;
            const int32_t dy = points[i].y - lastY;
            const int32_t distance = dx * dx + dy * dy;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = i;
            }
        }
        return best;
    };

    if (leftStickTouchActive) {
        leftStickPoint = findTrackedPoint(leftStickLastX, leftStickLastY);
        if (leftStickPoint >= 0) pointUsed[leftStickPoint] = true;
        else leftStickTouchActive = false;
    }
    if (rightStickTouchActive) {
        rightStickPoint = findTrackedPoint(rightStickLastX, rightStickLastY);
        if (rightStickPoint >= 0) pointUsed[rightStickPoint] = true;
        else rightStickTouchActive = false;
    }

    // New stick contacts start anywhere within the visible circular zones.
    // Captured contacts are then allowed to travel beyond those zones.
    for (int i = 0; i < count; ++i) {
        if (pointUsed[i]) continue;
        const int32_t lx = points[i].x - 142;
        const int32_t ly = points[i].y - 313;
        if (!leftStickTouchActive && lx * lx + ly * ly <= 96 * 96) {
            leftStickTouchActive = true;
            leftStickPoint = i;
            leftStickLastX = points[i].x;
            leftStickLastY = points[i].y;
            pointUsed[i] = true;
            continue;
        }
        const int32_t rx = points[i].x - 519;
        const int32_t ry = points[i].y - 313;
        if (!rightStickTouchActive && rx * rx + ry * ry <= 96 * 96) {
            rightStickTouchActive = true;
            rightStickPoint = i;
            rightStickLastX = points[i].x;
            rightStickLastY = points[i].y;
            pointUsed[i] = true;
        }
    }

    if (leftStickPoint >= 0) {
        leftStickLastX = points[leftStickPoint].x;
        leftStickLastY = points[leftStickPoint].y;
        const int32_t dx = leftStickLastX - 142;
        const int32_t dy = leftStickLastY - 313;
        if (max(abs(dx), abs(dy)) <= 24) buttons |= 1UL << BUTTON_THUMBL;
        else {
            stickX = constrain(dx * 127 / 82, -127, 127);
            stickY = constrain(dy * 127 / 82, -127, 127);
            hasStick = true;
        }
    }
    if (rightStickPoint >= 0) {
        rightStickLastX = points[rightStickPoint].x;
        rightStickLastY = points[rightStickPoint].y;
        const int32_t dx = rightStickLastX - 519;
        const int32_t dy = rightStickLastY - 313;
        if (max(abs(dx), abs(dy)) <= 24) buttons |= 1UL << BUTTON_THUMBR;
        else {
            rightStickX = constrain(dx * 127 / 82, -127, 127);
            rightStickY = constrain(dy * 127 / 82, -127, 127);
            hasRightStick = true;
        }
    }

    for (int i = 0; i < count; ++i) {
        if (pointUsed[i]) continue;
        const int16_t x = points[i].x;
        const int16_t y = points[i].y;
        if (y >= 70 && y < 125) {
            if (x >= 25 && x < 145) leftTrigger = true;
            else if (x >= 145 && x < 255) buttons |= 1UL << BUTTON_TL;
            else if (x >= 545 && x < 655) buttons |= 1UL << BUTTON_TR;
            else if (x >= 655 && x < 780) rightTrigger = true;
            else if (x >= 300 && x < 400) buttons |= 1UL << BUTTON_SELECT;
            else if (x >= 400 && x < 500) buttons |= 1UL << BUTTON_START;
        } else if (x >= 245 && x < 415 && y >= 210 && y < 375) {
            const int16_t dx = x - 330;
            const int16_t dy = y - 292;
            const int16_t ax = abs(dx), ay = abs(dy);
            if (ax < 22 && ay < 22) hat = HAT_CENTER;
            else if (ay > ax * 2) hat = dy < 0 ? HAT_UP : HAT_DOWN;
            else if (ax > ay * 2) hat = dx < 0 ? HAT_LEFT : HAT_RIGHT;
            else if (dx < 0 && dy < 0) hat = HAT_UP_LEFT;
            else if (dx > 0 && dy < 0) hat = HAT_UP_RIGHT;
            else if (dx < 0 && dy > 0) hat = HAT_DOWN_LEFT;
            else hat = HAT_DOWN_RIGHT;
        } else if (x >= 650 && x < 760 && y >= 205 && y < 275) buttons |= 1UL << BUTTON_Y;
        else if (x >= 600 && x < 690 && y >= 270 && y < 335) buttons |= 1UL << BUTTON_X;
        else if (x >= 720 && x < 800 && y >= 270 && y < 335) buttons |= 1UL << BUTTON_B;
        else if (x >= 650 && x < 760 && y >= 330 && y < 400) buttons |= 1UL << BUTTON_A;
    }
    gamepadX = hasStick ? static_cast<int8_t>(stickX) : 0;
    gamepadY = hasStick ? static_cast<int8_t>(stickY) : 0;
    gamepadZ = hasRightStick ? static_cast<int8_t>(rightStickX) : 0;
    gamepadRz = hasRightStick ? static_cast<int8_t>(rightStickY) : 0;
    gamepadLeftTrigger = leftTrigger ? 127 : 0;
    gamepadRightTrigger = rightTrigger ? -127 : 0;
    gamepadHat = hat;
    gamepadButtons = buttons & GAMEPAD_BUTTON_MASK;
    tabletGamepad.send(gamepadX, gamepadY, gamepadZ, gamepadRz,
                       gamepadLeftTrigger, gamepadRightTrigger, gamepadHat, gamepadButtons);
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
    loadWallpaperImage();
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
        modeGestureLastSeenMs = now;
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
    } else {
        // readPoints() consumes controller samples that arrive at about 100 Hz,
        // while this loop polls faster. Preserve a pending hold through those
        // brief empty or partial frames, but cancel if four contacts stay away.
        if (modeGesturePending && !modeGestureLatched &&
            now - modeGestureLastSeenMs >= TOUCH_RELEASE_DEBOUNCE_MS * 2) {
            modeGesturePending = false;
        }

        if (modeGestureLatched && count == 0) {
            if (!modeGestureReleasePending) {
                modeGestureReleasePending = true;
                modeGestureNoPointSinceMs = now;
            } else if (now - modeGestureNoPointSinceMs >= TOUCH_RELEASE_DEBOUNCE_MS) {
                modeGestureLatched = false;
                modeGestureReleasePending = false;
            }
        } else if (modeGestureLatched) {
            modeGestureReleasePending = false;
        }
    }

    if (modeGestureLatched || count >= 4) {
        if (touchActive) endTouch();
        const uint32_t heldMs = modeGesturePending ? now - modeGestureStartMs : MODE_GESTURE_HOLD_MS;
        setGestureDisplay(count, heldMs);
        if (inputMode == InputMode::Gamepad) {
            gamepadX = gamepadY = 0;
            gamepadButtons = 0;
            tabletGamepad.send(0, 0, 0, 0, 0, 0, HAT_CENTER, 0);
        }
        return;
    }

    if (inputMode == InputMode::Gamepad) {
        touchActive = false;
        touchReleasePending = false;
        if (count > 0) {
            gamepadReleasePending = false;
            updateGamepad(points, count);
        } else if (!gamepadReleasePending) {
            // GT911 reports at about 100 Hz while this loop polls every 8 ms.
            // Keep the last gamepad state across inter-report gaps so stick
            // axes and held buttons do not flicker back to neutral.
            gamepadReleasePending = true;
            gamepadNoPointSinceMs = now;
        } else if (now - gamepadNoPointSinceMs >= TOUCH_RELEASE_DEBOUNCE_MS) {
            gamepadReleasePending = false;
            updateGamepad(points, 0);
        }
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
