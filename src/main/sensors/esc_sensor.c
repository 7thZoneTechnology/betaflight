/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <platform.h>

#include "fc/config.h"
#include "config/feature.h"
#include "config/config_master.h"

#include "common/utils.h"

#include "drivers/system.h"
#include "drivers/serial.h"
#include "drivers/serial_uart.h"
#include "drivers/pwm_output.h"

#include "io/serial.h"

#include "flight/mixer.h"

#include "sensors/battery.h"

#include "build/debug.h"

#include "esc_sensor.h"

/*
KISS ESC TELEMETRY PROTOCOL
---------------------------

One transmission will have 10 times 8-bit bytes sent with 115200 baud and 3.6V.

Byte 0: Temperature
Byte 1: Voltage high byte
Byte 2: Voltage low byte
Byte 3: Current high byte
Byte 4: Current low byte
Byte 5: Consumption high byte
Byte 6: Consumption low byte
Byte 7: Rpm high byte
Byte 8: Rpm low byte
Byte 9: 8-bit CRC

*/

/*
DEBUG INFORMATION
-----------------

set debug_mode = DEBUG_ESC_TELEMETRY in cli

*/

#ifdef USE_DSHOT
enum {
    DEBUG_ESC_MOTOR_INDEX = 0,
    DEBUG_ESC_NUM_TIMEOUTS = 1,
    DEBUG_ESC_TEMPERATURE = 2,
    DEBUG_ESC_RPM = 3
};

typedef enum {
    ESC_SENSOR_FRAME_PENDING = 1 << 0,     // 1
    ESC_SENSOR_FRAME_COMPLETE = 1 << 1     // 2
} escTlmFrameState_t;

typedef enum {
    ESC_SENSOR_TRIGGER_WAIT = 0,
    ESC_SENSOR_TRIGGER_READY = 1 << 0,     // 1
    ESC_SENSOR_TRIGGER_PENDING = 1 << 1,   // 2
} escSensorTriggerState_t;

#define ESC_SENSOR_BAUDRATE 115200
#define ESC_SENSOR_BUFFSIZE 10
#define ESC_BOOTTIME 5000               // 5 seconds
#define ESC_REQUEST_TIMEOUT 100         // 100 ms (data transfer takes only 900us)

static bool tlmFrameDone = false;
static uint8_t tlm[ESC_SENSOR_BUFFSIZE] = { 0, };
static uint8_t tlmFramePosition = 0;
static serialPort_t *escSensorPort = NULL;
static escSensorData_t escSensorData[MAX_SUPPORTED_MOTORS];
static uint32_t escTriggerTimestamp = -1;
static uint32_t escLastResponseTimestamp;
static uint8_t timeoutRetryCount = 0;
static uint8_t totalRetryCount = 0;

static uint8_t escSensorMotor = 0;      // motor index
static bool escSensorEnabled = false;
static escSensorTriggerState_t escSensorTriggerState = ESC_SENSOR_TRIGGER_WAIT;

static void escSensorDataReceive(uint16_t c);
static uint8_t update_crc8(uint8_t crc, uint8_t crc_seed);
static uint8_t get_crc8(uint8_t *Buf, uint8_t BufLen);
static void selectNextMotor(void);

bool isEscSensorActive(void)
{
    return escSensorEnabled;
}

escSensorData_t getEscSensorData(uint8_t motorNumber)
{
    if (motorNumber < getMotorCount()) {
        return escSensorData[motorNumber];
    }

    escSensorData_t combinedEscSensorData = {
        .stale = true,
        .temperature = 0,
        .voltage = 0,
        .current = 0,
        .consumption = 0,
        .rpm = 0
    };
    if (motorNumber == ESC_SENSOR_COMBINED) {
        unsigned int activeSensors = 0;
        for (int i = 0; i < getMotorCount(); i = i + 1) {
            if (!escSensorData[i].stale) {
                combinedEscSensorData.temperature = MAX(combinedEscSensorData.temperature, escSensorData[i].temperature);
                combinedEscSensorData.voltage += escSensorData[i].voltage;
                combinedEscSensorData.current += escSensorData[i].current;
                combinedEscSensorData.consumption += escSensorData[i].consumption;
                combinedEscSensorData.rpm += escSensorData[i].rpm;
                activeSensors = activeSensors + 1;
            }
        }

        if (activeSensors > 0) {
            combinedEscSensorData.stale = false;
            combinedEscSensorData.voltage = combinedEscSensorData.voltage / activeSensors;
            combinedEscSensorData.rpm = combinedEscSensorData.rpm / activeSensors;

            DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_TEMPERATURE, combinedEscSensorData.temperature);
            DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_RPM, combinedEscSensorData.rpm);
        }
    }

    return combinedEscSensorData;
}

static void resetEscSensorData(void)
{
    for (int i; i < MAX_SUPPORTED_MOTORS; i = i + 1) {
        escSensorData[i].stale = true;
    }
}

bool escSensorInit(void)
{
    serialPortConfig_t *portConfig = findSerialPortConfig(FUNCTION_ESC_SENSOR);
    if (!portConfig) {
        return false;
    }

    portOptions_t options = (SERIAL_NOT_INVERTED);

    // Initialize serial port
    escSensorPort = openSerialPort(portConfig->identifier, FUNCTION_ESC_SENSOR, escSensorDataReceive, ESC_SENSOR_BAUDRATE, MODE_RX, options);

    if (escSensorPort) {
        escSensorEnabled = true;
    }

    resetEscSensorData();

    return escSensorPort != NULL;
}

static void freeEscSensorPort(void)
{
    closeSerialPort(escSensorPort);
    escSensorPort = NULL;
    escSensorEnabled = false;
}

// Receive ISR callback
static void escSensorDataReceive(uint16_t c)
{
    // KISS ESC sends some data during startup, ignore this for now (maybe future use)
    // startup data could be firmware version and serialnumber

    if (escSensorTriggerState == ESC_SENSOR_TRIGGER_WAIT) return;

    tlm[tlmFramePosition] = (uint8_t)c;

    if (tlmFramePosition == ESC_SENSOR_BUFFSIZE - 1) {
        tlmFrameDone = true;
        tlmFramePosition = 0;
    } else {
        tlmFramePosition++;
    }
}

static uint8_t escSensorFrameStatus(void)
{
    uint8_t frameStatus = ESC_SENSOR_FRAME_PENDING;
    uint16_t chksum, tlmsum;

    if (!tlmFrameDone) {
        return frameStatus;
    }

    tlmFrameDone = false;

    // Get CRC8 checksum
    chksum = get_crc8(tlm, ESC_SENSOR_BUFFSIZE - 1);
    tlmsum = tlm[ESC_SENSOR_BUFFSIZE - 1];     // last byte contains CRC value

    if (chksum == tlmsum) {
        escSensorData[escSensorMotor].stale = false;
        escSensorData[escSensorMotor].temperature = tlm[0];
        escSensorData[escSensorMotor].voltage = tlm[1] << 8 | tlm[2];
        escSensorData[escSensorMotor].current = tlm[3] << 8 | tlm[4];
        escSensorData[escSensorMotor].consumption = tlm[5] << 8 | tlm[6];
        escSensorData[escSensorMotor].rpm = tlm[7] << 8 | tlm[8];

        frameStatus = ESC_SENSOR_FRAME_COMPLETE;
    }

    return frameStatus;
}

void escSensorProcess(timeUs_t currentTimeUs)
{
    const timeMs_t currentTimeMs = currentTimeUs / 1000;

    if (!escSensorEnabled) {
        return;
    }

    // Wait period of time before requesting telemetry (let the system boot first)
    if (currentTimeMs < ESC_BOOTTIME) {
        return;
    }
    else if (escSensorTriggerState == ESC_SENSOR_TRIGGER_WAIT) {
        // Ready for starting requesting telemetry
        escSensorTriggerState = ESC_SENSOR_TRIGGER_READY;
        escSensorMotor = 0;
        escTriggerTimestamp = currentTimeMs;
        escLastResponseTimestamp = escTriggerTimestamp;
    }
    else if (escSensorTriggerState == ESC_SENSOR_TRIGGER_READY) {
        DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_MOTOR_INDEX, escSensorMotor + 1);

        motorDmaOutput_t * const motor = getMotorDmaOutput(escSensorMotor);
        motor->requestTelemetry = true;
        escSensorTriggerState = ESC_SENSOR_TRIGGER_PENDING;
    }
    else if (escSensorTriggerState == ESC_SENSOR_TRIGGER_PENDING) {

        if (escTriggerTimestamp + ESC_REQUEST_TIMEOUT < currentTimeMs) {
            // ESC did not repond in time, retry
            timeoutRetryCount++;
            escTriggerTimestamp = currentTimeMs;
            escSensorTriggerState = ESC_SENSOR_TRIGGER_READY;

            if (timeoutRetryCount == 4) {
                // Not responding after 3 times, skip motor
                escSensorData[escSensorMotor].stale = true;
                selectNextMotor();
            }

            DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_NUM_TIMEOUTS, ++totalRetryCount);
        }

        // Get received frame status
        uint8_t state = escSensorFrameStatus();

        if (state == ESC_SENSOR_FRAME_COMPLETE) {
            selectNextMotor();
            escSensorTriggerState = ESC_SENSOR_TRIGGER_READY;
            escLastResponseTimestamp = currentTimeMs;
        }
    }

    if (escLastResponseTimestamp + 10000 < currentTimeMs) {
        // ESCs did not respond for 10 seconds
        // Disable ESC telemetry and reset voltage and current to let the use know something is wrong
        freeEscSensorPort();

        resetEscSensorData();
    }
}

static void selectNextMotor(void)
{
    escSensorMotor++;
    if (escSensorMotor == getMotorCount()) {
        escSensorMotor = 0;
    }
    timeoutRetryCount = 0;
    escTriggerTimestamp = millis();
}

//-- CRC

static uint8_t update_crc8(uint8_t crc, uint8_t crc_seed)
{
    uint8_t crc_u = crc;
    crc_u ^= crc_seed;

    for (int i=0; i<8; i++) {
        crc_u = ( crc_u & 0x80 ) ? 0x7 ^ ( crc_u << 1 ) : ( crc_u << 1 );
    }

    return (crc_u);
}

static uint8_t get_crc8(uint8_t *Buf, uint8_t BufLen)
{
    uint8_t crc = 0;
    for(int i=0; i<BufLen; i++) crc = update_crc8(Buf[i], crc);
    return (crc);
}

#endif
