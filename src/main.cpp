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
const char *HOST = nullptr;
const char *TOKEN = nullptr;

const int BOX_OCCUPIED_THRE = 50.5;  // cm

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
        } else if (key == "API_HOST") {
            HOST = strdup(value.c_str());
        } else if (key == "API_TOKEN") {
            TOKEN = strdup(value.c_str());
        }
    }
}

bool line_notify(String msg)
{
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(HOST, 443)) {
    AtomS3.dis.drawpix(0xFFFF00);  //黄色
    AtomS3.update();
    return false;
  }
  String query = String("message=") + msg;
  String request = String("") + "POST /api/notify HTTP/1.1\r\n" + "Host: " + HOST + "\r\n" +
                   "Authorization: Bearer " + TOKEN + "\r\n" +
                   "Content-Length: " + String(query.length()) + "\r\n" +
                   "Content-Type: application/x-www-form-urlencoded\r\n\r\n" + query + "\r\n";
  client.print(request);
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
  if (cnt_detected == 1) {
    if (distance < BOX_OCCUPIED_THRE) {
      status = 2;
    }
  }
  else if (cnt_detected % INTERVAL_LONG == 0 && cnt_detected > 0) {
    if (distance < BOX_OCCUPIED_THRE) {
      status = 3;
    }
  }
  if (cnt_detected > 0 && BOX_OCCUPIED_THRE < distance) {
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
