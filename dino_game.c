// Offline Dinosaur Game (Part 2) - Keypad pause/resume, pushbuttons jump
// Uses PmodOLED for graphics, PmodKYPD for input, board RGB LED for state.

#include "FreeRTOS.h"
#include "task.h"

#include "xparameters.h"
#include "xgpio.h"
#include "xil_printf.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pmodkypd.h"
#include "PmodOLED.h"
#include "OLEDControllerCustom.h"

// ----------------------- Device config -----------------------

#define BTN_DEVICE_ID        XPAR_GPIO_INPUTS_BASEADDR
#define KYPD_DEVICE_ID       XPAR_GPIO_KEYPAD_BASEADDR
#define BTN_CHANNEL          1

#define RGB_LED_ADDR         XPAR_GPIO_LEDS_BASEADDR
#define RGB_CHANNEL          2

#define LED_RED              0x4
#define LED_GREEN            0x2
#define LED_BLUE             0x1
#define LED_OFF              0x0

// keypad key table (matches the provided example)
#define DEFAULT_KEYTABLE    "0FED789C456B123A"

// Set these to match the physical PmodOLED orientation and color scheme.
// Per your note, orientation=0x01 and invert=0x01 fix color + upside-down issues.
static const u8 orientation = 0x1;
static const u8 invert = 0x1;

// ----------------------- Game constants -----------------------

#define FRAME_DELAY_MS       30

// Make sprites larger and use more of the screen area.
// (OLED coords: x in [0..127], y in [0..31])
#define DINO_X               55
#define DINO_W               6
#define DINO_H               5

// Put the ground higher so vertical space above is used.
#define GROUND_Y             26  // baseline ground pixel row

#define OBSTACLE_W          4

#define JUMP_VEL_INITIAL    (-10)
#define GRAVITY             (1)

// ----------------------- Shared state -----------------------

static PmodOLED oledDevice;
static PmodKYPD KYPDInst;
static XGpio btnInst;
static XGpio rgbLedInst;

static volatile int g_paused = 0;
static volatile int g_game_over = 0;
static volatile int g_jump_request = 0;

static unsigned int g_score = 0;

// DINO physics state
static int dino_y = GROUND_Y - DINO_H;
static int dino_vel = 0;

// obstacle state (single obstacle keeps it simple)
static int obs_x = OledColMax;
static int obs_h = 2;
static int obs_passed = 0;
static int obs_speed = 1;

static void reset_game(void);
static int rects_overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh);

// ----------------------- Drawing helpers -----------------------

static void draw_ground(void)
{
    OLED_MoveTo(&oledDevice, 0, GROUND_Y);
    OLED_DrawLineTo(&oledDevice, OledColMax - 1, GROUND_Y);
    // Thicker ground line for better visibility on the small display
    if (GROUND_Y + 1 < OledRowMax) {
        OLED_MoveTo(&oledDevice, 0, GROUND_Y + 1);
        OLED_DrawLineTo(&oledDevice, OledColMax - 1, GROUND_Y + 1);
    }
    if (GROUND_Y + 2 < OledRowMax) {
        OLED_MoveTo(&oledDevice, 0, GROUND_Y + 2);
        OLED_DrawLineTo(&oledDevice, OledColMax - 1, GROUND_Y + 2);
    }
}

static void draw_dino(void)
{
    OLED_MoveTo(&oledDevice, DINO_X, dino_y);
    OLED_RectangleTo(&oledDevice, DINO_X + DINO_W, dino_y + DINO_H);
}

static void draw_obstacle(void)
{
    const int obs_y = GROUND_Y - obs_h;
    OLED_MoveTo(&oledDevice, obs_x, obs_y);
    OLED_RectangleTo(&oledDevice, obs_x + OBSTACLE_W, obs_y + obs_h);
}

static void draw_ui(void)
{
    char buf[20];
    OLED_SetCursor(&oledDevice, 0, 0);
    sprintf(buf, "Score:%u", g_score);
    OLED_PutString(&oledDevice, buf);

    if (g_game_over) {
        OLED_SetCursor(&oledDevice, 0, 2);
        OLED_PutString(&oledDevice, "GAME OVER");
    } else if (g_paused) {
        OLED_SetCursor(&oledDevice, 0, 2);
        OLED_PutString(&oledDevice, "PAUSED");
    }
}

static void update_led(void)
{
    // Active = green, Game over = red, Paused = off
    if (g_game_over) {
        XGpio_DiscreteWrite(&rgbLedInst, RGB_CHANNEL, LED_RED);
    } else if (g_paused) {
        XGpio_DiscreteWrite(&rgbLedInst, RGB_CHANNEL, LED_OFF);
    } else {
        XGpio_DiscreteWrite(&rgbLedInst, RGB_CHANNEL, LED_GREEN);
    }
}

// ----------------------- Game logic -----------------------

static void reset_game(void)
{
    g_score = 0;
    g_paused = 0;
    g_game_over = 0;
    g_jump_request = 0;

    dino_y = GROUND_Y - DINO_H;
    dino_vel = 0;

    obs_x = OledColMax + 10;
    obs_h = 3 + (rand() % 6); // obstacle height range
    if (obs_h > 4){
        obs_h = 4;
    }
    obs_h = obs_h/2;
    if (obs_h > (GROUND_Y - 2)) obs_h = GROUND_Y - 2;
    obs_passed = 0;
    obs_speed = 3;
}

static int rects_overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    // AABB overlap check
    return !(ax + aw < bx || bx + bw < ax || ay + ah < by || by + bh < ay);
}

// ----------------------- Tasks -----------------------

static void dino_keypadTask(void *pvParameters)
{
    u16 keystate;
    XStatus status, last_status = KYPD_NO_KEY;
    u8 new_key = '0';

    const TickType_t xDelay = pdMS_TO_TICKS(25);

    // Key meanings (from DEFAULT_KEYTABLE)
    // '5' toggles pause/resume
    // '0' resets when game over
    while (1) {
        keystate = KYPD_getKeyStates(&KYPDInst);
        status = KYPD_getKeyPressed(&KYPDInst, keystate, &new_key);

        if (status == KYPD_SINGLE_KEY && last_status == KYPD_NO_KEY) {
            if (new_key == '5') {
                // Demo convenience:
                // - During play: toggle pause/resume
                // - During game over: reset
                if (g_game_over) {
                    reset_game();
                } else {
                    g_paused = !g_paused;
                }
            } else if (new_key == '0') {
                // Always allow reset
                if (g_game_over) {
                    reset_game();
                }
            }
        }

        last_status = status;
        vTaskDelay(xDelay);
    }
}

static void dino_buttonTask(void *pvParameters)
{
    u32 prev_val = 0;
    while (1) {
        u32 button_val = XGpio_DiscreteRead(&btnInst, BTN_CHANNEL);

        // Treat BOTH transitions as "button pressed" so it works for either
        // active-high or active-low button wiring.
        if (button_val != prev_val) {
            if (g_game_over) {
                reset_game();
            } else {
                // Only allow a jump when on the ground.
                if (dino_y >= (GROUND_Y - DINO_H)) {
                    g_jump_request = 1;
                }
            }
        }

        prev_val = button_val;
        vTaskDelay(pdMS_TO_TICKS(50)); // debounce
    }
}

static void dino_oledTask(void *pvParameters)
{
    // Keep text rendering stable
    OLED_SetDrawMode(&oledDevice, 0);
    OLED_SetCharUpdate(&oledDevice, 0);

    while (1) {
        if (!g_paused && !g_game_over) {
            const int dino_ground_y = GROUND_Y - DINO_H;

            // Jump if requested and on ground
            if (g_jump_request) {
                if (dino_y >= dino_ground_y) {
                    dino_y = dino_ground_y;
                    dino_vel = JUMP_VEL_INITIAL;
                }
                g_jump_request = 0;
            }

            // Physics step
            dino_vel += GRAVITY;
            dino_y += dino_vel;

            if (dino_y >= dino_ground_y) {
                dino_y = dino_ground_y;
                dino_vel = 0;
            }
            if (dino_y < 0) {
                dino_y = 0;
                dino_vel = 0;
            }

            // Obstacle update
            obs_x -= obs_speed;

            // Score when obstacle passes the dino
            if (!obs_passed && (obs_x + OBSTACLE_W) < DINO_X) {
                obs_passed = 1;
                g_score++;

                // Mild difficulty scaling
                if (g_score % 10 == 0 && obs_speed < 8) {
                    obs_speed++;
                }
            }

            // Respawn when obstacle leaves screen
            if ((obs_x + OBSTACLE_W) < 0) {
                obs_x = OledColMax + 10;
                obs_passed = 0;
                obs_h = 10 + (rand() % 12);
                if (obs_h > (GROUND_Y - 2)) obs_h = GROUND_Y - 2;
            }

            // Collision check
            const int obs_y = GROUND_Y - obs_h;
            if (rects_overlap(DINO_X, dino_y, DINO_W, DINO_H,
                               obs_x, obs_y, OBSTACLE_W, obs_h)) {
                g_game_over = 1;
            }
        }

        update_led();

        // Draw frame (even when paused / game over)
        // Fill the *entire* OLED framebuffer using the library's fill rectangle
        // (so there is no "unused" region).
        OLED_SetDrawMode(&oledDevice, 0);                       // modOledSet
        OLED_SetFillPattern(&oledDevice, OLED_GetStdPattern(1)); // all-ones fill pattern
        OLED_ClearBuffer(&oledDevice);
        OLED_MoveTo(&oledDevice, 0, 0);
        OLED_FillRect(&oledDevice, OledColMax - 1, OledRowMax - 1);

        // Draw game elements on top (using opposite pixel value to make them visible).
        OLED_SetDrawColor(&oledDevice, 0);
        draw_ground();
        draw_dino();
        if (!g_game_over || 1) { // still draw obstacle for "frozen" look
            draw_obstacle();
        }
        draw_ui();
        OLED_Update(&oledDevice);

        vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
    }
}

int DinoGameMain(void)
{
    // RNG seed
    srand(1);

    // Init keypad
    KYPD_begin(&KYPDInst, KYPD_DEVICE_ID);
    KYPD_loadKeyTable(&KYPDInst, (u8 *)DEFAULT_KEYTABLE);

    // Init OLED
    OLED_Begin(&oledDevice,
               XPAR_GPIO_OLED_BASEADDR,
               XPAR_SPI_OLED_BASEADDR,
               orientation,
               invert);

    // Init buttons
    if (XGpio_Initialize(&btnInst, BTN_DEVICE_ID) != XST_SUCCESS) {
        xil_printf("GPIO init failed (buttons)\r\n");
        return XST_FAILURE;
    }
    XGpio_SetDataDirection(&btnInst, BTN_CHANNEL, 0xFF);

    // Init RGB LED output
    if (XGpio_Initialize(&rgbLedInst, RGB_LED_ADDR) != XST_SUCCESS) {
        xil_printf("GPIO init failed (RGB)\r\n");
        return XST_FAILURE;
    }
    XGpio_SetDataDirection(&rgbLedInst, RGB_CHANNEL, 0x0);
    XGpio_DiscreteWrite(&rgbLedInst, RGB_CHANNEL, LED_OFF);

    // Reset game state
    reset_game();

    // Create tasks
    xTaskCreate(dino_keypadTask, "dino_keypad",  configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);
    xTaskCreate(dino_oledTask,   "dino_oled",    2048,                    NULL, tskIDLE_PRIORITY, NULL);
    xTaskCreate(dino_buttonTask, "dino_buttons", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);

    vTaskStartScheduler();

    while (1) { }
    return XST_SUCCESS;
}
