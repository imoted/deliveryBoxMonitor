#include <M5AtomS3.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include "FS.h"
#include "SPIFFS.h"

const char *FW_VERSION = "1.1.0";

const int BOX_NUM = 2;

const int HCSR04Trg[] = {8, 6};   //使用するGPIOピン
const int HCSR04Echo[] = {7, 5};  //使用するGPIOピン

const String BOX_LABEL[] = {"上段", "下段"};

const char *SSID = nullptr;
const char *PASSWORD = nullptr;
const char *HOST = "api.line.me";
const char *USER_ID = nullptr;
const char *CHANNEL_TOKEN = nullptr;

// OTA設定（config.iniで上書き可能）
const char *OTA_HOSTNAME = "boxmonitor";
const char *OTA_PASSWORD = "boxmonitor";
const int OTA_PORT = 3232;

const int BOX_OCCUPIED_THRE = 50.0;  // cm
const int CNT_THRETHOLD = 50;

int cnt_detected = 0;
const int INTERVAL = 10 * 1000;  // ms
const int INTERVAL_LONG = 6 * 60 * 12;  // 10sec * 6 * 60 * 12h = 12h

// WiFi再接続の非同期制御
const unsigned long WIFI_ATTEMPT_TIMEOUT = 10 * 1000;  // ms
const unsigned long WIFI_RETRY_INTERVAL = 5 * 1000;    // ms

// INIファイルのパース関数
void parse_config(File file) {
    String line;
    while (file.available()) {
        line = file.readStringUntil('\n');
        line.trim();  // 前後の空白を取り除く

        // セクション行をスキップ
        if (line.startsWith("[") && line.endsWith("]")) {
            continue;
        }

        // コメント行をスキップ
        if (line.startsWith("#") || line.startsWith(";")) {
            continue;
        }

        int separator_pos = line.indexOf('=');
        if (separator_pos == -1) {
            continue;  // '=' がない行は無視
        }

        String key = line.substring(0, separator_pos);
        String value = line.substring(separator_pos + 1);
        key.trim();
        value.trim();

        // キーに応じて変数に値を代入
        if (key == "WIFI_SSID") {
            SSID = strdup(value.c_str());
        } else if (key == "WIFI_PASSWORD") {
            PASSWORD = strdup(value.c_str());
        } else if (key == "USER_ID") {
            USER_ID = strdup(value.c_str());
        } else if (key == "CHANNEL_TOKEN") {
            CHANNEL_TOKEN = strdup(value.c_str());
        } else if (key == "OTA_HOSTNAME") {
            OTA_HOSTNAME = strdup(value.c_str());
        } else if (key == "OTA_PASSWORD") {
            OTA_PASSWORD = strdup(value.c_str());
        }
    }
}

bool line_notify(String msg)
{
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(HOST, 443)) {
    AtomS3.dis.drawpix(0xFFFF00);  // Yellow
    AtomS3.update();
    return false;
  }

  // JSONデータの作成
  String jsonData = "{\"to\":\"" + String(USER_ID) + "\",\"messages\":[{\"type\":\"text\",\"text\":\"" + msg + "\"}]}";

  String request = String("POST /v2/bot/message/push HTTP/1.1\r\n") +
                   "Host: " + HOST + "\r\n" +
                   "Authorization: Bearer " + CHANNEL_TOKEN + "\r\n" +
                   "Content-Type: application/json\r\n" +
                   "Content-Length: " + String(jsonData.length()) + "\r\n" +
                   "Connection: close\r\n\r\n" +
                   jsonData;

  client.print(request);

  // レスポンスの確認（オプション）
  // タイムアウトの設定
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      client.stop();
      return false;
    }
  }

  // レスポンスの読み取り
  while (client.available()) {
    String line = client.readStringUntil('\r');
    // 必要に応じてレスポンスを処理
  }

  return true;
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
bool ota_initialized = false;

void setup_ota()
{
  if (ota_initialized) {
    return;
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setPort(OTA_PORT);

  ArduinoOTA.onStart([]() {
    // ファイルシステム更新の場合はSPIFFSをアンマウントしてから書き換える
    if (ArduinoOTA.getCommand() == U_SPIFFS) {
      SPIFFS.end();
      USBSerial.println("OTA start: filesystem");
    } else {
      USBSerial.println("OTA start: sketch");
    }
    AtomS3.dis.drawpix(0xff00ff);  //紫色: OTA書き込み中
    AtomS3.update();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    unsigned int percent = total ? (progress * 100 / total) : 0;
    USBSerial.printf("OTA progress: %u%%\r\n", percent);
    // 10%ごとにLEDを点滅させて進行中であることを示す
    AtomS3.dis.drawpix((percent / 10) % 2 ? 0xff00ff : 0x000000);
    AtomS3.update();
  });

  ArduinoOTA.onEnd([]() {
    USBSerial.println("OTA end. rebooting...");
    AtomS3.dis.drawpix(0x00ff00);  //緑色: OTA成功
    AtomS3.update();
  });

  ArduinoOTA.onError([](ota_error_t error) {
    USBSerial.printf("OTA error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      USBSerial.println("auth failed");
    } else if (error == OTA_BEGIN_ERROR) {
      USBSerial.println("begin failed");
    } else if (error == OTA_CONNECT_ERROR) {
      USBSerial.println("connect failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      USBSerial.println("receive failed");
    } else if (error == OTA_END_ERROR) {
      USBSerial.println("end failed");
    }
    AtomS3.dis.drawpix(0xff0000);  //赤色: OTA失敗
    AtomS3.update();
    delay(2000);
    // ファイルシステム更新に失敗した場合はSPIFFSを再マウントして通常動作へ戻す
    if (ArduinoOTA.getCommand() == U_SPIFFS) {
      SPIFFS.begin(true);
    }
    AtomS3.dis.drawpix(0x0000ff);  //青色
    AtomS3.update();
  });

  ArduinoOTA.begin();
  ota_initialized = true;
  USBSerial.println("OTA ready: " + String(OTA_HOSTNAME) + ".local:" + String(OTA_PORT) +
                    " (" + WiFi.localIP().toString() + ") fw=" + String(FW_VERSION));
}

// WiFi切断中はmDNS/UDPが無効になるため、再接続時に張り直す
void teardown_ota()
{
  if (!ota_initialized) {
    return;
  }
  ArduinoOTA.end();
  ota_initialized = false;
}

// ---------------------------------------------------------------------------
// WiFi（ノンブロッキング再接続）
// ---------------------------------------------------------------------------
bool wifi_connecting = false;
unsigned long wifi_attempt_start = 0;
unsigned long wifi_retry_at = 0;

void handle_wifi()
{
  if (WiFi.status() == WL_CONNECTED) {
    if (wifi_connecting) {
      wifi_connecting = false;
      USBSerial.println("Reconnected to WiFi!");
      AtomS3.dis.drawpix(0x0000ff);  //青色
      AtomS3.update();
    }
    setup_ota();  // 初回、および再接続後の張り直し
    return;
  }

  teardown_ota();

  if (wifi_connecting) {
    if (millis() - wifi_attempt_start > WIFI_ATTEMPT_TIMEOUT) {
      wifi_connecting = false;
      wifi_retry_at = millis() + WIFI_RETRY_INTERVAL;
      USBSerial.println("Failed to reconnect to WiFi.");
      AtomS3.dis.drawpix(0xffff00);  //黄色
      AtomS3.update();
    }
    return;
  }

  if ((long)(millis() - wifi_retry_at) < 0) {
    return;  // バックオフ待ち
  }

  USBSerial.println("WiFi disconnected. Attempting to reconnect...");
  AtomS3.dis.drawpix(0xff0000);  //赤色
  AtomS3.update();

  WiFi.disconnect();
  WiFi.begin(SSID, PASSWORD);
  wifi_connecting = true;
  wifi_attempt_start = millis();
}

class DeliveryBox
{
public:
  int judgeBoxOccupied(double distance);
  double measureDist(int Trg, int Echo);

private:
  int cnt_detected;
};

int DeliveryBox::judgeBoxOccupied(double distance)
{
  int status = 0;
  if (cnt_detected == CNT_THRETHOLD) {
    if (distance < BOX_OCCUPIED_THRE) {
      status = 2;
    }
  }
  else if (cnt_detected % INTERVAL_LONG == 0 && cnt_detected > 0) {
    if (distance < BOX_OCCUPIED_THRE) {
      status = 3;
    }
  }
  if (cnt_detected > CNT_THRETHOLD && BOX_OCCUPIED_THRE < distance) {
    status = 4;
  }
  if (distance < BOX_OCCUPIED_THRE) {
    cnt_detected++;
  } else {
    cnt_detected = 0;
  }
  return status;
}

double DeliveryBox::measureDist(int Trg, int HCSR04Echo)
{
  digitalWrite(Trg, LOW);
  delayMicroseconds(2);
  digitalWrite(Trg, HIGH);
  delayMicroseconds(5);
  digitalWrite(Trg, LOW);
  long duration = pulseIn(HCSR04Echo, HIGH);  //[usec] 指示しないのでTimeoutは1秒
  double soundvelocity = 34350. / 1000000.;   //[cm/usec at 20 degrees Celsius]
  double distance = duration * soundvelocity / 2.;
  return distance;
}

DeliveryBox deliveryBox[BOX_NUM];

// INTERVALごとにボックスを1つずつ順番に計測する
int current_box = 0;
unsigned long last_measure_at = 0;

void measure_box(int i)
{
  double distance = deliveryBox[i].measureDist(HCSR04Trg[i], HCSR04Echo[i]);
  USBSerial.println(BOX_LABEL[i] + "の距離: " + String(distance) + "cm");
  int box_status = deliveryBox[i].judgeBoxOccupied(distance);
  if (box_status == 2) {
    line_notify(BOX_LABEL[i] + "に荷物が入りました。");
  } else if (box_status == 3) {
    line_notify(BOX_LABEL[i] + "に荷物が入ったままです。");
  } else if (box_status == 4) {
    line_notify(BOX_LABEL[i] + "の荷物が取り出されました。");
  }
}

void setup()
{
  AtomS3.begin(true);  // Init M5AtomS3Lite.
  USBSerial.begin(115200);
  AtomS3.dis.setBrightness(100);

  USBSerial.println("BoxMonitor fw " + String(FW_VERSION));

  if (!SPIFFS.begin(true)) {
      USBSerial.println("SPIFFS Mount Failed");
      return;
  }

  File file = SPIFFS.open("/config.ini", "r");
  if (!file) {
      USBSerial.println("Failed to open config file");
      return;
  }

  parse_config(file);
  file.close();

  for (int i = 0; i < BOX_NUM; i++) {
    pinMode(HCSR04Trg[i], OUTPUT);
    pinMode(HCSR04Echo[i], INPUT);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // OTAの取りこぼしを防ぐ
  WiFi.begin(SSID, PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    AtomS3.dis.drawpix(0xff0000);  //赤色
    AtomS3.update();
    delay(300);
  }
  AtomS3.dis.drawpix(0x0000ff);  //青色
  AtomS3.update();

  setup_ota();

  last_measure_at = millis() - INTERVAL;  // 起動直後に1回目の計測を実行する
}

void loop()
{
  // OTAは常時受け付けたいので、loopはブロックせずに高速で回す
  if (ota_initialized) {
    ArduinoOTA.handle();
  }

  handle_wifi();

  if (millis() - last_measure_at >= (unsigned long)INTERVAL) {
    last_measure_at = millis();
    measure_box(current_box);
    current_box = (current_box + 1) % BOX_NUM;
  }

  delay(10);
}
