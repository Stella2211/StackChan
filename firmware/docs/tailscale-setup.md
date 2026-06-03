# StackChan ファームウェア — Tailscale 接続セットアップ

StackChan 本体（ファームウェア）を **Tailscale（tailnet）のノードとして参加**させ、カスタム AI
エージェントの backend WebSocket 接続を **WireGuard トンネル経由**にするための手順です。

`backend` が tailnet 内からしか到達できない（例: `100.102.53.22:8787`）場合に必要になります。
LAN 内に backend がある通常運用では **設定不要**（このページの設定をしなければ従来どおり LAN 直結で、
Tailscale コードは一切動きません）。

実装の入口は [`main/agent/tailscale.cpp`](../main/agent/tailscale.cpp) / [`tailscale.h`](../main/agent/tailscale.h)、
土台は [CamM2325/microlink](https://github.com/CamM2325/microlink)（ESP32 向け Tailscale クライアント）です。

---

## 1. 仕組み（概要）

- 起動シーケンス（`main/agent/agent.cpp::start()`）:
  WiFi 接続（`startNetwork()`）→ **Tailscale 起動（`tailscale_bring_up()`）** → 音声初期化 → backend へ接続。
- `tailscale_bring_up()` は microlink を起動し、`ML_STATE_CONNECTED` まで待ち（最大 90 秒）、
  backend ピアへ **WireGuard セッションを温める（プライミング）**。
- microlink は WG 仮想 NIC を **netmask `/10`（`100.64.0.0/10`）** で登録するため、lwIP が
  `100.x` 宛の通信を自動的にトンネルへ流します。よって既存の WebSocket クライアントは**無改造**で
  `ws://100.102.53.22:8787/...` に到達します（Tailscale が暗号化するので `wss` は不要）。
- 顔・口パク・なで反応（`GetStackChan()`）はネットワーク層と独立で、**影響ありません**。

---

## 2. 用意するデータ一覧

| データ | 例 | 取得元 | 設定キー |
|---|---|---|---|
| Tailscale 認証キー | `tskey-auth-xxxxxxxxCNTRL-xxxxxxxxxxxxxxxxxxxxxxxxxxxx` | Tailscale 管理コンソール（§3） | `TAILSCALE_DEFAULT_AUTHKEY` / NVS `tscale/authkey` |
| backend の tailnet IP | `100.102.53.22` | backend マシンで `tailscale ip -4`（§4） | `AGENT_DEFAULT_HOST` の IP 部 / NVS `cagent/host` |
| backend ポート | `8787` | backend の `PORT`（既定 8787） | `AGENT_DEFAULT_HOST` のポート部 / NVS `cagent/host` |
| デバイス名（任意） | `stackchan` | 自分で決める（DNS 安全な名前） | `TAILSCALE_DEFAULT_DEVNAME` / NVS `tscale/devname` |
| 共有トークン（任意） | `change-me` | backend の `AUTH_TOKEN` | `AGENT_DEFAULT_TOKEN` / NVS `cagent/token` |

> 認証キーと backend IP の 2 つが必須。残りは任意です。

---

## 3. Tailscale 認証キー（auth key）の取得

1. ブラウザで Tailscale 管理コンソールの **Keys** を開く:
   <https://login.tailscale.com/admin/settings/keys>
2. **Generate auth key…** をクリック。
3. 推奨設定:
   - **Reusable**: ON（再フラッシュ・初期化で再登録できるように）。
   - **Expiration**: 適切な期限（例 90 日）。期限切れになると再登録が必要。
   - **Ephemeral**: OFF（再起動後も同じノードとして残したい場合）。検証用に使い捨てなら ON。
   - **Pre-approved**: tailnet で「デバイス承認（Device approval）」を有効にしている場合は ON。
     OFF だと管理コンソールでの手動承認が必要です。
   - **Tags**（任意・推奨）: 例 `tag:stackchan`。ACL でアクセス範囲を絞れます（§7）。
4. 生成された **`tskey-auth-...`** を控える。**この値は秘密情報**です（§8）。

> 補足: microlink はノードの machine key を NVS（名前空間 `microlink`）に永続化します。auth key は
> 主に初回登録に使われ、以降は永続化されたノード情報が使われます。

---

## 4. backend の tailnet IP / ポートの確認

backend が動くマシンで:

```bash
tailscale ip -4        # 例: 100.102.53.22
```

または Tailscale 管理コンソールの **Machines** 一覧の該当マシンの IP を確認します。
ポートは backend の `PORT`（`.env`、既定 `8787`）。よって host は `100.102.53.22:8787`。

backend は tailnet インターフェース（または `0.0.0.0`）の `:8787` で待ち受けている必要があります（§7）。

---

## 5. 設定方法 A — ビルド時に焼き込む（初回・推奨）

最も手軽で確実です。`firmware/` で:

```bash
cd firmware
python3 fetch_repos.py            # 初回のみ: microlink を firmware/microlink に取得
idf.py build \
  -DAGENT_DEFAULT_HOST='"100.102.53.22:8787"' \
  -DTAILSCALE_DEFAULT_ENABLED=true \
  -DTAILSCALE_DEFAULT_AUTHKEY='"tskey-auth-xxxxxxxx"' \
  -DTAILSCALE_DEFAULT_DEVNAME='"stackchan"'        # 任意
  # 任意: -DAGENT_DEFAULT_TOKEN='"change-me"'  -DAGENT_DEFAULT_ROUTE=custom_agent::Route::Agent
idf.py flash monitor
```

ビルド時マクロ一覧（`main/CMakeLists.txt` が `-D` を該当ソースへ転送）:

| マクロ | 既定 | 意味 |
|---|---|---|
| `TAILSCALE_DEFAULT_ENABLED` | `false` | Tailscale を有効化（`true` で起動時にトンネルを張る） |
| `TAILSCALE_DEFAULT_AUTHKEY` | `""` | Tailscale 認証キー（`'"tskey-..."'` のように二重引用） |
| `TAILSCALE_DEFAULT_DEVNAME` | `""` | tailnet 上のホスト名（空なら MAC 由来の `esp32-xxxxxx`） |
| `AGENT_DEFAULT_HOST` | `""` | backend の `ip:port`。Tailscale 利用時は tailnet IP を指定 |
| `AGENT_DEFAULT_TLS` | `false` | `wss` を使うか。Tailscale 経由なら `false`（WG が暗号化） |
| `AGENT_DEFAULT_TOKEN` | `""` | backend `AUTH_TOKEN`（設定時のみ） |
| `AGENT_DEFAULT_ROUTE` | `custom_agent::Route::Agent` | `/ws/agent` か `/ws/live` か |

> **注意（重要）**
> - 文字列値は **`'"..."'`** とシェル＆C の両方の引用が要ります（数値・bool は不要）。
> - 空文字 `-DXXX=` を渡すと無視され、ソースの `#ifndef` 既定にフォールバックします。
> - `-D` 値は `build/` の CMake キャッシュに残ります。**auth key をリポジトリにコミットしない**こと。
>   別ビルドに切り替える/消すには `idf.py fullclean` か、別値で再 `-D`。
> - CI やチーム共有では、`firmware/sdkconfig.defaults.local`（git 管理外オーバーレイ。
>   `CMakeLists.txt` が自動読込）に Kconfig 系を置く運用と併用できます（auth key 等のマクロは `-D` で）。

---

## 6. 設定方法 B — 実行時 NVS で設定（再ビルド不要）

デバイスごとに値を変えたい／再フラッシュせず変更したい場合。値は NVS（不揮発メモリ）に保存します。
現状は本体に設定 UI が無いため、PC から NVS パーティションを書き込みます。

`Settings`（`xiaozhi-esp32/main/settings.cc`）の NVS エンコーディングは以下です（**この型に合わせること**）:

| 種別 | NVS API | `nvs_partition_gen` の `type,encoding` |
|---|---|---|
| 文字列 | `nvs_set_str` | `data,string` |
| 真偽 (bool) | `nvs_set_u8`（0/1） | `data,u8` |
| 整数 (int) | `nvs_set_i32` | `data,i32` |

### 使うキー

名前空間 **`tscale`**（Tailscale）:

| キー | 型 | 意味 |
|---|---|---|
| `en` | u8 | 有効化（1=有効, 0=無効） |
| `authkey` | string | `tskey-auth-...` |
| `devname` | string | ホスト名（空可） |
| `peer` | string | backend の VPN IP（空なら `cagent/host` から自動導出） |

名前空間 **`cagent`**（既存のエージェント設定）:

| キー | 型 | 意味 |
|---|---|---|
| `host` | string | `100.102.53.22:8787` |
| `tls` | u8 | 0（Tailscale 経由は ws 平文で可） |
| `token` | string | backend `AUTH_TOKEN`（任意） |
| `route` | i32 | 0=`/ws/agent`, 1=`/ws/live` |

### NVS パーティション画像を作って書き込む

`nvs.csv` を用意:

```csv
key,type,encoding,value
tscale,namespace,,
en,data,u8,1
authkey,data,string,tskey-auth-xxxxxxxx
devname,data,string,stackchan
peer,data,string,100.102.53.22
cagent,namespace,,
host,data,string,100.102.53.22:8787
tls,data,u8,0
route,data,i32,0
```

生成して書き込み（NVS パーティションは `partitions.csv` より **offset `0x9000` / size `0x4000`**）:

```bash
# 生成（IDF 同梱ツール）
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
  generate nvs.csv nvs.bin 0x4000

# 書き込み（COM ポートは環境に合わせる）
esptool.py --chip esp32s3 -p /dev/tty.usbmodemXXXX write_flash 0x9000 nvs.bin
```

> ⚠️ この方法は **NVS 全体を上書き**します（既存の WiFi 認証情報なども消える可能性）。WiFi 設定を
> 残したい場合は、既存キーも CSV に含めるか、ビルド時方式（§5）を推奨します。

---

## 7. backend 側のチェックリスト

- backend を **tailnet IP もしくは `0.0.0.0` の `:8787`** で待受（特定 IP に固定していると届かない場合あり）。
- StackChan デバイスを tailnet に **承認**（Pre-approved キー、または管理コンソールで承認）。
- ACL で **device → `100.102.53.22:8787`** を許可。タグ運用例:
  ```jsonc
  // Tailscale ACL（例）
  "acls": [
    { "action": "accept", "src": ["tag:stackchan"], "dst": ["100.102.53.22:8787"] }
  ]
  ```
- backend を外部公開する場合は `AUTH_TOKEN` を設定し、`cagent/token`（または `-DAGENT_DEFAULT_TOKEN`）に同値を設定。

---

## 8. セキュリティ上の注意

- **auth key は秘密情報**。リポジトリにコミットしない（ビルド時 `-D` はキャッシュに残る点に注意）。
- ビルド時に焼くと **flash に平文**で載り、NVS も既定では平文です。盗難・解析リスクを下げるには:
  - **Reusable + 期限付き** または **Ephemeral** キーを使う。
  - ACL の **タグ**でアクセス先を `backend:8787` のみに限定する。
  - 必要なら IDF の **NVS 暗号化 / flash 暗号化**を検討。
- 不要になったキーは管理コンソールで失効させる。

---

## 9. 動作確認（シリアルログ）

`idf.py monitor` で以下のタグ/メッセージを確認:

- `agent-ts` … `starting tailscale (device='...' peer='100.102.53.22')`
- `microlink state: WIFI_WAIT → CONNECTING → REGISTERING → CONNECTED`
- `agent-ts: tailscale up, vpn ip 100.x.y.z`
- `agent-ts: priming WG tunnel to 100.102.53.22:8787` → `tunnel primed`
- `agent-ws: ws connected` → `server ready (route agent)`

無効（未設定）時は `agent-ts: tailscale disabled/unprovisioned -> direct connection` と出て、従来の LAN 直結になります。

---

## 10. トラブルシューティング

| 症状 | 主な原因と対処 |
|---|---|
| `接続失敗。再試行します` を延々繰り返す | Tailscale は有効だが backend に届いていない。**現状リトライは無制限**（サーバー復帰を待ち続ける設計）。auth key の有効性・期限、管理コンソールにデバイスが出ているか、ACL、backend が `:8787` で待受か、`host`/`peer` の IP を確認。 |
| `tailscale not connected after 90000ms` | Tailscale 登録に失敗。auth key、インターネット/DERP 到達性、**時刻同期**（TLS は正しい時計が必要。SNTP が効いているか）を確認。 |
| TLS 証明書エラー（DERP/コントロールプレーン） | 自前 DERP / headscale など独自 CA を使う場合、同梱 CA バンドルに含まれない可能性。`sdkconfig.defaults` の `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL`（現状）を維持、または独自 CA を組み込む。逆に flash を詰めたいなら主要 CA のみの `..._DEFAULT_CMN` で足りるか実機確認。 |
| トンネルは張れるが WS が一度失敗 | プライミング後〜本接続の間に WG セッションが idle で落ちる窓。エージェントの再接続で自動回復します。 |
| 内蔵 SRAM 不足で不安定 | `CONFIG_ML_MAX_PEERS` を小さく（既定 4）、`CONFIG_ML_ENABLE_CONFIG_HTTPD=n`（既定）を維持。H2/JSON バッファは PSRAM。`LWIP_MAX_SOCKETS` の枯渇は `LWIP_STATS` で確認。 |
| Tailscale を無効化したい | `tscale/en=0`、または `-DTAILSCALE_DEFAULT_*` を付けずに再ビルド（`idf.py fullclean` 後）。 |

> 互換性メモ: microlink は Tailscale 公式のコーディネーションを前提にした独自実装です。**headscale**
> など非公式コーディネーションサーバーでの動作は未検証です。

---

## 11. 関連ファイル

- 実装: [`main/agent/tailscale.cpp`](../main/agent/tailscale.cpp), [`tailscale.h`](../main/agent/tailscale.h)
- 呼び出し: [`main/agent/agent.cpp`](../main/agent/agent.cpp)（`startNetwork()` 直後）
- エージェント接続設定: [`main/agent/agent_config.cpp`](../main/agent/agent_config.cpp)（`cagent` NVS / `AGENT_DEFAULT_*`）
- 依存取得: [`repos.json`](../repos.json) / `fetch_repos.py`（microlink を `firmware/microlink` へ）
- ビルド設定: [`CMakeLists.txt`](../CMakeLists.txt), [`main/CMakeLists.txt`](../main/CMakeLists.txt), [`sdkconfig.defaults`](../sdkconfig.defaults)
