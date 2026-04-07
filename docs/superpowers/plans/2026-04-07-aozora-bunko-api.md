# 青空文庫 Vercel API 実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 青空文庫の作品メタデータ検索とオンデマンドEPUB変換を提供するVercel Serverless Functions APIを構築する

**Architecture:** Node.js/TypeScript Vercel Serverless Functions。青空文庫公式CSVインデックスをメモリにロードし、3つのAPIエンドポイント（authors, works, convert）を提供。変換APIは青空文庫サーバーからTXTを取得し、青空文庫注記をパースしてEPUB3を生成して返却する。

**Tech Stack:** TypeScript, Vercel Serverless Functions, vitest, iconv-lite (Shift_JIS), archiver (ZIP/EPUB), papaparse (CSV)

**Spec:** `docs/superpowers/specs/2026-04-07-aozora-bunko-download-design.md`

**Repository:** `zrn-ns/aozora-epub-api`（新規作成）

---

## ファイル構成

```
aozora-epub-api/
├── api/
│   ├── authors.ts                 ← GET /api/authors — 作家一覧
│   ├── works.ts                   ← GET /api/works — 作品一覧
│   └── convert.ts                 ← GET /api/convert — EPUB変換・返却
├── lib/
│   ├── aozora-index.ts            ← CSVインデックス読み込み・検索ロジック
│   ├── aozora-parser.ts           ← 青空文庫注記パーサー（ルビ、見出し、字下げ等）
│   ├── chapter-splitter.ts        ← 見出しベース章分割
│   ├── epub-builder.ts            ← EPUB3 ZIP生成
│   └── types.ts                   ← 共有型定義
├── test/
│   ├── aozora-index.test.ts
│   ├── aozora-parser.test.ts
│   ├── chapter-splitter.test.ts
│   ├── epub-builder.test.ts
│   ├── api-authors.test.ts
│   ├── api-works.test.ts
│   └── api-convert.test.ts
├── test-fixtures/
│   ├── sample-aozora.txt          ← テスト用青空文庫テキスト
│   └── sample-index.csv           ← テスト用CSVインデックス
├── vercel.json
├── package.json
└── tsconfig.json
```

---

### Task 1: プロジェクト初期化

**Files:**
- Create: `package.json`
- Create: `tsconfig.json`
- Create: `vercel.json`
- Create: `.gitignore`

- [ ] **Step 1: GitHubリポジトリ作成**

```bash
gh repo create zrn-ns/aozora-epub-api --public --clone
cd aozora-epub-api
```

- [ ] **Step 2: package.json作成**

```json
{
  "name": "aozora-epub-api",
  "version": "0.1.0",
  "private": true,
  "scripts": {
    "test": "vitest run",
    "test:watch": "vitest",
    "dev": "vercel dev"
  },
  "devDependencies": {
    "@types/node": "^22.0.0",
    "typescript": "^5.7.0",
    "vitest": "^3.0.0",
    "@vercel/node": "^5.0.0"
  },
  "dependencies": {
    "iconv-lite": "^0.6.3",
    "archiver": "^7.0.0",
    "papaparse": "^5.5.0"
  }
}
```

- [ ] **Step 3: tsconfig.json作成**

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "ES2022",
    "moduleResolution": "bundler",
    "strict": true,
    "esModuleInterop": true,
    "outDir": "dist",
    "rootDir": ".",
    "resolveJsonModule": true,
    "declaration": true,
    "sourceMap": true,
    "skipLibCheck": true
  },
  "include": ["api/**/*.ts", "lib/**/*.ts", "test/**/*.ts"],
  "exclude": ["node_modules", "dist"]
}
```

- [ ] **Step 4: vercel.json作成**

```json
{
  "functions": {
    "api/*.ts": {
      "maxDuration": 30
    }
  },
  "headers": [
    {
      "source": "/api/authors",
      "headers": [{ "key": "Cache-Control", "value": "s-maxage=86400" }]
    },
    {
      "source": "/api/works",
      "headers": [{ "key": "Cache-Control", "value": "s-maxage=86400" }]
    },
    {
      "source": "/api/convert",
      "headers": [{ "key": "Cache-Control", "value": "s-maxage=604800" }]
    }
  ]
}
```

- [ ] **Step 5: .gitignore作成**

```
node_modules/
dist/
.vercel/
*.tgz
```

- [ ] **Step 6: 依存インストール・初回コミット**

```bash
npm install
git add -A
git commit -m "✨ プロジェクト初期化（package.json, tsconfig, vercel.json）"
```

---

### Task 2: 共有型定義

**Files:**
- Create: `lib/types.ts`

- [ ] **Step 1: 型定義を作成**

```typescript
// lib/types.ts

/** CSVインデックスから読み込んだ作家情報 */
export interface Author {
  id: number;
  name: string;
  kana: string;
  workCount: number;
}

/** CSVインデックスから読み込んだ作品情報 */
export interface Work {
  id: number;
  title: string;
  titleKana: string;
  authorId: number;
  authorName: string;
  ndc: string;
  textUrl: string;
  publishedDate: string;
}

/** 青空文庫注記パーサーの出力ノード */
export type AozoraNode =
  | { type: "text"; content: string }
  | { type: "ruby"; base: string; reading: string }
  | { type: "heading"; level: 1 | 2 | 3; children: AozoraNode[] }
  | { type: "heading_end"; level: 1 | 2 | 3 }
  | { type: "indent"; chars: number; children: AozoraNode[] }
  | { type: "emphasis"; style: "sesame" | "circle"; children: AozoraNode[] }
  | { type: "pagebreak" }
  | { type: "newline" };

/** 章分割後のチャプター */
export interface Chapter {
  title: string;
  nodes: AozoraNode[];
}

/** APIエラーレスポンス */
export interface ApiError {
  error: string;
  message: string;
}

/** /api/authors レスポンス */
export interface AuthorsResponse {
  authors: Array<{
    id: number;
    name: string;
    kana: string;
    work_count: number;
  }>;
}

/** /api/works レスポンス */
export interface WorksResponse {
  works: Array<{
    id: number;
    title: string;
    kana: string;
    ndc: string;
  }>;
}
```

- [ ] **Step 2: コミット**

```bash
git add lib/types.ts
git commit -m "✨ 共有型定義を追加（Author, Work, AozoraNode, Chapter等）"
```

---

### Task 3: CSVインデックスローダー

**Files:**
- Create: `lib/aozora-index.ts`
- Create: `test/aozora-index.test.ts`
- Create: `test-fixtures/sample-index.csv`

- [ ] **Step 1: テスト用CSVフィクスチャ作成**

青空文庫CSVの実際のカラム構成（`list_person_all_extended_utf8.csv`）に合わせる。主要カラム:
作品ID, 作品名, 作品名読み, ソート用読み, 副題, 副題読み, 原題, 初出, 分類番号, 文字遣い種別, 作品著作権フラグ, 公開日, 最終更新日, 人物ID, 姓, 名, 姓読み, 名読み, 姓読みソート用, 名読みソート用, 姓ローマ字, 名ローマ字, 役割フラグ, 生年月日, 没年月日, 人物著作権フラグ, 底本名1, ..., テキストファイルURL, ...

```csv
作品ID,作品名,作品名読み,ソート用読み,副題,副題読み,原題,初出,分類番号,文字遣い種別,作品著作権フラグ,公開日,最終更新日,人物ID,姓,名,姓読み,名読み,姓読みソート用,名読みソート用,姓ローマ字,名ローマ字,役割フラグ,生年月日,没年月日,人物著作権フラグ,底本名1,底本出版社名1,底本初版発行年1,入力に使用した版1,校正に使用した版1,底本の親本名1,底本の親本出版社名1,底本の親本初版発行年1,底本名2,底本出版社名2,底本初版発行年2,入力に使用した版2,校正に使用した版2,底本の親本名2,底本の親本出版社名2,底本の親本初版発行年2,テキストファイルURL,テキストファイル最終更新日,テキストファイル符号化方式,テキストファイル文字集合,テキストファイル修正回数,XHTML/HTMLファイルURL,XHTML/HTMLファイル最終更新日,XHTML/HTMLファイル符号化方式,XHTML/HTMLファイル文字集合,XHTML/HTMLファイル修正回数
000879,羅生門,らしょうもん,らしようもん,,,,,NDC 913,新字新仮名,なし,1999-01-11,2014-09-17,000879,芥川,龍之介,あくたがわ,りゅうのすけ,あくたかわ,りゆうのすけ,Akutagawa,Ryunosuke,著者,1892-03-01,1927-07-24,なし,芥川龍之介全集1,ちくま文庫,1986（昭和61）年10月28日,1997（平成9）年4月15日第6刷,,,,,,,,,,,,https://www.aozora.gr.jp/cards/000879/files/127_ruby_150.zip,2014-09-17,ShiftJIS,JIS X 0208,0,,,,,,
000879,鼻,はな,はな,,,,,NDC 913,新字新仮名,なし,1999-01-11,2014-09-17,000879,芥川,龍之介,あくたがわ,りゅうのすけ,あくたかわ,りゆうのすけ,Akutagawa,Ryunosuke,著者,1892-03-01,1927-07-24,なし,芥川龍之介全集1,ちくま文庫,1986（昭和61）年10月28日,1997（平成9）年4月15日第6刷,,,,,,,,,,,,https://www.aozora.gr.jp/cards/000879/files/42_ruby_150.zip,2014-09-17,ShiftJIS,JIS X 0208,0,,,,,,
000148,或る女,あるおんな,あるおんな,,,,,NDC 913,新字新仮名,なし,2001-03-15,2014-09-17,000148,有島,武郎,ありしま,たけお,ありしま,たけお,Arishima,Takeo,著者,1878-03-04,1923-06-09,なし,有島武郎全集2,筑摩書房,1980（昭和55）年11月20日,,,,,,,,,,,,https://www.aozora.gr.jp/cards/000025/files/227_ruby_150.zip,2014-09-17,ShiftJIS,JIS X 0208,0,,,,,,
001154,走れメロス,はしれめろす,はしれめろす,,,,,NDC 913,新字新仮名,なし,1999-03-12,2014-09-17,000035,太宰,治,だざい,おさむ,たさい,おさむ,Dazai,Osamu,著者,1909-06-19,1948-06-13,なし,走れメロス,新潮文庫,1967（昭和42）年1月,1986（昭和61）年4月30日第54刷,,,,,,,,,,,,https://www.aozora.gr.jp/cards/000035/files/1567_ruby_150.zip,2014-09-17,ShiftJIS,JIS X 0208,0,,,,,,
000906,吾輩は猫である,わがはいはねこである,わかはいはねこてある,,,,,NDC 913,新字新仮名,なし,1999-09-17,2014-09-17,000148,夏目,漱石,なつめ,そうせき,なつめ,そうせき,Natsume,Soseki,著者,1867-02-09,1916-12-09,なし,吾輩は猫である,岩波文庫,1990（平成2）年2月5日,,,,,,,,,,,,https://www.aozora.gr.jp/cards/000148/files/789_ruby_5969.zip,2014-09-17,ShiftJIS,JIS X 0208,0,,,,,,
```

注: 上記CSVは実際の青空文庫CSVフォーマットを簡略化したもの。`test-fixtures/sample-index.csv` として保存。

- [ ] **Step 2: テスト作成**

```typescript
// test/aozora-index.test.ts
import { describe, it, expect, beforeAll } from "vitest";
import { AozoraIndex } from "../lib/aozora-index.js";
import { readFileSync } from "fs";
import { resolve } from "path";

describe("AozoraIndex", () => {
  let index: AozoraIndex;

  beforeAll(() => {
    const csv = readFileSync(
      resolve(__dirname, "../test-fixtures/sample-index.csv"),
      "utf-8"
    );
    index = AozoraIndex.fromCsv(csv);
  });

  describe("getAuthorsByKanaPrefix", () => {
    it("ア行の作家を返す", () => {
      const authors = index.getAuthorsByKanaPrefix("ア");
      expect(authors).toHaveLength(2);
      expect(authors[0].name).toBe("芥川龍之介");
      expect(authors[0].workCount).toBe(2);
      expect(authors[1].name).toBe("有島武郎");
      expect(authors[1].workCount).toBe(1);
    });

    it("該当なしの行は空配列", () => {
      const authors = index.getAuthorsByKanaPrefix("カ");
      expect(authors).toHaveLength(0);
    });
  });

  describe("getWorksByAuthorId", () => {
    it("作家IDで作品を取得", () => {
      const works = index.getWorksByAuthorId(879);
      expect(works).toHaveLength(2);
      expect(works[0].title).toBe("羅生門");
      expect(works[1].title).toBe("鼻");
    });
  });

  describe("getWorksByTitleKanaPrefix", () => {
    it("作品名読みで検索", () => {
      const works = index.getWorksByTitleKanaPrefix("ハ");
      expect(works).toHaveLength(2); // はな, はしれめろす
    });
  });

  describe("getWorksByNdc", () => {
    it("NDC分類で検索", () => {
      const works = index.getWorksByNdc("913");
      expect(works).toHaveLength(5);
    });
  });

  describe("getNewestWorks", () => {
    it("公開日新しい順で取得", () => {
      const works = index.getNewestWorks(2);
      expect(works).toHaveLength(2);
    });
  });

  describe("getWorkById", () => {
    it("作品IDで1件取得", () => {
      const work = index.getWorkById(879);
      expect(work).not.toBeNull();
      expect(work!.title).toBe("羅生門");
      expect(work!.textUrl).toContain("aozora.gr.jp");
    });

    it("存在しないIDはnull", () => {
      const work = index.getWorkById(999999);
      expect(work).toBeNull();
    });
  });
});
```

- [ ] **Step 3: テスト実行して失敗を確認**

```bash
npx vitest run test/aozora-index.test.ts
```

Expected: FAIL（`AozoraIndex` が未定義）

- [ ] **Step 4: aozora-index.ts実装**

```typescript
// lib/aozora-index.ts
import type { Author, Work } from "./types.js";

interface CsvRow {
  作品ID: string;
  作品名: string;
  作品名読み: string;
  分類番号: string;
  公開日: string;
  人物ID: string;
  姓: string;
  名: string;
  姓読み: string;
  名読み: string;
  テキストファイルURL: string;
  作品著作権フラグ: string;
}

/** 50音の行→先頭カタカナ文字マッピング */
const KANA_ROW_MAP: Record<string, string[]> = {
  ア: ["ア", "イ", "ウ", "エ", "オ"],
  カ: ["カ", "キ", "ク", "ケ", "コ", "ガ", "ギ", "グ", "ゲ", "ゴ"],
  サ: ["サ", "シ", "ス", "セ", "ソ", "ザ", "ジ", "ズ", "ゼ", "ゾ"],
  タ: ["タ", "チ", "ツ", "テ", "ト", "ダ", "ヂ", "ヅ", "デ", "ド"],
  ナ: ["ナ", "ニ", "ヌ", "ネ", "ノ"],
  ハ: ["ハ", "ヒ", "フ", "ヘ", "ホ", "バ", "ビ", "ブ", "ベ", "ボ", "パ", "ピ", "プ", "ペ", "ポ"],
  マ: ["マ", "ミ", "ム", "メ", "モ"],
  ヤ: ["ヤ", "ユ", "ヨ"],
  ラ: ["ラ", "リ", "ル", "レ", "ロ"],
  ワ: ["ワ", "ヲ", "ン"],
};

function hiraganaToKatakana(str: string): string {
  return str.replace(/[\u3041-\u3096]/g, (ch) =>
    String.fromCharCode(ch.charCodeAt(0) + 0x60)
  );
}

function matchesKanaPrefix(kana: string, prefix: string): boolean {
  const katakana = hiraganaToKatakana(kana);
  const chars = KANA_ROW_MAP[prefix];
  if (!chars) return false;
  const firstChar = katakana.charAt(0);
  return chars.includes(firstChar);
}

export class AozoraIndex {
  private authors: Map<number, Author> = new Map();
  private works: Map<number, Work> = new Map();
  private worksByAuthor: Map<number, Work[]> = new Map();

  static fromCsv(csvContent: string): AozoraIndex {
    const index = new AozoraIndex();
    const lines = csvContent.split("\n");
    const header = lines[0];
    const columns = header.split(",");

    const colIndex = (name: string) => columns.indexOf(name);
    const iWorkId = colIndex("作品ID");
    const iTitle = colIndex("作品名");
    const iTitleKana = colIndex("作品名読み");
    const iNdc = colIndex("分類番号");
    const iPublished = colIndex("公開日");
    const iPersonId = colIndex("人物ID");
    const iLastName = colIndex("姓");
    const iFirstName = colIndex("名");
    const iLastNameKana = colIndex("姓読み");
    const iFirstNameKana = colIndex("名読み");
    const iTextUrl = colIndex("テキストファイルURL");
    const iCopyright = colIndex("作品著作権フラグ");

    const authorWorkCount = new Map<number, number>();

    for (let i = 1; i < lines.length; i++) {
      const line = lines[i].trim();
      if (!line) continue;

      // 簡易CSVパース（青空文庫CSVはクォートなし前提）
      const fields = line.split(",");

      const workId = parseInt(fields[iWorkId], 10);
      const authorId = parseInt(fields[iPersonId], 10);
      if (isNaN(workId) || isNaN(authorId)) continue;

      // 著作権ありは除外
      if (fields[iCopyright] === "あり") continue;

      const textUrl = fields[iTextUrl] || "";
      if (!textUrl) continue; // テキストURLがないものは除外

      const authorName = `${fields[iLastName]}${fields[iFirstName]}`;
      const authorKana = `${fields[iLastNameKana]} ${fields[iFirstNameKana]}`;
      const ndc = (fields[iNdc] || "").replace("NDC ", "").trim();

      const work: Work = {
        id: workId,
        title: fields[iTitle] || "",
        titleKana: fields[iTitleKana] || "",
        authorId,
        authorName,
        ndc,
        textUrl,
        publishedDate: fields[iPublished] || "",
      };

      if (!index.works.has(workId)) {
        index.works.set(workId, work);
        authorWorkCount.set(authorId, (authorWorkCount.get(authorId) || 0) + 1);

        const authorWorks = index.worksByAuthor.get(authorId) || [];
        authorWorks.push(work);
        index.worksByAuthor.set(authorId, authorWorks);
      }

      if (!index.authors.has(authorId)) {
        index.authors.set(authorId, {
          id: authorId,
          name: authorName,
          kana: authorKana,
          workCount: 0, // 後で設定
        });
      }
    }

    // 作品数を設定
    for (const [authorId, count] of authorWorkCount) {
      const author = index.authors.get(authorId);
      if (author) author.workCount = count;
    }

    return index;
  }

  getAuthorsByKanaPrefix(prefix: string): Author[] {
    return Array.from(this.authors.values())
      .filter((a) => matchesKanaPrefix(a.kana, prefix))
      .sort((a, b) => a.kana.localeCompare(b.kana, "ja"));
  }

  getWorksByAuthorId(authorId: number): Work[] {
    return (this.worksByAuthor.get(authorId) || []).sort((a, b) =>
      a.titleKana.localeCompare(b.titleKana, "ja")
    );
  }

  getWorksByTitleKanaPrefix(prefix: string): Work[] {
    return Array.from(this.works.values())
      .filter((w) => matchesKanaPrefix(w.titleKana, prefix))
      .sort((a, b) => a.titleKana.localeCompare(b.titleKana, "ja"));
  }

  getWorksByNdc(ndc: string): Work[] {
    return Array.from(this.works.values())
      .filter((w) => w.ndc === ndc)
      .sort((a, b) => a.titleKana.localeCompare(b.titleKana, "ja"));
  }

  getNewestWorks(limit: number): Work[] {
    return Array.from(this.works.values())
      .sort((a, b) => b.publishedDate.localeCompare(a.publishedDate))
      .slice(0, limit);
  }

  getWorkById(workId: number): Work | null {
    return this.works.get(workId) || null;
  }
}
```

- [ ] **Step 5: テスト実行してパスを確認**

```bash
npx vitest run test/aozora-index.test.ts
```

Expected: ALL PASS

- [ ] **Step 6: コミット**

```bash
git add lib/aozora-index.ts lib/types.ts test/aozora-index.test.ts test-fixtures/sample-index.csv
git commit -m "✨ CSVインデックスローダーとテストを追加"
```

---

### Task 4: 青空文庫注記パーサー

**Files:**
- Create: `lib/aozora-parser.ts`
- Create: `test/aozora-parser.test.ts`
- Create: `test-fixtures/sample-aozora.txt`

- [ ] **Step 1: テスト用青空文庫テキスト作成**

`test-fixtures/sample-aozora.txt` に保存（UTF-8）:

```
タイトル
著者名

-------------------------------------------------------
【テキスト中に現れる記号について】

《》：ルビ
（例）羅生門《らしょうもん》

［＃］：入力者注　主に外字の注記に使用
-------------------------------------------------------

［＃３字下げ］第一章　下人の行方［＃「第一章　下人の行方」は大見出し］

　ある日の暮方の事である。一人の下人《げにん》が、羅生門《らしょうもん》の下で雨やみを待っていた。

　下人は、大きな嚏《くさめ》をして、それから、大儀《たいぎ》そうに立上った。

［＃改ページ］

［＃３字下げ］第二章　老婆の話［＃「第二章　老婆の話」は大見出し］

　それは、「吉備津《きびつ》の釜《かま》」の事であった。

　「おい。どこへ行く。」と下人は、老婆の行手を「ふさいで」、こう云った。老婆は、それでも下人をつきのけて行こう、とする。下人はまた、それを行かすまいとして、押しもどす。二人は死骸《しがい》の中で、暫、無言のまま、つかみ合った。

底本：「芥川龍之介全集１」ちくま文庫、筑摩書房
```

- [ ] **Step 2: テスト作成**

```typescript
// test/aozora-parser.test.ts
import { describe, it, expect } from "vitest";
import { parseAozoraText } from "../lib/aozora-parser.js";
import { readFileSync } from "fs";
import { resolve } from "path";

describe("parseAozoraText", () => {
  const sampleText = readFileSync(
    resolve(__dirname, "../test-fixtures/sample-aozora.txt"),
    "utf-8"
  );

  it("ヘッダー（記号説明ブロック）を除去する", () => {
    const nodes = parseAozoraText(sampleText);
    const textContent = nodes
      .filter((n) => n.type === "text")
      .map((n) => n.content)
      .join("");
    expect(textContent).not.toContain("【テキスト中に現れる記号について】");
    expect(textContent).not.toContain("-------");
  });

  it("ルビを正しくパースする", () => {
    const nodes = parseAozoraText(sampleText);
    const rubyNodes = nodes.filter((n) => n.type === "ruby");
    expect(rubyNodes.length).toBeGreaterThan(0);
    const rashomon = rubyNodes.find(
      (n) => n.type === "ruby" && n.base === "羅生門"
    );
    expect(rashomon).toBeDefined();
    if (rashomon && rashomon.type === "ruby") {
      expect(rashomon.reading).toBe("らしょうもん");
    }
  });

  it("大見出しをパースする", () => {
    const nodes = parseAozoraText(sampleText);
    const headings = nodes.filter((n) => n.type === "heading");
    expect(headings.length).toBe(2);
    if (headings[0].type === "heading") {
      expect(headings[0].level).toBe(1);
    }
  });

  it("字下げをパースする", () => {
    const nodes = parseAozoraText(sampleText);
    const indents = nodes.filter((n) => n.type === "indent");
    expect(indents.length).toBeGreaterThan(0);
    if (indents[0].type === "indent") {
      expect(indents[0].chars).toBe(3);
    }
  });

  it("改ページをパースする", () => {
    const nodes = parseAozoraText(sampleText);
    const pagebreaks = nodes.filter((n) => n.type === "pagebreak");
    expect(pagebreaks.length).toBe(1);
  });

  it("底本注記を保持する", () => {
    const nodes = parseAozoraText(sampleText);
    const textNodes = nodes.filter((n) => n.type === "text");
    const hasColophon = textNodes.some(
      (n) => n.type === "text" && n.content.includes("底本")
    );
    expect(hasColophon).toBe(true);
  });
});
```

- [ ] **Step 3: テスト実行して失敗を確認**

```bash
npx vitest run test/aozora-parser.test.ts
```

Expected: FAIL

- [ ] **Step 4: aozora-parser.ts実装**

```typescript
// lib/aozora-parser.ts
import type { AozoraNode } from "./types.js";

/**
 * 青空文庫テキストをパースしてAozoraNode列に変換する。
 * Phase 1対応: ルビ、見出し、字下げ、傍点、改ページ、底本注記
 */
export function parseAozoraText(text: string): AozoraNode[] {
  // ヘッダー（記号説明ブロック）を除去
  const headerPattern =
    /-{5,}\n【テキスト中に現れる記号について】[\s\S]*?-{5,}\n/;
  let body = text.replace(headerPattern, "");

  // タイトル行（最初の数行）を除去 — 空行までがヘッダー
  const firstBlankLine = body.indexOf("\n\n");
  if (firstBlankLine !== -1 && firstBlankLine < 200) {
    body = body.substring(firstBlankLine + 2);
  }

  const lines = body.split("\n");
  const nodes: AozoraNode[] = [];

  for (const line of lines) {
    if (line.trim() === "") {
      nodes.push({ type: "newline" });
      continue;
    }

    // 改ページ
    if (line.includes("［＃改ページ］")) {
      nodes.push({ type: "pagebreak" });
      continue;
    }

    const lineNodes = parseLine(line);
    nodes.push(...lineNodes);
    nodes.push({ type: "newline" });
  }

  return nodes;
}

function parseLine(line: string): AozoraNode[] {
  const nodes: AozoraNode[] = [];

  // 字下げ検出
  const indentMatch = line.match(/^［＃(\d+)字下げ］(.*)$/);
  if (indentMatch) {
    const chars = parseInt(indentMatch[1], 10);
    const rest = indentMatch[2];
    const children = parseInline(rest);
    nodes.push({ type: "indent", chars, children });
    return nodes;
  }

  // 行全体をインラインパース
  const inlineNodes = parseInline(line);
  nodes.push(...inlineNodes);
  return nodes;
}

function parseInline(text: string): AozoraNode[] {
  const nodes: AozoraNode[] = [];
  let remaining = text;

  while (remaining.length > 0) {
    // 大見出し注記（行末）: ［＃「...」は大見出し］
    const headingEndMatch = remaining.match(
      /［＃「([^」]+)」は(大|中|小)見出し］/
    );
    if (headingEndMatch) {
      const beforeHeading = remaining.substring(
        0,
        headingEndMatch.index!
      );
      if (beforeHeading) {
        // 見出しテキスト部分からルビ等をパース
        const headingText = headingEndMatch[1];
        const level =
          headingEndMatch[2] === "大"
            ? 1
            : headingEndMatch[2] === "中"
              ? 2
              : 3;

        // beforeHeading内に見出しテキストが含まれている
        // 見出しテキストの前の部分を通常テキストとしてパース
        const headingStart = beforeHeading.indexOf(headingText);
        if (headingStart >= 0) {
          const preText = beforeHeading.substring(0, headingStart);
          if (preText) {
            nodes.push(...parseRubyAndEmphasis(preText));
          }
          const headingContent = parseRubyAndEmphasis(headingText);
          nodes.push({ type: "heading", level, children: headingContent });
        } else {
          nodes.push(...parseRubyAndEmphasis(beforeHeading));
        }
      }

      remaining = remaining.substring(
        headingEndMatch.index! + headingEndMatch[0].length
      );
      continue;
    }

    // 残りはルビ・傍点パース
    nodes.push(...parseRubyAndEmphasis(remaining));
    break;
  }

  return nodes;
}

function parseRubyAndEmphasis(text: string): AozoraNode[] {
  const nodes: AozoraNode[] = [];
  // ルビパターン: 漢字《かな》
  // 傍点パターン: ［＃「...」に傍点］
  const pattern = /([一-龥々〇ヶ]+)《([^》]+)》|［＃「([^」]+)」に傍点］/g;

  let lastIndex = 0;
  let match: RegExpExecArray | null;

  while ((match = pattern.exec(text)) !== null) {
    // マッチ前のテキスト
    if (match.index > lastIndex) {
      nodes.push({ type: "text", content: text.substring(lastIndex, match.index) });
    }

    if (match[1] && match[2]) {
      // ルビ
      nodes.push({ type: "ruby", base: match[1], reading: match[2] });
    } else if (match[3]) {
      // 傍点
      const emphChildren: AozoraNode[] = [
        { type: "text", content: match[3] },
      ];
      nodes.push({ type: "emphasis", style: "sesame", children: emphChildren });
    }

    lastIndex = match.index + match[0].length;
  }

  // 残りのテキスト
  if (lastIndex < text.length) {
    nodes.push({ type: "text", content: text.substring(lastIndex) });
  }

  return nodes;
}
```

- [ ] **Step 5: テスト実行してパスを確認**

```bash
npx vitest run test/aozora-parser.test.ts
```

Expected: ALL PASS

- [ ] **Step 6: コミット**

```bash
git add lib/aozora-parser.ts test/aozora-parser.test.ts test-fixtures/sample-aozora.txt
git commit -m "✨ 青空文庫注記パーサーとテストを追加（ルビ、見出し、字下げ、傍点、改ページ）"
```

---

### Task 5: 章分割ロジック

**Files:**
- Create: `lib/chapter-splitter.ts`
- Create: `test/chapter-splitter.test.ts`

- [ ] **Step 1: テスト作成**

```typescript
// test/chapter-splitter.test.ts
import { describe, it, expect } from "vitest";
import { splitChapters } from "../lib/chapter-splitter.js";
import type { AozoraNode } from "../lib/types.js";

describe("splitChapters", () => {
  it("見出しで章分割する", () => {
    const nodes: AozoraNode[] = [
      { type: "text", content: "序文テキスト" },
      { type: "newline" },
      { type: "heading", level: 1, children: [{ type: "text", content: "第一章" }] },
      { type: "newline" },
      { type: "text", content: "第一章の内容" },
      { type: "newline" },
      { type: "heading", level: 1, children: [{ type: "text", content: "第二章" }] },
      { type: "newline" },
      { type: "text", content: "第二章の内容" },
    ];

    const chapters = splitChapters(nodes);
    expect(chapters).toHaveLength(3);
    expect(chapters[0].title).toBe("");
    expect(chapters[1].title).toBe("第一章");
    expect(chapters[2].title).toBe("第二章");
  });

  it("改ページで章分割する", () => {
    const nodes: AozoraNode[] = [
      { type: "text", content: "前半" },
      { type: "pagebreak" },
      { type: "text", content: "後半" },
    ];

    const chapters = splitChapters(nodes);
    expect(chapters).toHaveLength(2);
  });

  it("章区切りが0個なら単一チャプター", () => {
    const nodes: AozoraNode[] = [
      { type: "text", content: "全テキスト" },
    ];

    const chapters = splitChapters(nodes);
    expect(chapters).toHaveLength(1);
    expect(chapters[0].title).toBe("");
  });

  it("空の序文チャプターは除外する", () => {
    const nodes: AozoraNode[] = [
      { type: "heading", level: 1, children: [{ type: "text", content: "第一章" }] },
      { type: "text", content: "内容" },
    ];

    const chapters = splitChapters(nodes);
    expect(chapters).toHaveLength(1);
    expect(chapters[0].title).toBe("第一章");
  });
});
```

- [ ] **Step 2: テスト実行して失敗を確認**

```bash
npx vitest run test/chapter-splitter.test.ts
```

Expected: FAIL

- [ ] **Step 3: chapter-splitter.ts実装**

```typescript
// lib/chapter-splitter.ts
import type { AozoraNode, Chapter } from "./types.js";

function extractHeadingText(children: AozoraNode[]): string {
  return children
    .map((n) => {
      if (n.type === "text") return n.content;
      if (n.type === "ruby") return n.base;
      return "";
    })
    .join("");
}

function hasContent(nodes: AozoraNode[]): boolean {
  return nodes.some(
    (n) =>
      (n.type === "text" && n.content.trim().length > 0) ||
      n.type === "ruby" ||
      n.type === "emphasis"
  );
}

export function splitChapters(nodes: AozoraNode[]): Chapter[] {
  const chapters: Chapter[] = [];
  let currentNodes: AozoraNode[] = [];
  let currentTitle = "";

  for (const node of nodes) {
    if (node.type === "heading") {
      // 現在のチャプターを保存（内容があれば）
      if (hasContent(currentNodes)) {
        chapters.push({ title: currentTitle, nodes: currentNodes });
      }
      currentTitle = extractHeadingText(node.children);
      currentNodes = [node];
      continue;
    }

    if (node.type === "pagebreak") {
      if (hasContent(currentNodes)) {
        chapters.push({ title: currentTitle, nodes: currentNodes });
      }
      currentTitle = "";
      currentNodes = [];
      continue;
    }

    currentNodes.push(node);
  }

  // 最後のチャプター
  if (hasContent(currentNodes)) {
    chapters.push({ title: currentTitle, nodes: currentNodes });
  }

  return chapters;
}
```

- [ ] **Step 4: テスト実行してパスを確認**

```bash
npx vitest run test/chapter-splitter.test.ts
```

Expected: ALL PASS

- [ ] **Step 5: コミット**

```bash
git add lib/chapter-splitter.ts test/chapter-splitter.test.ts
git commit -m "✨ 見出し・改ページベースの章分割ロジックとテストを追加"
```

---

### Task 6: EPUB3ビルダー

**Files:**
- Create: `lib/epub-builder.ts`
- Create: `test/epub-builder.test.ts`

- [ ] **Step 1: テスト作成**

```typescript
// test/epub-builder.test.ts
import { describe, it, expect } from "vitest";
import { buildEpub } from "../lib/epub-builder.js";
import type { Chapter, AozoraNode } from "../lib/types.js";
import * as AdmZip from "adm-zip"; // テスト用のみ

// adm-zipはdevDependenciesに追加: npm install -D adm-zip @types/adm-zip

describe("buildEpub", () => {
  const chapters: Chapter[] = [
    {
      title: "第一章",
      nodes: [
        { type: "text", content: "テスト" } as AozoraNode,
        { type: "ruby", base: "漢字", reading: "かんじ" } as AozoraNode,
      ],
    },
    {
      title: "第二章",
      nodes: [{ type: "text", content: "内容" } as AozoraNode],
    },
  ];

  it("有効なEPUB ZIPを生成する", async () => {
    const buffer = await buildEpub({
      title: "テスト書籍",
      author: "テスト著者",
      chapters,
    });

    expect(buffer).toBeInstanceOf(Buffer);
    expect(buffer.length).toBeGreaterThan(0);

    // ZIP構造を検証
    const zip = new AdmZip.default(buffer);
    const entries = zip.getEntries().map((e) => e.entryName);

    expect(entries).toContain("mimetype");
    expect(entries).toContain("META-INF/container.xml");
    expect(entries).toContain("OEBPS/content.opf");
    expect(entries).toContain("OEBPS/nav.xhtml");
    expect(entries).toContain("OEBPS/style.css");
    expect(entries).toContain("OEBPS/chapter_001.xhtml");
    expect(entries).toContain("OEBPS/chapter_002.xhtml");
  });

  it("mimetypeが非圧縮で先頭にある", async () => {
    const buffer = await buildEpub({
      title: "テスト",
      author: "著者",
      chapters: [{ title: "", nodes: [{ type: "text", content: "a" }] }],
    });

    // mimetypeの内容確認
    const zip = new AdmZip.default(buffer);
    const mimetype = zip.getEntry("mimetype");
    expect(mimetype).not.toBeNull();
    expect(mimetype!.getData().toString()).toBe("application/epub+zip");
  });

  it("XHTMLにルビタグが含まれる", async () => {
    const buffer = await buildEpub({
      title: "テスト",
      author: "著者",
      chapters,
    });

    const zip = new AdmZip.default(buffer);
    const ch1 = zip.getEntry("OEBPS/chapter_001.xhtml");
    const content = ch1!.getData().toString("utf-8");
    expect(content).toContain("<ruby>漢字<rt>かんじ</rt></ruby>");
  });

  it("nav.xhtmlに目次が含まれる", async () => {
    const buffer = await buildEpub({
      title: "テスト",
      author: "著者",
      chapters,
    });

    const zip = new AdmZip.default(buffer);
    const nav = zip.getEntry("OEBPS/nav.xhtml");
    const content = nav!.getData().toString("utf-8");
    expect(content).toContain("第一章");
    expect(content).toContain("第二章");
  });
});
```

- [ ] **Step 2: adm-zipのdevDependency追加**

```bash
npm install -D adm-zip @types/adm-zip
```

- [ ] **Step 3: テスト実行して失敗を確認**

```bash
npx vitest run test/epub-builder.test.ts
```

Expected: FAIL

- [ ] **Step 4: epub-builder.ts実装**

```typescript
// lib/epub-builder.ts
import type { AozoraNode, Chapter } from "./types.js";
import archiver from "archiver";
import { PassThrough } from "stream";

interface EpubOptions {
  title: string;
  author: string;
  chapters: Chapter[];
}

function escapeXml(text: string): string {
  return text
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function nodesToXhtml(nodes: AozoraNode[]): string {
  return nodes.map(nodeToXhtml).join("");
}

function nodeToXhtml(node: AozoraNode): string {
  switch (node.type) {
    case "text":
      return escapeXml(node.content);
    case "ruby":
      return `<ruby>${escapeXml(node.base)}<rt>${escapeXml(node.reading)}</rt></ruby>`;
    case "heading":
      return `<h${node.level}>${nodesToXhtml(node.children)}</h${node.level}>`;
    case "heading_end":
      return "";
    case "indent":
      return `<p style="text-indent: ${node.chars}em">${nodesToXhtml(node.children)}</p>`;
    case "emphasis":
      return `<em class="${node.style}">${nodesToXhtml(node.children)}</em>`;
    case "pagebreak":
      return "";
    case "newline":
      return "<br/>\n";
  }
}

function generateChapterXhtml(
  chapter: Chapter,
  title: string
): string {
  return `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja" lang="ja">
<head>
  <meta charset="UTF-8"/>
  <title>${escapeXml(chapter.title || title)}</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
${nodesToXhtml(chapter.nodes)}
</body>
</html>`;
}

function generateContentOpf(
  title: string,
  author: string,
  chapterCount: number
): string {
  const items = Array.from({ length: chapterCount }, (_, i) => {
    const id = `chapter_${String(i + 1).padStart(3, "0")}`;
    return `    <item id="${id}" href="${id}.xhtml" media-type="application/xhtml+xml"/>`;
  }).join("\n");

  const spine = Array.from({ length: chapterCount }, (_, i) => {
    const id = `chapter_${String(i + 1).padStart(3, "0")}`;
    return `    <itemref idref="${id}"/>`;
  }).join("\n");

  return `<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:aozora:${Date.now()}</dc:identifier>
    <dc:title>${escapeXml(title)}</dc:title>
    <dc:creator>${escapeXml(author)}</dc:creator>
    <dc:language>ja</dc:language>
    <meta property="dcterms:modified">${new Date().toISOString().replace(/\.\d+Z$/, "Z")}</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="style" href="style.css" media-type="text/css"/>
${items}
  </manifest>
  <spine>
${spine}
  </spine>
</package>`;
}

function generateNav(chapters: Chapter[]): string {
  const toc = chapters
    .map((ch, i) => {
      const id = `chapter_${String(i + 1).padStart(3, "0")}`;
      const label = ch.title || `セクション ${i + 1}`;
      return `      <li><a href="${id}.xhtml">${escapeXml(label)}</a></li>`;
    })
    .join("\n");

  return `<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja" lang="ja">
<head>
  <meta charset="UTF-8"/>
  <title>目次</title>
</head>
<body>
  <nav epub:type="toc">
    <h1>目次</h1>
    <ol>
${toc}
    </ol>
  </nav>
</body>
</html>`;
}

const CONTAINER_XML = `<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>`;

const STYLE_CSS = `body {
  font-family: serif;
  line-height: 1.8;
  margin: 1em;
}
h1, h2, h3 { margin: 1.5em 0 0.5em; }
em.sesame {
  text-emphasis-style: sesame;
  -webkit-text-emphasis-style: sesame;
  font-style: normal;
}
ruby rt { font-size: 0.6em; }
`;

export async function buildEpub(options: EpubOptions): Promise<Buffer> {
  const { title, author, chapters } = options;

  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    const passthrough = new PassThrough();
    passthrough.on("data", (chunk: Buffer) => chunks.push(chunk));
    passthrough.on("end", () => resolve(Buffer.concat(chunks)));
    passthrough.on("error", reject);

    const archive = archiver("zip", { zlib: { level: 0 } });
    archive.pipe(passthrough);

    // mimetype must be first and uncompressed
    archive.append("application/epub+zip", {
      name: "mimetype",
      store: true,
    });

    // Switch to deflate for remaining files
    archive.append(CONTAINER_XML, { name: "META-INF/container.xml" });
    archive.append(generateContentOpf(title, author, chapters.length), {
      name: "OEBPS/content.opf",
    });
    archive.append(generateNav(chapters), { name: "OEBPS/nav.xhtml" });
    archive.append(STYLE_CSS, { name: "OEBPS/style.css" });

    for (let i = 0; i < chapters.length; i++) {
      const id = `chapter_${String(i + 1).padStart(3, "0")}`;
      const xhtml = generateChapterXhtml(chapters[i], title);
      archive.append(xhtml, { name: `OEBPS/${id}.xhtml` });
    }

    archive.finalize();
  });
}
```

- [ ] **Step 5: テスト実行してパスを確認**

```bash
npx vitest run test/epub-builder.test.ts
```

Expected: ALL PASS

- [ ] **Step 6: コミット**

```bash
git add lib/epub-builder.ts test/epub-builder.test.ts
git commit -m "✨ EPUB3ビルダーとテストを追加（ZIP生成、XHTML、目次、スタイル）"
```

---

### Task 7: Authors APIエンドポイント

**Files:**
- Create: `api/authors.ts`
- Create: `test/api-authors.test.ts`
- Create: `lib/index-loader.ts` (CSVフェッチ＋キャッシュ用シングルトン)

- [ ] **Step 1: テスト作成**

```typescript
// test/api-authors.test.ts
import { describe, it, expect, vi, beforeEach } from "vitest";
import type { VercelRequest, VercelResponse } from "@vercel/node";

// index-loaderをモック
vi.mock("../lib/index-loader.js", () => {
  const { AozoraIndex } = require("../lib/aozora-index.js");
  const { readFileSync } = require("fs");
  const { resolve } = require("path");
  const csv = readFileSync(
    resolve(__dirname, "../test-fixtures/sample-index.csv"),
    "utf-8"
  );
  const index = AozoraIndex.fromCsv(csv);
  return { getIndex: vi.fn().mockResolvedValue(index) };
});

import handler from "../api/authors.js";

function mockReqRes(query: Record<string, string>) {
  const req = { query } as unknown as VercelRequest;
  const res = {
    status: vi.fn().mockReturnThis(),
    json: vi.fn().mockReturnThis(),
    setHeader: vi.fn().mockReturnThis(),
  } as unknown as VercelResponse;
  return { req, res };
}

describe("GET /api/authors", () => {
  it("kana_prefix=ア で作家を返す", async () => {
    const { req, res } = mockReqRes({ kana_prefix: "ア" });
    await handler(req, res);
    expect(res.json).toHaveBeenCalledWith(
      expect.objectContaining({
        authors: expect.arrayContaining([
          expect.objectContaining({ name: "芥川龍之介" }),
        ]),
      })
    );
  });

  it("kana_prefix未指定で400エラー", async () => {
    const { req, res } = mockReqRes({});
    await handler(req, res);
    expect(res.status).toHaveBeenCalledWith(400);
  });
});
```

- [ ] **Step 2: index-loader.ts作成**

```typescript
// lib/index-loader.ts
import { AozoraIndex } from "./aozora-index.js";

const AOZORA_CSV_URL =
  "https://www.aozora.gr.jp/index_pages/list_person_all_extended_utf8.zip";

let cachedIndex: AozoraIndex | null = null;

export async function getIndex(): Promise<AozoraIndex> {
  if (cachedIndex) return cachedIndex;

  const response = await fetch(AOZORA_CSV_URL);
  if (!response.ok) {
    throw new Error(`Failed to fetch index: ${response.status}`);
  }

  const arrayBuffer = await response.arrayBuffer();
  const buffer = Buffer.from(arrayBuffer);

  // ZIPを展開してCSVを取得
  const AdmZip = (await import("adm-zip")).default;
  const zip = new AdmZip(buffer);
  const entries = zip.getEntries();
  const csvEntry = entries.find((e) => e.entryName.endsWith(".csv"));
  if (!csvEntry) throw new Error("CSV not found in ZIP");

  const csvContent = csvEntry.getData().toString("utf-8");
  cachedIndex = AozoraIndex.fromCsv(csvContent);
  return cachedIndex;
}
```

- [ ] **Step 3: api/authors.ts作成**

```typescript
// api/authors.ts
import type { VercelRequest, VercelResponse } from "@vercel/node";
import { getIndex } from "../lib/index-loader.js";

export default async function handler(
  req: VercelRequest,
  res: VercelResponse
) {
  const kanaPrefix = req.query.kana_prefix as string | undefined;

  if (!kanaPrefix) {
    return res.status(400).json({
      error: "INVALID_PARAMS",
      message: "kana_prefix パラメータが必要です",
    });
  }

  try {
    const index = await getIndex();
    const authors = index.getAuthorsByKanaPrefix(kanaPrefix);

    return res.json({
      authors: authors.map((a) => ({
        id: a.id,
        name: a.name,
        kana: a.kana,
        work_count: a.workCount,
      })),
    });
  } catch (err) {
    return res.status(500).json({
      error: "INTERNAL_ERROR",
      message: "インデックスの読み込みに失敗しました",
    });
  }
}
```

- [ ] **Step 4: テスト実行してパスを確認**

```bash
npx vitest run test/api-authors.test.ts
```

Expected: ALL PASS

- [ ] **Step 5: コミット**

```bash
git add api/authors.ts lib/index-loader.ts test/api-authors.test.ts
git commit -m "✨ /api/authors エンドポイントとCSVフェッチローダーを追加"
```

---

### Task 8: Works APIエンドポイント

**Files:**
- Create: `api/works.ts`
- Create: `test/api-works.test.ts`

- [ ] **Step 1: テスト作成**

```typescript
// test/api-works.test.ts
import { describe, it, expect, vi } from "vitest";
import type { VercelRequest, VercelResponse } from "@vercel/node";

vi.mock("../lib/index-loader.js", () => {
  const { AozoraIndex } = require("../lib/aozora-index.js");
  const { readFileSync } = require("fs");
  const { resolve } = require("path");
  const csv = readFileSync(
    resolve(__dirname, "../test-fixtures/sample-index.csv"),
    "utf-8"
  );
  const index = AozoraIndex.fromCsv(csv);
  return { getIndex: vi.fn().mockResolvedValue(index) };
});

import handler from "../api/works.js";

function mockReqRes(query: Record<string, string>) {
  const req = { query } as unknown as VercelRequest;
  const res = {
    status: vi.fn().mockReturnThis(),
    json: vi.fn().mockReturnThis(),
    setHeader: vi.fn().mockReturnThis(),
  } as unknown as VercelResponse;
  return { req, res };
}

describe("GET /api/works", () => {
  it("author_idで作品を返す", async () => {
    const { req, res } = mockReqRes({ author_id: "879" });
    await handler(req, res);
    expect(res.json).toHaveBeenCalledWith(
      expect.objectContaining({
        works: expect.arrayContaining([
          expect.objectContaining({ title: "羅生門" }),
        ]),
      })
    );
  });

  it("kana_prefixで作品名検索", async () => {
    const { req, res } = mockReqRes({ kana_prefix: "ハ" });
    await handler(req, res);
    const call = (res.json as any).mock.calls[0][0];
    expect(call.works.length).toBeGreaterThan(0);
  });

  it("ndcでジャンル検索", async () => {
    const { req, res } = mockReqRes({ ndc: "913" });
    await handler(req, res);
    const call = (res.json as any).mock.calls[0][0];
    expect(call.works.length).toBe(5);
  });

  it("sort=newestで新着順", async () => {
    const { req, res } = mockReqRes({ sort: "newest", limit: "2" });
    await handler(req, res);
    const call = (res.json as any).mock.calls[0][0];
    expect(call.works.length).toBe(2);
  });

  it("パラメータ未指定で400エラー", async () => {
    const { req, res } = mockReqRes({});
    await handler(req, res);
    expect(res.status).toHaveBeenCalledWith(400);
  });
});
```

- [ ] **Step 2: api/works.ts作成**

```typescript
// api/works.ts
import type { VercelRequest, VercelResponse } from "@vercel/node";
import { getIndex } from "../lib/index-loader.js";

export default async function handler(
  req: VercelRequest,
  res: VercelResponse
) {
  const authorId = req.query.author_id as string | undefined;
  const kanaPrefix = req.query.kana_prefix as string | undefined;
  const ndc = req.query.ndc as string | undefined;
  const sort = req.query.sort as string | undefined;
  const limit = parseInt((req.query.limit as string) || "50", 10);

  if (!authorId && !kanaPrefix && !ndc && sort !== "newest") {
    return res.status(400).json({
      error: "INVALID_PARAMS",
      message:
        "author_id, kana_prefix, ndc, または sort=newest のいずれかが必要です",
    });
  }

  try {
    const index = await getIndex();
    let works;

    if (authorId) {
      works = index.getWorksByAuthorId(parseInt(authorId, 10));
    } else if (kanaPrefix) {
      works = index.getWorksByTitleKanaPrefix(kanaPrefix);
    } else if (ndc) {
      works = index.getWorksByNdc(ndc);
    } else {
      works = index.getNewestWorks(limit);
    }

    return res.json({
      works: works.map((w) => ({
        id: w.id,
        title: w.title,
        kana: w.titleKana,
        ndc: w.ndc,
      })),
    });
  } catch (err) {
    return res.status(500).json({
      error: "INTERNAL_ERROR",
      message: "インデックスの読み込みに失敗しました",
    });
  }
}
```

- [ ] **Step 3: テスト実行してパスを確認**

```bash
npx vitest run test/api-works.test.ts
```

Expected: ALL PASS

- [ ] **Step 4: コミット**

```bash
git add api/works.ts test/api-works.test.ts
git commit -m "✨ /api/works エンドポイントを追加（作家ID/作品名/ジャンル/新着検索）"
```

---

### Task 9: Convert APIエンドポイント

**Files:**
- Create: `api/convert.ts`
- Create: `test/api-convert.test.ts`

- [ ] **Step 1: テスト作成**

```typescript
// test/api-convert.test.ts
import { describe, it, expect, vi } from "vitest";
import type { VercelRequest, VercelResponse } from "@vercel/node";

// index-loaderモック
vi.mock("../lib/index-loader.js", () => {
  const { AozoraIndex } = require("../lib/aozora-index.js");
  const { readFileSync } = require("fs");
  const { resolve } = require("path");
  const csv = readFileSync(
    resolve(__dirname, "../test-fixtures/sample-index.csv"),
    "utf-8"
  );
  const index = AozoraIndex.fromCsv(csv);
  return { getIndex: vi.fn().mockResolvedValue(index) };
});

// fetch（青空文庫ZIP取得）をモック
const sampleText = `テスト書籍
芥川龍之介

-------------------------------------------------------
【テキスト中に現れる記号について】
《》：ルビ
-------------------------------------------------------

　むかし、一人の男《おとこ》がいた。

底本：「テスト」テスト出版`;

vi.mock("../lib/aozora-fetcher.js", () => ({
  fetchAozoraText: vi.fn().mockResolvedValue(sampleText),
}));

import handler from "../api/convert.js";

describe("GET /api/convert", () => {
  it("work_idを指定するとEPUBバイナリを返す", async () => {
    const req = { query: { work_id: "879" } } as unknown as VercelRequest;
    const chunks: Buffer[] = [];
    const res = {
      status: vi.fn().mockReturnThis(),
      json: vi.fn().mockReturnThis(),
      setHeader: vi.fn().mockReturnThis(),
      write: vi.fn((chunk: Buffer) => chunks.push(chunk)),
      end: vi.fn((chunk?: Buffer) => {
        if (chunk) chunks.push(chunk);
      }),
    } as unknown as VercelResponse;

    await handler(req, res);

    expect(res.setHeader).toHaveBeenCalledWith(
      "Content-Type",
      "application/epub+zip"
    );
    expect(res.setHeader).toHaveBeenCalledWith(
      "Content-Disposition",
      expect.stringContaining(".epub")
    );
  });

  it("work_id未指定で400エラー", async () => {
    const req = { query: {} } as unknown as VercelRequest;
    const res = {
      status: vi.fn().mockReturnThis(),
      json: vi.fn().mockReturnThis(),
      setHeader: vi.fn().mockReturnThis(),
    } as unknown as VercelResponse;

    await handler(req, res);
    expect(res.status).toHaveBeenCalledWith(400);
  });

  it("存在しないwork_idで404エラー", async () => {
    const req = {
      query: { work_id: "999999" },
    } as unknown as VercelRequest;
    const res = {
      status: vi.fn().mockReturnThis(),
      json: vi.fn().mockReturnThis(),
      setHeader: vi.fn().mockReturnThis(),
    } as unknown as VercelResponse;

    await handler(req, res);
    expect(res.status).toHaveBeenCalledWith(404);
  });
});
```

- [ ] **Step 2: aozora-fetcher.ts作成（青空文庫からのテキスト取得）**

```typescript
// lib/aozora-fetcher.ts
import * as iconv from "iconv-lite";

/**
 * 青空文庫のZIP URLからテキストを取得しUTF-8に変換する
 */
export async function fetchAozoraText(zipUrl: string): Promise<string> {
  const response = await fetch(zipUrl);
  if (!response.ok) {
    throw new Error(`Failed to fetch: ${response.status} ${zipUrl}`);
  }

  const arrayBuffer = await response.arrayBuffer();
  const buffer = Buffer.from(arrayBuffer);

  const AdmZip = (await import("adm-zip")).default;
  const zip = new AdmZip(buffer);
  const entries = zip.getEntries();

  const txtEntry = entries.find((e) => e.entryName.endsWith(".txt"));
  if (!txtEntry) {
    throw new Error("TXT file not found in ZIP");
  }

  const rawBuffer = txtEntry.getData();

  // Shift_JIS → UTF-8 変換
  return iconv.decode(rawBuffer, "Shift_JIS");
}
```

- [ ] **Step 3: api/convert.ts作成**

```typescript
// api/convert.ts
import type { VercelRequest, VercelResponse } from "@vercel/node";
import { getIndex } from "../lib/index-loader.js";
import { fetchAozoraText } from "../lib/aozora-fetcher.js";
import { parseAozoraText } from "../lib/aozora-parser.js";
import { splitChapters } from "../lib/chapter-splitter.js";
import { buildEpub } from "../lib/epub-builder.js";

export default async function handler(
  req: VercelRequest,
  res: VercelResponse
) {
  const workIdStr = req.query.work_id as string | undefined;

  if (!workIdStr) {
    return res.status(400).json({
      error: "INVALID_PARAMS",
      message: "work_id パラメータが必要です",
    });
  }

  const workId = parseInt(workIdStr, 10);

  try {
    const index = await getIndex();
    const work = index.getWorkById(workId);

    if (!work) {
      return res.status(404).json({
        error: "WORK_NOT_FOUND",
        message: "指定された作品が見つかりません",
      });
    }

    // 青空文庫からテキスト取得
    let text: string;
    try {
      text = await fetchAozoraText(work.textUrl);
    } catch {
      return res.status(502).json({
        error: "AOZORA_FETCH_FAILED",
        message: "青空文庫サーバーからの取得に失敗しました",
      });
    }

    // パース → 章分割 → EPUB生成
    const nodes = parseAozoraText(text);
    const chapters = splitChapters(nodes);
    const epubBuffer = await buildEpub({
      title: work.title,
      author: work.authorName,
      chapters,
    });

    // ファイル名生成（ASCII安全なフォールバック付き）
    const safeTitle = work.title.replace(/[<>:"/\\|?*]/g, "_").substring(0, 50);
    const filename = `${work.id}_${safeTitle}.epub`;

    res.setHeader("Content-Type", "application/epub+zip");
    res.setHeader(
      "Content-Disposition",
      `attachment; filename="${filename}"`
    );
    res.setHeader("Content-Length", epubBuffer.length.toString());
    return res.end(epubBuffer);
  } catch (err) {
    return res.status(500).json({
      error: "CONVERSION_FAILED",
      message: "EPUB変換中にエラーが発生しました",
    });
  }
}
```

- [ ] **Step 4: テスト実行してパスを確認**

```bash
npx vitest run test/api-convert.test.ts
```

Expected: ALL PASS

- [ ] **Step 5: コミット**

```bash
git add api/convert.ts lib/aozora-fetcher.ts test/api-convert.test.ts
git commit -m "✨ /api/convert エンドポイントを追加（青空文庫TXT取得→EPUB変換→返却）"
```

---

### Task 10: 全テスト実行・Vercelデプロイ

- [ ] **Step 1: 全テスト実行**

```bash
npx vitest run
```

Expected: ALL PASS

- [ ] **Step 2: Vercelにデプロイ**

```bash
npx vercel --prod
```

- [ ] **Step 3: デプロイ後のAPIテスト**

```bash
# 作家一覧
curl "https://aozora-epub-api.vercel.app/api/authors?kana_prefix=ア"

# 作品一覧
curl "https://aozora-epub-api.vercel.app/api/works?author_id=879"

# EPUB変換（ファイル保存）
curl -o test.epub "https://aozora-epub-api.vercel.app/api/convert?work_id=879"
```

- [ ] **Step 4: コミット（デプロイ設定等の微調整があれば）**

```bash
git add -A
git commit -m "👍 Vercelデプロイ設定の調整"
```
