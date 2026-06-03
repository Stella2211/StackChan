# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## リポジトリ概要

StackChan（M5Stack 製 AI デスクトップロボット）のオープンソース一式を収めたモノレポです。互いに独立した 4 つのコンポーネントから成り、それぞれビルド系・言語・ツールチェーンが異なります。

| ディレクトリ | 役割 | 言語 / 主要技術 |
|---|---|---|
| `firmware/` | 本体（M5Stack CoreS3 / ESP32-S3）ファームウェア | C++ / ESP-IDF v5.5.4 |
| `remote/` | ESP-NOW リモコン（StickC-Plus + JoyC Hat、ESP32） | C++ / ESP-IDF v5.4.2 |
| `app/` | モバイルアプリ（iOS / Android） | Dart / Flutter |
| `server/` | バックエンド API サーバー | Go / GoFrame v2 + MySQL |

これらを横断する共通テーマが 2 つあります。1 つは **XiaoZhi（小智）AI** 連携（ファーム・アプリ・サーバーすべてが関与）、もう 1 つは **アプリ↔サーバー間の RSA 暗号化通信**です。

---

## firmware/ — 本体ファームウェア

### 最重要: 外部依存の xiaozhi-esp32 ベースで成り立っている

このファームは単体では完結しておらず、サードパーティの [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32)（v2.2.4）を**土台**にしている。`firmware/main/CMakeLists.txt` は xiaozhi-esp32 の `main/` 配下のソース（audio / display / protocols / boards など）を直接コンパイル対象に取り込み、そこへ StackChan 固有コード（`main/apps`・`main/hal`・`main/stackchan`・`main/assets`）を上乗せしてビルドする。

`xiaozhi-esp32/` と `components/`（mooncake 等）は git 管理外で、`fetch_repos.py` がクローン・チェックアウト・パッチ適用を行う。**ビルド前に必ず実行すること**。取得対象とブランチは `repos.json` に定義され、xiaozhi-esp32 には `patches/xiaozhi-esp32.patch` が適用される。

### ビルド

```bash
cd firmware
python3 ./fetch_repos.py   # 依存リポジトリの取得 + パッチ適用（初回・依存更新時に必須）
idf.py build               # ESP-IDF v5.5.4 が必要
idf.py flash               # 書き込み
```

ターゲットは ESP32-S3、ボードは `CONFIG_BOARD_TYPE_M5STACK_STACK_CHAN`（`sdkconfig.defaults` 参照）。IDF 管理コンポーネント（LCD/タッチ/カメラ/音声コーデック/ESP-SR 等、多数のボード対応ドライバ）は `main/idf_component.yml` で宣言され、`managed_components/` に取得される。

### アーキテクチャ

- **2 段構成のブート**（`main/main.cpp`）: `app_main` はまず軽量アプリフレームワーク **Mooncake** を立ち上げ、ランチャー＋各アプリを動かす。`isXiaozhiStartRequested()` が立つと Mooncake を破棄し、`GetHAL().startXiaozhi()` で xiaozhi 本体に制御を渡して戻らない。AI 会話モードと StackChan 独自 UI モードがこの境界で切り替わる。
- **アプリ群**（`main/apps/`、Mooncake にインストール）: `AppLauncher` / `AppAiAgent` / `AppAvatar` / `AppEspnowControl` / `AppAppCenter` / `AppEzdata` / `AppDance` / `AppSetup`。雛形は `app_template/`。
- **HAL 層**（`main/hal/`）: ハードウェアと OS サービスを `GetHAL()` シングルトン経由で抽象化。サーボ・IMU・BLE・ESP-NOW・ネットワーク・OTA・RTC・頭部タッチ・音声・IO エクスパンダなどが `hal_*.cpp` に分かれている。
- **ドメインモデル**（`main/stackchan/`、`stackchan` 名前空間）: `GetStackChan()` シングルトンが `Avatar`（顔表情）・`Motion`（サーボ動作）・`NeonLight`（LED）と `Modifier` プールを束ねる。挙動は JSON 駆動で更新可能（`updateAvatarFromJson` / `updateMotionFromJson` / `updateNeonLightFromJson`）。
- **外部コンポーネント**（`repos.json`）: `mooncake`（アプリFW）・`mooncake_log`（ログ）・`smooth_ui_toolkit`（LVGL ベース UI）・`ArduinoJson`・`esp-now`。

`scan_secrets.py` は秘密情報の混入を検査するスタンドアロンスクリプト（コミット前チェック用）。

---

## remote/ — ESP-NOW リモコン

StickC-Plus + JoyC Hat 上で動き、ESP-NOW でサーボ動作コマンドを本体へ送る独立ファーム（`remote/code/`）。本体ファームとは別ツールチェーン（**ESP-IDF v5.4.2 / ターゲット esp32**）。

```bash
cd remote/code
idf.py build
idf.py flash --baud 1500000
```

注意: ビルド前に M5GFX 内の `__has_include(<driver/i2c_master.h>)` を全置換で `0` にする必要がある（`remote/code/README.md` 記載の既知のワークアラウンド）。UI は LVGL（`main/ui/`）、ESP-NOW 送信は `main/esp_now/`、ジョイスティック入力は `main/joystick/`。

---

## app/ — Flutter モバイルアプリ

### コマンド

```bash
cd app
flutter pub get                 # 依存取得
flutter run -d ios|android      # 実機/エミュレータで起動
flutter build apk --release     # Android リリースビルド
flutter build appbundle --release
flutter build ios --release     # iOS（事前に cd ios && pod install）
flutter analyze                 # 静的解析（lint）
flutter test                    # 全テスト
flutter test test/widget_test.dart  # 単一テスト
```

### ビルド前に必須の設定

- `lib/network/urls.dart` の `Urls.url` をバックエンドサーバーのアドレス（`"ip:port/"` 形式）に設定。HTTP は `http://<url>stackChan/`、WebSocket は `ws://<url>stackChan/ws` として組み立てられる（リポジトリ既定はプレースホルダ `00.000.000.000:0000/`）。
- `lib/util/value_constant.dart` にサーバー公開鍵・クライアント秘密鍵（RSA）を設定。
- Android リリース署名は `android/key.properties` + JKS で行う（`app/README.md` に手順あり。`key.properties` と `*.jks` は gitignore 済み）。

### アーキテクチャ

- **状態管理**: GetX。`AppState`（`lib/app_state.dart`）が `GetxController` のグローバルシングルトン（`AppState.shared`）で、`main.dart` で `Get.put` 後 `initData()` を実行。
- **層構成**（`lib/`）: `model/`（データモデル。`model/XiaoZhi/` は AI 関連）・`network/`（`http.dart` = dio クライアント、`urls.dart`、`web_socket_util.dart`）・`util/`（`rsa_util.dart`・`blue_util.dart`・`XiaoZhi_util.dart`・`music_util.dart` など）・`view/`（`home/`・`popup/`・`util/`）。
- **2 つのバックエンド**: ① StackChan バックエンド（デバイス管理・ダンス保存・認証・ファイル、`lib/network/urls.dart`）、② XiaoZhi AI サービス（会話・エージェント設定・TTS・ライセンス、`lib/util/XiaoZhi_util.dart`）。
- **主要機能の依存**: BLE = `flutter_blue_plus`、3D 顔表情 = `three_js`、カメラ/顔検出 = `camera` + `google_mlkit_face_detection` + `opencv_dart`、音声 = `just_audio` + `opus_codec` + `ffmpeg_kit_flutter_new`、QR = `mobile_scanner`。`opencv_dart` は `pubspec.yaml` の `hooks.user_defines` で imgproc/imgcodecs のみに絞ってビルドしている。

---

## server/ — Go バックエンド（GoFrame v2）

### コマンド

```bash
cd server
go mod download
mysql -u <user> -p < check_list/create_mysql_database.sql   # DB 初期化（MySQL 8.0+）
go run main.go        # 開発実行
make build            # GoFrame CLI でビルド（gf build -ew）
go build -o stackchan-server main.go   # 素の Go ビルド
```

設定は `manifest/config/config.yaml`（DB 接続文字列・JWT secret・RSA 鍵・XiaoZhi の secret_key 等）。

### コード生成（GoFrame CLI = `gf`、`Makefile`/`hack/hack.mk`）

GoFrame はスキャフォールドを多用する。`api/<module>/` の定義を変えたら `make ctrl` で `internal/controller` を再生成、DB スキーマ変更時は `make dao`、サービス層は `make service`、enum は `make enums`。これらは手書きせず生成すること。

### アーキテクチャ

GoFrame 標準の層構成: **api**（リクエスト/レスポンス構造体・ルート）→ **controller**（HTTP ハンドラ）→ **logic / service**（業務ロジック）→ **dao**（DB アクセス）→ **model**（エンティティ）。エントリは `main.go` → `internal/cmd`。モジュール単位（`admin` / `appstore` / `dance` / `device` / `friend` / `post` / `user` / `xiaozhi` / `pano` / `file` / `stackchandevice`）で `api/` と `internal/controller/` が対応する。リアルタイム通信は `internal/web_socket/`、AI 連携は `internal/xiaozhi/`。認証は JWT（`internal/middleware/`）。

---

## ライセンス

全コンポーネント MIT（SPDX ヘッダ `SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD` を新規ファイルに付与する慣習）。
