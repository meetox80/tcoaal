const ASSET_SIG = [84, 67, 79, 65, 65, 76];
const LANG_SIG = "LANGDATA";
const LANG_FILE = "data/9c7050ae76645487";

function indexByGamePath(files) {
  const index = new Map();
  for (const file of files) {
    if (!file.blob) continue;
    const match = file.path.replace(/\\/g, "/").match(/(?:^|\/)www\/(.+)$/i);
    if (match && !index.has(match[1].toLowerCase())) index.set(match[1].toLowerCase(), file);
  }
  return index;
}

async function sha256Hex(text) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

export async function hashPath(path) {
  const parts = path.replace(/\\/g, "/").split("/");
  const last = parts.at(-1);
  let hashed = (await sha256Hex(parts.join("/"))).slice(0, 16);
  if (last.toUpperCase().includes("[BUST]")) hashed += "[BUST]";
  if (last.startsWith("!")) hashed = `!${hashed}`;
  parts[parts.length - 1] = hashed;
  return parts.join("/");
}

function nameMask(stored) {
  let name = stored.replace(/\\/g, "/").split("/").at(-1);
  const dot = name.lastIndexOf(".");
  if (dot > 0) name = name.slice(0, dot);
  let mask = 0;
  for (const ch of name.toUpperCase()) mask = (mask << 1) ^ ch.charCodeAt(0);
  return mask;
}

export function decryptAsset(bytes, stored) {
  if (bytes.length < ASSET_SIG.length + 1 || ASSET_SIG.some((value, i) => bytes[i] !== value)) return bytes;
  const body = bytes.subarray(ASSET_SIG.length + 1);
  const count = bytes[ASSET_SIG.length] || body.length;
  let key = (((nameMask(stored) + 1) % 256) + 256) % 256;
  const out = body.slice();
  for (let i = 0; i < Math.min(count, body.length); i++) {
    const value = body[i];
    out[i] = value ^ key;
    key = ((key << 1) ^ value) & 255;
  }
  return out;
}

function stripCodes(text) {
  return text
    .replace(/\\n<[^>]*>/g, "")
    .replace(/\\[A-Za-z]+\[[^\]]*\]/g, "")
    .replace(/\\[A-Za-z]+/g, "")
    .replace(/\\[{}<>^$.|!]/g, "")
    .replace(/[ \t]{2,}/g, " ")
    .replace(/ *\n */g, "\n")
    .trim();
}

function richSegments(text) {
  const source = text.replace(/\\n<[^>]*>/g, "");
  const segments = [];
  let style = { color: null, bold: false, italic: false };
  let buffer = "";
  const flush = () => {
    if (buffer) segments.push({ text: buffer, ...style });
    buffer = "";
  };
  for (let i = 0; i < source.length; ) {
    if (source[i] !== "\\") {
      buffer += source[i++];
      continue;
    }
    const rest = source.slice(i + 1);
    const color = rest.match(/^c\[(\d+)\]/i);
    const font = rest.match(/^f([rbi])/i);
    const code = rest.match(/^(?:[A-Za-z]+(?:\[[^\]]*\])?|[{}<>^$.|!\\])/);
    if (color) {
      flush();
      style = { ...style, color: Number(color[1]) };
      i += 1 + color[0].length;
    } else if (font) {
      flush();
      const kind = font[1].toLowerCase();
      if (kind === "r") style = { color: 0, bold: false, italic: false };
      else if (kind === "b") style = { ...style, bold: !style.bold };
      else style = { ...style, italic: !style.italic };
      i += 3;
    } else if (code) {
      i += 1 + code[0].length;
    } else {
      buffer += source[i++];
    }
  }
  flush();
  if (segments.length) {
    segments[0].text = segments[0].text.trimStart();
    segments.at(-1).text = segments.at(-1).text.trimEnd();
  }
  return segments.filter((segment) => segment.text);
}

export async function openGameAssets(files) {
  if (!globalThis.crypto?.subtle) return null;
  const index = indexByGamePath(files);
  const urls = [];
  const resolved = new Map();

  const resolve = async (path) => {
    if (resolved.has(path)) return resolved.get(path);
    let stored = null;
    const lower = path.toLowerCase();
    const dot = path.lastIndexOf(".");
    const stem = dot > path.lastIndexOf("/") ? path.slice(0, dot) : path;
    if (index.has(lower)) stored = path;
    else if (index.has(stem.toLowerCase())) stored = stem;
    else {
      const hashed = await hashPath(path);
      if (index.has(hashed.toLowerCase())) stored = hashed;
    }
    resolved.set(path, stored);
    return stored;
  };

  const bytes = async (path) => {
    const stored = await resolve(path);
    if (!stored) return null;
    const raw = new Uint8Array(await index.get(stored.toLowerCase()).blob.arrayBuffer());
    return decryptAsset(raw, stored);
  };

  const json = async (path) => {
    const data = await bytes(path);
    return data ? JSON.parse(new TextDecoder().decode(data)) : null;
  };

  const system = await json("data/System.json");
  if (!system) return null;

  const langBytes = await bytes(LANG_FILE);
  let lines = {};
  let labels = {};
  if (langBytes) {
    const text = new TextDecoder().decode(langBytes);
    if (text.startsWith(LANG_SIG)) {
      const lang = JSON.parse(text.slice(LANG_SIG.length));
      lines = lang.linesLUT ?? {};
      labels = lang.labelLUT ?? {};
    }
  }

  return {
    system,

    async image(path) {
      const data = await bytes(path);
      if (!data) throw new Error(`missing ${path}`);
      const blob = new Blob([data], { type: "image/png" });
      const bitmap = await createImageBitmap(blob);
      const url = URL.createObjectURL(blob);
      urls.push(url);
      return { url, bitmap, width: bitmap.width, height: bitmap.height };
    },

    async sound(path) {
      const data = await bytes(path);
      return data ? new Blob([data], { type: "audio/ogg" }) : null;
    },

    async font(path) {
      const data = await bytes(path);
      if (!data) throw new Error(`missing ${path}`);
      return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
    },

    json,

    line(key) {
      const value = lines[key];
      if (value === undefined) return null;
      const segments = richSegments(Array.isArray(value) ? value.join("\n") : String(value));
      return segments.length ? segments : null;
    },

    label(key) {
      const value = labels[key];
      return typeof value === "string" ? stripCodes(value) : null;
    },

    release() {
      for (const url of urls) URL.revokeObjectURL(url);
      urls.length = 0;
    },
  };
}
