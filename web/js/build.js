export const CONSOLES = {
  psp: { name: "PSP", output: "EBOOT.PBP", engine: "EBOOT.PBP", screen: "Scaling the artwork for the PSP screen", write: "eboot" },
  vita: {
    name: "PS Vita",
    output: "TCOAAL.vpk",
    folder: true,
    engine: "eboot.bin",
    extras: ["param.sfo"],
    screen: "Scaling the artwork for the PS Vita screen",
    write: "vpk",
  },
  "3ds": { name: "3DS", output: "tcoaal.3dsx", engine: "tcoaal.3dsx", screen: "Scaling the artwork for the 3DS screen", write: "3dsx" },
  wii: { name: "Wii", output: "apps/tcoaal/boot.dol", folder: true, engine: "tcoaal.dol", screen: "Scaling the artwork for the Wii", write: "homebrew channel app" },
  wiiu: { name: "Wii U", output: "wiiu/apps/tcoaal/tcoaal.wuhb", folder: true, engine: "tcoaal.rpx", screen: "Converting the artwork for the Wii U", write: "wuhb", big: true },
  ps2: { name: "PS2", output: "TCOAAL.iso", engine: "tcoaal.elf", screen: "Scaling the artwork for the PS2", write: "iso" },
  ps3: { name: "PS3", output: "TCOAAL.pkg", engine: "EBOOT.BIN", screen: "Converting the artwork for the RSX", write: "package", big: true },
  xbox360: { name: "Xbox 360", output: "xenon.elf", folder: true, engine: "xenon.elf", screen: "Converting the artwork for the Xbox 360", write: "xell files", big: true },
};

function phasesFor(target) {
  const { output, screen, write, big } = CONSOLES[target];
  return {
    database: { from: 0.03, to: 0.06, text: "Reading the game's data" },
    maps: { from: 0.06, to: 0.1, text: "Converting maps and events" },
    images: { from: 0.1, to: big ? 0.7 : 0.8, text: screen },
    audio: { from: big ? 0.7 : 0.8, to: big ? 0.74 : 0.84, text: "Copying music and sounds" },
    fonts: { from: big ? 0.74 : 0.84, to: big ? 0.8 : 0.9, text: "Rendering the font" },
    [write]: { from: big ? 0.8 : 0.9, to: 0.99, text: `Writing ${output}` },
  };
}

function megabytes(bytes) {
  return (bytes / 1048576).toFixed(1);
}

export class Builder {
  constructor() {
    this.worker = null;
    this.engines = new Map();
  }

  start(message, onLine) {
    this.worker ??= new Worker(new URL("./worker.js", import.meta.url), { type: "module" });

    return new Promise((resolve) => {
      const lines = [];
      this.worker.onmessage = ({ data }) => {
        if (data.type === "line") {
          lines.push(data.line);
          onLine(data.line);
          return;
        }
        if (data.type === "crash") {
          resolve({ ok: false, crashed: true, message: data.message.split("\n")[0], lines });
          return;
        }
        resolve({
          ok: data.code === 0 && Boolean(data.output || data.files?.length),
          code: data.code,
          seconds: data.seconds,
          output: data.output instanceof Uint8Array ? data.output : data.output ? new Uint8Array(data.output) : null,
          files: data.files ?? null,
          lines,
        });
      };
      this.worker.postMessage({ wasmUrl: new URL("../tcoaal-builder.wasm", import.meta.url).href, ...message });
    });
  }

  async download(target, file, onProgress) {
    const { name } = CONSOLES[target];
    const response = await fetch(new URL(`../engines/${target}/${file}`, import.meta.url));
    if (!response.ok) throw new Error(`Couldn't download the ${name} engine (${response.status}).`);
    const total = Number(response.headers.get("content-length")) || 0;
    const reader = response.body.getReader();
    const chunks = [];
    let received = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
      received += value.length;
      if (total) onProgress(0.03 * (received / total), `Downloading the ${name} engine: ${megabytes(received)} of ${megabytes(total)} MB`);
    }
    return { path: file, blob: new Blob(chunks) };
  }

  async engine(target, onProgress) {
    const { engine, extras = [] } = CONSOLES[target];
    if (this.engines.has(target)) return this.engines.get(target);
    const files = [];
    for (const file of [engine, ...extras]) files.push(await this.download(target, file, onProgress));
    this.engines.set(target, files);
    return files;
  }

  async run({ target, gameFiles, env, onProgress, onLine }) {
    const { output, engine, folder } = CONSOLES[target];
    const phases = phasesFor(target);
    const engineFiles = await this.engine(target, onProgress);
    const args = ["tcoaal-builder", "/game", "--target", target, "-o", folder ? "/out" : `/out/${output}`, "--engine", `/engine/${engine}`];

    let phase = phases.database;
    const result = await this.start(
      { args, gameFiles, engineFiles, output, collect: Boolean(folder), env },
      (line) => {
        onLine(line);
        const word = line.trim();
        if (phases[word]) {
          phase = phases[word];
          onProgress(phase.from, `${phase.text}...`);
        }
        const step = word.match(/^progress (\d+)\/(\d+)$/);
        if (step) {
          const done = Number(step[1]);
          const total = Number(step[2]);
          onProgress(phase.from + (phase.to - phase.from) * (done / total), `${phase.text}: ${done} of ${total}`);
        }
      },
    );
    const cracked = result.lines.map((line) => line.match(/looks cracked: (.*)$/)).find(Boolean);
    return { ...result, crackReason: result.code === 3 && cracked ? cracked[1] : null };
  }

  makeCia({ app, onLine }) {
    return this.start(
      {
        args: ["tcoaal-builder", "--make-cia", "/out/tcoaal.3dsx", "-o", "/out/tcoaal.cia"],
        outFiles: [{ path: "tcoaal.3dsx", blob: new Blob([app]) }],
        output: "tcoaal.cia",
        env: {},
      },
      onLine,
    );
  }
}
