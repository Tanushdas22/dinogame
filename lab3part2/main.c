// Include FreeRTOS Libraries
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

// Include xilinx Libraries
#include "xparameters.h"
#include "xgpio.h"
#include "xscugic.h"
#include "xil_exception.h"
#include "xil_printf.h"
#include "xil_cache.h"

// Other miscellaneous libraries
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include "pmodkypd.h"
#include "sleep.h"
#include "PmodOLED.h"
#include "OLEDControllerCustom.h"

// Dino game entry (implemented in dino_game.c)
int DinoGameMain(void);


#define BTN_DEVICE_ID  XPAR_GPIO_INPUTS_BASEADDR
#define KYPD_DEVICE_ID XPAR_GPIO_KEYPAD_BASEADDR
#define KYPD_BASE_ADDR XPAR_GPIO_KEYPAD_BASEADDR
#define BTN_CHANNEL    1

// RGB LED (3rd peripheral device for Part 2)
#define RGB_LED_ADDR             XPAR_GPIO_LEDS_BASEADDR
#define RGB_CHANNEL              2

#define LED_RED                  0x4
#define LED_GREEN                0x2
#define LED_BLUE                 0x1
#define LED_OFF                  0x0


#define FRAME_DELAY 50000

// keypad key table
#define DEFAULT_KEYTABLE 	"0FED789C456B123A"

// Declaring the devices
XGpio btnInst;
XGpio rgbLed;
PmodOLED oledDevice;
PmodKYPD 	KYPDInst;

// Function prototypes
void InitializeKeypad();
void initializeScreen();
static void keypadTask( void *pvParameters );
static void oledTask( void *pvParameters );
static void buttonTask( void *pvParameters );
int grphClampXco(int xco);
int grphClampYco(int yco);
int grphAbs(int foo);
void OLED_DrawLineTo(PmodOLED *InstancePtr, int xco, int yco);
void OLED_getPos(PmodOLED *InstancePtr, int *pxco, int *pyco);
void drawTarget(u8 targetX, u8 targetY, u8 width, u8 length);



const u8 orientation = 0x0; // Set up for Normal PmodOLED(false) vs normal
                            // Onboard OLED(true)
const u8 invert = 0x1; // true = whitebackground/black letters
                       // false = black background /white letters
u8 keypad_val = 'x';
u8 size = 8, origin = 0;
u8 aimx=0, aimy=0;
u8 targetx=30, targety=5;
u8 targetWidth=8, targetLength=15;
int score = 0, lives = 3;

int main()
{
	// Use the requested Part 2 offline dinosaur game.
	return DinoGameMain();
}


void InitializeKeypad()
{
   KYPD_begin(&KYPDInst, KYPD_BASE_ADDR);
   KYPD_loadKeyTable(&KYPDInst, (u8*) DEFAULT_KEYTABLE);
}


static void keypadTask( void *pvParameters )
{
   u16 keystate;
   u8 flag = 0;
   XStatus status, last_status = KYPD_NO_KEY;
   u8 new_key = 'x';

   const TickType_t xDelay = 25 / portTICK_RATE_MS;

   xil_printf("Pmod KYPD app started. Press any key on the Keypad.\r\n");
   while (1) {
	  // Capture state of the keypad
	  keystate = KYPD_getKeyStates(&KYPDInst);

	  // Determine which single key is pressed, if any
	  // if a key is pressed, store the value of the new key in new_key
	  status = KYPD_getKeyPressed(&KYPDInst, keystate, &new_key);

	  // Print key detect if a new key is pressed or if status has changed
	  if (status == KYPD_SINGLE_KEY){
	  } else if (status == KYPD_MULTI_KEY && status != last_status){
		 xil_printf("Error: Multiple keys pressed\r\n");
	  } else if (status == KYPD_NO_KEY && last_status == KYPD_NO_KEY ){
		  new_key = '0';
	  }

	  last_status = status;
	  keypad_val = new_key;
	  if (keypad_val == '4'){
		  flag = !flag;
		  if(aimy > 0 && flag) {
			  aimy-=1;
		  }
		  if(origin > 0 && flag){
			  origin--;
		  }
	  } else if (keypad_val == '6'){
		  flag = !flag;
		  if(aimy < (OledRowMax - size - 1) && flag) {
			  aimy+=1;
		  }
		  if(origin < (OledRowMax - size - 1) && flag) {
			  origin++;
		  }
	  } else if (keypad_val == '2'){
		  flag = !flag;
		  if(aimx < (OledColMax - 1) && flag) {
			  aimx+=1;
		  }
	  } else if (keypad_val == '5'){
		  flag = !flag;
	  } else if (keypad_val == '8'){
		  flag = !flag;
		  if(aimx > 0 && flag) {
			  aimx-=1;
		  }
	  }
	  vTaskDelay(xDelay); // Scanning Delay
   }
}


void drawCrossHair(u8 xco, u8 yco)
{
	OLED_MoveTo(&oledDevice, xco, 0);
	OLED_DrawLineTo(&oledDevice, xco, OledRowMax - 1);
	OLED_MoveTo(&oledDevice, 0, yco);
	OLED_DrawLineTo(&oledDevice, OledColMax - 1, yco);
}


void drawTarget(u8 targetx, u8 targety, u8 width, u8 length)
{
	if (targetx > OledColMax - 1){
		targetx = 0;
	}
	if (targety > OledRowMax - 1){
		targety = 0;
	}
	if (targetx + length > OledColMax - 1){
		targetLength = OledColMax - 1;
	}
	if (targety + width > OledRowMax - 1){
		targetWidth = OledRowMax - 1;
	}
	OLED_MoveTo(&oledDevice, targetx, targety);
	OLED_RectangleTo(&oledDevice, targetx + length, targety + width);
}


void checkShot(void)
{
	if (aimx >= targetx && aimx < (targetx + targetLength) && aimy >= targety && aimy < (targety + targetWidth) ){
		score++;
		xil_printf("hit!, score: %d\n", score);
		// Indicate a hit on the RGB LED
		XGpio_DiscreteWrite(&rgbLed, RGB_CHANNEL, LED_GREEN);
		OLED_ClearBuffer(&oledDevice);
		targetx = rand() % OledColMax;
		targety = rand() % OledRowMax;
		targetLength = rand() % 20;
		targetWidth = rand() % 10;
	} else {
		lives--;
		xil_printf("missed, lives: %d\n", lives);
		// Indicate a miss on the RGB LED
		XGpio_DiscreteWrite(&rgbLed, RGB_CHANNEL, LED_RED);
	}
}


static void oledTask( void *pvParameters )
{
	u8 buttonVal = 0;
	char temp[10];
	xil_printf("UART and SPI opened for PmodOLED Demo\n");
	OLED_SetDrawMode(&oledDevice, 0);
	// Turn automatic updating off
	OLED_SetCharUpdate(&oledDevice, 0);

	while(1){
		buttonVal = XGpio_DiscreteRead(&btnInst, BTN_CHANNEL);
		if (lives == 0){
			score = 0;
			aimx = 0;
			aimy = 0;
			XGpio_DiscreteWrite(&rgbLed, RGB_CHANNEL, LED_OFF);
			OLED_ClearBuffer(&oledDevice);
			OLED_SetCursor(&oledDevice, 0, 1);
			OLED_PutString(&oledDevice, "Game Over");
			OLED_Update(&oledDevice);
		} else {
			if (buttonVal == 0){
				drawCrossHair(aimx, aimy);
				drawTarget(targetx, targety, targetWidth, targetLength);
				OLED_Update(&oledDevice);
				usleep(FRAME_DELAY);
				OLED_ClearBuffer(&oledDevice);
			} else if (buttonVal == 2){
				OLED_ClearBuffer(&oledDevice);
				OLED_SetCursor(&oledDevice, 0, 0);
				sprintf(temp, "score: %d", score);
				OLED_PutString(&oledDevice, temp);
				OLED_SetCursor(&oledDevice, 0, 2);
				sprintf(temp, "lives: %d", lives);
				OLED_PutString(&oledDevice, temp);
				OLED_Update(&oledDevice);
			} else if (buttonVal == 4){
				OLED_ClearBuffer(&oledDevice);
				OLED_SetCursor(&oledDevice, 0, 1);
				u32 ticks = xTaskGetTickCount();
				ticks = ticks / 100;
				sprintf(temp, "time: %lu", ticks);
				OLED_PutString(&oledDevice, temp);
				OLED_Update(&oledDevice);
			}
		}
	}
}


static void buttonTask( void *pvParameters )
{
	u8 buttonVal = 0;
	while(1){
		buttonVal = XGpio_DiscreteRead(&btnInst, BTN_CHANNEL);
		if (buttonVal == 1 && lives > 0){
			checkShot();
		} else if (buttonVal == 1 && lives == 0){
			xil_printf("game over, reset with BTN3\n");
		} else if (buttonVal == 8){
			xil_printf("reset\n");
			lives = 3;
			score = 0;
		}
		vTaskDelay(10);
	}
}
