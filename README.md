# AtomS3 Lite Delivery Box Monitor

このプロジェクトは、AtomS3 Liteを使用して、複数の配達ボックスの状態を監視するコードです。ボックスが占有されているかどうかを超音波センサーで検出し、その結果をWiFi経由でLINEに通知します。PlatformIOを使用して開発されています。

## 必要なハードウェア
- M5Stack AtomS3 Lite
- 超音波距離センサー (HC-SR04)
- 配達ボックス（センサーを取り付けるためのもの）

## 必要なソフトウェア
- PlatformIO IDE
- PlatformIO Core

## プロジェクトのセットアップ

1. PlatformIOを使用してプロジェクトをクローンまたは新規作成します。

2. 必要なライブラリを`platformio.ini`に追加します。

   ```ini
   [env:m5stack-atoms3]
   platform = espressif32
   board = m5stack-atoms3
   framework = arduino
   upload_port = /dev/ttyACM0
   lib_deps =
       m5stack/M5AtomS3@^1.0.0
       fastled/FastLED@^3.7.1
       m5stack/M5Unified@^0.1.16

   build_flags =
       -D FILESYSTEM=SPIFFS
    ```

3. 配達ボックスの超音波センサーのトリガーとエコー用のGPIOピンを指定し、各ボックスのラベルを設定します。

```
const int HCSR04Trg[] = {8, 6};   // トリガーピン
const int HCSR04Echo[] = {7, 5};  // エコーピン
const String BOX_LABEL[] = {"上段", "下段"};
```

4. config.ini ファイルを /data ディレクトリに作成し、WiFiのSSID、パスワード、APIホスト、およびAPIトークンを設定します。

```
[WIFI]
WIFI_SSID = "your_wifi_ssid"
WIFI_PASSWORD = "your_wifi_password"

[API]
API_HOST = "api.host.url"
API_TOKEN = "your_api_token"
```

5. プロジェクトをコンパイルし、M5Stack AtomS3 Liteにアップロードします。

## 使用方法
デバイスが起動すると、config.iniファイルを読み込み、WiFiに接続します。

ボックスに物が入ると、距離センサーがその距離を計測し、定義されたしきい値を超えると通知が送信されます。

物が取り出された場合や、一定時間以上物が入ったままの場合も、LINEに通知が送信されます。

## ファイル構成

src/main.cpp: メインコードファイル
platformio.ini: プロジェクト設定ファイル
/data/config.ini: WiFiおよびAPI設定ファイル（SPIFFSに保存）

## ライセンス
このプロジェクトはMITライセンスのもとで公開されています。詳細はLICENSEファイルを参照してください。
