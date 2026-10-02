#include "UartJson.h"
#include "CommandDispatcher.h"
#include "Events.h"
#include "hw/Pins.h"

#include <Arduino.h>

namespace uartjson {

static constexpr size_t LINE_CAP = 4096;   // fits a full 64-element pattern

// -------- RX task ----------------------------------------------------------

static void rxTask(void*) {
    static char   line[LINE_CAP];
    static size_t len = 0;
    bool overflow = false;

    for (;;) {
        while (Serial.available() > 0) {
            int c = Serial.read();
            if (c < 0) break;

            if (c == '\r') continue;
            if (c == '\n') {
                if (overflow) {
                    evt::postError("", "line_too_long");
                } else if (len > 0) {
                    line[len] = '\0';
                    cmd::Result res = cmd::dispatch(line, len, livesync::Source::Uart);
                    if (res.ok) {
                        evt::postAck(res.cmd);
                    } else {
                        evt::postError(res.cmd, res.reason);
                    }
                }
                len = 0;
                overflow = false;
                continue;
            }
            if (len < LINE_CAP - 1) line[len++] = (char)c;
            else                    overflow = true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void init() {
    Serial.begin(115200, SERIAL_8N1, pins::UART_RX, pins::UART_TX);
    Serial.setRxBufferSize(2048);
    xTaskCreatePinnedToCore(rxTask, "uart_rx", 8192, nullptr, 4, nullptr, 0);
}

} // namespace uartjson
