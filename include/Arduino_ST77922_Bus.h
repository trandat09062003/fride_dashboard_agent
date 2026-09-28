#pragma once

#include <Arduino.h>
#include <driver/spi_master.h>

#define ST77922_CMD_BITS   8
#define ST77922_ADDR_BITS  24
#define ST77922_OPCODE_CMD   0x02
#define ST77922_OPCODE_COLOR 0x32
#define ST77922_RAMWR_ADDR   0x002C00

class Arduino_ST77922_Bus {
public:
    int8_t _cs, _sck, _d0, _d1, _d2, _d3;
    spi_device_handle_t _spi;

    Arduino_ST77922_Bus(int8_t cs, int8_t sck, int8_t d0, int8_t d1, int8_t d2, int8_t d3)
        : _cs(cs), _sck(sck), _d0(d0), _d1(d1), _d2(d2), _d3(d3), _spi(NULL) {}

    bool begin(uint32_t speed = 40000000) {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = _sck;
        buscfg.data0_io_num = _d0;
        buscfg.data1_io_num = _d1;
        buscfg.data2_io_num = _d2;
        buscfg.data3_io_num = _d3;
        buscfg.max_transfer_sz = 320 * 80 * sizeof(uint16_t) + 32;
        buscfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;
        
        esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return false;

        spi_device_interface_config_t devcfg = {};
        devcfg.command_bits = ST77922_CMD_BITS;
        devcfg.address_bits = ST77922_ADDR_BITS;
        devcfg.mode = 0;
        devcfg.clock_speed_hz = speed;
        devcfg.spics_io_num = _cs;
        devcfg.flags = SPI_DEVICE_HALFDUPLEX;
        devcfg.queue_size = 1;

        ret = spi_bus_add_device(SPI2_HOST, &devcfg, &_spi);
        return (ret == ESP_OK);
    }

    void writeCommand(uint8_t cmd) {
        spi_transaction_ext_t t = {};
        t.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
        t.base.cmd = ST77922_OPCODE_CMD;
        t.base.addr = ((uint32_t)cmd) << 8;
        t.base.length = 0;
        spi_device_polling_transmit(_spi, (spi_transaction_t *)&t);
    }

    void writeCommandParam(uint8_t cmd, const uint8_t *params, size_t len) {
        spi_transaction_ext_t t = {};
        t.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
        t.base.cmd = ST77922_OPCODE_CMD;
        t.base.addr = ((uint32_t)cmd) << 8;
        t.base.tx_buffer = params;
        t.base.length = len * 8;
        spi_device_polling_transmit(_spi, (spi_transaction_t *)&t);
    }

    void setAddrWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
        uint8_t ca[4] = { (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF), (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF) };
        uint8_t pa[4] = { (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF), (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF) };
        writeCommandParam(0x2A, ca, 4);
        writeCommandParam(0x2B, pa, 4);
    }

    void writePixels(const uint16_t *pixels, size_t count) {
        spi_transaction_ext_t t = {};
        t.base.flags = SPI_TRANS_MODE_QIO;
        t.base.cmd = ST77922_OPCODE_COLOR;
        t.base.addr = ST77922_RAMWR_ADDR;
        t.base.tx_buffer = pixels;
        t.base.length = count * 16;
        spi_device_polling_transmit(_spi, (spi_transaction_t *)&t);
    }

    void invertDisplay(bool invert) {
        writeCommand(invert ? 0x21 : 0x20);
    }

    void init() {
        // Sleep Out
        writeCommand(0x11);
        delay(120);
        {
            static const uint8_t d[] = {0x00};
            writeCommandParam(0xF1, d, 1);
        }
        {
            static const uint8_t d[] = {0x00, 0x00, 0x00};
            writeCommandParam(0x60, d, 3);
        }
        {
            static const uint8_t d[] = {0x80};
            writeCommandParam(0x65, d, 1);
        }
        {
            static const uint8_t d[] = {0x06};
            writeCommandParam(0x79, d, 1);
        }
        {
            static const uint8_t d[] = {0x00, 0x08, 0x08};
            writeCommandParam(0x7B, d, 3);
        }
        {
            static const uint8_t d[] = {0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA};
            writeCommandParam(0x80, d, 11);
        }
        {
            static const uint8_t d[] = {0x26, 0x52, 0x72, 0x27};
            writeCommandParam(0x81, d, 4);
        }
        {
            static const uint8_t d[] = {0x92, 0x25};
            writeCommandParam(0x84, d, 2);
        }
        {
            static const uint8_t d[] = {0x10, 0x10, 0x58, 0x00, 0x02, 0x3A};
            writeCommandParam(0x87, d, 6);
        }
        {
            static const uint8_t d[] = {0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06};
            writeCommandParam(0x88, d, 15);
        }
        {
            static const uint8_t d[] = {0x00, 0x00, 0x00};
            writeCommandParam(0x89, d, 3);
        }
        {
            static const uint8_t d[] = {0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19};
            writeCommandParam(0x8A, d, 11);
        }
        {
            static const uint8_t d[] = {0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E};
            writeCommandParam(0x8B, d, 9);
        }
        {
            static const uint8_t d[] = {0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09};
            writeCommandParam(0x8C, d, 13);
        }
        {
            static const uint8_t d[] = {0x0C};
            writeCommandParam(0x8D, d, 1);
        }
        {
            static const uint8_t d[] = {0x33, 0x01, 0x0C, 0x13, 0x01, 0x01};
            writeCommandParam(0x8E, d, 6);
        }
        {
            static const uint8_t d[] = {0x00, 0x30};
            writeCommandParam(0xB3, d, 2);
        }
        {
            static const uint8_t d[] = {0x00};
            writeCommandParam(0xF1, d, 1);
        }
        {
            static const uint8_t d[] = {0xD0};
            writeCommandParam(0x71, d, 1);
        }
        {
            static const uint8_t d[] = {0x02, 0x3F};
            writeCommandParam(0x66, d, 2);
        }
        {
            static const uint8_t d[] = {0x26, 0x00, 0x9D};
            writeCommandParam(0xBE, d, 3);
        }
        {
            static const uint8_t d[] = {0x01, 0xA0, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x69, 0x11, 0x00, 0x00, 0x1A};
            writeCommandParam(0x70, d, 12);
        }
        {
            static const uint8_t d[] = {0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x27, 0x27};
            writeCommandParam(0x90, d, 9);
        }
        {
            static const uint8_t d[] = {0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x27, 0x27};
            writeCommandParam(0x91, d, 9);
        }
        {
            static const uint8_t d[] = {0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x27};
            writeCommandParam(0x92, d, 10);
        }
        {
            static const uint8_t d[] = {0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x27};
            writeCommandParam(0x93, d, 10);
        }
        {
            static const uint8_t d[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            writeCommandParam(0x94, d, 6);
        }
        {
            static const uint8_t d[] = {0x96, 0x16, 0x00, 0x00, 0xFF};
            writeCommandParam(0x95, d, 5);
        }
        {
            static const uint8_t d[] = {0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x94, 0x27, 0x00, 0x44};
            writeCommandParam(0x96, d, 12);
        }
        {
            static const uint8_t d[] = {0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x94, 0x27, 0x00, 0x44};
            writeCommandParam(0x97, d, 12);
        }
        {
            static const uint8_t d[] = {0x55, 0x94, 0x2D, 0x94, 0x27};
            writeCommandParam(0xBA, d, 5);
        }
        {
            static const uint8_t d[] = {0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00};
            writeCommandParam(0x9A, d, 7);
        }
        {
            static const uint8_t d[] = {0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00};
            writeCommandParam(0x9B, d, 7);
        }
        {
            static const uint8_t d[] = {0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00};
            writeCommandParam(0x9C, d, 13);
        }
        {
            static const uint8_t d[] = {0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01};
            writeCommandParam(0x9D, d, 8);
        }
        {
            static const uint8_t d[] = {0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01};
            writeCommandParam(0x9E, d, 7);
        }
        {
            static const uint8_t d[] = {0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10};
            writeCommandParam(0xB4, d, 12);
        }
        {
            static const uint8_t d[] = {0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12};
            writeCommandParam(0xB5, d, 12);
        }
        {
            static const uint8_t d[] = {0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF};
            writeCommandParam(0xB6, d, 7);
        }
        {
            static const uint8_t d[] = {0xCD, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5};
            writeCommandParam(0x86, d, 14);
        }
        {
            static const uint8_t d[] = {0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D};
            writeCommandParam(0xB7, d, 16);
        }
        {
            static const uint8_t d[] = {0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D};
            writeCommandParam(0xB8, d, 16);
        }
        {
            static const uint8_t d[] = {0x23, 0x23};
            writeCommandParam(0xB9, d, 2);
        }
        {
            static const uint8_t d[] = {0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B};
            writeCommandParam(0xBF, d, 6);
        }
        {
            static const uint8_t d[] = {0x00};
            writeCommandParam(0xF2, d, 1);
        }
        {
            static const uint8_t d[] = {0x04, 0xDA, 0x12, 0x54, 0x47};
            writeCommandParam(0x73, d, 5);
        }
        {
            static const uint8_t d[] = {0x6B, 0x5B, 0xFD, 0xC3, 0xC5};
            writeCommandParam(0x77, d, 5);
        }
        {
            static const uint8_t d[] = {0x15, 0x27};
            writeCommandParam(0x7A, d, 2);
        }
        {
            static const uint8_t d[] = {0x04, 0x57};
            writeCommandParam(0x7B, d, 2);
        }
        {
            static const uint8_t d[] = {0x01, 0x0E};
            writeCommandParam(0x7E, d, 2);
        }
        {
            static const uint8_t d[] = {0x36};
            writeCommandParam(0xBF, d, 1);
        }
        {
            static const uint8_t d[] = {0x40, 0x40};
            writeCommandParam(0xE3, d, 2);
        }
        {
            static const uint8_t d[] = {0x00};
            writeCommandParam(0xF0, d, 1);
        }
        {
            static const uint8_t d[] = {0x00};
            writeCommandParam(0xD0, d, 1);
        }
        {
            static const uint8_t d[] = {0x00, 0x00, 0x01, 0x3F};
            writeCommandParam(0x2A, d, 4);
        }
        {
            static const uint8_t d[] = {0x00, 0x00, 0x01, 0xDF};
            writeCommandParam(0x2B, d, 4);
        }
        writeCommand(0x21); // 0x21 = Display Inversion ON (IPS panel inverted color fix - prevents white screen)
        writeCommand(0x11);
        delay(120);
        writeCommand(0x29);
        writeCommand(0x2C);
        {
            static const uint8_t d[] = {0x01};
            writeCommandParam(0x3A, d, 1);
        }
        {
            static const uint8_t d[] = {0x00};
            writeCommandParam(0x36, d, 1);
        }
        {
            static const uint8_t d[] = {0x01};
            writeCommandParam(0x35, d, 1);
        }
        delay(20);

        // Display ON
        writeCommand(0x29);
        delay(20);
    }
};
