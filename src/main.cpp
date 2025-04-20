#include <M5AtomS3.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "FS.h"
#include "SPIFFS.h"

const int BOX_NUM = 2;

const int HCSR04Trg[] = {8, 6};   //使用するGPIOピン
const int HCSR04Echo[] = {7, 5};  //使用するGPIOピン

const String BOX_LABEL[] = {"上段", "下段"};

const char *SSID = nullptr;
const char *PASSWORD = nullptr;
const char *HOST = "api.line.me";
const char *USER_ID = nullptr;
const char *CHANNEL_TOKEN = nullptr;

const int BOX_OCCUPIED_THRE = 50.0;  // cm
const int CNT_THRETHOLD = 50;

int cnt_detected = 0;
const int INTERVAL = 10 * 1000;  // ms
const int INTERVAL_LONG = 6 * 60 * 12;  // 10sec * 6 * 60 * 12h = 12h

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

void setup()
{
  AtomS3.begin(true);  // Init M5AtomS3Lite.
  USBSerial.begin(115200);
  AtomS3.dis.setBrightness(100);

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

  WiFi.begin(SSID, PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    AtomS3.dis.drawpix(0xff0000);  //赤色
    AtomS3.update();
    delay(300);
  }
  AtomS3.dis.drawpix(0x0000ff);  //青色
  AtomS3.update();
}

void loop()
{
  // Check if WiFi is disconnected, and reconnect if necessary
  if (WiFi.status() != WL_CONNECTED) {
    AtomS3.dis.drawpix(0xff0000);  //赤色
    AtomS3.update();
    USBSerial.println("WiFi disconnected. Attempting to reconnect...");

    WiFi.disconnect();
    WiFi.begin(SSID, PASSWORD);

    unsigned long startAttemptTime = millis();

    // Attempt to reconnect for 10 seconds
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
      delay(500);
      USBSerial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      USBSerial.println("Reconnected to WiFi!");
      AtomS3.dis.drawpix(0x0000ff);  //青色
      AtomS3.update();
    } else {
      USBSerial.println("Failed to reconnect to WiFi.");
      AtomS3.dis.drawpix(0xffff00);  //黄色
      AtomS3.update();
      // You may want to return here or delay further attempts
    }
  }

  for (int i = 0; i < BOX_NUM; i++) {
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
    delay(INTERVAL);
  }
}
