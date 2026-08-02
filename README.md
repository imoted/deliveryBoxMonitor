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

4. テンプレートから `data/config.ini` を作成し、WiFiのSSID、パスワード、LINEのトークン、OTAの設定を記述します。

```bash
cp data/config.ini.example data/config.ini
```

```
WIFI_SSID=your_wifi_ssid
WIFI_PASSWORD=your_wifi_password
CHANNEL_TOKEN=your_line_channel_access_token
USER_ID=your_line_user_id
OTA_HOSTNAME=boxmonitor
OTA_PASSWORD=change_me
```

`data/config.ini` は `.gitignore` 済みなのでコミットされません（`data/config.ini.example` のみ追跡対象）。`#` または `;` で始まる行はコメントとして無視されます。

5. プロジェクトをコンパイルし、M5Stack AtomS3 Liteにアップロードします。

## 使用方法
デバイスが起動すると、config.iniファイルを読み込み、WiFiに接続します。

ボックスに物が入ると、距離センサーがその距離を計測し、定義されたしきい値を超えると通知が送信されます。

物が取り出された場合や、一定時間以上物が入ったままの場合も、LINEに通知が送信されます。

## WiFiの再接続と通知キュー

WiFiの接続・再接続はノンブロッキングの状態機械で扱っており、**接続できていなくてもセンサー計測とOTA待受は動き続けます**（AP側が停電から復旧するより先にデバイスが起動しても、そのまま復帰します）。

- **指数バックオフ**: 1回の接続試行は10秒で打ち切り、失敗するたびに 5s → 10s → 20s → … と待ち時間を倍にし、5分で頭打ちにして永久にリトライします。接続に成功した時点でバックオフはリセットされます。
- **通知キュー**: オフライン中に発生したイベントは最大8件までリングバッファに溜め、再接続後にまとめて送信します。溜まっていた通知には送信時に「(約N分前)」が付きます。8件を超えると古いものから捨て、キューを吐き切ったあとに破棄件数を通知します。
- **切断理由の記録**: WiFiイベントから切断理由コードを拾い、シリアルログに `WiFi lost (reason=N)` として出します。

自動再起動によるリカバリは入れていません。再起動すると在荷カウンタ（`cnt_detected`）がリセットされ、荷物が入ったままの箱に対して「荷物が入りました」が再送されてしまうためです。

## OTA（無線でのファームウェア更新）

WiFi経由でファームウェアを書き換えられます（ArduinoOTA / espota）。デバイスと作業PCが**同じLAN**にいる必要があります。

### 事前準備（初回のみ）

1. `data/config.ini` に OTA 設定を書く。

   ```
   OTA_HOSTNAME=boxmonitor
   OTA_PASSWORD=boxmonitor
   ```

2. `platformio.ini` の `[env:ota]` の `upload_port` / `--auth` を config.ini と一致させる。

   ```ini
   [env:ota]
   upload_port = boxmonitor.local
   upload_flags =
       --port=3232
       --auth=boxmonitor
   ```

3. **1回だけUSBで書き込む**。OTA対応のファームが載っていないと無線更新は始められない。

   ```bash
   pio run -e m5stack-atoms3 -t uploadfs   # config.ini をSPIFFSへ
   pio run -e m5stack-atoms3 -t upload     # ファームウェア
   ```

4. シリアルモニタで OTA が待ち受けに入ったことを確認する。

   ```bash
   pio device monitor -e m5stack-atoms3
   # => OTA ready: boxmonitor.local:3232 (192.168.x.x) fw=1.1.0
   ```

### 2回目以降（OTAでの更新手順）

```bash
# 1. コードを編集する
# 2. ビルドが通ることを確認
pio run -e ota

# 3. 無線で書き込み
pio run -e ota -t upload
```

デバイス側のLEDが**紫の点滅**になれば転送中、完了すると**緑**になって自動的に再起動します。再起動後は通常どおり青（WiFi接続済み）に戻ります。

設定ファイル（`data/config.ini`）だけを更新したい場合は SPIFFS も無線で送れます。

```bash
pio run -e ota -t uploadfs
```

> ファームのOTAはSPIFFSを書き換えないため、config.iniを変えたときだけ `uploadfs` を実行すればよい。

### mDNSが引けない場合

`boxmonitor.local` が名前解決できない環境（Windowsやゲスト分離されたWiFiなど）では、IPアドレスを直接指定する。

```bash
pio run -e ota -t upload --upload-port 192.168.1.42
```

IPアドレスはシリアルモニタの `OTA ready:` 行か、ルーターのDHCPクライアント一覧で確認できる。

### LEDの色と状態

| 色 | 状態 |
|----|------|
| 青 | WiFi接続済み・通常動作 |
| 水 | WiFi接続済みだが未送信の通知が残っている |
| 赤 | WiFi接続試行中／OTA失敗 |
| 黄 | WiFi接続に失敗してリトライ待ち |
| 紫（点滅） | OTA転送中 |
| 緑 | OTA成功（この直後に再起動） |

### 注意点

- OTA中はセンサー計測とLINE通知が停止する（転送が終わると自動復帰）。
- パーティションは `default_8MB.csv`（app0 / app1 各3.3MB）を使っており、OTA用の設定変更は不要。現在のファームは約1.07MBなので余裕がある。
- OTAが失敗した場合はデバイスは書き換え前のファームで動き続ける。復旧不能になった場合はUSBで書き直す。
- `OTA_PASSWORD` は認証に使われる。LAN内からの意図しない書き換えを防ぐため、運用時はデフォルトから変更すること。

## ファイル構成

src/main.cpp: メインコードファイル
platformio.ini: プロジェクト設定ファイル（`m5stack-atoms3` = USB書き込み / `ota` = 無線書き込み）
/data/config.ini: WiFi・LINE・OTA設定ファイル（SPIFFSに保存 / gitignore対象）
/data/config.ini.example: 上記のテンプレート

## ライセンス
このプロジェクトはMITライセンスのもとで公開されています。詳細はLICENSEファイルを参照してください。
