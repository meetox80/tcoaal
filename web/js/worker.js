import { MemoryDirectory, WasiProcess } from "./wasi.js";

let compiled = null;

function makeFileSystem(gameFiles, engineFiles, outFiles) {
  const root = new MemoryDirectory();
  const game = root.makeDirectories(["game"]);
  const engine = root.makeDirectories(["engine"]);
  const out = root.makeDirectories(["out"]);
  for (const file of gameFiles) game.addFile(file.path, file.blob);
  for (const file of engineFiles) engine.addFile(file.path, file.blob);
  for (const file of outFiles) out.addFile(file.path, file.blob);
  return root;
}

function collect(directory, prefix, files) {
  for (const [name, node] of directory.children) {
    const path = prefix ? `${prefix}/${name}` : name;
    if (node.isDirectory) collect(node, path, files);
    else if (!name.endsWith(".tmp")) files.push({ path, bytes: node.contents() });
  }
  return files;
}

async function run({ wasmUrl, args, gameFiles = [], engineFiles = [], outFiles = [], output, collect: all = false, env }) {
  compiled ??= WebAssembly.compileStreaming(fetch(wasmUrl));
  const module = await compiled;

  const root = makeFileSystem(gameFiles, engineFiles, outFiles);
  const process = new WasiProcess({
    args,
    env,
    root,
    onLine: (line) => self.postMessage({ type: "line", line }),
  });

  const started = performance.now();
  const code = process.run(module);
  const seconds = (performance.now() - started) / 1000;

  if (all) {
    const files = code === 0 ? collect(root.find(["out"]), "", []) : [];
    self.postMessage({ type: "done", code, seconds, files }, files.map((file) => file.bytes.buffer));
    return;
  }
  const file = root.find(["out", output]);
  if (code !== 0 || !file) {
    self.postMessage({ type: "done", code, seconds });
    return;
  }
  const bytes = file.contents();
  self.postMessage({ type: "done", code, seconds, output: bytes }, [bytes.buffer]);
}

self.onmessage = async (event) => {
  try {
    await run(event.data);
  } catch (error) {
    self.postMessage({ type: "crash", message: String(error?.stack ?? error) });
  }
};
