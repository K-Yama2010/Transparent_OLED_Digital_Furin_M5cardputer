#include <M5Cardputer.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <LittleFS.h>
#include <driver/i2s.h>

#define W 120
#define H 67

M5Canvas canvas(&M5Cardputer.Display);

int16_t bufR1[W * H] = {0}, bufR2[W * H] = {0};
int16_t bufG1[W * H] = {0}, bufG2[W * H] = {0};
int16_t bufB1[W * H] = {0}, bufB2[W * H] = {0};

int16_t *pR1 = bufR1, *pR2 = bufR2;
int16_t *pG1 = bufG1, *pG2 = bufG2;
int16_t *pB1 = bufB1, *pB2 = bufB2;

bool prevPressed[256] = {false};
uint32_t lastKeyboardAccess = 0;

volatile int currentVolume = 180;

volatile bool countReceivedFlag = false;
volatile bool playTriggered = false;
volatile int selectedWav = 0;

// ボリューム表示用変数
uint32_t volPopupStartTime = 0;
bool showVolPopup = false;

// CHIME RATE表示用変数と確率設定
uint32_t ratePopupStartTime = 0;
bool showRatePopup = false;
int playProbIndex = 3; // 0: 25%, 1: 33%, 2: 50%, 3: 100%
int playProbability[4] = {25, 33, 50, 100};

void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    countReceivedFlag = true;
}

void audioTask(void *pvParameters) {
    int16_t buf[1024]; // 2048バイトのバッファ
    File f;
    bool playing = false;
    while(1) {
        if (playTriggered) {
            playTriggered = false;
            if (f) f.close();
            // I2S DMAバッファをクリアして新しく再生開始（ノイズ防止）
            i2s_zero_dma_buffer(I2S_NUM_0);
            String path;
            if (selectedWav == 0) path = "/sound1.wav";
            else if (selectedWav == 1) path = "/sound2.wav";
            else path = "/sound3.wav";
            f = LittleFS.open(path, "r");
            if (f) {
                f.seek(44);
                playing = true;
            }
        }

        if (playing && f) {
            size_t bytes = f.read((uint8_t*)buf, sizeof(buf));
            if (bytes > 0) {
                // ソフトウェアボリューム制御（読み込んだ波形データにボリューム比率を掛ける）
                for (int i = 0; i < bytes / 2; i++) {
                    buf[i] = (int16_t)(((int32_t)buf[i] * currentVolume) / 255);
                }
                size_t bytes_written;
                // I2Sへ直接データを流し込む（バッファが空くまで自動的にブロックして待機するため途切れない）
                i2s_write(I2S_NUM_0, buf, bytes, &bytes_written, portMAX_DELAY);
            } else {
                playing = false;
                f.close();
            }
        } else {
            vTaskDelay(10);
        }
    }
}

void resetI2CBus() {
    pinMode(SDA, OUTPUT_OPEN_DRAIN);
    pinMode(SCL, OUTPUT_OPEN_DRAIN);
    for (int i = 0; i < 9; i++) {
        digitalWrite(SCL, LOW); delayMicroseconds(5);
        digitalWrite(SCL, HIGH); delayMicroseconds(5);
    }
    digitalWrite(SDA, LOW); delayMicroseconds(5);
    digitalWrite(SDA, HIGH);
    Wire.end();
    Wire.begin(SDA, SCL);
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);
    
    // 一度M5Unifiedのスピーカーを立ち上げてアンプを確実に初期化する
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.tone(1000, 10);
    delay(50);
    
    // M5Unifiedのスピーカー管理を停止し、I2Sを直接制御するために解放
    M5Cardputer.Speaker.end();

    // I2Sの初期化設定（M5CardputerのMAX98357A向け）
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 48000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = 41,
        .ws_io_num = 43,
        .data_out_num = 42,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);

    canvas.setColorDepth(16);
    canvas.createSprite(W, H);

    LittleFS.begin(true);

    WiFi.mode(WIFI_STA);
    String mac = WiFi.macAddress();
    
    canvas.fillScreen(canvas.color888(0, 0, 0));
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font0); // フォントを小さくしてはみ出しを修正
    canvas.setTextSize(1);
    canvas.setTextColor(canvas.color888(255, 255, 255));
    
    // MACアドレスを2段に分けて表示
    canvas.drawString("MAC:", W / 2, H / 2 - 8);
    canvas.drawString(mac, W / 2, H / 2 + 8);
    
    canvas.pushRotateZoom(120, 67.5, 0, 2.0, 2.0);
    
    delay(4000);
    canvas.fillScreen(canvas.color888(0, 0, 0));
    
    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(OnDataRecv);
    }

    // 音声タスクのスタックサイズを8192に拡大してオーバーフロー（リセット）を防止
    xTaskCreatePinnedToCore(audioTask, "audioTask", 8192, NULL, 1, NULL, 0);
}

void updateWave(int16_t* b1, int16_t* b2) {
    int16_t *ptr1 = b1 + W + 1;
    int16_t *ptr2 = b2 + W + 1;
    for (int y = 1; y < H - 1; y++) {
        for (int x = 1; x < W - 1; x++) {
            *ptr2 = ((*(ptr1 - 1) + *(ptr1 + 1) + *(ptr1 - W) + *(ptr1 + W)) >> 1) - *ptr2;
            *ptr2 -= *ptr2 >> 5;
            ptr1++;
            ptr2++;
        }
        ptr1 += 2;
        ptr2 += 2;
    }
}

void injectDrop(int px, int py, int force) {
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int x = px + dx, y = py + dy;
            if (x > 0 && x < W - 1 && y > 0 && y < H - 1) {
                int i = y * W + x;
                pG1[i] = force;
                pB1[i] = force;
            }
        }
    }
}

void loop() {
    if (countReceivedFlag) {
        countReceivedFlag = false;
        injectDrop(random(10, W - 10), random(10, H - 10), 6000);
        selectedWav = random(0, 3);
        if (random(0, 100) < playProbability[playProbIndex]) {
            playTriggered = true;
        }
    }

    updateWave(pR1, pR2); updateWave(pG1, pG2); updateWave(pB1, pB2);
    int16_t *temp;
    temp = pR1; pR1 = pR2; pR2 = temp;
    temp = pG1; pG1 = pG2; pG2 = temp;
    temp = pB1; pB1 = pB2; pB2 = temp;

    uint16_t *img_buf = (uint16_t *)canvas.getBuffer();
    int16_t *rPtr = pR2;
    int16_t *gPtr = pG2;
    int16_t *bPtr = pB2;
    for (int i = 0; i < W * H; i++) {
        int r = *rPtr++; if (r < 0) r = -r; if (r > 255) r = 255;
        int g = *gPtr++; if (g < 0) g = -g; if (g > 255) g = 255;
        int b = *bPtr++; if (b < 0) b = -b; if (b > 255) b = 255;
        uint16_t color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        *img_buf++ = (color >> 8) | (color << 8);
    }

    // ボリューム表示
    if (showVolPopup) {
        if (millis() - volPopupStartTime > 1000) {
            showVolPopup = false;
        } else {
            canvas.setTextDatum(bottom_right);
            canvas.setFont(&fonts::Font0); // 見やすいフォントサイズ
            canvas.setTextSize(1);
            canvas.setTextColor(canvas.color888(255, 255, 255));
            canvas.drawString("VOL:" + String(currentVolume), W - 2, H - 2);
        }
    }

    // CHIME RATE表示
    if (showRatePopup) {
        if (millis() - ratePopupStartTime > 1000) {
            showRatePopup = false;
        } else {
            canvas.setTextDatum(bottom_right);
            canvas.setFont(&fonts::Font0); 
            canvas.setTextSize(1);
            canvas.setTextColor(canvas.color888(255, 255, 255));
            canvas.drawString("CHIME RATE:" + String(playProbability[playProbIndex]) + "%", W - 2, H - 2);
        }
    }

    canvas.pushRotateZoom(120, 67.5, 0, 2.0, 2.0);

    if (millis() - lastKeyboardAccess > 30) {
        M5Cardputer.update();
        auto state = M5Cardputer.Keyboard.keysState();

        bool currentPressed[256] = {false};
        for (auto k : state.word) {
            uint8_t key_code = (uint8_t)k;
            if (key_code >= 'A' && key_code <= 'Z') key_code = key_code - 'A' + 'a';
            else {
                switch (key_code) {
                    case '!': key_code = '1'; break; case '@': key_code = '2'; break; case '#': key_code = '3'; break;
                    case '$': key_code = '4'; break; case '%': key_code = '5'; break; case '^': key_code = '6'; break;
                    case '&': key_code = '7'; break; case '*': key_code = '8'; break; case '(': key_code = '9'; break;
                    case ')': key_code = '0'; break; case '_': key_code = '-'; break; case '+': key_code = '='; break;
                    case '{': key_code = '['; break; case '}': key_code = ']'; break; case '|': key_code = '\\'; break;
                    case ':': key_code = ';'; break; case '"': key_code = '\''; break; case '<': key_code = ','; break;
                    case '>': key_code = '.'; break; case '?': key_code = '/'; break; case '~': key_code = '`'; break;
                }
            }
            currentPressed[key_code] = true;
        }
        if (state.ctrl) currentPressed[128] = true;
        if (state.shift) currentPressed[129] = true;
        if (state.fn) currentPressed[130] = true;
        if (state.alt) currentPressed[131] = true;
        if (state.opt) currentPressed[132] = true;
        if (state.tab) currentPressed[9] = true;
        if (state.enter) currentPressed['\n'] = true;
        if (state.del) currentPressed[8] = true;

        for (int k = 0; k < 256; k++) {
            if (currentPressed[k] && !prevPressed[k]) {
                if (k == ';') {
                    currentVolume += 10;
                    if (currentVolume > 255) currentVolume = 255;
                    showVolPopup = true;
                    volPopupStartTime = millis();
                    showRatePopup = false; // 表示被りを防ぐ
                } else if (k == '.') {
                    currentVolume -= 10;
                    if (currentVolume < 0) currentVolume = 0;
                    showVolPopup = true;
                    volPopupStartTime = millis();
                    showRatePopup = false; // 表示被りを防ぐ
                } else if (k == '1') {
                    injectDrop(random(10, W - 10), random(10, H - 10), 6000);
                    selectedWav = 0;
                    if (random(0, 100) < playProbability[playProbIndex]) {
                        playTriggered = true;
                    }
                } else if (k == '2') {
                    injectDrop(random(10, W - 10), random(10, H - 10), 6000);
                    selectedWav = 1;
                    if (random(0, 100) < playProbability[playProbIndex]) {
                        playTriggered = true;
                    }
                } else if (k == '3') {
                    injectDrop(random(10, W - 10), random(10, H - 10), 6000);
                    selectedWav = 2;
                    if (random(0, 100) < playProbability[playProbIndex]) {
                        playTriggered = true;
                    }
                } else if (k == ',') {
                    playProbIndex--;
                    if (playProbIndex < 0) playProbIndex = 0;
                    showRatePopup = true;
                    ratePopupStartTime = millis();
                    showVolPopup = false; // 表示被りを防ぐ
                } else if (k == '/') {
                    playProbIndex++;
                    if (playProbIndex > 3) playProbIndex = 3;
                    showRatePopup = true;
                    ratePopupStartTime = millis();
                    showVolPopup = false; // 表示被りを防ぐ
                }
            }
            prevPressed[k] = currentPressed[k];
        }
        lastKeyboardAccess = millis();
    }

    vTaskDelay(1); // システムの息継ぎ（ディレイ・音途切れ解消の要）
}