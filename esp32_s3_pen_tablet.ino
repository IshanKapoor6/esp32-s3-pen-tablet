#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include <USB.h>
#include <USBHIDMouse.h>

using namespace esp_panel::drivers;
using namespace esp_panel::board;

// Relative USB HID motion makes the touchscreen behave like a mousepad.
static constexpr uint16_t TOUCH_POLL_MS = 8;
static constexpr uint16_t TOUCH_UI_REFRESH_MS = 33;
static constexpr uint16_t TOUCH_RELEASE_DEBOUNCE_MS = 40;
static constexpr uint8_t TOUCH_DEADZONE_PX = 1;
static constexpr uint8_t CLICK_SLOP_PX = 8;
static constexpr int8_t POINTER_SENSITIVITY = 2;
static constexpr int16_t LCD_WIDTH = 800;
static constexpr int16_t LCD_HEIGHT = 480;

USBHIDMouse tabletMouse;
Board *board = nullptr;
lv_obj_t *statusLabel = nullptr;
lv_obj_t *touchDot = nullptr;
bool touchActive = false;
bool touchMoved = false;
int16_t touchStartX = 0;
int16_t touchStartY = 0;
int16_t lastTouchX = 0;
int16_t lastTouchY = 0;
uint32_t lastTouchPollMs = 0;
uint32_t lastTouchUiMs = 0;
uint32_t neutralReportsUntilMs = 0;
uint32_t touchNoPointSinceMs = 0;
bool touchReleasePending = false;

static int8_t scaleDelta(int32_t delta)
{
    const int32_t scaled = delta * POINTER_SENSITIVITY;
    if (scaled < -127) return -127;
    if (scaled > 127) return 127;
    return static_cast<int8_t>(scaled);
}

static void setTouchDisplay(bool touched, int16_t x = 0, int16_t y = 0)
{
    if (statusLabel == nullptr || touchDot == nullptr) return;
    if (!lvgl_port_lock(1000)) return;

    if (touched) {
        lv_label_set_text_fmt(statusLabel, "TOUCH DETECTED\nX: %d    Y: %d", x, y);
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
}

static void beginTouch(const TouchPoint &point)
{
    touchActive = true;
    touchMoved = false;
    touchReleasePending = false;
    touchStartX = lastTouchX = point.x;
    touchStartY = lastTouchY = point.y;
    setTouchDisplay(true, point.x, point.y);
    lastTouchUiMs = millis();
    Serial.printf("Touch down: %d,%d\n", point.x, point.y);
}

static void updateTouch(const TouchPoint &point)
{
    int32_t dx = static_cast<int32_t>(point.x) - lastTouchX;
    int32_t dy = static_cast<int32_t>(point.y) - lastTouchY;
    lastTouchX = point.x;
    lastTouchY = point.y;
    // The diagnostic label and dot are only for visual feedback. Updating
    // them for every GT911 sample forces excessive LVGL/RGB display work and
    // competes with timely USB mouse reports.
    const uint32_t now = millis();
    if (now - lastTouchUiMs >= TOUCH_UI_REFRESH_MS) {
        setTouchDisplay(true, point.x, point.y);
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

    TouchPoint point;
    const int count = board->getTouch()->readPoints(&point, 1, 0);
    if (count > 0) {
        touchReleasePending = false;
        if (!touchActive) {
            beginTouch(point);
        } else {
            updateTouch(point);
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
    }
}
