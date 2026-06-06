<!--
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
-->

# カスタムエージェントの内部RAM収支とウェイクワードについて

最終更新: 2026-06-06

## 要約

カスタムAIエージェント（`main/agent/`）で「セッション開始後に音声がサーバへ送られず、応答も返らない」不具合の原因は、**ESP32-S3 の内部RAM（DMA可能RAM）の枯渇**だった。WiFi/lwIP/WireGuard の送受信バッファは PSRAM へ DMA できず内部RAM必須なのに、初期化完了時点で内部RAMの空きが約15KB・DMA最大連続ブロックが数KBしかなく、セッション開始＋音声ストリーミングのわずかな追加要求で DMA可能RAMが数十バイトまで枯渇 → WiFiがTXバッファを確保できず（`wifi:m f null` 多発、`STUN send failed: 12`＝ENOMEM）、音声が届かずトンネルも片方向死にする、というもの。

**対処（採用）:** ウェイクワード検出を無効化し、**タップ＋頭なで起動のみ**にした。これでウェイクワード用 AFE（WakeNet）が確保していた約25KBの内部RAMが空き、DMAの断片化も解消して、セッション開始＋音声TXに耐えられるようになる。`agent.cpp` の `constexpr bool kWakeWordEnabled`（`agent.cpp:94`）で切替可能。

## 計測（内部RAMの内訳）

`agent.cpp` に仕込んだ段階別プローブ `log_heap()`（`agent.cpp:140`）と、`session_loop` 冒頭の定期HEAPログ（`agent.cpp:612` 付近）による実測（logs/20260606-04, -05）。

`start()` 開始時点の内部空き ≈ 174KB が、初期化完了までに ≈ 15KB まで落ちる。各ステップの消費:

| ステップ | int_free | dma_max_blk | 消費 |
|---|---:|---:|---|
| start()冒頭（BT解放後） | ~174KB | — | — |
| pre-net（avatar/LVGL/タスク） | ~155KB | 63488 | ~21KB |
| post-wifi | ~114KB | 63488 | **~40KB（WiFi）** |
| post-tailscale | ~61KB | 30720 | **~53KB（microlink/WG）** ← 最大 |
| post-audio | ~46KB | 30720 | ~15KB |
| post-srmodels | ~46KB | 28672 | ~0（モデルはPSRAM） |
| post-vc-afe | ~40KB | 28672 | ~6KB（セッション用AFE, AEC無効/半二重） |
| post-wake-afe | ~15KB | 7680 | **~25KB（wakeワードAFE=WakeNet）** |

接続確立後はさらに数KB減り、内部空き ~9KB / DMA ~5KB 程度。ここに音声セッションが乗ると破綻していた。

## なぜ公式xiaozhiはウェイクワードと両立できていたか

**公式も特別なことはしていない。** `xiaozhi-esp32/main/audio/audio_service.cc` を見ると、公式も `AfeAudioProcessor`（`audio_service.cc:96`）と `AfeWakeWord`（`audio_service.cc:705-707`）の**2つのAFEを同時に生かしたまま** Start/Stop で切り替えているだけで、我々の使い方とほぼ同じ（lazy-init で一度作ったら破棄しない）。

**真の差は microlink/WireGuard トンネル（約53KB の内部RAM消費, 上表 post-tailscale）。** これはカスタムエージェント独自の追加で、公式xiaozhiは backend に直結（wss）するためこのコストを払っていない。つまり wakeワードAFE（25KB）は公式では一度も問題化しておらず、**我々が 53KB のトンネルを足したことで 25KB の wake AFE が入りきらなくなった**のが本質。根本は「WakeNetが重い」ではなく「トンネルが重い」。

## 検討したが採用しなかった案

### 1. wake AFE の AEC を無効化（効果なし → 撤回済み）
`afe_wake_word.cc` の `aec_init` を false にしてみたが、内部RAMはほぼ変わらなかった（post-wake-afe ≈ 15KB のまま）。wake AFE の 25KB の正体は **AEC ではなく WakeNet 本体**で、WakeNet の推論バッファは `AFE_MEMORY_ALLOC_MORE_PSRAM` 設定下でも内部RAMを要する。変更は実体・パッチとも撤回し pristine に戻した。

### 2. ウェイクワード検知後に AFE をアンロード、セッション終了後に再ロード（保留）
メモリ的には筋が良い（枯渇はセッション中＝音声TX時に起きるので、セッション開始時に wake AFE の 25KB を解放すれば必要なときに余裕ができる。アイドル時はトンネルが軽いので wake AFE を載せたままでも成立する）。

ただし**そのままでは実装不可**で、ベンダーコード `AfeWakeWord` への改変が必要:

- 解放対象は「モデルの重み」ではない（重みは assets パーティションの mmap＝PSRAM で内部RAMをほぼ使わない）。25KB の実体は WakeNet 推論バッファ＋AFEパイプライン＋検出タスクで、解放するには **AfeWakeWord インスタンスごと destroy** する必要がある。
- `AfeWakeWord` は**実行時 destroy に対して安全ではない**:
  - `AudioDetectionTask()` は `while(true)` で `fetch_with_delay(afe_data_, portMAX_DELAY)` をブロック待ちし続け、**終了シグナルが無く、タスクハンドルも保持していない**（`afe_wake_word.cc` の AudioDetectionTask / Initialize 内の xTaskCreate）。
  - デストラクタは `afe_data_` を destroy するが、**この検出タスクを停止も join もしない** → 走り続けるタスクが解放済み `afe_data_` を触り **use-after-free でクラッシュ**する。
  - デストラクタは共有モデルリスト `g_models` を `esp_srmodel_deinit` する（`afe_wake_word.cc:32`）ため、g_vc 用とは別のモデルリストを wake 専用に分離する必要がある。
- 公式が破棄せず作りっぱなしにしているのは、まさにこの危険を避けるため。

実装するなら `AfeWakeWord` にパッチ（①SHUTDOWNイベントビット ②`AudioDetectionTask` が break→`vTaskDelete(NULL)` ③タスクハンドル保持 ④デストラクタで「ビットセット→タスク終了待ち→`afe_data_` 破棄」 ⑤モデルリスト分離）が要る。再生成コストは1回 ~150-170ms（スリープ復帰時なので体感は許容範囲）。

### 3. microlink の内部RAM（53KB）削減（探索的・未実施）
公式との真の差はここなので、削れれば何もせず両AFEが収まる。ただし WG は tailnet backend に必須で、`max_peers` は既に 4、TLSバッファは既に PSRAM 化済み（`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` + `DYNAMIC_BUFFER`）のため、残りは WG/DERP/coord タスクのスタックや lwIP 構造体など削りにくい部分。効果未知。

### 4. BLE の内部RAM解放（ほぼ効果なし）
agentモードは BLE 未使用（`startBleServer()` は Dance アプリのみ）。`esp_bt_mem_release(ESP_BT_MODE_BLE)` を `start()` 冒頭（`agent.cpp:722`）で実行しているが、コントローラが未初期化のため解放できたのは約2KB のみ。安価で安全なので残置。

## 不採用の理由（2026-06-06 時点の判断）

案2・案3はいずれも**他者が作ったコード（ベンダーの `xiaozhi-esp32` / `microlink`）の改変**を伴い、アップストリーム更新への追従が弱くなる。当面は最もシンプルで安定な**タップのみ（`kWakeWordEnabled=false`）で確定**とする。

## ウェイクワードを将来再有効化するには

1. `agent.cpp:94` の `kWakeWordEnabled` を `true` に戻す（これだけで wake AFE が復活し、タップ/頭なでに加えてウェイクワードで起動）。
2. ただし**内部RAMの余裕（約25KB＋断片化分）を別途確保しないと、元の枯渇不具合が再発する**。確保策は案2（wake AFE の動的破棄＝AfeWakeWordパッチ）または案3（microlink削減）。
3. 併せて**ウェイクワード検出自体の動作確認**が必要（無効化前から不安定だった疑いあり。いつから壊れていたかは未特定）。

## 診断ログ（暫定）

検証用に以下を残してある。原因確定後は削除して良い:
- `log_heap()` と各 `log_heap("...")` 呼び出し（`agent.cpp:140` ほか）= `start()` 各ステップの内部/DMA/PSRAM空き。
- `session_loop` 冒頭の定期HEAPログ（`agent.cpp:612` 付近）= 5秒ごとの内部/DMA空き＋最小値。
- `BT mem release: ...`（`agent.cpp:722`）。
