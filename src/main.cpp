// Experimental half-duplex, push-to-talk Codec2 voice link.
// ESP32 DevKit + external SX1262.
// Experimental half-duplex PTT voice link.
#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <RadioLib.h>
#include <codec2.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
// Increase Arduino loopTask stack for Codec2 operations.
size_t getArduinoLoopTaskStackSize(void) {
    return 32768;
}
#ifndef NODE_ID
#define NODE_ID 8
#endif
// ESP32 + SX1262 wiring
static constexpr int PIN_NSS  = 5;
static constexpr int PIN_DIO1 = 26;
static constexpr int PIN_RST  = 14;
static constexpr int PIN_BUSY = 27;
static constexpr int PIN_SCK  = 18;
static constexpr int PIN_MISO = 19;
static constexpr int PIN_MOSI = 23;
// LoRa parameters
// Verify legal RF operating settings before transmitting.
static constexpr float LORA_FREQ_MHZ = 866.0f;
static constexpr float LORA_BW_KHZ   = 500.0f;
static constexpr uint8_t LORA_SF = 7;
static constexpr uint8_t LORA_CR = 5;
static constexpr int8_t LORA_POWER_DBM = 14;
// Codec2 parameters
static constexpr int FRAMES_PER_PACKET = 10;
static constexpr int MAX_SAMPLES_PER_FRAME = 320;
static constexpr int MAX_BYTES_PER_FRAME = 16;
static constexpr size_t PCM_BUFFER_BYTES = 8192;
static constexpr uint32_t PARTIAL_FLUSH_MS = 260;
// Three-byte control frames are distinguishable from Codec2 voice frames.
static constexpr uint8_t CTRL_MAGIC = 0xAC;
static constexpr uint8_t CTRL_START = 0xA1;
static constexpr uint8_t CTRL_END = 0xA2;
static constexpr uint32_t REMOTE_TIMEOUT_MS = 2200;
static constexpr uint32_t CLAIM_LISTEN_MS = 180;
AsyncWebServer server(80);
AsyncWebSocket ws("/audio");
SX1262 radio = new Module(
    PIN_NSS,
    PIN_DIO1,
    PIN_RST,
    PIN_BUSY
);
CODEC2* encoder = nullptr;
CODEC2* decoder = nullptr;
StreamBufferHandle_t pcmQueue = nullptr;
int samplesPerFrame = 0;
int bytesPerFrame = 0;
volatile bool rxInterrupt = false;
volatile bool pttRequested = false;
volatile bool radioReady = false;
bool transmitting = false;
bool receiving = false;
bool remoteBusy = false;
uint8_t remoteNode = 0;
uint32_t lastRemoteMs = 0;
uint32_t claimStartedMs = 0;
bool claimPending = false;
uint32_t lastStatusMs = 0;
uint8_t txSeq = 0;
uint8_t lastRxSeq = 0;
bool haveRxSeq = false;
uint32_t lastRxMs = 0;
uint32_t lastFrameMs = 0;
int pendingFrames = 0;
uint8_t txPacket[
    1 + FRAMES_PER_PACKET * MAX_BYTES_PER_FRAME
];
int16_t pcmFrame[MAX_SAMPLES_PER_FRAME];
int16_t decoded[
    FRAMES_PER_PACKET * MAX_SAMPLES_PER_FRAME
];
uint32_t txPackets = 0;
uint32_t rxPackets = 0;
uint32_t errors = 0;
void IRAM_ATTR onRadioDio1() {
    rxInterrupt = true;
}
void beginReceive() {
    rxInterrupt = false;
    int rc = radio.startReceive();
    receiving = (rc == RADIOLIB_ERR_NONE);
    if (!receiving) {
        errors++;
        Serial.printf("startReceive error %d\r\n", rc);
    }
}
// Called from loopTask only (never from the WebSocket callback).
void publishChannel() {
    char msg[190];
    snprintf(msg, sizeof(msg),
        "{\"type\":\"channel\",\"node\":%d,\"busy\":%s,\"speaker\":%u,\"localTx\":%s,\"rx\":%lu,\"tx\":%lu}",
        NODE_ID, remoteBusy ? "true" : "false",
        remoteBusy ? (unsigned)remoteNode : 0u,
        transmitting ? "true" : "false",
        (unsigned long)rxPackets, (unsigned long)txPackets);
    ws.textAll(msg);
}

void sendControl(uint8_t kind) {
    uint8_t packet[3] = {CTRL_MAGIC, kind, (uint8_t)NODE_ID};
    receiving = false;
    int rc = radio.transmit(packet, sizeof(packet));
    if (rc != RADIOLIB_ERR_NONE) {
        errors++;
        Serial.printf("CTRL TX error %d\r\n", rc);
    } else {
        Serial.printf("CTRL %s N%d\r\n", kind == CTRL_START ? "START" : "END", NODE_ID);
    }
    beginReceive();
}

void sendPending() {
    if (pendingFrames == 0) {
        return;
    }
    txPacket[0] = txSeq++;
    size_t len = 1 + pendingFrames * bytesPerFrame;
    receiving = false;
    // SX1262 transmission is blocking and half-duplex.
    int rc = radio.transmit(txPacket, len);
    if (rc == RADIOLIB_ERR_NONE) {
        txPackets++;
        Serial.printf(
            "TX seq=%u bytes=%u\r\n",
            txPacket[0],
            (unsigned)len
        );
    } else {
        errors++;
        Serial.printf(
            "TX error %d\r\n",
            rc
        );
    }
    pendingFrames = 0;
    beginReceive();
}
void processRx() {
    if (!rxInterrupt || transmitting) {
        return;
    }
    rxInterrupt = false;
    size_t len = radio.getPacketLength();
    if (len < 2 || len > 255) {
        beginReceive();
        return;
    }
    uint8_t packet[255];
    int rc = radio.readData(packet, len);
    if (rc != RADIOLIB_ERR_NONE) {
        errors++;
        beginReceive();
        return;
    }
    // Control frames never enter the Codec2 decoder.
    if (len == 3 && packet[0] == CTRL_MAGIC &&
        (packet[1] == CTRL_START || packet[1] == CTRL_END)) {
        const uint8_t sender = packet[2];
        if (sender != NODE_ID) {
            if (packet[1] == CTRL_START) {
                remoteBusy = true;
                pttRequested = false; // Reject a competing local PTT request.
                claimPending = false;
                remoteNode = sender;
                lastRemoteMs = millis();
                Serial.printf("REMOTE N%u TALKING\r\n", (unsigned)sender);
            } else if (remoteBusy && remoteNode == sender) {
                remoteBusy = false;
                remoteNode = 0;
                Serial.printf("REMOTE N%u RELEASED\r\n", (unsigned)sender);
            }
            publishChannel();
        }
        beginReceive();
        return;
    }
    if ((len - 1) % bytesPerFrame != 0) {
        errors++;
        beginReceive();
        return;
    }
    int frames = (len - 1) / bytesPerFrame;
    if (frames < 1 || frames > FRAMES_PER_PACKET) {
        beginReceive();
        return;
    }
    rxPackets++;
    // Any voice packet also refreshes the busy lease, even if START was lost.
    remoteBusy = true;
    pttRequested = false;
    claimPending = false;
    remoteNode = (NODE_ID == 8 ? 9 : 8); // This demo has exactly two nodes.
    lastRemoteMs = millis();
    publishChannel();

    const uint32_t now = millis();
    if (now - lastRxMs > 1200) {
        haveRxSeq = false;
    }
    lastRxMs = now;
    uint8_t seq = packet[0];
    if (haveRxSeq) {
        uint8_t gap = (uint8_t)(seq - lastRxSeq - 1);
        if (gap > 0 && gap <= 2 && ws.count() > 0) {
            static int16_t silence[
                FRAMES_PER_PACKET * MAX_SAMPLES_PER_FRAME
            ] = {0};
            for (int i = 0; i < gap; i++) {
                ws.binaryAll(
                    (uint8_t*)silence,
                    FRAMES_PER_PACKET * samplesPerFrame * 2
                );
            }
        }
    }
    haveRxSeq = true;
    lastRxSeq = seq;
    for (int f = 0; f < frames; f++) {
        codec2_decode(
            decoder,
            decoded + f * samplesPerFrame,
            packet + 1 + f * bytesPerFrame
        );
    }
    if (ws.count() > 0) {
        ws.binaryAll(
            (uint8_t*)decoded,
            frames * samplesPerFrame * 2
        );
    }
    char status[128];
    snprintf(
        status,
        sizeof(status),
        "{\"rx\":%lu,\"tx\":%lu,\"rssi\":%.1f,\"snr\":%.1f}",
        (unsigned long)rxPackets,
        (unsigned long)txPackets,
        radio.getRSSI(),
        radio.getSNR()
    );
    ws.textAll(status);
    Serial.printf(
        "RX seq=%u frames=%d RSSI=%.1f\r\n",
        seq,
        frames,
        radio.getRSSI()
    );
    beginReceive();
}
void onWebSocket(
    AsyncWebSocket*,
    AsyncWebSocketClient* client,
    AwsEventType event,
    void* arg,
    uint8_t* data,
    size_t len
) {
    if (event == WS_EVT_CONNECT) {
        Serial.printf(
            "WebSocket client %u connected\r\n",
            client->id()
        );
        return;
    }
    if (event == WS_EVT_DISCONNECT) {
        pttRequested = false;
        return;
    }
    if (event != WS_EVT_DATA) {
        return;
    }
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!info ||
        !info->final ||
        info->index != 0 ||
        info->len != len) {
        return;
    }
    if (info->opcode == WS_TEXT) {
        if (len == 4 && memcmp(data, "PTT1", 4) == 0) {
            pttRequested = true;
        }
        if (len == 4 && memcmp(data, "PTT0", 4) == 0) {
            pttRequested = false;
        }
    }
    else if (
        info->opcode == WS_BINARY &&
        pttRequested &&
        pcmQueue &&
        len == (size_t)samplesPerFrame * 2
    ) {
        xStreamBufferSend(
            pcmQueue,
            data,
            len,
            0
        );
    }
}
void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.printf(
        "LoRa Voice node N%d (loopTask stack free: %u bytes)\r\n",
        NODE_ID,
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );
    encoder = codec2_create(CODEC2_MODE_2400);
    decoder = codec2_create(CODEC2_MODE_2400);
    if (!encoder || !decoder) {
        Serial.print("Codec2 init failed\r\n");
        while (true) {
            delay(1000);
        }
    }
    samplesPerFrame = codec2_samples_per_frame(encoder);
    bytesPerFrame =
        (codec2_bits_per_frame(encoder) + 7) / 8;
    Serial.printf(
        "Codec2: samples/frame=%d bytes/frame=%d (loopTask stack free: %u bytes)\r\n",
        samplesPerFrame,
        bytesPerFrame,
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );
    if (
        samplesPerFrame > MAX_SAMPLES_PER_FRAME ||
        bytesPerFrame > MAX_BYTES_PER_FRAME
    ) {
        Serial.print("Unexpected Codec2 frame size\r\n");
        while (true) {
            delay(1000);
        }
    }
    pcmQueue = xStreamBufferCreate(
        PCM_BUFFER_BYTES,
        1
    );
    if (!pcmQueue) {
        Serial.print("PCM queue allocation failed\r\n");
        while (true) {
            delay(1000);
        }
    }
    if (!LittleFS.begin(true)) {
        Serial.print("LittleFS failed\r\n");
        while (true) {
            delay(1000);
        }
    }
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    char ssid[24];
    snprintf(
        ssid,
        sizeof(ssid),
        "LORA_VOICE_%d",
        NODE_ID
    );
    WiFi.softAP(ssid, "12345678");
    Serial.printf(
        "Wi-Fi SSID=%s IP=%s\r\n",
        ssid,
        WiFi.softAPIP().toString().c_str()
    );
    ws.onEvent(onWebSocket);
    server.addHandler(&ws);
    server.serveStatic(
        "/",
        LittleFS,
        "/"
    ).setDefaultFile("index.html");
    server.begin();
    // Initialize ESP32 SPI.
    SPI.begin(
        PIN_SCK,
        PIN_MISO,
        PIN_MOSI,
        PIN_NSS
    );
    // IMPORTANT FIX:
    // Do not force TCXO voltage to 0.0.
    // Use RadioLib default TCXO setting, matching
    // the initialization style of the working ACORN firmware.
    int rc = radio.begin(
        LORA_FREQ_MHZ,
        LORA_BW_KHZ,
        LORA_SF,
        LORA_CR,
        RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
        LORA_POWER_DBM,
        8
    );
    if (rc != RADIOLIB_ERR_NONE) {
        Serial.printf(
            "SX1262 init failed: %d\r\n",
            rc
        );
        return;
    }
    Serial.print("SX1262 initialization successful\r\n");
    radio.setPacketReceivedAction(onRadioDio1);
    radioReady = true;
    beginReceive();
    Serial.println(
        "Ready: use two Android phones; PTT one phone at a time."
    );
}
void loop() {
    ws.cleanupClients();
    if (!radioReady) {
        delay(100);
        return;
    }
    if (!transmitting) {
        processRx();
    }
    // Lost END packets cannot leave the other phone locked indefinitely.
    if (remoteBusy && (uint32_t)(millis() - lastRemoteMs) > REMOTE_TIMEOUT_MS) {
        remoteBusy = false;
        remoteNode = 0;
        Serial.print("REMOTE timeout - channel ready\r\n");
        publishChannel();
    }

    if (millis() - lastStatusMs >= 750) {
        lastStatusMs = millis();
        publishChannel(); // Also updates newly connected browser clients.
    }

    bool requested = pttRequested;
    if (!requested) claimPending = false;

    // Listen briefly before claiming the channel. This reduces (but cannot
    // eliminate) simultaneous starts in a half-duplex radio network.
    if (requested && !transmitting && !remoteBusy && !claimPending) {
        claimPending = true;
        claimStartedMs = millis();
    }
    if (requested && !transmitting && !remoteBusy && claimPending &&
        (uint32_t)(millis() - claimStartedMs) >= CLAIM_LISTEN_MS) {
        claimPending = false;
        transmitting = true;
        pendingFrames = 0;
        xStreamBufferReset(pcmQueue);
        radio.standby();
        receiving = false;
        rxInterrupt = false;
        sendControl(CTRL_START);
        publishChannel();
        Serial.print("PTT start\r\n");
    }
    if (transmitting) {
        for (int i = 0; i < FRAMES_PER_PACKET; i++) {
            if (
                xStreamBufferBytesAvailable(pcmQueue) <
                (size_t)samplesPerFrame * 2
            ) {
                break;
            }
            size_t n = xStreamBufferReceive(
                pcmQueue,
                pcmFrame,
                samplesPerFrame * 2,
                0
            );
            if (n != (size_t)samplesPerFrame * 2) {
                break;
            }
            codec2_encode(
                encoder,
                txPacket + 1 + pendingFrames * bytesPerFrame,
                pcmFrame
            );
            pendingFrames++;
            lastFrameMs = millis();
            if (pendingFrames == FRAMES_PER_PACKET) {
                sendPending();
                break;
            }
        }
        if (
            pendingFrames > 0 &&
            millis() - lastFrameMs >= PARTIAL_FLUSH_MS
        ) {
            sendPending();
        }
        if (!pttRequested) {
            for (int i = 0; i < FRAMES_PER_PACKET; i++) {
                if (
                    xStreamBufferBytesAvailable(pcmQueue) <
                    (size_t)samplesPerFrame * 2
                ) {
                    break;
                }
                if (
                    xStreamBufferReceive(
                        pcmQueue,
                        pcmFrame,
                        samplesPerFrame * 2,
                        0
                    ) != (size_t)samplesPerFrame * 2
                ) {
                    break;
                }
                codec2_encode(
                    encoder,
                    txPacket + 1 + pendingFrames * bytesPerFrame,
                    pcmFrame
                );
                pendingFrames++;
                if (pendingFrames == FRAMES_PER_PACKET) {
                    sendPending();
                }
            }
            sendPending();
            xStreamBufferReset(pcmQueue);
            sendControl(CTRL_END);

            transmitting = false;
            publishChannel();
            beginReceive();
            Serial.print("PTT stop\r\n");
        }
    }
    delay(1);
}
