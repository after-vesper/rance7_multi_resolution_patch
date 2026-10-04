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
2. `DllMain` で exe の IAT をパッチ:
   - `GDI32!BitBlt` → `StretchBlt` に置き換えて 800×600 → 出力解像度へ
     スケーリング (HALFTONE / COLORONCOLOR を Filter 設定で切替)
   - `USER32!ScreenToClient` → 返却座標を 800×600 論理座標に逆変換
     (マウスヒットテスト整合のため)
3. `Sys40WindowClass` のウィンドウを検出し、クライアント領域が
   出力解像度になるよう `SetWindowPos` でリサイズ
4. 設定は `MultiRes.ini` (`[Display] Width/Height/Filter`)

### 実機検証結果 (1600×1200)

- ウィンドウが 1600×1200 クライアントで表示され、ゲーム画面全体が
  滑らかにスケーリングされることを確認
- タイトル画面の「ゲーム終了」ボタンをスケール後座標でクリック →
  終了演出 (「午前5時」画面) まで正常に到達。マウス座標変換も機能
- 検証スクリーンショット: `docs/verification/`

### 残課題

- 非 4:3 解像度指定時の挙動 (現在は引き伸ばし。レターボックス化は今後)
- フルスクリーン (Alt+Enter) 時の経路は別系統。ウィンドウモードのみ対応
- SetCursorPos によるカーソル移動系は未対応 (現状 ScreenToClient 逆変換で
  実用上は問題なし)
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
