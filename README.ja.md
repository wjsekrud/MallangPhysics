# MallangPhysics

[English](README.md) · [한국어](README.ko.md)

Windows 版 Unreal Engine 5.6・5.7・5.8 で、スケルタルメッシュとスタティックメッシュを柔らかくするオープンソースのソフトボディプラグインです（MIT ライセンス）。手や物体が押した所はへこみ、周りの肉は盛り上がります。動く手は肌を引きずり、体が動くと肉が揺れます。エディタでメッシュを一度ベイクしてコンポーネントを一つ付けると、変形は GPU で計算されます。Unity 6.3 URP 版もベータとして同梱しています。

[![柔らかいボールを握る手と、柔らかいクッションを押す手](.github/media/hero.webp)](https://www.youtube.com/watch?v=n5HpN2XGicA)

**動画:** https://www.youtube.com/watch?v=n5HpN2XGicA

**ドキュメント:** https://wjsekrud.github.io/MallangPhysics/ （英語・韓国語のみ）

## ダウンロード

| | 場所 | 内容 |
|---|---|---|
| ソース | このリポジトリの [`Unreal/SoftbodySystem`](Unreal/SoftbodySystem) | C++ ソースをすべて含むプラグイン。同じソースが 5.6・5.7・5.8 でビルドできます。 |
| ビルド済み版 | [Releases](https://github.com/wjsekrud/MallangPhysics/releases): `MallangPhysics-1.0.0-UE5.x-Win64.zip` | エンジンのバージョンごとにビルド済みのプラグイン。シェーダーと公開ヘッダーを含みます。ブループリントのみのプロジェクトでも使えます。Development と Shipping でビルド・パッケージでき、DebugGame はビルドできません。 |
| デモプロジェクト | [Releases](https://github.com/wjsekrud/MallangPhysics/releases): `MallangPhysicsDemo-1.0.0-UE5.x.zip` | ビルド済み版が入ったブループリントプロジェクト。同じバージョンのエンジンで Visual Studio なしで開けます。 |
| Unity（ベータ） | このリポジトリの [`Unity/com.wjsekrud.mallangphysics`](Unity/com.wjsekrud.mallangphysics) | Unity 6.3 + URP。[Unity ドキュメント](https://wjsekrud.github.io/MallangPhysics/unity/)（英語）を参照。 |

## インストール

- ソース: `Unreal/SoftbodySystem` をプロジェクトの `Plugins` フォルダにコピーし、プロジェクトをビルドします。
- ビルド済み版: エンジンのバージョンに合った zip の `SoftbodySystem` フォルダを、プロジェクトの `Plugins` フォルダに展開します。
- Unity: Package Manager で git URL `https://github.com/wjsekrud/MallangPhysics.git?path=/Unity/com.wjsekrud.mallangphysics` を追加します。

その後は [Quick Start](https://wjsekrud.github.io/MallangPhysics/Quick-Start)（英語）に従います。

## ライセンス

- プラグインのソース、ビルド済み版、Unity パッケージ: [MIT ライセンス](LICENSE)。Unity パッケージに含まれるサードパーティのコードは、それぞれのライセンスに従います（[Third Party Notices](Unity/com.wjsekrud.mallangphysics/Third%20Party%20Notices.md)）。
- デモプロジェクトとドキュメント: Copyright © 2026 wjsekrud. All Rights Reserved. 他の制作者のコンテンツは、それぞれのライセンスに従います（[Credits and License](https://wjsekrud.github.io/MallangPhysics/Credits-and-License)）。

## 補足

- 2026 年 11 月から、作者は長期間 Issue やプルリクエストに返信できない場合があります。ユーザー同士の質問は [Discussions](https://github.com/wjsekrud/MallangPhysics/discussions) へ。
- コードのコメントは韓国語です（各シェーダーファイル冒頭の概要のみ英語）。コメント中の 일지 123 や ADR-0012 などの番号は、公開していない設計記録を指します。
- バグ報告: [Issues](https://github.com/wjsekrud/MallangPhysics/issues)。
