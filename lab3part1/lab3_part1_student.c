/******************************************************************************/
/* ECE - 315    : WINTER 2021
 * Created on   : 07 August, 2021
 *
 * Created by   : Shyama M. Gandhi, Mazen Elbaz
 * Modified by  : Shyama M. Gandhi, Winter 2023
 * Modified by  : Antonio Andara Lara, Winter 2025
 * Modified by  : Antonio Andara Lara, Winter 2026
 *
 * LAB 3: Implementation of SPI in Zynq-7000
 *------------------------------------------------------------------------------
 * This lab uses SPI in polled mode. The hardware diagram has a loop back connection hard coded where SPI0 - MASTER and SPI1 - SLAVE.
 * In this code SPI0 MASTER writes to SPI1 slave. The data received by SPI1 is transmitted back to the SPI0 master.
 * The driver function used to achieve this are provided by the Xilinx as xspips.h and xspipshw_h.
 * They are present in the provided initialization.h header file.
 *
 * There are two commands in the menu (options in the menu).
 * 1. Toggle loop back for UART manager task enable or disable (loop back mode)
 * 2. Toggle loop back for spi0-spi1 connection enable or disable (loop back mode)
 *
 * User enters the command in following ways:
 * For example, after you load the application on to the board, User may wish to execute the menu command 1. command is detected by using * <ENTER><1><ENTER>.
 *
 * Menu command 1:
 * Initially,
 * <ENTER><1><ENTER> can be used to change the UART Manager task loop back from enable to disable mode. This can also be done using the * * <ENTER><%><ENTER>.
 * To change the UART Manager loop back from disable to enable mode, use <ENTER><1><ENTER>.
 *
 * Menu command 2:
 * Initially,
 * <ENTER><2><ENTER> enables the spi loop back. Which means the connection between SPI0 and SPI1 is enabled.
 * You can disable the spi loop back (disable the SPI 1 - SPI 0 connection) using <ENTER><2><ENTER> or <ENTER><%><ENTER>.
 * However, to enable the SPI 1 - SPI 0 connection, you must use <ENTER><2><ENTER>.
 *
/******************************************************************************/

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "xgpio.h"
#include "xparameters.h"
#include "xil_printf.h"
#include "stdio.h"
#include <projdefs.h>
#include <stddef.h>
#include "string.h"
#include "my_uart.h"
#include "my_spi.h"

/************************** Constant Definitions *****************************/
#define UART_BASEADDR            XPAR_UART1_BASEADDR
#define SPI0_BASEADDR            XPAR_SPI0_BASEADDR
#define SPI1_BASEADDR            XPAR_SPI1_BASEADDR
#define RGB_LED_ADDR             XPAR_GPIO_LEDS_BASEADDR
#define RGB_CHANNEL              2

#define CHAR_CARRIAGE_RETURN     0x0D
#define CHAR_PERCENT             0x25
#define CHAR_DOLLAR              0x24

#define LED_RED                  0x4
#define LED_GREEN                0x2
#define LED_BLUE                 0x1
#define LED_OFF                  0x0

#define QUEUE_LENGTH             256

/************************* Task Prototypes ***********************************/
static void vUartManagerTask(void *pvParameters);
static void vSpiMainTask(void *pvParameters);
static void vSpiSubTask(void *pvParameters);
static void vRgbLedTask(void *pvParameters);

/************************* Helper Prototypes *********************************/
static void printMenu(void);
static void updateRollingBuffer(u8 rolling[3], u8 byte);
static BaseType_t terminationSequence(const u8 rolling[3]);
static BaseType_t checkCommand(const u8 rolling[3]);
static void terminateInput(void);

/************************* Global Variables *********************************/
static XGpio rgbLed;

static QueueHandle_t uart_to_spi = NULL;
static QueueHandle_t spi_to_uart = NULL;

static volatile u8 uart_loopback = 0;
static volatile u8 spi_loopback = 0;    /* 0: local SPI-main loopback 1: real main-sub loop */
static volatile u8 command_flag = 1;    /* 1: UART mode, 2: SPI mode */
static volatile u8 report_flag = 0;     /* Set by SPI sub when report is ready */

static volatile int total_bytes_received_over_spi = 0;
static volatile int last_message_byte_count = 0;
static volatile int total_messages_received = 0;

/******************************************************************************
/* MAIN */
/******************************************************************************/

int main(void)
{
    int status;

    status = uartInit(UART_BASEADDR);
    if (status != XST_SUCCESS) {
        xil_printf("UART Initialization failed\r\n");
        return XST_FAILURE;
    }

    status = spiInit(SPI0_BASEADDR, SPI1_BASEADDR);
    if (status != XST_SUCCESS) {
        xil_printf("SPI Initialization failed\r\n");
        return XST_FAILURE;
    }

    status = XGpio_Initialize(&rgbLed, RGB_LED_ADDR);
    if (status != XST_SUCCESS) {
        xil_printf("RGB Initialization failed\r\n");
        return XST_FAILURE;
    }

    XGpio_SetDataDirection(&rgbLed, 2, 0x0);

    uart_to_spi = xQueueCreate(QUEUE_LENGTH, sizeof(u8));
    spi_to_uart = xQueueCreate(QUEUE_LENGTH, sizeof(u8));

    xTaskCreate(vUartManagerTask, "UART", 512, NULL, 2, NULL);
    xTaskCreate(vSpiMainTask, "SPI_MAIN", 512, NULL, 2, NULL);
    xTaskCreate(vSpiSubTask, "SPI_SUB", 512, NULL, 2, NULL);
    xTaskCreate(vRgbLedTask, "RGB", 256, NULL, 2, NULL);

    configASSERT(uart_to_spi);
    configASSERT(spi_to_uart);
    configASSERT(vUartManagerTask);
    configASSERT(vSpiMainTask);
    configASSERT(vSpiSubTask);
    configASSERT(vRgbLedTask);

    printMenu();

    vTaskStartScheduler();

    while (1) {
    }
}

/******************************************************************************
/* UART MANAGER TASK */
/******************************************************************************/

static void vUartManagerTask(void *pvParameters)
{
    const u8 dummy = CHAR_DOLLAR;
    u8 rolling[3] = {0, 0, CHAR_CARRIAGE_RETURN};
    u8 uart_byte = 0;
    u8 spi_byte = 0;
    int i;

    while (1) {
        if (report_flag) {
            // TODO 14: send $ until a $ is received
            xQueueSend(uart_to_spi, (void *)&dummy, 0);

            if (xQueueReceive(spi_to_uart, &spi_byte, 0)) {
                if (spi_byte == CHAR_DOLLAR) {
                    /* '$' marks the end of the report stream */
                    report_flag = 0;
                    // After report is sent, terminate input if we were in SPI loopback mode
                    if (spi_loopback && command_flag == 2) {
                        terminateInput();
                    }
                } else {
                    /* Forward report bytes to the UART */
                    uartWriteByte(spi_byte);
                }
            }

            vTaskDelay(1);
            continue;
        }
        
        if (uartReadByte(&uart_byte)) {
            updateRollingBuffer(rolling, uart_byte);
            
            // use checkCommand() before taking any action
            // if a command is verified then continue with the next iteration
            if (checkCommand(rolling)) {
                vTaskDelay(1);
                continue;
            }

            if (uart_loopback && command_flag == 1) {
                // TODO 1: write to uart
                uartWriteByte(uart_byte);

                if (terminationSequence(rolling)) {
                    terminateInput();
                }
            } else if (command_flag == 2) {
                // TODO 2: send to uart_to_spi
                xQueueSend(uart_to_spi, (void *)&uart_byte, 0);

                // Check termination sequence for both SPI loopback enabled and disabled
                // When SPI loopback is ON, the report will be generated by vSpiSubTask
                // and terminateInput() will be called after the report is sent
                if (terminationSequence(rolling)) {
                    if (!spi_loopback) {
                        // If SPI loopback is OFF, terminate immediately
                        terminateInput();
                    }
                    // If SPI loopback is ON, wait for report to finish before terminating
                    // (terminateInput() will be called after report_flag is cleared)
                }
            }
        }

        while (xQueueReceive(spi_to_uart, &spi_byte, 0)) {
            // Filter out control characters ($) before displaying
            if (spi_byte != CHAR_DOLLAR) {
                uartWriteByte(spi_byte);
            }
        }

        vTaskDelay(1);
    }
}

/******************************************************************************
/* SPI MAIN TASK */
/******************************************************************************/

static void vSpiMainTask(void *pvParameters)
{
    u8 uart_byte = 0;
    u8 tx_frame[TRANSFER_SIZE_IN_BYTES];
    u8 rx_frame[TRANSFER_SIZE_IN_BYTES];
    u8 frame_index = 0;
    int i;

    memset(tx_frame, 0, TRANSFER_SIZE_IN_BYTES);
    memset(rx_frame, 0, TRANSFER_SIZE_IN_BYTES);

    while (1) {
        if (xQueueReceive(uart_to_spi, &uart_byte, pdMS_TO_TICKS(10))) {
            if (command_flag == 2) {
                if (!spi_loopback) { // if spi_loopback is disabled echoes back the received bytes
                    // TODO 3: echo back received bytes by sending to the appropriate queue
                    // after this is implemented spi loopback diabled should echo back the received bytes
                    xQueueSend(spi_to_uart, (void *)&uart_byte, 0);
                } else {        // if spi loopback is enabled prepare to send data frames
                    tx_frame[frame_index] = uart_byte; // load byte into data frame
                    frame_index++;
                    
                    // when data frame is complete transmmit data using the spi write and sepi read functions
                    if (frame_index == TRANSFER_SIZE_IN_BYTES) {
                        // TODO 9: master transfer
                        // perform the SPI sequence for a master data transfer (write and read)
                        // after transmission send data to queue
                        spiMasterTransfer(tx_frame, rx_frame, TRANSFER_SIZE_IN_BYTES);
                        for (i = 0; i < TRANSFER_SIZE_IN_BYTES; i++) {
                            u8 out = rx_frame[i];
                            xQueueSend(spi_to_uart, (void *)&out, 0);
                        }

                        frame_index = 0;
                    }
                }
            } else {
                frame_index = 0;
            }
        }

        if (!spi_loopback || command_flag != 2) {
            frame_index = 0;
        }

        vTaskDelay(50);
    }
}

/******************************************************************************
/* SPI SUB TASK */
/******************************************************************************/

static void vSpiSubTask(void *pvParameters)
{
    u8 tx_frame[TRANSFER_SIZE_IN_BYTES];
    u8 rx_frame[TRANSFER_SIZE_IN_BYTES];
    u8 rolling[3] = {0, 0, 0};
    char report[256];
    int report_len = 0;
    int report_idx = 0;
    int message_byte_count = 0;
    BaseType_t report_stream_active = pdFALSE;
    int i;

    memset(rx_frame, 0, TRANSFER_SIZE_IN_BYTES);
    // prepare for transmission, load data into tx_frame
    memset(tx_frame, CHAR_DOLLAR, TRANSFER_SIZE_IN_BYTES);

    while (1) {
        if (spi_loopback && command_flag == 2) {
            // TODO 10: prepare for transmission, load data into tx_frame
            spiSlaveTransfer(tx_frame, rx_frame, TRANSFER_SIZE_IN_BYTES);
            if (report_stream_active) {
                // fill tx_buffer with control characters
                memset(tx_frame, CHAR_DOLLAR, TRANSFER_SIZE_IN_BYTES);

                if (report_idx < report_len) {
                    int chunk_len = report_len - report_idx;

                    if (chunk_len > TRANSFER_SIZE_IN_BYTES) {
                        chunk_len = TRANSFER_SIZE_IN_BYTES;
                    }
                    // load report chunk to tx frame
                    memcpy(tx_frame, &report[report_idx], (size_t)chunk_len);
                    report_idx += chunk_len;
                } else {
                    report_stream_active = pdFALSE;
                }

                vTaskDelay(1);
                continue;
            }
            
            // in normal operation the device copies rx_frame into tx_frame in order to echo the characters
            memcpy(tx_frame, rx_frame, TRANSFER_SIZE_IN_BYTES);

            for (i = 0; i < TRANSFER_SIZE_IN_BYTES; i++) {
                u8 current = rx_frame[i];
                
                // ignore any received control characters
                if (current == CHAR_DOLLAR) {
                    continue;
                }
                
                // TODO 11: keep track of total received bytes over SPI and the current message byte count
                // Update rolling buffer for ALL characters (including control chars) so termination sequence can be detected
                updateRollingBuffer(rolling, current);
                
                // Check for termination sequence after updating buffer (for all characters)
                if (terminationSequence(rolling)) {
                    int chunk_len;
                    // TODO 12: keep track of the number of messages received
                    total_messages_received++;
                    last_message_byte_count = message_byte_count;
                    message_byte_count = 0;
                    
                    // TODO 13: generate report string. hint: use report_len = snprintf()
                    report_len = snprintf(
                        report,
                        sizeof(report),
                        "InNumber of bytes received over SPI:%d\nLast message byte count: %d\nTotal messages received: %d\n",
                        total_bytes_received_over_spi,
                        last_message_byte_count,
                        total_messages_received);

                    report_idx = 0;  // index of sent byte
                    report_flag = 1; // signals uart task to flush the report
                    report_stream_active = pdTRUE; // local flag
                    
                    // prepare to send first chunk of the report
                    // all other chunks will be taken care of by the block on the beginning
                    memset(tx_frame, CHAR_DOLLAR, TRANSFER_SIZE_IN_BYTES);
                    chunk_len = TRANSFER_SIZE_IN_BYTES;
                    memcpy(tx_frame, &report[report_idx], TRANSFER_SIZE_IN_BYTES);
                    report_idx += chunk_len;
                    break;
                }
                
                // Don't count control characters in byte counts, but already processed them for termination detection
                if ((current == CHAR_CARRIAGE_RETURN) || (current == CHAR_PERCENT)) {
                    continue; // Skip counting but already updated buffer and checked termination
                }

                total_bytes_received_over_spi++;
                message_byte_count++;
            }
        } else { // reset device
            memset(tx_frame, CHAR_DOLLAR, TRANSFER_SIZE_IN_BYTES);
            memset(rolling, 0, sizeof(rolling));
            message_byte_count = 0;
            report_len = 0;
            report_idx = 0;
            report_stream_active = pdFALSE;
        }

        vTaskDelay(10);
    }
}

/******************************************************************************
/* RGB LED TASK */
/******************************************************************************/

static void vRgbLedTask(void *pvParameters)
{
    u32 led_value = LED_OFF;

    while (1) {
        if (uart_loopback && command_flag == 1) {
            led_value = LED_RED;
        } else if (spi_loopback && command_flag == 2) {
            led_value = LED_GREEN;
        } else if (!spi_loopback && command_flag == 2) {
            led_value = LED_BLUE;
        } else {
            led_value = LED_OFF;
        }

        XGpio_DiscreteWrite(&rgbLed, RGB_CHANNEL, led_value);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/******************************************************************************
/* Helpers */
/******************************************************************************/

static void updateRollingBuffer(u8 rolling[3], u8 byte)
{
    rolling[0] = rolling[1];
    rolling[1] = rolling[2];
    rolling[2] = byte;
}

static BaseType_t terminationSequence(const u8 rolling[3])
{
    if ((rolling[0] == CHAR_CARRIAGE_RETURN) &&
        (rolling[1] == CHAR_PERCENT) &&
        (rolling[2] == CHAR_CARRIAGE_RETURN)) {
        return pdTRUE;
    }

    return pdFALSE;
}

static BaseType_t checkCommand(const u8 rolling[3])
{
    if ((rolling[0] == CHAR_CARRIAGE_RETURN) && (rolling[2] == CHAR_CARRIAGE_RETURN)) {
        if (rolling[1] == '1') {
            uart_loopback = (uart_loopback == 0) ? 1 : 0;
            command_flag = 1;

            xil_printf("\r\n*** UART Loop-back %s ***\r\n",
                (uart_loopback == 1) ? "ON" : "OFF");

            return pdTRUE;
        }

        if (rolling[1] == '2') {
            spi_loopback = (spi_loopback == 0) ? 1 : 0;
            command_flag = 2;

            xil_printf("\r\n*** SPI Loop-back %s ***\r\n",
                (spi_loopback == 1) ? "ON" : "OFF");

            return pdTRUE;
        }
    }

    return pdFALSE;
}

static void terminateInput(void)
{
    command_flag = 1;
    report_flag = 0;
    spi_loopback = 0;
    uart_loopback = 0;

    xil_printf("\r\n*** Text entry ended using termination sequence ***\r\n");
}

static void printMenu(void)
{
    xil_printf("\r\n================ ECE-315 Lab 3: UART + SPI =================\r\n");
    xil_printf("Commands: <ENTER>1<ENTER> toggles UART loopback mode\r\n");
    xil_printf("          <ENTER>2<ENTER> toggles SPI loopback mode\r\n");
    xil_printf("Termination sequence: <ENTER>%<ENTER>\r\n");
    xil_printf("\r\nModes:\r\n");
    xil_printf("  UART loopback ON   : UART echoes locally\r\n");
    xil_printf("  SPI loopback OFF   : SPI main echoes queue byte\r\n");
    xil_printf("  SPI loopback ON    : Real SPI0 -> SPI1 -> SPI0 loop\r\n");
}
