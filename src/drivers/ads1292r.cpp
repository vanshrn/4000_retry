/**
 * ads1292r.cpp — ProtoCentral-matched implementation
 * -----------------------------------------------------------
 * Root cause of "all registers read 0x00" identified from the
 * ProtoCentral library source (protocentralAds1292r.cpp):
 *
 * Their ads1292SPICommandData() and ads1292RegWrite() use a
 * specific CS PRE-PULSE before every transfer:
 *
 *   CS LOW  → delay(2ms)
 *   CS HIGH → delay(2ms)   ← CS goes HIGH before the data!
 *   CS LOW  → delay(2ms)
 *   SPI.transfer(data)
 *   delay(2ms)
 *   CS HIGH
 *
 * This pre-pulse resets the ADS1292R's internal SPI state
 * machine before each command. Without it, the chip does not
 * register incoming bytes correctly — writes are silently
 * discarded and reads return 0x00.
 *
 * Other matches from ProtoCentral library:
 *  - No beginTransaction/endTransaction (direct SPI.transfer)
 *  - Reset uses 100 ms delays on PWDN/RESET pin
 *  - Init sequence: Reset → START toggle → START/STOP cmds →
 *    delay(300) → SDATAC → registers → RDATAC → START HIGH
 *  - Register values: CH1SET=0x40, LOFF_SENS=0x00,
 *    LOFF=0x10, RESP1=0xF2, CONFIG2=0xA0
 *  - ads1292ReadData transfers 0xFF (not 0x00) as dummy byte
 *  - Data reads do NOT use the CS pre-pulse (RDATAC mode:
 *    just CS LOW → 9×transfer → CS HIGH)
 * -----------------------------------------------------------
 */

#include "ads1292r.h"

// ==========================================================
// Constructor
// ==========================================================
ADS1292R::ADS1292R()
    : _state(ADS1292_State::UNINIT),
      _spi(nullptr),
      _workingSpiMode(1)
{}

// ==========================================================
// CS Control
// ==========================================================
void ADS1292R::csLow()  { digitalWrite(ADS1292_CS_PIN, LOW);  }
void ADS1292R::csHigh() { digitalWrite(ADS1292_CS_PIN, HIGH); }

// ==========================================================
// CS Pre-Pulse
// -----------------------------------------------------------
// CRITICAL: The ProtoCentral library does this before EVERY
// command byte and register write:
//   CS→LOW(2ms) → CS→HIGH(2ms) → CS→LOW(2ms)
// Then transfers the byte, then CS→HIGH.
//
// This pulse resets the ADS1292R's SPI state machine so it
// correctly receives the next command. Without this pulse the
// chip silently ignores all writes and returns 0x00 on reads.
// ==========================================================
void ADS1292R::spiPrePulse() {
    csLow();
    delay(2);
    csHigh();
    delay(2);
    csLow();
    delay(2);
}

// ==========================================================
// Send single-byte command (with CS pre-pulse)
// ==========================================================
void ADS1292R::sendCommand(uint8_t cmd) {
    spiPrePulse();
    _spi->transfer(cmd);
    delay(2);
    csHigh();
    delay(5);
}

// ==========================================================
// Hardware Reset
// -----------------------------------------------------------
// Matches ProtoCentral ads1292Reset(pwdnPin):
//   HIGH(100ms) → LOW(100ms) → HIGH(100ms)
// Their PWDN pin = our RESET pin (GPIO16 in both cases).
// ==========================================================
void ADS1292R::hardwareReset() {
    digitalWrite(ADS1292_RESET_PIN, HIGH);
    delay(100);
    digitalWrite(ADS1292_RESET_PIN, LOW);
    delay(ADS1292_RESET_PULSE_MS);      // 100 ms
    digitalWrite(ADS1292_RESET_PIN, HIGH);
    delay(100);
    _state = ADS1292_State::RESET_DONE;
}

// ==========================================================
// Register Write (with CS pre-pulse — matches ads1292RegWrite)
// -----------------------------------------------------------
// ProtoCentral applies bitmasks to certain registers before
// writing. We apply the same masks so written values match
// what the chip actually accepts per datasheet constraints.
// ==========================================================
void ADS1292R::writeRegister(uint8_t reg, uint8_t value) {
    // Apply ProtoCentral's register-specific bit masks
    switch (reg) {
        case 1:  value = value & 0x87;          break; // CONFIG1
        case 2:  value = (value & 0xFB) | 0x80; break; // CONFIG2: bit7 always 1, bit2 cleared
        case 3:  value = (value & 0xFD) | 0x10; break; // LOFF: bit4 always 1, bit1 cleared
        case 7:  value = value & 0x3F;          break; // LOFF_SENS
        case 8:  value = value & 0x5F;          break; // LOFF_STAT (read-only, no effect)
        case 9:  value |= 0x02;                 break; // RESP1: bit1 always 1
        case 10: value = (value & 0x87) | 0x01; break; // RESP2: bit0 always 1
        case 11: value = value & 0x0F;          break; // GPIO
        default: break;
    }

    uint8_t addr = reg | 0x40;  // WREG opcode

    spiPrePulse();
    _spi->transfer(addr);    // register address + WREG
    _spi->transfer(0x00);    // write 1 register (n-1 = 0)
    _spi->transfer(value);   // value
    delay(2);
    csHigh();
    delay(5);
}

// ==========================================================
// Register Read (with CS pre-pulse)
// ==========================================================
uint8_t ADS1292R::readRegister(uint8_t reg) {
    spiPrePulse();
    _spi->transfer(0x20 | (reg & 0x1F));  // RREG opcode
    _spi->transfer(0x00);                  // read 1 register
    uint8_t value = _spi->transfer(0xFF);  // 0xFF dummy (matches library)
    delay(2);
    csHigh();
    delay(5);
    return value;
}

// ==========================================================
// Verify register (print mismatch — silent flag for fallback)
// ==========================================================
bool ADS1292R::verifyRegister(uint8_t reg, uint8_t expected, bool silent) {
    uint8_t rb = readRegister(reg);
    if (rb != expected) {
        if (!silent) {
            Serial.print(F("#VERIFY FAIL reg=0x")); Serial.print(reg, HEX);
            Serial.print(F(" wrote=0x"));    Serial.print(expected, HEX);
            Serial.print(F(" readback=0x")); Serial.println(rb, HEX);
        }
        return false;
    }
    return true;
}

uint8_t ADS1292R::readDeviceID() { return readRegister(ADS1292_REG_ID); }

// ==========================================================
// Dump all registers
// ==========================================================
ADS1292_RegisterSnapshot ADS1292R::dumpRegisters() {
    // Registers cannot be read while chip is in RDATAC mode.
    // Pause continuous data, read, then resume.
    sendCommand(ADS1292_CMD_SDATAC);
    delay(20);
    ADS1292_RegisterSnapshot s;
    s.id        = readRegister(ADS1292_REG_ID);
    s.config1   = readRegister(ADS1292_REG_CONFIG1);
    s.config2   = readRegister(ADS1292_REG_CONFIG2);
    s.loff      = readRegister(ADS1292_REG_LOFF);
    s.ch1set    = readRegister(ADS1292_REG_CH1SET);
    s.ch2set    = readRegister(ADS1292_REG_CH2SET);
    s.rld_sens  = readRegister(ADS1292_REG_RLD_SENS);
    s.loff_sens = readRegister(ADS1292_REG_LOFF_SENS);
    s.loff_stat = readRegister(ADS1292_REG_LOFF_STAT);
    s.resp1     = readRegister(ADS1292_REG_RESP1);
    s.resp2     = readRegister(ADS1292_REG_RESP2);
    s.gpio      = readRegister(ADS1292_REG_GPIO);
    // Resume streaming
    sendCommand(ADS1292_CMD_RDATAC);
    delay(10);
    return s;
}

ADS1292_State ADS1292R::getState() const { return _state; }
uint8_t ADS1292R::getWorkingSpiMode() const { return _workingSpiMode; }

// ==========================================================
// begin()
// -----------------------------------------------------------
// Matches the ProtoCentral ads1292Init() sequence exactly:
//
//  1. Pin setup (OUTPUT/INPUT + idle levels)
//  2. SPI.begin() with fixed VSPI pins
//  3. RESET pulse (HIGH→LOW 100ms→HIGH 100ms) on RESET/PWDN
//  4. delay(100)
//  5. START → LOW (DisableStart)
//  6. START → HIGH (EnableStart)
//  7. START → LOW, delay(100) (HardStop)
//  8. Send START command (0x08)
//  9. Send STOP command  (0x0A)
// 10. delay(50)
// 11. Send SDATAC (0x11)
// 12. delay(300)
// 13. Write all registers with delay(10) between each
// 14. Send RDATAC (0x10)
// 15. delay(10)
// 16. START → HIGH (EnableStart)
//
// After init, read back ID to confirm chip is responding.
// ==========================================================
ADS1292_Status ADS1292R::begin() {
    // --- 1. Pin setup ---
    pinMode(ADS1292_CS_PIN,    OUTPUT);
    pinMode(ADS1292_START_PIN, OUTPUT);
    pinMode(ADS1292_RESET_PIN, OUTPUT);
    pinMode(ADS1292_DRDY_PIN,  INPUT);

    digitalWrite(ADS1292_CS_PIN,    HIGH);
    digitalWrite(ADS1292_START_PIN, LOW);
    digitalWrite(ADS1292_RESET_PIN, HIGH);

    // --- 2. SPI bus ---
    if (_spi != nullptr) {
        delete _spi;
        _spi = nullptr;
    }
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3) || defined(FSPI)
    _spi = new SPIClass(FSPI);
#else
    _spi = new SPIClass(VSPI);
#endif
    _spi->begin(ADS1292_SCK_PIN, ADS1292_MISO_PIN, ADS1292_MOSI_PIN, ADS1292_CS_PIN);
    // Set SPI mode once — no beginTransaction used after this
    _spi->setDataMode(ADS1292_SPI_MODE);
    _spi->setBitOrder(ADS1292_SPI_BIT_ORDER);
    _spi->setFrequency(ADS1292_SPI_CLOCK_HZ);

    delay(ADS1292_POWERUP_DELAY_MS);

    // --- 3. RESET pulse (matches ads1292Reset) ---
    Serial.println(F("#RESET pulse..."));
    hardwareReset();

    // --- 4. delay after reset ---
    delay(100);

    // --- 5-7. START pin sequence (DisableStart → EnableStart → HardStop) ---
    Serial.println(F("#START pin sequence..."));
    digitalWrite(ADS1292_START_PIN, LOW);   delay(20);  // DisableStart
    digitalWrite(ADS1292_START_PIN, HIGH);  delay(20);  // EnableStart
    digitalWrite(ADS1292_START_PIN, LOW);   delay(100); // HardStop

    // --- 8. Send START command ---
    Serial.println(F("#Sending START command..."));
    sendCommand(ADS1292_CMD_START);

    // --- 9. Send STOP command ---
    Serial.println(F("#Sending STOP command..."));
    sendCommand(ADS1292_CMD_STOP);
    delay(50);

    // --- 11. SDATAC ---
    Serial.println(F("#Sending SDATAC..."));
    sendCommand(ADS1292_CMD_SDATAC);
    delay(300);

    // --- 13. Write all registers ---
    Serial.println(F("#Writing registers..."));
    writeRegister(ADS1292_REG_CONFIG1,   ADS1292_CONFIG1_VAL); delay(10);
    writeRegister(ADS1292_REG_CONFIG2,   ADS1292_CONFIG2_VAL); delay(10);
    writeRegister(ADS1292_REG_LOFF,      ADS1292_LOFF_VAL);    delay(10);
    writeRegister(ADS1292_REG_CH1SET,    ADS1292_CH1SET_VAL);  delay(10);
    writeRegister(ADS1292_REG_CH2SET,    ADS1292_CH2SET_VAL);  delay(10);
    writeRegister(ADS1292_REG_RLD_SENS,  ADS1292_RLD_SENS_VAL);delay(10);
    writeRegister(ADS1292_REG_LOFF_SENS, ADS1292_LOFF_SENS_VAL);delay(10);
    writeRegister(ADS1292_REG_RESP1,     ADS1292_RESP1_VAL);   delay(10);
    writeRegister(ADS1292_REG_RESP2,     ADS1292_RESP2_VAL);   delay(10);

    // --- 14. RDATAC ---
    Serial.println(F("#Sending RDATAC..."));
    sendCommand(ADS1292_CMD_RDATAC);
    delay(10);

    // --- 16. START HIGH ---
    Serial.println(F("#START HIGH (EnableStart)..."));
    digitalWrite(ADS1292_START_PIN, HIGH);
    delay(20);

    // --- Verify: read ID register ---
    // Stop RDATAC first so we can do register reads
    sendCommand(ADS1292_CMD_SDATAC);
    delay(10);

    uint8_t id = readDeviceID();
    Serial.print(F("#Device ID: 0x")); Serial.println(id, HEX);

    if (id != ADS1292_EXPECTED_ID) {
        Serial.println(F("#ID mismatch! Expected 0x73"));
        // Print what all registers look like now
        Serial.print(F("#CONFIG1 readback: 0x"));
        Serial.println(readRegister(ADS1292_REG_CONFIG1), HEX);
        Serial.print(F("#CH1SET  readback: 0x"));
        Serial.println(readRegister(ADS1292_REG_CH1SET), HEX);
        _state = ADS1292_State::UNINIT;
        return ADS1292_Status::DEVICE_NOT_FOUND;
    }

    Serial.println(F("#ID = 0x73 confirmed!"));

    // Re-enable RDATAC for data streaming
    sendCommand(ADS1292_CMD_RDATAC);
    delay(10);

    _workingSpiMode = 1;
    _state = ADS1292_State::CONFIGURED;
    return ADS1292_Status::OK;
}

// ==========================================================
// Conversion Control
// ==========================================================
void ADS1292R::startConversion() {
    digitalWrite(ADS1292_START_PIN, HIGH);
    delay(20);
    _state = ADS1292_State::RUNNING;
}

void ADS1292R::stopConversion() {
    sendCommand(ADS1292_CMD_SDATAC);
    sendCommand(ADS1292_CMD_STOP);
    digitalWrite(ADS1292_START_PIN, LOW);
    _state = ADS1292_State::STOPPED;
}

void ADS1292R::enableDataContinuous()  { sendCommand(ADS1292_CMD_RDATAC); }
void ADS1292R::disableDataContinuous() { sendCommand(ADS1292_CMD_SDATAC); }

bool ADS1292R::isDataReady() {
    return (digitalRead(ADS1292_DRDY_PIN) == LOW);
}

// ==========================================================
// Sign-extend 24-bit → int32_t  (matches ProtoCentral decode)
// -----------------------------------------------------------
// ProtoCentral uses:
//   uecgtemp = (uint32_t)((b0<<16)|(b1<<8)|b2)
//   uecgtemp = uecgtemp << 8
//   secgtemp = (int32_t)uecgtemp
//   secgtemp = secgtemp >> 8   ← arithmetic right shift
// This is equivalent to standard sign-extension below.
// ==========================================================
int32_t ADS1292R::decode24bit(uint8_t msb, uint8_t mid, uint8_t lsb) {
    uint32_t u = ((uint32_t)msb << 16) | ((uint32_t)mid << 8) | (uint32_t)lsb;
    u = u << 8;                     // shift left so sign bit is in bit31
    int32_t s = (int32_t)u;
    s = s >> 8;                     // arithmetic right shift restores sign
    return s;
}

// ==========================================================
// readECGSample()
// -----------------------------------------------------------
// Matches ProtoCentral ads1292ReadData() for the 9-byte read:
//   - CS LOW (no pre-pulse — RDATAC mode just needs CS low)
//   - 9 × SPI.transfer(0xFF)   ← 0xFF dummy like the library
//   - CS HIGH
//
// Status word (bytes [0..2]):
//   statusByte = (b2 | b1<<8 | b0<<16)
//   statusByte = (statusByte & 0x0f8000) >> 15  ← lead status
//   LeadStatus = (uint8_t)statusByte
//   leadOff = (LeadStatus & 0x1f) != 0
//
// Data:
//   sDaqVals[0] = decode(frame[3], frame[4], frame[5])  ← CH1 resp
//   sDaqVals[1] = decode(frame[6], frame[7], frame[8])  ← CH2 ECG
// ==========================================================
ECGSample ADS1292R::readECGSample() {
    ECGSample s;
    s.valid = false; s.channel1 = 0; s.channel2 = 0;
    s.leadOffDetected = false; s.loPlusOff = false; s.loMinusOff = false;
    s.timestamp_ms = millis();

    // If chip is not running/configured, return immediately without polling DRDY
    if (_state != ADS1292_State::RUNNING || _spi == nullptr) {
        return s;
    }

    // Poll DRDY with microsecond timeout (tight spin for zero-latency capture at 2000 SPS)
    uint32_t t0 = micros();
    while (!isDataReady()) {
        if ((micros() - t0) > (ADS1292_DRDY_TIMEOUT_MS * 1000UL)) {
            // Auto-recovery: Re-assert START pin + RDATAC in case power glitch paused ADC conversion
            digitalWrite(ADS1292_START_PIN, HIGH);
            sendCommand(ADS1292_CMD_START);
            sendCommand(ADS1292_CMD_RDATAC);
            return s;
        }
    }

    // Read 9-byte frame — NO pre-pulse in RDATAC mode (matches library)
    uint8_t frame[9];
    csLow();
    for (int i = 0; i < 9; i++) {
        frame[i] = _spi->transfer(0xFF);   // 0xFF matches library's CONFIG_SPI_MASTER_DUMMY
    }
    csHigh();

    // Parse lead-off status (matches ProtoCentral exactly)
    long statusByte = (long)((long)frame[2] |
                             ((long)frame[1] << 8) |
                             ((long)frame[0] << 16));
    statusByte  = (statusByte & 0x0f8000) >> 15;
    uint8_t leadsStatus = (uint8_t)statusByte;
    s.loPlusOff  = (leadsStatus & 0x02) != 0;  // IN2P (Bit 1)
    s.loMinusOff = (leadsStatus & 0x04) != 0;  // IN2N (Bit 2)
    s.leadOffDetected = (s.loPlusOff || s.loMinusOff);

    // Decode channels (matches ProtoCentral sDaqVals decode)
    s.channel1 = decode24bit(frame[3], frame[4], frame[5]);  // CH1 respiration
    s.channel2 = decode24bit(frame[6], frame[7], frame[8]);  // CH2 ECG

#if ECG_INVERT_CH2
    s.channel2 = -s.channel2;
#endif

    s.timestamp_ms = millis();
    s.valid = true;
    return s;
}
