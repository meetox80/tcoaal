import { zipSync } from "../vendor/fflate.js";
import { Builder, CONSOLES } from "./build.js";
import { showCrackWarning } from "./crack-warning.js";
import { filesFromDrop, filesFromInput, filesFromZip, isZip, missingGameParts, readGameVersion, sortFiles } from "./files.js";
import { clearGame, loadGame, saveGame } from "./storage.js";
import { Theme } from "./theme.js";

const SUPPORTED_VERSION = "3.0.12";
const CONSOLE_KEY = "tcoaal-console";

const ui = Object.fromEntries(
  [
    "drop", "pick-folder", "pick-zip", "game-empty", "game-ready", "game-summary", "game-error", "replace-game", "remove-game",
    "import-status", "import-bar", "import-text", "build", "build-status", "build-bar", "build-text",
    "log-box", "log", "build-error", "step-done", "download-psp-zip", "download-psp-eboot", "download-3ds-app", "download-3ds-zip",
    "download-ps2-iso", "download-ps2-usb", "download-ps3-pkg", "download-vita-vpk", "download-vita-data", "download-wii-zip",
    "download-wiiu-zip", "download-xbox360-zip", "step-cia", "make-cia", "cia-status", "cia-bar", "cia-text", "cia-error",
  ].map((id) => [id.replace(/-(\w)/g, (_, c) => c.toUpperCase()), document.getElementById(id)]),
);

const consoleInputs = [...document.querySelectorAll('input[name="console"]')];

const state = {
  game: null,
  target: "psp",
  output: null,
  files: null,
  builtFor: null,
  cia: null,
  building: false,
  makingCia: false,
};

const builder = new Builder();
const theme = new Theme();

function showProgress(status, bar, label, fraction, text, running = true) {
  status.hidden = false;
  bar.style.width = `${Math.round(Math.min(1, Math.max(0, fraction)) * 100)}%`;
  label.textContent = text;
  theme.progress(status, fraction, running);
}

function showImportProgress(fraction, text) {
  showProgress(ui.importStatus, ui.importBar, ui.importText, fraction, text);
}

function showBuildProgress(fraction, text, running = true) {
  showProgress(ui.buildStatus, ui.buildBar, ui.buildText, fraction, text, running);
}

function setError(element, message) {
  element.hidden = !message;
  element.textContent = message ?? "";
}

function readSavedConsole() {
  try {
    return localStorage.getItem(CONSOLE_KEY);
  } catch {
    return null;
  }
}

function saveConsole(target) {
  try {
    localStorage.setItem(CONSOLE_KEY, target);
  } catch {}
}

function renderConsole() {
  for (const element of document.querySelectorAll("[data-console]")) element.hidden = element.dataset.console !== state.target;
  for (const input of consoleInputs) input.checked = input.value === state.target;
  document.title = `Coffin of Andy and Leyley for ${CONSOLES[state.target].name}`;
  const done = (state.output || state.files) && state.builtFor === state.target;
  ui.stepDone.hidden = !done;
  ui.stepCia.hidden = !done || state.target !== "3ds";
}

function renderGame() {
  const game = state.game;
  ui.gameEmpty.hidden = Boolean(game);
  ui.gameReady.hidden = !game;
  ui.build.disabled = !game || state.building;
  if (!game) return;

  const where = game.stored ? "" : " (only for this visit; this browser can't keep it)";
  ui.gameSummary.textContent = `Game ready: version ${game.version}, ${game.files.length} files${where}`;
  setError(
    ui.gameError,
    game.version === SUPPORTED_VERSION ? "" : `This port is made for version ${SUPPORTED_VERSION} and your copy is ${game.version}, so the build will most likely refuse it.`,
  );
}

async function importGame(readFiles) {
  setError(ui.gameError, "");
  showImportProgress(0, "Reading your files...");
  try {
    const { stored, listed } = sortFiles(await readFiles());
    const missing = missingGameParts(stored.map((file) => file.path));
    if (missing.length > 0) throw new Error(`This doesn't look like the game folder. Missing: ${missing.join(", ")}.`);

    const version = await readGameVersion(stored);
    const megabytes = Math.round(stored.reduce((sum, file) => sum + file.blob.size, 0) / 1048576);
    const index = await saveGame(stored, listed, version, (fraction) =>
      showImportProgress(fraction, `Saving the game in this browser: ${Math.round(fraction * megabytes)} of ${megabytes} MB`),
    );

    state.game = { version, files: stored, listed, stored: Boolean(index) };
    theme.progress(ui.importStatus, 1, false);
    ui.importStatus.hidden = true;
    renderGame();
    theme.wake(stored).then(() => theme.greet(true));
  } catch (error) {
    theme.progress(ui.importStatus, 0, false);
    ui.importStatus.hidden = true;
    setError(ui.gameError, error.message ?? String(error));
  }
}

function gameFilesForBuild() {
  const listed = state.game.listed.map((path) => ({ path, blob: null }));
  return [...state.game.files, ...listed];
}

async function runBuild(target, env) {
  ui.log.textContent = "";
  ui.logBox.hidden = false;
  setError(ui.buildError, "");
  showBuildProgress(0, "Getting ready...");

  return builder.run({
    target,
    gameFiles: gameFilesForBuild(),
    env,
    onProgress: showBuildProgress,
    onLine: (line) => {
      ui.log.textContent += `${line}\n`;
    },
  });
}

function finishBuild(target, result) {
  if (result.ok) {
    state.output = result.output;
    state.files = result.files;
    state.builtFor = target;
    const bytes = result.files ? result.files.reduce((sum, file) => sum + file.bytes.length, 0) : result.output.length;
    const megabytes = (bytes / 1048576).toFixed(1);
    showBuildProgress(1, `Done in ${Math.round(result.seconds)} seconds: ${megabytes} MB`, false);
    renderConsole();
    ui.stepDone.scrollIntoView({ behavior: "smooth", block: "start" });
    theme.celebrate();
    return;
  }

  showBuildProgress(0, "Stopped.", false);
  theme.warn();
  if (result.crashed) {
    setError(ui.buildError, `The build crashed in this browser. Please try a current Chrome, Edge or Firefox. (${result.message})`);
    return;
  }
  const reason = result.lines.filter((line) => /error/i.test(line)).pop() ?? result.lines.at(-1) ?? "";
  setError(ui.buildError, `The build failed. ${reason}`);
}

function lockConsoles(locked) {
  for (const input of consoleInputs) input.disabled = locked;
}

async function startBuild() {
  const target = state.target;
  state.building = true;
  ui.build.disabled = true;
  lockConsoles(true);
  state.output = null;
  state.files = null;
  state.builtFor = null;
  state.cia = null;
  ui.makeCia.textContent = "Make tcoaal.cia";
  ui.ciaStatus.hidden = true;
  setError(ui.ciaError, "");
  renderConsole();

  try {
    let result = await runBuild(target, {});
    if (result.crackReason) {
      const accepted = await showCrackWarning(result.crackReason);
      if (!accepted) {
        showBuildProgress(0, "Stopped. Buy the game on Steam, then build again.", false);
        return;
      }
      result = await runBuild(target, { TCOAAL_CRACK_OK: "1" });
    }
    finishBuild(target, result);
  } catch (error) {
    showBuildProgress(0, "Stopped.", false);
    theme.warn();
    setError(ui.buildError, error.message?.startsWith("Couldn't download") ? error.message : `Couldn't read the stored game files. Try choosing the game again. (${error.message ?? error})`);
  } finally {
    state.building = false;
    ui.build.disabled = !state.game;
    lockConsoles(false);
  }
}

function download(blob, name) {
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = name;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 60000);
}

function downloadZip(tree, name) {
  download(new Blob([zipSync(tree)], { type: "application/zip" }), name);
}

function outputFile(path) {
  return state.files?.find((file) => file.path === path)?.bytes ?? null;
}

function downloadFolder(prefix, name) {
  const tree = {};
  for (const file of state.files ?? []) {
    if (!file.path.startsWith(prefix)) continue;
    const parts = file.path.split("/");
    let node = tree;
    for (const part of parts.slice(0, -1)) node = node[part] ??= {};
    node[parts.at(-1)] = [file.bytes, { level: 0 }];
  }
  downloadZip(tree, name);
}

function isoFile(iso, name) {
  const view = new DataView(iso.buffer, iso.byteOffset, iso.byteLength);
  const pvd = 16 * 2048;
  const root = view.getUint32(pvd + 158, true);
  const size = view.getUint32(pvd + 166, true);
  const want = `${name};1`;
  let pos = root * 2048;
  const end = pos + size;
  while (pos < end) {
    const length = iso[pos];
    if (!length) {
      pos = (Math.floor(pos / 2048) + 1) * 2048;
      continue;
    }
    const nameLength = iso[pos + 32];
    const found = new TextDecoder().decode(iso.subarray(pos + 33, pos + 33 + nameLength));
    if (found === want) {
      const start = view.getUint32(pos + 2, true) * 2048;
      return iso.subarray(start, start + view.getUint32(pos + 10, true));
    }
    pos += length;
  }
  return null;
}

function downloadUsb() {
  const elf = isoFile(state.output, "TCAL_030.12");
  const pak = isoFile(state.output, "TCOAAL.PAK");
  if (!elf || !pak) return;
  downloadZip({ TCOAAL: { "TCOAAL.ELF": [elf, { level: 6 }], "TCOAAL.PAK": [pak, { level: 0 }] } }, "TCOAAL-USB.zip");
}

async function makeCia() {
  if (!state.output || state.builtFor !== "3ds" || state.makingCia || state.building) return;
  if (state.cia) {
    download(new Blob([state.cia]), "tcoaal.cia");
    return;
  }
  state.makingCia = true;
  ui.makeCia.disabled = true;
  ui.build.disabled = true;
  lockConsoles(true);
  setError(ui.ciaError, "");
  showProgress(ui.ciaStatus, ui.ciaBar, ui.ciaText, 0.3, "Making tcoaal.cia...");
  try {
    const result = await builder.makeCia({
      app: state.output,
      onLine: (line) => {
        ui.log.textContent += `${line}\n`;
      },
    });
    if (!result.ok) {
      showProgress(ui.ciaStatus, ui.ciaBar, ui.ciaText, 0, "Stopped.", false);
      theme.warn();
      const reason = result.crashed ? result.message : (result.lines.filter((line) => /error/i.test(line)).pop() ?? "");
      setError(ui.ciaError, `Making the CIA failed. ${reason}`);
      return;
    }
    state.cia = result.output;
    showProgress(ui.ciaStatus, ui.ciaBar, ui.ciaText, 1, `Done: ${(state.cia.length / 1048576).toFixed(1)} MB`, false);
    theme.celebrate();
    ui.makeCia.textContent = "Download tcoaal.cia";
    download(new Blob([state.cia]), "tcoaal.cia");
  } finally {
    state.makingCia = false;
    ui.makeCia.disabled = false;
    ui.build.disabled = !state.game;
    lockConsoles(false);
  }
}

function setUpDropZone() {
  const highlight = (on) => (event) => {
    event.preventDefault();
    ui.drop.classList.toggle("over", on);
  };
  ui.drop.addEventListener("dragenter", highlight(true));
  ui.drop.addEventListener("dragover", highlight(true));
  ui.drop.addEventListener("dragleave", highlight(false));
  ui.drop.addEventListener("drop", (event) => {
    highlight(false)(event);
    const items = [...event.dataTransfer.items];
    const single = event.dataTransfer.files.length === 1 ? event.dataTransfer.files[0] : null;
    if (single && isZip(single)) {
      importGame(() => filesFromZip(single, (fraction) => showImportProgress(fraction, `Unpacking ${single.name}: ${Math.round(fraction * 100)}%`)));
    } else {
      importGame(() => filesFromDrop(items, (count) => showImportProgress(0, `Reading folder: ${count} files found`)));
    }
  });
}

function setUpControls() {
  ui.pickFolder.addEventListener("change", (event) => {
    const files = filesFromInput(event.target.files);
    event.target.value = "";
    if (files.length > 0) importGame(async () => files);
  });

  ui.pickZip.addEventListener("change", (event) => {
    const zip = event.target.files[0];
    event.target.value = "";
    if (zip) importGame(() => filesFromZip(zip, (fraction) => showImportProgress(fraction, `Unpacking ${zip.name}: ${Math.round(fraction * 100)}%`)));
  });

  ui.replaceGame.addEventListener("click", () => {
    ui.gameEmpty.hidden = false;
    ui.gameReady.hidden = true;
  });

  ui.removeGame.addEventListener("click", async () => {
    if (!confirm("Remove the stored game files from this browser? Your saves on the console are not affected.")) return;
    await clearGame();
    state.game = null;
    theme.sleep();
    renderGame();
  });

  for (const input of consoleInputs) {
    input.addEventListener("change", () => {
      if (!input.checked) return;
      state.target = input.value;
      saveConsole(state.target);
      renderConsole();
    });
  }

  ui.build.addEventListener("click", startBuild);
  ui.downloadPspZip.addEventListener("click", () =>
    downloadZip({ PSP: { GAME: { TCOAAL: { "EBOOT.PBP": [state.output, { level: 0 }] } } } }, "TCOAAL-PSP.zip"),
  );
  ui.downloadPspEboot.addEventListener("click", () => download(new Blob([state.output]), "EBOOT.PBP"));
  ui.download3dsApp.addEventListener("click", () => download(new Blob([state.output]), "tcoaal.3dsx"));
  ui.download3dsZip.addEventListener("click", () => downloadZip({ "3ds": { tcoaal: { "tcoaal.3dsx": [state.output, { level: 0 }] } } }, "TCOAAL-3DS.zip"));
  ui.downloadPs2Iso.addEventListener("click", () => download(new Blob([state.output]), "TCOAAL.iso"));
  ui.downloadPs2Usb.addEventListener("click", downloadUsb);
  ui.downloadPs3Pkg.addEventListener("click", () => download(new Blob([state.output]), "TCOAAL.pkg"));
  ui.downloadVitaVpk.addEventListener("click", () => {
    const vpk = outputFile("TCOAAL.vpk");
    if (vpk) download(new Blob([vpk]), "TCOAAL.vpk");
  });
  ui.downloadVitaData.addEventListener("click", () => downloadFolder("data/", "TCOAAL-Vita-data.zip"));
  ui.downloadWiiZip.addEventListener("click", () => downloadFolder("apps/", "TCOAAL-Wii.zip"));
  ui.downloadWiiuZip.addEventListener("click", () => downloadFolder("wiiu/", "TCOAAL-WiiU.zip"));
  ui.downloadXbox360Zip.addEventListener("click", () => downloadFolder("", "TCOAAL-Xbox360.zip"));
  ui.makeCia.addEventListener("click", makeCia);

  window.addEventListener("beforeunload", (event) => {
    if (state.building || state.makingCia) event.preventDefault();
  });
}

async function restoreGame() {
  const saved = await loadGame();
  if (!saved) return null;
  return { version: saved.index.version, files: saved.files, listed: saved.index.listed, stored: true };
}

async function start() {
  const saved = readSavedConsole();
  if (saved && CONSOLES[saved]) state.target = saved;
  setUpDropZone();
  setUpControls();
  renderConsole();
  state.game = await restoreGame();
  renderGame();
  if (state.game) theme.wake(state.game.files).then(() => theme.greet(false));
}

start();
