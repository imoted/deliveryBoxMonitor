#include <M5AtomS3.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include "FS.h"
#include "SPIFFS.h"

const char *FW_VERSION = "1.3.0";

const int BOX_NUM = 2;

const int HCSR04Trg[] = {8, 6};   //使用するGPIOピン
const int HCSR04Echo[] = {7, 5};  //使用するGPIOピン

// 在荷表示LED（赤）。Grove端子から1m延長した先に取り付ける。
// G1(黄) = 上段 / G2(白) = 下段。120Ωを基板側に直列に入れて約10mAで駆動する。
const int LED_PIN[] = {1, 2};

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

// 在荷表示LEDの判定
const double DIST_MIN_VALID = 2.0;  // cm 未満は計測失敗とみなす（HC-SR04の最短測定距離）
const int LED_DEBOUNCE = 2;         // 同じ判定がこの回数続いたらLEDに反映する

int cnt_detected = 0;
const int INTERVAL = 10 * 1000;  // ms
const int INTERVAL_LONG = 6 * 60 * 12;  // 10sec * 6 * 60 * 12h = 12h

// WiFi再接続（ノンブロッキング＋指数バックオフ）
const unsigned long WIFI_ATTEMPT_TIMEOUT = 10 * 1000;      // 1回の接続試行を諦めるまで
const unsigned long WIFI_RETRY_MIN = 5 * 1000;             // 最初のリトライ間隔
const unsigned long WIFI_RETRY_MAX = 5 * 60 * 1000;        // リトライ間隔の頭打ち

// オフライン中の通知を溜めるキュー
const int NOTIFY_QUEUE_SIZE = 8;
const unsigned long NOTIFY_RETRY_INTERVAL = 10 * 1000;  // 送信失敗後の再送待ち

// ステータスLEDの色
const uint32_t LED_OFF        = 0x000000;
const uint32_t LED_CONNECTED  = 0x0000ff;  //青色: WiFi接続済み
const uint32_t LED_CONNECTING = 0xff0000;  //赤色: WiFi接続試行中
const uint32_t LED_BACKOFF    = 0xffff00;  //黄色: 接続失敗・リトライ待ち
const uint32_t LED_PENDING    = 0x00ffff;  //水色: 未送信の通知が残っている
const uint32_t LED_OTA        = 0xff00ff;  //紫色: OTA書き込み中
const uint32_t LED_OTA_DONE   = 0x00ff00;  //緑色: OTA成功

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

// ---------------------------------------------------------------------------
// ステータスLED
// ---------------------------------------------------------------------------
uint32_t led_current = 0xffffffff;  // 未設定を表す番兵

// 同じ色の描画を繰り返さないようにして、OTA中の点滅と競合させない
void set_led(uint32_t color)
{
  if (color == led_current) {
    return;
  }
  led_current = color;
  AtomS3.dis.drawpix(color);
  AtomS3.update();
}

// ---------------------------------------------------------------------------
// LINE通知
// ---------------------------------------------------------------------------
bool line_notify(String msg)
{
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);  // [sec] 接続・読み出しでloopを長時間止めない
  if (!client.connect(HOST, 443)) {
    USBSerial.println("LINE connect failed");
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
// 通知キュー（WiFi切断中に発生した通知を溜め、復帰後にまとめて送る）
// ---------------------------------------------------------------------------
struct Notification {
  String msg;
  unsigned long created_at;  // イベント発生時のmillis()
};

Notification notify_queue[NOTIFY_QUEUE_SIZE];
int notify_head = 0;    // 次に送るスロット
int notify_count = 0;   // 未送信件数
int notify_dropped = 0; // キュー溢れで捨てた件数
unsigned long notify_next_try_at = 0;

// 発生からの経過時間を「(約N分前)」の形にする。1分未満なら空文字。
String format_elapsed(unsigned long created_at)
{
  unsigned long elapsed_sec = (millis() - created_at) / 1000;
  if (elapsed_sec < 60) {
    return "";
  }
  if (elapsed_sec < 60 * 60) {
    return "(約" + String(elapsed_sec / 60) + "分前) ";
  }
  return "(約" + String(elapsed_sec / 3600) + "時間前) ";
}

void notify_enqueue(String msg)
{
  if (notify_count == NOTIFY_QUEUE_SIZE) {
    // 溢れたら最も古いものを捨てる（直近の状態を優先して残す）
    notify_head = (notify_head + 1) % NOTIFY_QUEUE_SIZE;
    notify_count--;
    notify_dropped++;
    USBSerial.println("notify queue overflow, dropped oldest");
  }
  int tail = (notify_head + notify_count) % NOTIFY_QUEUE_SIZE;
  notify_queue[tail].msg = msg;
  notify_queue[tail].created_at = millis();
  notify_count++;
  USBSerial.println("queued: " + msg + " (pending=" + String(notify_count) + ")");
}

// 1回のloopで1件だけ送る。失敗してもキューには残したままリトライする。
void handle_notify_queue()
{
  if (notify_count == 0 || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if ((long)(millis() - notify_next_try_at) < 0) {
    return;
  }

  Notification &n = notify_queue[notify_head];
  String msg = format_elapsed(n.created_at) + n.msg;

  if (!line_notify(msg)) {
    notify_next_try_at = millis() + NOTIFY_RETRY_INTERVAL;
    USBSerial.println("notify failed, will retry: " + msg);
    return;
  }

  USBSerial.println("notify sent: " + msg);
  n.msg = String();  // Stringのヒープを解放しておく
  notify_head = (notify_head + 1) % NOTIFY_QUEUE_SIZE;
  notify_count--;

  // 溢れて捨てた分があれば、全部吐き出しきったところで一度だけ知らせる。
  // これもキューに積むので、送信に失敗しても失われない。
  if (notify_count == 0 && notify_dropped > 0) {
    int dropped = notify_dropped;
    notify_dropped = 0;
    notify_enqueue("通知キューが溢れ、" + String(dropped) + "件の通知を破棄しました。");
  }
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
bool ota_initialized = false;
bool ota_in_progress = false;  // 転送中はステータスLEDの自動更新を止める

void setup_ota()
{
  if (ota_initialized) {
    return;
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setPort(OTA_PORT);

  ArduinoOTA.onStart([]() {
    ota_in_progress = true;
    // ファイルシステム更新の場合はSPIFFSをアンマウントしてから書き換える
    if (ArduinoOTA.getCommand() == U_SPIFFS) {
      SPIFFS.end();
      USBSerial.println("OTA start: filesystem");
    } else {
      USBSerial.println("OTA start: sketch");
    }
    set_led(LED_OTA);
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    unsigned int percent = total ? (progress * 100 / total) : 0;
    USBSerial.printf("OTA progress: %u%%\r\n", percent);
    // 10%ごとにLEDを点滅させて進行中であることを示す
    set_led((percent / 10) % 2 ? LED_OTA : LED_OFF);
  });

  ArduinoOTA.onEnd([]() {
    USBSerial.println("OTA end. rebooting...");
    set_led(LED_OTA_DONE);
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
    set_led(LED_CONNECTING);  //赤色: OTA失敗
    delay(2000);
    // ファイルシステム更新に失敗した場合はSPIFFSを再マウントして通常動作へ戻す
    if (ArduinoOTA.getCommand() == U_SPIFFS) {
      SPIFFS.begin(true);
    }
    ota_in_progress = false;  // 以降はステータスLEDの自動更新に戻す
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
// WiFi（ノンブロッキング再接続＋指数バックオフ）
//
// 起動時も再接続時も同じ状態機械で扱う。接続できていなくてもloopは回り続け、
// センサー計測とOTA待受は止めない。通知は notify_enqueue() に溜まる。
// ---------------------------------------------------------------------------
bool wifi_connecting = false;
bool wifi_was_connected = false;
unsigned long wifi_attempt_start = 0;
unsigned long wifi_retry_at = 0;
unsigned long wifi_retry_interval = WIFI_RETRY_MIN;
int wifi_fail_count = 0;
volatile uint8_t wifi_last_disconnect_reason = 0;

// WiFiイベントは別タスクから呼ばれるので、ここでは記録だけしてloop側で使う
void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info)
{
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    wifi_last_disconnect_reason = info.wifi_sta_disconnected.reason;
  }
}

void start_wifi_connect()
{
  if (SSID == nullptr) {
    // config.iniにWIFI_SSIDが無い。繋がりようがないのでリトライも意味がない。
    USBSerial.println("WIFI_SSID is not set in config.ini");
    wifi_retry_at = millis() + WIFI_RETRY_MAX;
    return;
  }
  USBSerial.println("WiFi connecting to " + String(SSID) + "...");
  WiFi.disconnect();
  WiFi.begin(SSID, PASSWORD);
  wifi_connecting = true;
  wifi_attempt_start = millis();
}

void handle_wifi()
{
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifi_was_connected) {
      wifi_was_connected = true;
      wifi_connecting = false;
      wifi_fail_count = 0;
      wifi_retry_interval = WIFI_RETRY_MIN;  // バックオフをリセット
      USBSerial.println("WiFi connected: " + WiFi.localIP().toString() +
                        " rssi=" + String(WiFi.RSSI()) + "dBm");
    }
    setup_ota();  // 初回、および再接続後の張り直し
    return;
  }

  if (wifi_was_connected) {
    wifi_was_connected = false;
    USBSerial.println("WiFi lost (reason=" + String(wifi_last_disconnect_reason) + ")");
  }

  teardown_ota();

  if (wifi_connecting) {
    if (millis() - wifi_attempt_start > WIFI_ATTEMPT_TIMEOUT) {
      wifi_connecting = false;
      wifi_fail_count++;
      wifi_retry_at = millis() + wifi_retry_interval;
      USBSerial.println("WiFi connect failed (" + String(wifi_fail_count) +
                        "回目, reason=" + String(wifi_last_disconnect_reason) +
                        "). retry in " + String(wifi_retry_interval / 1000) + "s");
      // 5s → 10s → 20s → … → 5分で頭打ち
      wifi_retry_interval = min(wifi_retry_interval * 2, WIFI_RETRY_MAX);
    }
    return;
  }

  if ((long)(millis() - wifi_retry_at) < 0) {
    return;  // バックオフ待ち
  }

  start_wifi_connect();
}

// WiFi/通知の状態からステータスLEDを決める。OTA転送中は触らない。
void update_status_led()
{
  if (ota_in_progress) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    set_led(notify_count > 0 ? LED_PENDING : LED_CONNECTED);
  } else {
    set_led(wifi_connecting ? LED_CONNECTING : LED_BACKOFF);
  }
}

class DeliveryBox
{
public:
  int judgeBoxOccupied(double distance);
  double measureDist(int Trg, int Echo);
  void updateOccupiedLed(int led_pin, const String &label, double distance);

private:
  int cnt_detected;
  bool led_on;        // 現在のLED状態
  bool last_occupied; // 直近の計測での在荷判定
  int cnt_same;       // 同じ判定が連続した回数
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

// 通知（judgeBoxOccupied）はCNT_THRETHOLD回の連続検知を待つが、LEDは目視用なので
// 短いデバウンスだけかけて直近の計測を素直に反映する。
void DeliveryBox::updateOccupiedLed(int led_pin, const String &label, double distance)
{
  // pulseInがタイムアウトするとdistanceは0になる。そのまま判定すると在荷扱いに
  // なってしまうので、無効な計測としてLEDは直前の状態を保つ。
  if (distance < DIST_MIN_VALID) {
    return;
  }

  bool occupied = distance < BOX_OCCUPIED_THRE;
  if (occupied == last_occupied) {
    cnt_same++;
  } else {
    last_occupied = occupied;
    cnt_same = 1;
  }

  // 単発のノイズでLEDがちらつかないよう、同じ判定が続いたときだけ切り替える
  if (cnt_same < LED_DEBOUNCE || led_on == occupied) {
    return;
  }

  led_on = occupied;
  digitalWrite(led_pin, led_on ? HIGH : LOW);
  USBSerial.println(label + "の在荷LED: " + (led_on ? "点灯" : "消灯"));
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
  deliveryBox[i].updateOccupiedLed(LED_PIN[i], BOX_LABEL[i], distance);
  int box_status = deliveryBox[i].judgeBoxOccupied(distance);
  // 送信はキュー経由。WiFiが切れていても計測とイベント検出は止めない。
  if (box_status == 2) {
    notify_enqueue(BOX_LABEL[i] + "に荷物が入りました。");
  } else if (box_status == 3) {
    notify_enqueue(BOX_LABEL[i] + "に荷物が入ったままです。");
  } else if (box_status == 4) {
    notify_enqueue(BOX_LABEL[i] + "の荷物が取り出されました。");
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
    // ブート直後の中途半端な点灯を避けるため、出力にしたら即座にLOWへ落とす
    pinMode(LED_PIN[i], OUTPUT);
    digitalWrite(LED_PIN[i], LOW);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // OTAの取りこぼしを防ぐ
  WiFi.setAutoReconnect(false);  // 再接続はhandle_wifi()の状態機械が一元管理する
  WiFi.onEvent(on_wifi_event);

  // 接続完了を待たずにloopへ入る。APが落ちていても計測とOTA待受は動かす。
  start_wifi_connect();
  update_status_led();

  last_measure_at = millis() - INTERVAL;  // 起動直後に1回目の計測を実行する
}

void loop()
{
  // OTAは常時受け付けたいので、loopはブロックせずに高速で回す
  if (ota_initialized) {
    ArduinoOTA.handle();
  }

  handle_wifi();
  handle_notify_queue();

  if (millis() - last_measure_at >= (unsigned long)INTERVAL) {
    last_measure_at = millis();
    measure_box(current_box);
    current_box = (current_box + 1) % BOX_NUM;
  }

  update_status_led();
  delay(10);
}
