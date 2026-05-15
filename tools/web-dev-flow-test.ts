import { mkdtemp, readdir, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

type FileEntry = {
  name: string;
  size: number;
  isDirectory: boolean;
  isEpub: boolean;
};

type SettingEntry = {
  key: string;
  value: unknown;
};

type WifiEntry = {
  ssid: string;
};

type OpdsEntry = {
  name: string;
};

type FontList = {
  families: Array<{
    name: string;
    sizes: number[];
    files: Array<{ name: string; size: number }>;
  }>;
};

const port = Number(process.env.WEB_TEST_PORT || "3099");
const origin = `http://127.0.0.1:${port}`;
const sdRoot = await mkdtemp(join(tmpdir(), "crosspoint-web-test-"));

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

async function request(path: string, init?: RequestInit): Promise<Response> {
  return await fetch(`${origin}${path}`, init);
}

async function text(path: string, init?: RequestInit): Promise<string> {
  const response = await request(path, init);
  const body = await response.text();
  assert(response.ok, `${init?.method || "GET"} ${path} failed: ${response.status} ${body}`);
  return body;
}

async function json<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await request(path, init);
  const body = await response.text();
  assert(response.ok, `${init?.method || "GET"} ${path} failed: ${response.status} ${body}`);
  return JSON.parse(body) as T;
}

async function expectStatus(path: string, status: number, init?: RequestInit): Promise<void> {
  const response = await request(path, init);
  assert(response.status === status, `${init?.method || "GET"} ${path}: expected ${status}, got ${response.status}`);
}

async function waitForServer(): Promise<void> {
  const deadline = Date.now() + 10_000;
  while (Date.now() < deadline) {
    try {
      const response = await request("/api/status");
      if (response.ok) return;
    } catch {
      // Server is still starting.
    }
    await Bun.sleep(100);
  }
  throw new Error("web dev server did not start");
}

async function form(fields: Record<string, string>): Promise<FormData> {
  const data = new FormData();
  for (const [key, value] of Object.entries(fields)) data.append(key, value);
  return data;
}

async function testPagesAndApis(): Promise<void> {
  assert((await request("/")).ok, "home page should load");
  assert((await request("/files")).ok, "files page should load");
  assert((await request("/fonts")).ok, "fonts page should load");
  assert((await request("/settings")).ok, "settings page should load");
  assert((await request("/app.css")).ok, "app.css should load");
  assert((await request("/favicon.ico")).ok, "favicon.ico should load");

  await text("/upload?path=/", {
    method: "POST",
    body: (() => {
      const data = new FormData();
      data.append("file", new File(["hello"], "book.epub", { type: "application/epub+zip" }));
      return data;
    })(),
  });
  await text("/mkdir", { method: "POST", body: await form({ path: "/", name: "Folder A" }) });
  await text("/rename", { method: "POST", body: await form({ path: "/book.epub", name: "renamed.epub" }) });
  await text("/move", { method: "POST", body: await form({ path: "/renamed.epub", dest: "/Folder A" }) });

  const folderFiles = await json<FileEntry[]>("/api/files?path=/Folder%20A");
  assert(folderFiles.some((entry) => entry.name === "renamed.epub" && entry.isEpub), "moved EPUB should be listed");

  await text("/delete", { method: "POST", body: await form({ path: "/Folder A/renamed.epub" }) });
  await expectStatus("/delete", 400, { method: "POST", body: await form({ paths: "[/" }) });
  await expectStatus("/delete", 400, { method: "POST", body: await form({ paths: "\"/Folder A\"" }) });
  await text("/delete", { method: "POST", body: await form({ path: "/Folder A" }) });
  const rootFiles = await json<FileEntry[]>("/api/files?path=/");
  assert(!rootFiles.some((entry) => entry.name === "Folder A"), "deleted folder should not be listed");

  const missingFiles = await json<FileEntry[]>("/api/files?path=/Missing/Subfolder");
  assert(missingFiles.length === 0, "missing folders should list as empty");
  const rootAfterMissingBrowse = await json<FileEntry[]>("/api/files?path=/");
  assert(!rootAfterMissingBrowse.some((entry) => entry.name === "Missing"), "listing missing folders should not create them");

  await expectStatus("/upload?path=/", 400, {
    method: "POST",
    body: (() => {
      const data = new FormData();
      data.append("file", new File(["bad"], "../outside.epub", { type: "application/epub+zip" }));
      return data;
    })(),
  });
  await expectStatus("/mkdir", 400, { method: "POST", body: await form({ path: "/", name: "../outside" }) });

  await text("/api/settings", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ fontSize: 20, showStatusBar: 0 }),
  });
  const settings = await json<SettingEntry[]>("/api/settings");
  assert(settings.find((setting) => setting.key === "fontSize")?.value === 20, "font size should persist");

  await text("/api/wifi", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ ssid: "Test Net", password: "secret" }),
  });
  let wifi = await json<WifiEntry[]>("/api/wifi");
  assert(wifi.some((entry) => entry.ssid === "Test Net"), "Wi-Fi network should be added");
  await text("/api/wifi/delete", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ index: wifi.findIndex((entry) => entry.ssid === "Test Net") }),
  });
  wifi = await json<WifiEntry[]>("/api/wifi");
  assert(!wifi.some((entry) => entry.ssid === "Test Net"), "Wi-Fi network should be deleted");
  await expectStatus("/api/wifi", 400, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ index: 99, ssid: "Bad Index" }),
  });
  await expectStatus("/api/wifi/delete", 400, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ index: 99 }),
  });

  await text("/api/opds", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ name: "Test OPDS", url: "http://example.test/opds", username: "u", password: "p" }),
  });
  let opds = await json<OpdsEntry[]>("/api/opds");
  assert(opds.some((entry) => entry.name === "Test OPDS"), "OPDS server should be added");
  await text("/api/opds/delete", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ index: opds.findIndex((entry) => entry.name === "Test OPDS") }),
  });
  opds = await json<OpdsEntry[]>("/api/opds");
  assert(!opds.some((entry) => entry.name === "Test OPDS"), "OPDS server should be deleted");
  await expectStatus("/api/opds", 400, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ index: 99, name: "Bad Index", url: "http://example.test/opds" }),
  });
  await expectStatus("/api/opds/delete", 400, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ index: 99 }),
  });

  let fonts = await json<FontList>("/api/fonts");
  assert(Array.isArray(fonts.families), "font list should return families");
  await text("/api/fonts/upload", {
    method: "POST",
    body: (() => {
      const data = new FormData();
      data.append("family", "TestFont");
      data.append("file", new File([new Uint8Array([67, 80, 70, 79, 78, 84, 0, 0, 1])], "TestFont_12.cpfont"));
      return data;
    })(),
  });
  fonts = await json<FontList>("/api/fonts");
  assert(fonts.families.some((family) => family.name === "TestFont" && family.sizes.includes(12)), "font upload should be listed");
  await text("/api/fonts/delete", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ family: "TestFont" }),
  });
  fonts = await json<FontList>("/api/fonts");
  assert(!fonts.families.some((family) => family.name === "TestFont"), "font delete should remove family");
}

async function testWebDav(): Promise<void> {
  await expectStatus("/DAV", 201, { method: "MKCOL" });
  await expectStatus("/DAV/file.txt", 201, { method: "PUT", body: "dav-file" });
  await expectStatus("/DAV/source.txt", 201, { method: "PUT", body: "copy-source" });

  const propfind = await request("/DAV", { method: "PROPFIND", headers: { Depth: "1" } });
  assert(propfind.status === 207, `PROPFIND /DAV expected 207, got ${propfind.status}`);
  assert((await propfind.text()).includes("/DAV/file.txt"), "PROPFIND should include child file");

  await expectStatus("/DAV/file.txt", 201, { method: "MOVE", headers: { Destination: "/DAV/moved.txt" } });
  await expectStatus("/DAV/source.txt", 201, { method: "COPY", headers: { Destination: "/DAV/copied.txt" } });
  assert((await text("/DAV/moved.txt")) === "dav-file", "moved file content should match");
  assert((await text("/DAV/copied.txt")) === "copy-source", "copied file content should match");

  await expectStatus("/DAV/moved.txt", 204, { method: "DELETE" });
  await expectStatus("/DAV/copied.txt", 204, { method: "DELETE" });
  await expectStatus("/DAV/source.txt", 204, { method: "DELETE" });
  await expectStatus("/DAV", 204, { method: "DELETE" });

  await expectStatus("/.secret", 403, { method: "PUT", body: "hidden" });
  await expectStatus("/Visible/.secret", 403, { method: "PUT", body: "hidden" });
  await expectStatus("/api/files?path=/../../tmp", 400);
  await expectStatus("/api/files?path=%E0%A4%A", 400);
}

async function runWebSocketUpload(startMessage: string, chunks: Array<string | Uint8Array> = []): Promise<string[]> {
  const events: string[] = [];
  const ws = new WebSocket(`ws://127.0.0.1:${port}/__crosspoint_ws`);
  const toArrayBuffer = (chunk: string | Uint8Array): ArrayBuffer => {
    const bytes = typeof chunk === "string" ? new TextEncoder().encode(chunk) : chunk;
    return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
  };

  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("WebSocket upload timed out")), 10_000);
    ws.addEventListener("open", () => ws.send(startMessage));
    ws.addEventListener("message", (event) => {
      const message = String(event.data);
      events.push(message);
      if (message === "READY") {
        for (const chunk of chunks) ws.send(toArrayBuffer(chunk));
      }
      if (message === "DONE") {
        clearTimeout(timer);
        ws.close();
        resolve();
      }
      if (message.startsWith("ERROR:")) {
        clearTimeout(timer);
        ws.close();
        resolve();
      }
    });
    ws.addEventListener("error", () => {
      clearTimeout(timer);
      reject(new Error("WebSocket error"));
    });
  });

  return events;
}

async function runSecondStartUpload(): Promise<string[]> {
  const events: string[] = [];
  const ws = new WebSocket(`ws://127.0.0.1:${port}/__crosspoint_ws`);

  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("WebSocket second START test timed out")), 10_000);
    ws.addEventListener("open", () => ws.send("START:first.epub:10:/WS"));
    ws.addEventListener("message", (event) => {
      const message = String(event.data);
      events.push(message);
      if (message === "READY") ws.send("START:second.epub:1:/WS");
      if (message.startsWith("ERROR:")) {
        clearTimeout(timer);
        ws.close();
        resolve();
      }
    });
    ws.addEventListener("error", () => {
      clearTimeout(timer);
      reject(new Error("WebSocket error"));
    });
  });

  await Bun.sleep(100);
  return events;
}

async function testWebSocketPartialCloseCleanup(): Promise<void> {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/__crosspoint_ws`);
  const bytes = new TextEncoder().encode("partial");

  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("WebSocket partial close test timed out")), 10_000);
    ws.addEventListener("open", () => ws.send("START:partial.epub:100:/WS"));
    ws.addEventListener("message", (event) => {
      if (String(event.data) !== "READY") return;
      ws.send(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer);
      ws.close();
      clearTimeout(timer);
      resolve();
    });
    ws.addEventListener("error", () => {
      clearTimeout(timer);
      reject(new Error("WebSocket error"));
    });
  });

  await Bun.sleep(100);
  const files = await json<FileEntry[]>("/api/files?path=/WS");
  assert(!files.some((entry) => entry.name === "partial.epub"), "partial WebSocket close should not create final file");
  const rawEntries = await readdir(join(sdRoot, "WS"));
  assert(!rawEntries.some((entry) => entry.includes("partial.epub")), "partial WebSocket close should remove temp file");
}

async function testWebSocketUpload(): Promise<void> {
  const events = await runWebSocketUpload("START:ws-book.epub:11:/WS", ["hello", " world"]);

  assert(events.includes("READY"), "WebSocket should send READY");
  assert(events.includes("PROGRESS:5:11"), "WebSocket should send partial progress");
  assert(events.includes("PROGRESS:11:11"), "WebSocket should send full progress");
  assert(events.includes("DONE"), "WebSocket should send DONE");

  const files = await json<FileEntry[]>("/api/files?path=/WS");
  assert(files.some((entry) => entry.name === "ws-book.epub" && entry.size === 11), "WebSocket upload should create file");
  assert((await text("/download?path=/WS/ws-book.epub")) === "hello world", "WebSocket upload chunks should be written in order");

  const emptyEvents = await runWebSocketUpload("START:empty.epub:0:/WS");
  assert(emptyEvents.includes("DONE"), "empty WebSocket upload should complete");
  const filesAfterEmpty = await json<FileEntry[]>("/api/files?path=/WS");
  assert(filesAfterEmpty.some((entry) => entry.name === "empty.epub" && entry.size === 0), "empty WebSocket upload should create file");

  await text("/mkdir", { method: "POST", body: await form({ path: "/", name: "WS:Colon" }) });
  const colonPathEvents = await runWebSocketUpload("START:colon-path.epub:5:/WS:Colon", ["colon"]);
  assert(colonPathEvents.includes("DONE"), "WebSocket upload should support colons in destination path");
  const colonPathFiles = await json<FileEntry[]>("/api/files?path=/WS%3AColon");
  assert(colonPathFiles.some((entry) => entry.name === "colon-path.epub" && entry.size === 5), "colon path upload should land in the full destination path");

  const overflowEvents = await runWebSocketUpload("START:overflow.epub:1:/WS", ["too much"]);
  assert(overflowEvents.some((event) => event.startsWith("ERROR:")), "WebSocket upload should reject oversized chunks");
  const filesAfterOverflow = await json<FileEntry[]>("/api/files?path=/WS");
  assert(!filesAfterOverflow.some((entry) => entry.name === "overflow.epub"), "overflow WebSocket upload should not create file");

  const traversalEvents = await runWebSocketUpload("START:../outside.epub:1:/WS", ["x"]);
  assert(traversalEvents.some((event) => event.startsWith("ERROR:")), "WebSocket upload should reject traversal names");

  const secondStartEvents = await runSecondStartUpload();
  assert(secondStartEvents.includes("ERROR:Upload already in progress"), "WebSocket upload should reject a second START");

  await testWebSocketPartialCloseCleanup();
}

const server = Bun.spawn({
  cmd: ["bun", "tools/web-dev-server.ts"],
  cwd: join(import.meta.dir, ".."),
  env: {
    ...process.env,
    WEB_PORT: String(port),
    WEB_HOST: "127.0.0.1",
    CROSSPOINT_SD_ROOT: sdRoot,
  },
  stdout: "pipe",
  stderr: "pipe",
});

try {
  await waitForServer();
  await testPagesAndApis();
  await testWebDav();
  await testWebSocketUpload();
  console.log("web dev flow tests passed");
} finally {
  server.kill();
  await server.exited.catch(() => {});
  await rm(sdRoot, { recursive: true, force: true });
}
