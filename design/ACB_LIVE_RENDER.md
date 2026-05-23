# ACB Live Render on PS1 — Design Doc

**Status**: In progress. Started 2026-05-23 with M16 (Phase 1 kickoff).
**Goal**: PS1 上で全主要 voice (303×2 + 808 + 909 のうち 10 voice) を
**component-level な ACB (Analog Circuit Behavior) ローポリモデル** で
**毎サンプル live render** する。Roland TR-8 / System-1 が PC でやってる事を
PS1 の整数 33 MHz CPU に合わせて解像度を落として再現する。

ノイズ系 (HH/CY/CB) は phase 11 で追加検討、ベースの 10 voice が安定動作
してから様子を見て live 化判断。

---

## 1. 現状 (M15 まで) のおさらい

- 回路モデルは `host_tests/` で C++ DSP として実装済み (cmath 使用)
- host で WAV → PSX ADPCM 化 → `src/generated/voice_samples.h` に焼き込み
- PS1 では SPU が ADPCM サンプル再生するだけ。**回路は走ってない**
- live なのは: pitch (SPU sample rate)、volume (accent boost、EQ scale)、reverb send
- 焼き込み済み: filter cutoff sweep、envelope、内部 modulation

ユーザー要望: ARIA / ACB シリーズみたいに knob を動かしたら本当に音色が変わる
acid line を PS1 で出したい。

---

## 2. 目指す方向 — ACB ローポリ live

### 解像度の選択肢

| レベル | 例 | PS1 で可能? |
|---|---|---|
| 本格 ACB | Roland TR-8、Newton-Raphson で非線形 node 方程式を毎 sample 解く | ❌ CPU 数倍足りない |
| **ローポリ ACB** | 各 component を Q24 整数でラフ近似 (ダイオード = tanh LUT、OTA = linear + soft-clip、cap = 1-pole filter) | ✅ やれる (本ドキュメントの対象) |
| 現状 (behavioural) | filter ブロックだけ正確 (Stilson-Smith ladder、Chamberlin SVF) | M15 までの実装 |

### 信号レート (SR)

- **22050 Hz** を採用 (NTSC PS1 BIOS の SPU default 44100 の半分)
- 33.8688 MHz CPU で 22050 sample/sec → 1 sample あたり **~1500 cycle 予算** (全 CPU)
- GPU/scene/入力で ~40% 持って行かれる前提 → live render に **~900 cycle/sample** 予算
- 同時 10 voice live なら 1 voice あたり **90 cycle/sample** 予算

### 出力経路 — SPU streaming

- SPU には voice channel に "stream samples を流し込む" 標準 API がない
- 解決: **double-buffered ADPCM streaming**
  - 2 つの ADPCM buffer を SPU RAM に確保 (例: 1024 sample / buffer × 2 = 2 KB ADPCM)
  - 1 つ再生中、もう 1 つに次フレーム分の sample を encode
  - SPU IRQ (loop point 到達) で buffer 切替
  - 既に FF9 / MGS 等の PS1 BGM streaming で実績ある手法
- mix engine: 10 voice の Q24 出力を stereo PCM (16-bit) に合成

---

## 3. 全体アーキテクチャ

```
+---------------------+
| Sequencer (frame@60Hz)
|  - step trigger
|  - knob updates -> voice param store
+----------+----------+
           |
           v
+---------------------+
| Voice Param Cache (per-voice, updated 60 Hz)
|  - cutoff, reso, env mod, decay
|  - note, accent, slide
|  - 残り time-to-next-step
+----------+----------+
           |
           v frame で param 更新、22050 Hz で render
+-------------------------------------------------+
| Audio Pipeline (running @22050 Hz, in IRQ ctx) |
|                                                |
|   for each voice (10 live + 3 sample):         |
|     if isLive: voice.tickQ24() -> q24          |
|     else:      sample.next() -> q24             |
|                                                |
|   mix -> stereo PCM (s16, s16)                  |
|   PCM -> ADPCM encoder (4-bit/sample)           |
|   ADPCM -> SPU RAM (active buffer)              |
+-------------------------------------------------+
                |
                v SPU IRQ on loop -> swap buffer
+---------------------+
|   SPU plays buffer  | -> CD audio / speakers
+---------------------+
```

### 主要モジュール

| モジュール | 場所 | 役割 |
|---|---|---|
| `acb::qmath` | `src/dsp/qmath.{hpp,cpp}` | Q24 sin/cos/exp/tanh LUT + helpers (mul/div) |
| `acb::voice::*` | `src/voices/acb_*.{hpp,cpp}` | voice ごとの ACB component model (DCO/VCF/VCA/Env) |
| `acb::stream` | `src/audio/adpcm_stream.{hpp,cpp}` | double-buffer SPU streaming engine + IRQ handler |
| `acb::encode` | `src/audio/adpcm_encode.{hpp,cpp}` | PCM (s16) → ADPCM (4-bit) encoder |
| `acb::mix` | `src/audio/mix.{hpp,cpp}` | N voice → stereo PCM mixer |
| `acb::engine` | `src/audio/engine.{hpp,cpp}` | 全体パイプラインの glue |

---

## 4. 技術仕様

### 4.1 Q24 fixed-point

```cpp
namespace acb::qmath {
constexpr int Q = 24;
using q24 = int32_t;  // signed Q24

constexpr q24 ONE  = 1 << Q;           // 1.0
constexpr q24 HALF = 1 << (Q - 1);     // 0.5

inline q24 mul(q24 a, q24 b) {
    int64_t x = static_cast<int64_t>(a) * static_cast<int64_t>(b);
    return static_cast<q24>(x >> Q);
}
inline q24 div(q24 a, q24 b) {
    int64_t x = (static_cast<int64_t>(a) << Q) / static_cast<int64_t>(b);
    return static_cast<q24>(x);
}
}
```

### 4.2 LUT (整数版超越関数)

| 関数 | LUT サイズ | 補間 | 精度目標 |
|---|---|---|---|
| `sin_q24(q24 phase)` | 256 entries (1/4 wave、symmetry で 4 倍化) | linear | ±0.001 (-60 dB) |
| `exp_q24(q24 x)` | 256 entries (x = -8..0、env 用) | linear | ±0.005 |
| `tanh_q24(q24 x)` | 128 entries (x = 0..4、symmetry で正負化) | linear | ±0.003 |
| `pow2_q24(q24 x)` | 256 entries (x = 0..1、note→Hz 変換用) | linear | ±0.0005 |

LUT は `constexpr` で main RAM に置く (各 1-2 KB)。host_tests で cmath 版との
diff を出して acceptance チェック (M16 acceptance criterion)。

### 4.3 ADPCM streaming protocol

- buffer size: **1024 PCM sample / buffer × 2 buffer** = 2048 sample = ~46ms @ 22050Hz
- ADPCM 圧縮率 4:1 → SPU RAM 占有: 1024 × 2 byte / 4 × 2 buffer = **1 KB**
- SPU IRQ: voice 0 (= streaming voice) の loop point に IRQ flag セット
  - buffer 末尾到達で SPU → CPU IRQ
  - CPU は IRQ handler で次 buffer を render + encode + DMA
- レイテンシ: buffer 1 つ分 ≈ 23ms (NTSC frame 約 17ms より長い → 1 frame 遅延)
- 同期: 各 buffer 開始時点で sequencer step を再評価、step 内 sample 数分だけ
  trigger 適用

### 4.4 Voice config schema

```cpp
struct VoiceConfig {
    const char *name;
    bool        isLive;      // true: ACB live、false: sample playback
    int         band;        // EQ band: 0=LOW, 1=MID, 2=HIGH
    // Sample mode only:
    const uint8_t *sampleData;
    unsigned       sampleBytes;
    // Live mode only:
    // (voice 種別ごとに dispatch される ACB instance への pointer は engine 側)
};
```

各 voice の `isLive` を runtime で切り替え可能にして、Phase 移行と
A/B 比較 (live vs sample) を簡単に。

### 4.5 ACB voice common interface

```cpp
namespace acb {
struct VoiceCommon {
    virtual void noteOn(q24 noteHz, bool accent, bool slide) = 0;
    virtual void noteOff() = 0;
    virtual void setParams(q24 cutoff, q24 reso, q24 envMod, q24 decay) = 0;
    virtual q24  tick() = 0;  // 1 sample 進めて出力返す
};
}
```

10 voice 全部この interface を実装。mix engine は polymorphism (or
type-erased dispatch) で呼ぶ。

---

## 5. Phase 分解

各 Phase は独立 commit、CI green、acceptance criteria を満たしたら次へ。

### Phase 1: Q24 DSP lib (M16) — 1-2 セッション
- **Deliverables**:
  - `src/dsp/qmath.{hpp,cpp}` — Q24 helpers + LUT
  - `host_tests/test_qmath.cpp` — cmath との diff 検証
- **Acceptance**:
  - sin_q24 / exp_q24 / tanh_q24 / pow2_q24 が cmath 版と spec 精度内一致
  - ctest 全 green
  - PS1 build (CI) green

### Phase 2: ADPCM streaming engine (M17-M18) — 2-3 セッション
- **Deliverables**:
  - `src/audio/adpcm_encode.{hpp,cpp}` — PCM s16 → 4-bit ADPCM (PSX format)
  - `src/audio/adpcm_stream.{hpp,cpp}` — SPU double buffer + IRQ
  - `src/audio/engine.{hpp,cpp}` — glue + silent stream
- **Acceptance**:
  - silence stream を SPU に流して無音だが正常 (loop / drift なし)
  - 1 kHz sine wave (Q24 で生成) を流して耳判定 OK
  - PS1 build CI green、frame drop なし

### Phase 3: TB-303 stage1 を ACB live 化 (M19) — 2-3 セッション
- **Deliverables**:
  - `src/voices/acb_tb303_stage1.{hpp,cpp}` — component-level モデル
    (DCO + Diode VCF + OTA VCA + Env Mod + Accent)
  - voice config の SAW (idx 7) を isLive=true に切替
- **Acceptance**:
  - host_tests で integer 版が host C++ 版と spec 精度内一致
  - PS1 上で SAW を trigger → knob を動かして音色が **本当に** 変わる
  - 残り 12 voice は sample のまま (hybrid 動作)
  - frame drop なし

### Phase 4-7: 残り 303 + 808 主要 voice (M20-M23) — 各 1-2 セッション
- Phase 4: TB-303 stage1 SQR、stage2 SAW/SQR (303 完成)
- Phase 5: 808 BD、SD
- Phase 6: 808 TOM、CP
- Phase 7: 909 BD、SD

### Phase 8: 統合チューニング (M24) — 1 セッション
- 全 10 live voice 同時再生で CPU 計測
- mix gain 調整、accent 動作確認
- EQ scale 適用が live でも効くか確認

### Phase 9: HH/CY/CB の追加 live 検討 (M25) — 1-2 セッション、オプション
- CPU 余裕あれば LFSR + BPF で integer 実装
- 1 voice ずつ追加して frame drop ないか確認

### Phase 10: 最終耳判定 + design doc 更新 (M26) — 1 セッション

**合計目安: 12-18 セッション (約 4-6 週間)**

---

## 6. リスクと mitigation

| リスク | 影響 | 対策 |
|---|---|---|
| CPU 90%+ で frame drop | 致命的 | 各 phase で計測。超えそうなら voice steal / SR 落とす / ノイズ系を sample に戻す |
| ADPCM encoder のバグで音が割れる | 致命的 | Phase 2 で silence + sine wave で十分検証してから voice 統合 |
| SPU IRQ タイミング不安定 | 音飛び | psyqo の SPU IRQ API 調査、無ければ low-level SPU register 直叩き |
| Q24 オーバーフロー | 雑音 / 発散 | 各 voice の DSP に CHECK macro で範囲確認、host_tests で saturation 検出 |
| 移植中 sample 版を壊す | regression | isLive フラグで分岐、sample 版コードは触らず残す |
| 設計乖離 / 文脈喪失 | 完成度低下 | 本 doc を毎 Phase 開始時に参照、変更あれば doc 更新 |

---

## 7. ファイル構成 (完成形)

```
src/
├── main.cpp                          # 既存、engine init を追加
├── dsp/
│   ├── fixed.hpp                     # 既存 Q24
│   ├── qmath.{hpp,cpp}               # NEW: Q24 LUT (sin/exp/tanh/pow2)
│   ├── chamberlin_q24.{hpp,cpp}      # NEW: Chamberlin SVF integer 版
│   ├── ladder_q24.{hpp,cpp}          # NEW: Stilson-Smith ladder integer 版
│   ├── env_q24.{hpp,cpp}             # NEW: exp env integer 版
│   └── ...                           # 既存 host 用 .cpp は残す (sample render 用)
├── voices/
│   ├── acb_tb303_stage1.{hpp,cpp}    # NEW
│   ├── acb_tb303_stage2.{hpp,cpp}    # NEW
│   ├── acb_808_bd.{hpp,cpp}          # NEW
│   ├── acb_808_sd.{hpp,cpp}          # NEW
│   ├── acb_808_tom.{hpp,cpp}         # NEW
│   ├── acb_808_cp.{hpp,cpp}          # NEW
│   ├── acb_909_bd.{hpp,cpp}          # NEW
│   ├── acb_909_sd.{hpp,cpp}          # NEW
│   ├── acb_808_hh.{hpp,cpp}          # NEW (Phase 9)
│   ├── acb_808_cy.{hpp,cpp}          # NEW (Phase 9)
│   └── acb_808_cb.{hpp,cpp}          # NEW (Phase 9)
├── audio/
│   ├── adpcm_encode.{hpp,cpp}        # NEW
│   ├── adpcm_stream.{hpp,cpp}        # NEW
│   ├── mix.{hpp,cpp}                 # NEW
│   └── engine.{hpp,cpp}              # NEW
├── generated/
│   └── voice_samples.h               # 既存。HH/CY/CB は当面ここから再生
host_tests/
├── test_qmath.cpp                    # NEW (Phase 1)
├── test_acb_tb303_stage1.cpp         # NEW (Phase 3)
└── ... (各 voice ごと)
design/
└── ACB_LIVE_RENDER.md                # 本ドキュメント
```

---

## 8. 進捗トラッキング

進捗は [[project_ps1_acid_rom]] memory に M16 以降の milestone として
記録。各 Phase 完了時に本 doc の "Status" 欄を更新。

| Phase | Milestone | 状態 |
|---|---|---|
| 1 | M16 Q24 DSP lib | ✅ 2026-05-23 (commit ef43c8c、CI #26324066190 green、ctest all 12 pass、qmath 12/12 内 sin/cos err≤0.001、exp≤0.005、tanh≤0.003、pow2≤0.0005 全て tol 内) |
| 2 | M17-M18 ADPCM streaming | ⬜ |
| 3 | M19 TB-303 stage1 live | ⬜ |
| 4 | M20 残り 303 live | ⬜ |
| 5 | M21 808 BD/SD live | ⬜ |
| 6 | M22 808 TOM/CP live | ⬜ |
| 7 | M23 909 BD/SD live | ⬜ |
| 8 | M24 統合チューニング | ⬜ |
| 9 | M25 HH/CY/CB live (optional) | ⬜ |
| 10 | M26 最終耳判定 + doc 更新 | ⬜ |
