/**
 * ads1292r.h
 * -----------------------------------------------------------
 * ADS1292R driver — ProtoCentral library-matched
 * -----------------------------------------------------------
 */

#pragma once

#include <Arduino.h>
#include <SPI.h>
#include "config.h"
#include "pin_config.h"
#include "types.h"

// Register addresses
#define ADS1292_REG_ID          0x00
#define ADS1292_REG_CONFIG1     0x01
#define ADS1292_REG_CONFIG2     0x02
#define ADS1292_REG_LOFF        0x03
#define ADS1292_REG_CH1SET      0x04
#define ADS1292_REG_CH2SET      0x05
#define ADS1292_REG_RLD_SENS    0x06
#define ADS1292_REG_LOFF_SENS   0x07
#define ADS1292_REG_LOFF_STAT   0x08
#define ADS1292_REG_RESP1       0x09
#define ADS1292_REG_RESP2       0x0A
#define ADS1292_REG_GPIO        0x0B

#define ADS1292_EXPECTED_ID     0x73

// SPI commands
#define ADS1292_CMD_WAKEUP      0x02
#define ADS1292_CMD_STANDBY     0x04
#define ADS1292_CMD_RESET       0x06
#define ADS1292_CMD_START       0x08
#define ADS1292_CMD_STOP        0x0A
#define ADS1292_CMD_RDATAC      0x10
#define ADS1292_CMD_SDATAC      0x11
#define ADS1292_CMD_RDATA       0x12
#define ADS1292_CMD_RREG        0x20
#define ADS1292_CMD_WREG        0x40


class ADS1292R {
public:
    ADS1292R();

    ADS1292_Status begin();
    void           hardwareReset();

    uint8_t  readRegister(uint8_t reg);
    void     writeRegister(uint8_t reg, uint8_t value);
    bool     verifyRegister(uint8_t reg, uint8_t expected, bool silent = false);
    uint8_t  readDeviceID();

    void startConversion();
    void stopConversion();
    void enableDataContinuous();
    void disableDataContinuous();

    bool      isDataReady();
    ECGSample readECGSample();

    ADS1292_State            getState() const;
    ADS1292_RegisterSnapshot dumpRegisters();
    uint8_t                  getWorkingSpiMode() const;

private:
    ADS1292_State _state;
    SPIClass*     _spi;
    uint8_t       _workingSpiMode;

    void    csLow();
    void    csHigh();
    void    spiPrePulse();      // CS LOW→HIGH→LOW pre-pulse (critical for ADS1292R)
    void    sendCommand(uint8_t cmd);
    int32_t decode24bit(uint8_t msb, uint8_t mid, uint8_t lsb);
};
