# 戦国ランス マルチ解像度パッチ 実装仕様

## 1. 対象環境

- ゲーム: Sengoku Rance (戦国ランス) Kagura Games 版
- エンジン: Alicesoft System 4.0 (Ver.1.31), VM = Sys42VM
- 内部描画解像度: 800×600 固定 (SACT 座標系)
- 起動設定: `System40.ini` の `ViewWidth` / `ViewHeight`

## 2. 描画経路の実態 (実機 + 逆アセンブル調査)

### ini の ViewWidth/ViewHeight 単独変更の検証結果

`1600×1200` に変更して起動:

- ウィンドウクライアント領域は 1600×1200 で作成される
- ゲーム内容は左上 800×600 にのみ描画、残りは黒
- → View サイズはバックバッファ/ウィンドウサイズのみ変更し、
  コンテンツ描画はスケールされない

### レンダラ構成 (プロセス内モジュール列挙で確認)

- **ウィンドウモードは GDI 描画**: プロセスに d3d9.dll / ddraw.dll /
  D3D9Factory.dll はロードされない。exe は `CreateDIBSection` +
  `CreateCompatibleDC` + `BitBlt` をインポートしており、メインサーフェース
  (800×600 DIB) をウィンドウ DC に `BitBlt` する方式
- `D3D9Factory.dll` / `ddraw` はフルスクリーン系の経路と推定
- exe は `DINPUT.dll` から `DirectInputCreateEx` のみをインポート
  (ただし `UseJoypad=false` のため実行時には未呼出)

## 3. Phase 1 実装: 出力解像度切替 (実証済み)

### 方式: dinput.dll プロキシ + BitBlt フック

`src/proxy/dinput_proxy.c` として実装・実機検証済み:

1. ゲームディレクトリに `dinput.dll` を配置 (暗黙リンクのロード順で
   exe ディレクトリが優先)。dinput の 7 エクスポートを
   `SysWOW64\dinput.dll` にフォワード
2. `DllMain` で**全ロード済みモジュール**の IAT をパッチ (マウス/描画処理は
   exe ではなくエンジン DLL 側にあるため):
   - `GDI32!BitBlt` → `StretchBlt` に置き換えて 800×600 → 出力解像度へ
     スケーリング (HALFTONE / COLORONCOLOR を Filter 設定で切替)。
     ゲームウィンドウ宛ての blit は部分更新を含めて全て比例変換する
     (全画面一致のみ拡大するとタイトル画面で左上に等倍描画が残り
     点滅するため)
   - `USER32!GetCursorPos` → 800×600 論理空間のスクリーン座標を返す
   - `USER32!SetCursorPos` → 引数 (論理座標) を一律スケールして実位置へ
   - `USER32!ScreenToClient` → 素通り (GetCursorPos 側で論理化済みの
     ため二重変換を防ぐ)
3. `Sys40WindowClass` のウィンドウを検出し、クライアント領域が
   出力解像度になるよう `SetWindowPos` でリサイズ。あわせて WndProc を
   サブクラス化し、マウス系メッセージ (WM_MOUSEMOVE/ボタン系) の
   lParam クライアント座標を論理座標に変換
4. 設定は `MultiRes.ini` (`[Display] Width/Height/Filter`)

### カーソル自動移動 (ワープ) への対応

ゲームは特定操作時にカーソルを自動移動させる。その際、移動開始位置を
実クライアント座標 (WM_MOUSEMOVE lParam / GetCursorPos 経路) として
読み、論理目標座標との間を補間するため、座標系が混在する。

対策: 全てのカーソル入力 (GetCursorPos・マウスメッセージ) を論理座標に
統一し、SetCursorPos の出力を一律スケールすることで、補間全体が
論理空間で行われ、見た目も現在位置→目的位置への滑らかな移動になる。

### 実機検証結果 (1600×1200)

- ウィンドウが 1600×1200 クライアントで表示され、ゲーム画面全体が
  滑らかにスケーリングされることを確認
- 企業ロゴ・タイトル画面とも左上への等倍描画の点滅がないことを確認
- タイトルメニューの各ボタンでクリック・ホバーが正しい位置に届く
  ことを確認
- カーソル自動移動が現在位置→目的位置へ滑らかに移動し、最終位置が
  正しいことを確認
- 検証スクリーンショット: `docs/verification/`

### 表示方式 (2026-10-04 更新)

- ゲームの blit は全て 800×600 の内部バックバッファへ 1:1 でミラーし、
  ウィンドウへの転送はバッファ全体を 1 回の `StretchBlt` で行う。
  部分 blit 毎の座標丸めによる継ぎ目の揺れ (非整数スケール時の
  「ブルブル」) を解消するため。
- 非 4:3 の出力サイズでは、クライアント内に 4:3 のビューポートを
  中央配置してレターボックス/ピラーボックス表示。バー部分は
  `PatBlt` で黒塗り (ジオメトリ変化時のみ)。
- `MultiRes.ini` の `ScaleMode`: 0=引き伸ばし, 1=4:3 維持,
  2=最大整数倍 (小数スケールの揺れを完全に避けたい場合)
- マウス・カーソル座標の変換は全てビューポート基準

### ボーダーレス全画面

- `Fullscreen=1` で WS_POPUP 化してモニター全域に拡大。
  ビューポート機構により 16:9 等では両サイドに黒帯 (ピラーボックス)
- Alt+Enter で実行時にボーダーレス全画面⇔ウィンドウを切替
  (元のスタイル・メニュー・位置を退避して復帰)
- 排他フルスクリーンは使用しない (実機でも d3d9/ddraw はロード
  されず、表示は GDI 経路のまま)

### 残課題

- スナップショット保存 (jpg/bmp) が 800×600 のままかは未確認

## 4. Phase 2: アセット高解像度化

- `Rance7*.ald` の展開 → CG を AI アップスケール (x2) → 再パック or
  優先度の高い差分 `.ald` を追加
- 要調査: `ALDLoader.dll` のアーカイブ解決順序 (同名ファイルの優先度)
- `.fnl` フォントの高解像度版も必要になる可能性

## 5. Phase 3: HD アセットモード切替

- HD モードでは実解像度描画が必要:
  - View を実解像度に設定した上で、SACT のスプライト座標・
    `ISurface` サイズをスケール倍率で変換するレイヤを挟む
    (`SACT2.dll` の `IDrawPluginDrawer` フック)
  - フォント (`SengokuRanceFont.dll` / `DrawText2.dll`) の
    サイズ変換も別途対応
- SD/HD 切替は ini 設定 + 起動時切替で実現

## 6. 配布形式

公式パッチ (`Sengoku Rance JP Patch v1.exe`) は Indigo Rose Setup Factory
製インストーラーでファイルを丸ごと差し替える形式。
本パッチは `dinput.dll` + `MultiRes.ini` の配置のみで導入可能なため、
zip 展開型または同等の簡易インストーラーで配布する想定。

## 7. 開発環境メモ

- ビルド: `src/proxy/build.bat` (clang + lld-link、Windows SDK のみ、
  CRT なし、i686 ターゲット)
- 診断ログ: ゲームフォルダの `multires_log.txt` (開発中のみ)
