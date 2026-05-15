import { watch } from "node:fs";
import { copyFile, mkdir, open, readdir, readFile, rename, rm, rmdir, stat, writeFile } from "node:fs/promises";
import { dirname, join, relative, resolve } from "node:path";
import type { FileHandle } from "node:fs/promises";

const htmlDir = join(import.meta.dir, "..", "src", "network", "html");
const faviconPath = join(htmlDir, "favicon.ico");
const appCssPath = join(htmlDir, "app.css");
const sdRoot = resolve(process.env.CROSSPOINT_SD_ROOT || join(import.meta.dir, "..", ".crosspoint-web-dev-sd"));
const statePath = join(sdRoot, ".dev-state.json");
const fontsRoot = join(sdRoot, ".crosspoint", "fonts");
const port = Number(process.env.WEB_PORT || "3000");
const host = process.env.WEB_HOST || "127.0.0.1";
const deviceOrigin = process.env.CROSSPOINT_DEVICE ? normalizeDeviceOrigin(process.env.CROSSPOINT_DEVICE) : "";
const deviceWsUrl = deviceOrigin ? createDeviceWsUrl(deviceOrigin) : "";

const reloadClients = new Set<ReadableStreamDefaultController<string>>();
const protectedNames = new Set(["System Volume Information", "XTCache"]);

const routes: Record<string, string> = {
  "/": "HomePage.html",
  "/files": "FilesPage.html",
  "/fonts": "FontsPage.html",
  "/settings": "SettingsPage.html",
};

type DevState = {
  settings: Array<Record<string, unknown>>;
  wifi: Array<Record<string, unknown>>;
  opds: Array<Record<string, unknown>>;
};

type LocalUpload = {
  fileName: string;
  path: string;
  size: number;
  received: number;
  tempPath: string;
  handle: FileHandle;
};

type UploadStart = {
  fileName: string;
  size: number;
  path: string;
};

const defaultState: DevState = {
  settings: [
    { key: "fontSize", name: "Font Size", category: "Reader", type: "value", value: 18, min: 12, max: 36 },
    { key: "lineSpacing", name: "Line Spacing", category: "Reader", type: "value", value: 4, min: 0, max: 20 },
    {
      key: "rotation",
      name: "Screen Rotation",
      category: "Display",
      type: "enum",
      value: 0,
      options: ["Portrait", "Landscape"],
    },
    { key: "showStatusBar", name: "Show Status Bar", category: "Display", type: "toggle", value: 1 },
    { key: "koreaderUser", name: "KOReader Username", category: "KOReader Sync", type: "string", value: "" },
  ],
  wifi: [{ ssid: "Home WiFi", hasPassword: true, isLastConnected: true }],
  opds: [{ name: "Local Calibre", url: "http://calibre.local/opds", username: "", hasPassword: false }],
};

function normalizeDeviceOrigin(value: string): string {
  const withProtocol = /^https?:\/\//.test(value) ? value : `http://${value}`;
  const url = new URL(withProtocol);
  url.pathname = "";
  url.search = "";
  url.hash = "";
  return url.toString().replace(/\/$/, "");
}

function createDeviceWsUrl(origin: string): string {
  const url = new URL(origin);
  const protocol = url.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${url.hostname}:81/`;
}

function contentType(pathname: string): string {
  if (pathname.endsWith(".html")) return "text/html; charset=utf-8";
  if (pathname.endsWith(".js")) return "text/javascript; charset=utf-8";
  if (pathname.endsWith(".css")) return "text/css; charset=utf-8";
  return "application/octet-stream";
}

async function serveLocalHtml(fileName: string): Promise<Response> {
  const html = await readFile(join(htmlDir, fileName), "utf8");
  return new Response(injectDevScript(html), {
    headers: {
      "content-type": "text/html; charset=utf-8",
      "cache-control": "no-store",
    },
  });
}

function injectDevScript(html: string): string {
  const script = `
<script>
(() => {
  const events = new EventSource('/__crosspoint_events');
  events.addEventListener('change', () => location.reload());
  window.addEventListener('beforeunload', () => events.close());
  window.getWsUrl = function() {
    const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return protocol + '//' + location.host + '/__crosspoint_ws';
  };
})();
</script>`;

  const closingHtml = html.toLowerCase().lastIndexOf("</html>");
  if (closingHtml === -1) return `${html}${script}`;
  return `${html.slice(0, closingHtml)}${script}\n${html.slice(closingHtml)}`;
}

function proxyHeaders(request: Request): Headers {
  const headers = new Headers(request.headers);
  headers.delete("host");
  headers.delete("connection");
  headers.delete("upgrade");
  headers.delete("sec-websocket-key");
  headers.delete("sec-websocket-version");
  headers.delete("sec-websocket-extensions");
  return headers;
}

async function proxyHttp(request: Request): Promise<Response> {
  const requestUrl = new URL(request.url);
  const target = new URL(requestUrl.pathname + requestUrl.search, deviceOrigin);

  try {
    return await fetch(target, {
      method: request.method,
      headers: proxyHeaders(request),
      body: request.method === "GET" || request.method === "HEAD" ? undefined : request.body,
      redirect: "manual",
    });
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    return new Response(`CrossPoint device proxy failed for ${target.toString()}\n\n${message}\n`, {
      status: 502,
      headers: { "content-type": "text/plain; charset=utf-8" },
    });
  }
}

function json(data: unknown, init?: ResponseInit): Response {
  return new Response(JSON.stringify(data), {
    ...init,
    headers: {
      "content-type": "application/json; charset=utf-8",
      ...init?.headers,
    },
  });
}

function text(data: string, init?: ResponseInit): Response {
  return new Response(data, {
    ...init,
    headers: {
      "content-type": "text/plain; charset=utf-8",
      ...init?.headers,
    },
  });
}

function toWebPath(input: string | null): string {
  if (!input || input === "/") return "/";
  let value: string;
  try {
    value = decodeURIComponent(input).replaceAll("\\", "/");
  } catch {
    throw new Error("Invalid path encoding");
  }
  if (!value.startsWith("/")) value = `/${value}`;
  value = value.replace(/\/+/g, "/");
  return value.length > 1 && value.endsWith("/") ? value.slice(0, -1) : value;
}

function fsPath(webPath: string): string {
  const normalized = toWebPath(webPath);
  if (hasTraversalSegment(normalized)) throw new Error("Invalid path segment");
  const resolved = resolve(sdRoot, `.${normalized}`);
  const rel = relative(sdRoot, resolved);
  if (rel.startsWith("..") || resolve(rel) === rel) {
    throw new Error("Path escapes dev SD root");
  }
  return resolved;
}

function itemName(webPath: string): string {
  const normalized = toWebPath(webPath);
  return normalized.slice(normalized.lastIndexOf("/") + 1);
}

function webPathSegments(webPath: string): string[] {
  const normalized = toWebPath(webPath);
  return normalized === "/" ? [] : normalized.slice(1).split("/");
}

function isProtectedName(name: string): boolean {
  return name.startsWith(".") || protectedNames.has(name);
}

function hasTraversalSegment(webPath: string): boolean {
  return webPathSegments(webPath).some((segment) => segment === "." || segment === "..");
}

function isProtectedWebPath(webPath: string): boolean {
  return webPathSegments(webPath).some(isProtectedName);
}

function validateChildName(name: string): string | undefined {
  if (!name || name === "." || name === "..") return "Missing or invalid name";
  if (name.includes("/") || name.includes("\\")) return "Name cannot contain path separators";
  if (isProtectedName(name)) return "Cannot modify protected items";
  return undefined;
}

function isValidFontFamilyName(name: string): boolean {
  return /^[A-Za-z0-9_-]+$/.test(name);
}

function isValidCpfontFilename(name: string): boolean {
  return /^[A-Za-z0-9_-]+\.cpfont$/i.test(name);
}

function cpfontSize(name: string): number | undefined {
  const match = /[_-](\d+)\.cpfont$/i.exec(name);
  return match ? Number(match[1]) : undefined;
}

function parseIndex(value: unknown, length: number): number | undefined {
  if (typeof value === "undefined" || value === null || value === "") return undefined;
  const index = Number(value);
  return Number.isInteger(index) && index >= 0 && index < length ? index : undefined;
}

function hasIndex(value: Record<string, unknown>): boolean {
  return Object.hasOwn(value, "index") && value.index !== "" && typeof value.index !== "undefined" && value.index !== null;
}

async function exists(path: string): Promise<boolean> {
  try {
    await stat(path);
    return true;
  } catch {
    return false;
  }
}

async function isDirectory(path: string): Promise<boolean> {
  return (await stat(path)).isDirectory();
}

async function removePath(path: string): Promise<void> {
  if (await isDirectory(path)) await rmdir(path);
  else await rm(path);
}

async function listLocalFonts(): Promise<Record<string, unknown>> {
  if (!(await exists(fontsRoot))) return { families: [], maxFamilies: 128 };

  const families = [];
  for (const entry of await readdir(fontsRoot, { withFileTypes: true })) {
    if (!entry.isDirectory() || !isValidFontFamilyName(entry.name)) continue;
    const familyDir = join(fontsRoot, entry.name);
    const files = [];
    const sizes = new Set<number>();
    for (const file of await readdir(familyDir, { withFileTypes: true })) {
      if (!file.isFile() || !isValidCpfontFilename(file.name)) continue;
      const info = await stat(join(familyDir, file.name));
      const size = cpfontSize(file.name);
      if (typeof size === "number") sizes.add(size);
      files.push({ name: file.name, size: info.size });
    }
    if (files.length > 0) {
      families.push({
        name: entry.name,
        sizes: [...sizes].sort((a, b) => a - b),
        files,
      });
    }
  }

  families.sort((a, b) => a.name.localeCompare(b.name));
  return { families, maxFamilies: 128 };
}

function davXmlEscape(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function davHref(webPath: string): string {
  const normalized = toWebPath(webPath);
  if (normalized === "/") return "/";
  return normalized
    .split("/")
    .map((part, index) => (index === 0 ? "" : encodeURIComponent(part)))
    .join("/");
}

async function davEntry(webPath: string): Promise<string> {
  const localPath = fsPath(webPath);
  const info = await stat(localPath);
  const isDir = info.isDirectory();
  const href = davXmlEscape(davHref(webPath) + (isDir && webPath !== "/" && !webPath.endsWith("/") ? "/" : ""));
  return `<D:response><D:href>${href}</D:href><D:propstat><D:prop><D:resourcetype>${
    isDir ? "<D:collection/>" : ""
  }</D:resourcetype><D:getcontentlength>${isDir ? 0 : info.size}</D:getcontentlength><D:getlastmodified>${new Date(
    info.mtimeMs,
  ).toUTCString()}</D:getlastmodified></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>`;
}

async function handleLocalWebDav(request: Request): Promise<Response | undefined> {
  const url = new URL(request.url);
  const webPath = toWebPath(url.pathname);
  const localPath = fsPath(webPath);

  if (request.method === "OPTIONS") {
    return new Response(null, {
      status: 200,
      headers: {
        DAV: "1",
        Allow: "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, MKCOL, MOVE, COPY, LOCK, UNLOCK",
        "MS-Author-Via": "DAV",
      },
    });
  }

  if (request.method === "PROPFIND") {
    if (isProtectedWebPath(webPath)) return text("Forbidden", { status: 403 });
    if (!(await exists(localPath))) return text("Not Found", { status: 404 });
    const depth = request.headers.get("depth") || "infinity";
    const entries = [await davEntry(webPath)];
    if ((depth === "1" || depth === "infinity") && (await isDirectory(localPath))) {
      for (const entry of await readdir(localPath, { withFileTypes: true })) {
        if (entry.name.startsWith(".") || protectedNames.has(entry.name)) continue;
        entries.push(await davEntry(`${webPath === "/" ? "" : webPath}/${entry.name}`));
      }
    }
    return new Response(`<?xml version="1.0" encoding="utf-8"?><D:multistatus xmlns:D="DAV:">${entries.join("")}</D:multistatus>`, {
      status: 207,
      headers: { "content-type": "application/xml; charset=utf-8" },
    });
  }

  if (request.method === "HEAD") {
    if (isProtectedWebPath(webPath)) return new Response(null, { status: 403 });
    if (!(await exists(localPath))) return new Response(null, { status: 404 });
    const info = await stat(localPath);
    return new Response(null, { status: 200, headers: { "content-length": String(info.isDirectory() ? 0 : info.size) } });
  }

  if (request.method === "GET" && !routes[url.pathname] && !url.pathname.startsWith("/api/")) {
    if (isProtectedWebPath(webPath)) return text("Forbidden", { status: 403 });
    if (!(await exists(localPath))) return undefined;
    if (await isDirectory(localPath)) return text("Path is a directory", { status: 400 });
    return new Response(Bun.file(localPath));
  }

  if (request.method === "PUT") {
    if (isProtectedWebPath(webPath)) return text("Forbidden", { status: 403 });
    await mkdir(dirname(localPath), { recursive: true });
    const didExist = await exists(localPath);
    await writeFile(localPath, Buffer.from(await request.arrayBuffer()));
    return new Response(null, { status: didExist ? 204 : 201 });
  }

  if (request.method === "DELETE") {
    if (webPath === "/" || isProtectedWebPath(webPath)) return text("Forbidden", { status: 403 });
    if (!(await exists(localPath))) return text("Not Found", { status: 404 });
    if ((await stat(localPath)).isDirectory() && (await readdir(localPath)).length > 0) {
      return text("Directory not empty", { status: 409 });
    }
    await removePath(localPath);
    return new Response(null, { status: 204 });
  }

  if (request.method === "MKCOL") {
    if (isProtectedWebPath(webPath)) return text("Forbidden", { status: 403 });
    if (await exists(localPath)) return text("Already exists", { status: 405 });
    if (!(await exists(dirname(localPath)))) return text("Parent directory does not exist", { status: 409 });
    await mkdir(localPath);
    return new Response(null, { status: 201 });
  }

  if (request.method === "MOVE" || request.method === "COPY") {
    const destination = request.headers.get("destination");
    if (!destination) return text("Missing Destination header", { status: 400 });
    const dstUrl = new URL(destination, url.origin);
    const dstWebPath = toWebPath(dstUrl.pathname);
    const dstPath = fsPath(dstWebPath);
    const overwrite = (request.headers.get("overwrite") || "T").toUpperCase() !== "F";
    if (isProtectedWebPath(webPath) || isProtectedWebPath(dstWebPath)) return text("Forbidden", { status: 403 });
    if (!(await exists(localPath))) return text("Source not found", { status: 404 });
    if (!(await exists(dirname(dstPath)))) return text("Destination parent does not exist", { status: 409 });
    const dstExists = await exists(dstPath);
    if (dstExists && !overwrite) return text("Destination exists and Overwrite is F", { status: 412 });
    if (dstExists) await removePath(dstPath);
    if (request.method === "MOVE") await rename(localPath, dstPath);
    else {
      if (await isDirectory(localPath)) return text("Cannot copy directories", { status: 403 });
      await copyFile(localPath, dstPath);
    }
    return new Response(null, { status: dstExists ? 204 : 201 });
  }

  if (request.method === "LOCK" || request.method === "UNLOCK") {
    return new Response(null, { status: request.method === "LOCK" ? 200 : 204 });
  }

  return undefined;
}

async function loadState(): Promise<DevState> {
  await mkdir(sdRoot, { recursive: true });
  try {
    return JSON.parse(await readFile(statePath, "utf8")) as DevState;
  } catch {
    await saveState(defaultState);
    return structuredClone(defaultState);
  }
}

async function saveState(state: DevState): Promise<void> {
  await mkdir(sdRoot, { recursive: true });
  await writeFile(statePath, JSON.stringify(state, null, 2));
}

function publicWifi(item: Record<string, unknown>): Record<string, unknown> {
  return {
    ssid: item.ssid || "",
    hasPassword: Boolean(item.password || item.hasPassword),
    isLastConnected: Boolean(item.isLastConnected),
  };
}

function publicOpds(item: Record<string, unknown>): Record<string, unknown> {
  return {
    name: item.name || "",
    url: item.url || "",
    username: item.username || "",
    hasPassword: Boolean(item.password || item.hasPassword),
  };
}

function websocketPayload(message: string | ArrayBuffer | Uint8Array): string | ArrayBuffer {
  if (typeof message === "string" || message instanceof ArrayBuffer) return message;
  const copy = new Uint8Array(message.byteLength);
  copy.set(message);
  return copy.buffer;
}

function parseUploadSize(sizeText: string): number | undefined {
  if (!/^\+?\d+$/.test(sizeText)) return undefined;
  const size = Number(sizeText);
  return Number.isSafeInteger(size) && size >= 0 ? size : undefined;
}

function parseUploadStart(message: string): UploadStart | undefined {
  const firstColon = message.indexOf(":", 6);
  const secondColon = message.indexOf(":", firstColon + 1);
  if (firstColon <= 0 || secondColon <= 0) return undefined;
  const size = parseUploadSize(message.slice(firstColon + 1, secondColon));
  if (typeof size === "undefined") return undefined;
  return {
    fileName: message.slice(6, firstColon),
    size,
    path: message.slice(secondColon + 1) || "/",
  };
}

async function startLocalWebSocketUpload(fileName: string, webPath: string, size: number): Promise<LocalUpload> {
  const targetDir = fsPath(webPath);
  await mkdir(targetDir, { recursive: true });
  const tempPath = join(targetDir, `.${fileName}.${Date.now()}.uploading`);
  return {
    fileName,
    path: webPath,
    size,
    received: 0,
    tempPath,
    handle: await open(tempPath, "w"),
  };
}

async function cleanupLocalWebSocketUpload(upload: LocalUpload): Promise<void> {
  await upload.handle.close().catch(() => {});
  await rm(upload.tempPath, { force: true }).catch(() => {});
}

async function finishLocalWebSocketUpload(upload: LocalUpload): Promise<void> {
  await upload.handle.close();
  await rename(upload.tempPath, join(dirname(upload.tempPath), upload.fileName));
}

async function parseBody(request: Request): Promise<Record<string, any>> {
  const contentType = request.headers.get("content-type") || "";
  if (contentType.includes("application/json")) {
    return await request.json();
  }
  const form = await request.formData();
  return Object.fromEntries(form.entries());
}

async function handleLocalApi(request: Request): Promise<Response | undefined> {
  const url = new URL(request.url);
  const state = await loadState();

  if (request.method === "GET" && url.pathname === "/api/status") {
    return json({
      version: "web-dev-local",
      ip: "127.0.0.1",
      mode: "LOCAL",
      rssi: -42,
      freeHeap: 123456,
      uptime: Math.floor(process.uptime()),
    });
  }

  if (request.method === "GET" && url.pathname === "/api/files") {
    const webPath = toWebPath(url.searchParams.get("path") || "/");
    if (hasTraversalSegment(webPath)) return text("Invalid path segment", { status: 400 });
    if (isProtectedWebPath(webPath)) return text("Cannot access protected items", { status: 403 });
    const dirPath = fsPath(webPath);
    if (!(await exists(dirPath))) return json([]);
    if (!(await isDirectory(dirPath))) return json([]);
    const entries = await readdir(dirPath, { withFileTypes: true });
    const visible = entries.filter((entry) => !isProtectedName(entry.name));
    const files = await Promise.all(
      visible.map(async (entry) => {
        const entryPath = join(dirPath, entry.name);
        const info = await stat(entryPath);
        return {
          name: entry.name,
          size: entry.isDirectory() ? 0 : info.size,
          isDirectory: entry.isDirectory(),
          isEpub: !entry.isDirectory() && entry.name.toLowerCase().endsWith(".epub"),
        };
      }),
    );
    files.sort((a, b) => Number(b.isDirectory) - Number(a.isDirectory) || a.name.localeCompare(b.name));
    return json(files);
  }

  if (request.method === "GET" && url.pathname === "/download") {
    const webPath = toWebPath(url.searchParams.get("path") || "/");
    if (webPath === "/") return text("Invalid path", { status: 400 });
    if (isProtectedWebPath(webPath)) return text("Cannot access protected items", { status: 403 });
    const path = fsPath(webPath);
    const file = Bun.file(path);
    if (!(await file.exists())) return text("Item not found", { status: 404 });
    if (await isDirectory(path)) return text("Path is a directory", { status: 400 });
    return new Response(file);
  }

  if (request.method === "POST" && url.pathname === "/upload") {
    const form = await request.formData();
    const upload = form.get("file");
    if (!(upload instanceof File)) return text("Missing file", { status: 400 });
    const webPath = toWebPath(url.searchParams.get("path") || "/");
    if (isProtectedWebPath(webPath)) return text("Cannot access protected items", { status: 403 });
    const invalidName = validateChildName(upload.name);
    if (invalidName) return text(invalidName, { status: 400 });
    const targetDir = fsPath(webPath);
    await mkdir(targetDir, { recursive: true });
    await writeFile(join(targetDir, upload.name), Buffer.from(await upload.arrayBuffer()));
    return text(`File uploaded successfully: ${upload.name}`);
  }

  if (request.method === "POST" && url.pathname === "/mkdir") {
    const body = await parseBody(request);
    const name = String(body.name || "").trim();
    const invalidName = validateChildName(name);
    if (invalidName) return text(invalidName, { status: 400 });
    const parentWebPath = toWebPath(String(body.path || "/"));
    if (isProtectedWebPath(parentWebPath)) return text("Cannot access protected items", { status: 403 });
    const parent = fsPath(parentWebPath);
    if (!(await exists(parent))) return text("Parent folder not found", { status: 404 });
    if (!(await isDirectory(parent))) return text("Parent path is not a folder", { status: 400 });
    const target = join(parent, name);
    if (await exists(target)) return text("Folder already exists", { status: 400 });
    await mkdir(target, { recursive: false });
    return text(`Folder created: ${name}`);
  }

  if (request.method === "POST" && url.pathname === "/delete") {
    const body = await parseBody(request);
    let paths: unknown[];
    if (body.paths) {
      try {
        const parsed = JSON.parse(String(body.paths));
        if (!Array.isArray(parsed)) return text("Invalid paths payload", { status: 400 });
        paths = parsed;
      } catch {
        return text("Invalid paths payload", { status: 400 });
      }
    } else {
      paths = [String(body.path || "")];
    }
    const failed: string[] = [];
    for (const candidate of paths) {
      const path = toWebPath(String(candidate));
      const target = fsPath(path);
      if (path === "/") {
        failed.push(`${path} (cannot delete root)`);
      } else if (isProtectedWebPath(path)) {
        failed.push(`${path} (protected file)`);
      } else if (!(await exists(target))) {
        failed.push(`${path} (not found)`);
      } else if ((await stat(target)).isDirectory() && (await readdir(target)).length > 0) {
        failed.push(`${path} (folder not empty)`);
      } else {
        await removePath(target);
      }
    }
    if (failed.length > 0) return text(`Failed to delete some items: ${failed.join("; ")}`, { status: 500 });
    return text("All items deleted successfully");
  }

  if (request.method === "POST" && url.pathname === "/rename") {
    const body = await parseBody(request);
    const oldPath = fsPath(String(body.path || ""));
    const newName = String(body.name || "").trim();
    const oldWebPath = toWebPath(String(body.path || ""));
    const invalidName = validateChildName(newName);
    if (invalidName) return text(invalidName, { status: 400 });
    if (isProtectedWebPath(oldWebPath) || isProtectedWebPath(newName)) return text("Cannot rename protected item", { status: 403 });
    if (!(await exists(oldPath))) return text("Item not found", { status: 404 });
    if (await isDirectory(oldPath)) return text("Only files can be renamed", { status: 400 });
    const target = join(dirname(oldPath), newName);
    if (await exists(target)) return text("Target already exists", { status: 409 });
    await rename(oldPath, target);
    return text("Renamed successfully");
  }

  if (request.method === "POST" && url.pathname === "/move") {
    const body = await parseBody(request);
    const oldWebPath = toWebPath(String(body.path || ""));
    const destWebPath = toWebPath(String(body.dest || "/"));
    const oldPath = fsPath(oldWebPath);
    const destDir = fsPath(destWebPath);
    if (isProtectedWebPath(oldWebPath) || isProtectedWebPath(destWebPath)) return text("Cannot move protected item", { status: 403 });
    if (!(await exists(oldPath))) return text("Item not found", { status: 404 });
    if (await isDirectory(oldPath)) return text("Only files can be moved", { status: 400 });
    if (!(await exists(destDir))) return text("Destination not found", { status: 404 });
    if (!(await isDirectory(destDir))) return text("Destination is not a folder", { status: 400 });
    const target = join(destDir, itemName(oldWebPath));
    if (target === oldPath) return text("Already in destination");
    if (await exists(target)) return text("Target already exists", { status: 409 });
    await rename(oldPath, target);
    return text("Moved successfully");
  }

  if (request.method === "GET" && url.pathname === "/api/settings") {
    return json(state.settings);
  }

  if (request.method === "GET" && url.pathname === "/api/fonts") {
    return json(await listLocalFonts());
  }

  if (request.method === "POST" && url.pathname === "/api/fonts/upload") {
    const form = await request.formData();
    const family = String(form.get("family") || "");
    const upload = form.get("file");
    if (!isValidFontFamilyName(family)) return json({ error: "Invalid family name" }, { status: 400 });
    if (!(upload instanceof File) || !isValidCpfontFilename(upload.name)) {
      return json({ error: "Invalid .cpfont file" }, { status: 400 });
    }
    const bytes = Buffer.from(await upload.arrayBuffer());
    if (bytes.length < 8 || !bytes.subarray(0, 8).equals(Buffer.from("CPFONT\0\0"))) {
      return json({ error: "Invalid .cpfont file" }, { status: 400 });
    }
    const familyDir = join(fontsRoot, family);
    await mkdir(familyDir, { recursive: true });
    await writeFile(join(familyDir, upload.name), bytes);
    return json({ ok: true });
  }

  if (request.method === "POST" && url.pathname === "/api/fonts/delete") {
    const body = await parseBody(request);
    const family = String(body.family || "");
    if (!isValidFontFamilyName(family)) return json({ error: "Invalid request" }, { status: 400 });
    const target = join(fontsRoot, family);
    if (!(await exists(target))) return json({ error: "Delete failed" }, { status: 500 });
    await rm(target, { recursive: true, force: true });
    return json({ ok: true });
  }

  if (request.method === "POST" && url.pathname === "/api/settings") {
    const changes = await parseBody(request);
    state.settings = state.settings.map((setting) =>
      Object.hasOwn(changes, String(setting.key)) ? { ...setting, value: changes[String(setting.key)] } : setting,
    );
    await saveState(state);
    return text("Settings saved");
  }

  if (request.method === "GET" && url.pathname === "/api/wifi") {
    return json(state.wifi.map(publicWifi));
  }

  if (request.method === "POST" && url.pathname === "/api/wifi") {
    const body = await parseBody(request);
    const index = parseIndex(body.index, state.wifi.length);
    if (hasIndex(body) && typeof index === "undefined") return text("Invalid Wi-Fi index", { status: 400 });
    const next: Record<string, unknown> = {
      ...(typeof index === "number" ? state.wifi[index] : {}),
      ...body,
      hasPassword: Boolean(body.password),
    };
    delete next.index;
    if (typeof index === "number") state.wifi[index] = next;
    else state.wifi.push(next);
    await saveState(state);
    return text("Wi-Fi network saved");
  }

  if (request.method === "POST" && url.pathname === "/api/wifi/delete") {
    const body = await parseBody(request);
    const index = parseIndex(body.index, state.wifi.length);
    if (typeof index === "undefined") return text("Invalid Wi-Fi index", { status: 400 });
    state.wifi.splice(index, 1);
    await saveState(state);
    return text("Wi-Fi network deleted");
  }

  if (request.method === "GET" && url.pathname === "/api/opds") {
    return json(state.opds.map(publicOpds));
  }

  if (request.method === "POST" && url.pathname === "/api/opds") {
    const body = await parseBody(request);
    const index = parseIndex(body.index, state.opds.length);
    if (hasIndex(body) && typeof index === "undefined") return text("Invalid OPDS index", { status: 400 });
    const next: Record<string, unknown> = {
      ...(typeof index === "number" ? state.opds[index] : {}),
      ...body,
      hasPassword: Boolean(body.password),
    };
    delete next.index;
    if (typeof index === "number") state.opds[index] = next;
    else state.opds.push(next);
    await saveState(state);
    return text("OPDS server saved");
  }

  if (request.method === "POST" && url.pathname === "/api/opds/delete") {
    const body = await parseBody(request);
    const index = parseIndex(body.index, state.opds.length);
    if (typeof index === "undefined") return text("Invalid OPDS index", { status: 400 });
    state.opds.splice(index, 1);
    await saveState(state);
    return text("OPDS server deleted");
  }

  return undefined;
}

function createEventStream(): Response {
  let streamController: ReadableStreamDefaultController<string> | undefined;
  const stream = new ReadableStream<string>({
    start(controller) {
      streamController = controller;
      reloadClients.add(controller);
      controller.enqueue("event: ready\ndata: ok\n\n");
    },
    cancel() {
      if (streamController) reloadClients.delete(streamController);
    },
  });

  return new Response(stream, {
    headers: {
      "content-type": "text/event-stream",
      "cache-control": "no-store",
      connection: "keep-alive",
    },
  });
}

function notifyReload(fileName: string): void {
  for (const controller of reloadClients) {
    try {
      controller.enqueue(`event: change\ndata: ${fileName}\n\n`);
    } catch {
      reloadClients.delete(controller);
    }
  }
}

let reloadTimer: Timer | undefined;
watch(htmlDir, { recursive: true }, (_eventType, fileName) => {
  if (!fileName || (!fileName.endsWith(".html") && !fileName.endsWith(".js") && !fileName.endsWith(".css"))) return;
  clearTimeout(reloadTimer);
  reloadTimer = setTimeout(() => {
    console.log(`[web:dev] changed ${fileName}, reloading browsers`);
    notifyReload(String(fileName));
  }, 80);
});

const server = Bun.serve<{
  remote?: WebSocket;
  pending?: Array<string | ArrayBuffer | Uint8Array>;
  localUpload?: LocalUpload;
  localUploadStarting?: boolean;
  writeQueue?: Promise<void>;
}>({
  port,
  hostname: host,
  idleTimeout: 255,
  async fetch(request, server) {
    const url = new URL(request.url);

    if ((request.method === "GET" || request.method === "HEAD") && url.pathname === "/favicon.ico") {
      return new Response(request.method === "HEAD" ? null : await readFile(faviconPath), {
        headers: {
          "content-type": "image/x-icon",
          "cache-control": "public, max-age=3600",
        },
      });
    }

    if ((request.method === "GET" || request.method === "HEAD") && url.pathname === "/app.css") {
      return new Response(request.method === "HEAD" ? null : await readFile(appCssPath), {
        headers: {
          "content-type": "text/css; charset=utf-8",
          "cache-control": "no-store",
        },
      });
    }

    if (url.pathname === "/__crosspoint_events") {
      return createEventStream();
    }

    if (url.pathname === "/__crosspoint_ws") {
      if (server.upgrade(request, { data: { pending: [] } })) {
        return;
      }
      return new Response("WebSocket upgrade failed", { status: 400 });
    }

    const localPage = routes[url.pathname];
    if (request.method === "GET" && localPage) {
      return serveLocalHtml(localPage);
    }

    if (request.method === "GET" && url.pathname.startsWith("/js/")) {
      const filePath = join(htmlDir, url.pathname);
      const file = Bun.file(filePath);
      if (await file.exists()) {
        return new Response(file, {
          headers: {
            "content-type": contentType(url.pathname),
            "cache-control": "no-store",
          },
        });
      }
    }

    if (deviceOrigin) {
      return proxyHttp(request);
    }

    try {
      const localResponse = await handleLocalApi(request);
      if (localResponse) return localResponse;
      const webDavResponse = await handleLocalWebDav(request);
      if (webDavResponse) return webDavResponse;
      return text("Not found", { status: 404 });
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      if (message === "Path escapes dev SD root" || message === "Invalid path segment" || message === "Invalid path encoding") {
        return text(message, { status: 400 });
      }
      return text(message, { status: 500 });
    }
  },
  websocket: {
    open(client) {
      if (!deviceOrigin) {
        client.data.localUpload = undefined;
        return;
      }

      const remote = new WebSocket(deviceWsUrl);
      client.data.remote = remote;

      remote.binaryType = "arraybuffer";

      remote.addEventListener("open", () => {
        for (const message of client.data.pending || []) {
          remote.send(websocketPayload(message));
        }
        client.data.pending = [];
      });

      remote.addEventListener("message", (event) => {
        client.send(event.data);
      });

      remote.addEventListener("close", () => {
        client.close();
      });

      remote.addEventListener("error", () => {
        client.close(1011, "Device WebSocket proxy failed");
      });
    },
    async message(client, message) {
      if (!deviceOrigin) {
        if (typeof message === "string") {
          if (!message.startsWith("START:")) {
            client.send("ERROR:Expected START message");
            return;
          }
          if (client.data.localUpload || client.data.localUploadStarting) {
            client.send("ERROR:Upload already in progress");
            return;
          }

          const start = parseUploadStart(message);
          if (!start) {
            client.send("ERROR:Invalid START format");
            return;
          }
          const webPath = toWebPath(start.path);
          const invalidName = validateChildName(start.fileName);
          if (invalidName) {
            client.send(`ERROR:${invalidName}`);
            return;
          }
          if (hasTraversalSegment(webPath)) {
            client.send("ERROR:Invalid path segment");
            return;
          }
          if (isProtectedWebPath(webPath)) {
            client.send("ERROR:Cannot access protected items");
            return;
          }
          client.data.localUploadStarting = true;
          try {
            client.data.localUpload = await startLocalWebSocketUpload(start.fileName, webPath, start.size);
          } catch (error) {
            client.data.localUploadStarting = false;
            client.data.localUpload = undefined;
            client.send(`ERROR:${error instanceof Error ? error.message : "Failed to start upload"}`);
            return;
          }
          client.data.localUploadStarting = false;
          if (start.size === 0) {
            await finishLocalWebSocketUpload(client.data.localUpload);
            client.data.localUpload = undefined;
            client.data.writeQueue = undefined;
            client.send("DONE");
          } else {
            client.send("READY");
          }
          return;
        }

        const upload = client.data.localUpload;
        if (!upload) {
          client.send("ERROR:Upload not initialized");
          return;
        }

        const chunk = Buffer.from(message);
        const previousWrite = client.data.writeQueue ?? Promise.resolve();
        const currentWrite = previousWrite.then(async () => {
          const currentUpload = client.data.localUpload;
          if (!currentUpload) {
            client.send("ERROR:Upload not initialized");
            return;
          }
          if (currentUpload.received + chunk.byteLength > currentUpload.size) {
            await cleanupLocalWebSocketUpload(currentUpload);
            client.data.localUpload = undefined;
            client.data.writeQueue = undefined;
            client.send("ERROR:Upload exceeds declared size");
            return;
          }
          await currentUpload.handle.write(chunk);
          currentUpload.received += chunk.byteLength;
          client.send(`PROGRESS:${currentUpload.received}:${currentUpload.size}`);

          if (currentUpload.received >= currentUpload.size) {
            await finishLocalWebSocketUpload(currentUpload);
            client.data.localUpload = undefined;
            client.data.writeQueue = undefined;
            client.send("DONE");
          }
        });
        const trackedWrite = currentWrite.catch(async (error) => {
          if (client.data.localUpload) await cleanupLocalWebSocketUpload(client.data.localUpload);
          client.data.localUpload = undefined;
          client.data.writeQueue = undefined;
          client.send(`ERROR:${error instanceof Error ? error.message : "Upload failed"}`);
        });
        client.data.writeQueue = trackedWrite;
        return;
      }

      const remote = client.data.remote;
      if (!remote || remote.readyState === WebSocket.CLOSING || remote.readyState === WebSocket.CLOSED) {
        client.close(1011, "Device WebSocket unavailable");
        return;
      }

      if (remote.readyState === WebSocket.OPEN) {
        remote.send(message);
        return;
      }

      client.data.pending?.push(websocketPayload(message));
    },
    close(client) {
      if (client.data.localUpload) void cleanupLocalWebSocketUpload(client.data.localUpload);
      client.data.writeQueue = undefined;
      client.data.remote?.close();
    },
  },
});

console.log("[web:dev] CrossPoint web UI dev server");
console.log(`[web:dev] local:  http://${host}:${server.port}/`);
console.log(deviceOrigin ? `[web:dev] mode:   proxying device at ${deviceOrigin}` : "[web:dev] mode:   local ESP32-like simulator");
console.log(`[web:dev] sd:     ${sdRoot}`);
console.log(`[web:dev] pages are served from ${htmlDir}`);
console.log("[web:dev] set CROSSPOINT_DEVICE=http://<device-ip> to proxy a real reader instead");
