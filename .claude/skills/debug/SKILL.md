---
name: debug
disable-model-invocation: false
description: Constellation を実機（macOS）で起動し、mcp_flutter 経由で動作確認・スクリーンショット取得・UI 操作を行います
---

# 実機動作確認ガイドライン

Constellation を macOS で起動し、Claude が `mcp_flutter` 経由で UI を操作・観察するための手順。
リモート接続は `test_docker/` の SFTP/SMB を使用する。

## いつ使うか

- 新機能追加・リファクタ後にユニットテストでは検出できない UI/統合バグを確認するとき
- ユーザーから「動作確認して」「実機で検証して」と依頼されたとき
- code-review の指摘修正が UI に反映されているか確認するとき

## 前提

- mcp_flutter MCP サーバーがユーザー側で起動済み（`docs/dev_setup.md` §3 参照）
- 利用可能ツール: `mcp__flutter-inspector__*` および `runClientTool` で `screenshot` / `screenshot_to_file` / `current_route` / `navigate` / `tap` / `type_text` / `widget_tree` / `wait_for_idle` / `seed_test_state`
- Docker Desktop が起動している（リモート接続検証時のみ）

## 1. アプリ起動

### 1.1 既存プロセスをクリーンアップ

```bash
pkill -f Constellation 2>&1 || true
pkill -f "flutter run" 2>&1 || true
sleep 2
```

### 1.2 nohup でバックグラウンド起動

`flutter run` は stdin EOF で即終了するため、必ず `nohup` を使う。`tail -f /dev/null |` 経由は不安定。

```bash
nohup flutter run -d macos --debug \
  --host-vmservice-port=8182 --dds-port=8181 \
  --disable-service-auth-codes \
  > /tmp/constellation_run.log 2>&1 &
disown
```

`--enable-vm-service` フラグは Flutter 3+ で削除済みなので使わない。

### 1.3 VM Service 起動を Monitor で待つ

```bash
tail -f /tmp/constellation_run.log | \
  grep -E --line-buffered "VM Service on|A Dart VM|Error|Failed to|Lost connection|Built build"
```

`A Dart VM Service on macOS is available at: http://127.0.0.1:8181/` が出れば準備完了。
"Failed to foreground app; open returned 1" は無害な警告（既に他インスタンスが foreground にいる）。

### 1.4 MCP 接続確認

```
mcp__flutter-inspector__get_vm  → VM 情報が返れば OK
mcp__flutter-inspector__listClientToolsAndResources  → 9 カスタムツールが登録されていれば OK
```

登録されるツール: `screenshot` / `screenshot_to_file` / `widget_tree` / `wait_for_idle` / `current_route` / `navigate` / `tap` / `type_text` / `seed_test_state`

## 2. UI 操作・観察

### 2.1 現在のルート確認

```
mcp__flutter-inspector__current_route
```

### 2.2 ナビゲーション

```
mcp__flutter-inspector__navigate path=/browser query='{"location":"local:/Users/me"}'
mcp__flutter-inspector__navigate path=/settings
mcp__flutter-inspector__navigate path=/
```

遷移直後のフレーム race 対策:

- `screenshot` / `screenshot_to_file` 内部で `SchedulerBinding.endOfFrame` を
  最大 5 フレーム / 500ms 待つため、通常の軽い遷移なら `navigate` → `screenshot`
  だけで新しいフレームが撮れる
- 重い画面（設定タブ切り替えで多数の Widget を構築するなど）で上記が不足な場合は
  `navigate` → `wait_for_idle` → `screenshot` の順で呼ぶ
- `wait_for_idle` は `timeoutMs` を渡せる（デフォルト 1000ms、最大 30000ms）

### 2.3 タップ・入力

```
mcp__flutter-inspector__tap text=ホーム             # 表示テキストで指定
mcp__flutter-inspector__tap key=my_button_key       # ValueKey<String> で指定
mcp__flutter-inspector__type_text text=hello        # primary focus の TextField に入力
```

### 2.4 スクリーンショット取得

**推奨: `screenshot_to_file`（低 context 消費）**

```
mcp__flutter-inspector__screenshot_to_file
```

戻り値はファイルパスのみ（数十バイト）。返されたパスを `Read` でそのまま画像
として開けるため Claude の context を圧迫しない。保存先は
`$TMPDIR/constellation_screenshots/shot_<micros>.png`。

```
mcp__flutter-inspector__screenshot_to_file max_width=800   # 縮小してサイズ削減
```

**base64 取得が必要な場合: `screenshot`**

```
mcp__flutter-inspector__screenshot
mcp__flutter-inspector__screenshot max_width=800           # 縮小版
```

戻り値が大きい（通常 80〜200 KB、`max_width` で削減可）ため Claude のレスポンス
は `tool-results/*.txt` に保存される。画像ファイルとして取り出すワンライナー:

```bash
python3 -c "
import json, base64, sys
src = sys.argv[1]
with open(src) as f: data = json.load(f)
for item in data:
    text = item.get('text', '') if isinstance(item, dict) else ''
    if text.startswith('{'):
        body = json.loads(text)
        if 'image' in body:
            with open('/tmp/constellation_screenshots/<name>.png', 'wb') as f:
                f.write(base64.b64decode(body['image']))
            print('saved')
            break
" /Users/mario/.claude/projects/.../tool-results/mcp-flutter-inspector-screenshot-XXX.txt
```

その後 `Read /tmp/constellation_screenshots/<name>.png` で画像を直接見られる。
`screenshot_to_file` ならこの変換は不要。

### 2.5 ウィジェットツリー確認

```
mcp__flutter-inspector__widget_tree           # default depth=15
mcp__flutter-inspector__widget_tree depth=50  # 深いサブツリー向け
```

デフォルト 15、最大 100。テキストサイズが 100KB を超えると末尾が自動的に省略
され、戻り値の `truncated=true` で検知できる。アプリレイヤー（`HomePage` /
`SettingsPage` など）までは通常 depth=15 程度で到達する。

## 3. リモート接続検証（test_docker）

### 3.1 docker 起動

```bash
cd test_docker
./scripts/seed_sample_data.sh    # 初回のみ
docker compose up -d
docker compose ps                 # constellation-sftp / constellation-smb の Status 確認
```

### 3.2 接続情報

| プロトコル | host      | port | user     | password | 共有/ディレクトリ                     |
| ---------- | --------- | ---- | -------- | -------- | ------------------------------------- |
| SFTP       | localhost | 2222 | testuser | testpass | `/upload`（書込）、`/samples`（読込） |
| SMB        | localhost | 1445 | smbuser  | smbpass  | `public`, `private`                   |

SMB の port は **1445**（ローカル 445 衝突回避）。

### 3.3 アプリから接続

`tap` + `type_text` での自動化は型疲れするので、複雑な接続フォームは
`navigate` + `seed_test_state` で初期状態をセットするか、ユーザーに手動操作を依頼する。

### 3.4 docker 停止

```bash
cd test_docker
docker compose down
```

## 4. ホットリロード・再起動

```
mcp__flutter-inspector__hot_reload_flutter   # コード変更を即反映、状態保持
mcp__flutter-inspector__hot_restart_flutter  # state リセット
```

## 5. 後片付け

```bash
pkill -f Constellation 2>&1 || true
pkill -f "flutter run" 2>&1 || true
docker compose -f test_docker/compose.yml down 2>&1 || true
```

## 6. トラブルシューティング

| 症状                                                | 原因                                         | 対処                                                                                        |
| --------------------------------------------------- | -------------------------------------------- | ------------------------------------------------------------------------------------------- |
| `SocketException: Operation not permitted` (port 0) | macOS Sandbox に `network.server` 権限が無い | `macos/Runner/DebugProfile.entitlements` で確認                                             |
| `Lost connection to device`                         | flutter run が stdin EOF で終了              | `nohup` で起動、`tail -f /dev/null \|` は使わない                                           |
| `VM service not connected` (MCP 側)                 | アプリが終了済み or VM Service 接続前        | `lsof -iTCP:8181` で確認、再起動                                                            |
| screenshot に古い画面が映る                         | navigate → screenshot 直後の race            | 新しい `screenshot` は endOfFrame を自動待機する。それでも古い場合は `wait_for_idle` を挟む |
| `listClientToolsAndResources` が空                  | mcp_toolkit 初期化前 / app 未起動            | アプリ起動後 5 秒待つ                                                                       |
| docker pull で `linux/amd64` 警告                   | image が arm64 非対応                        | multi-arch image に差し替え（既に対応済み）                                                 |

## 7. 検証結果の記録

スクリーンショットと観察事項は `/tmp/constellation_screenshots/` に集約する。
重大な発見はその場で issue として TaskCreate に登録、軽微なら `docs/<feature>/implementations.md` の「既知の問題」節に追記。
