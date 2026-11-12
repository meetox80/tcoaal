import { openGameAssets } from "./assets.js";
import { MessageBox } from "./message.js";
import { Sounds } from "./sound.js";
import { Direction, Sprite, Ticker, prefersStill } from "./sprite.js";

const CELL_W = 48;
const CELL_H = 96;
const BALLOON = 48;
const TITLE_W = 1280;
const STRIP = { x: 249, y: 306, w: 783, h: 166 };
const TAPE = { x: 97, w: 505, h: 43, on: 4, off: 53 };
const WALK = { speed: 180, step: 250 };
const DASH = { speed: 360, step: 200, gap: CELL_W * 2 };
const PAIR_W = CELL_W * 2;
const LEAD_X = CELL_W * 1.5;
const KONAMI =["ArrowUp", "ArrowUp", "ArrowDown", "ArrowDown", "ArrowLeft", "ArrowRight", "ArrowLeft", "ArrowRight", "b", "a"];
const IDLE_MS = 90000;

const SOUND = { cursor: 0, ok: 1, cancel: 2, equip: 5 };
const BALLOONS = { surprise: 0, star: 1, skull: 2, heart: 3, swirl: 4, eye: 5, angry: 6, silence: 7, andrewHeart: 8, ashleyHeart: 9 };
const ICONS = { toggles: "options", footer: "credits" };

const PEOPLE = {
  ashley: { actor: 1, label: "x86GCl9v", color: 2, side: "right", face: "38b82c55b3529488" },
  andrew: { actor: 2, label: "kR5Fy4cp", color: 1, side: "left", face: "430dce743b60280b" },
};

const TALKS = {
  welcome: [["ashley", "3DDh5ShF", "00dfc02ea4ecdd77"]],
  imported: [
    ["ashley", "nTDCdL6l", "f3e25bcfc5cece64"],
    ["andrew", "Xpp1RsZy"],
    ["andrew", "wyn3btbz", "e916b6077d0a55a7"],
  ],
  done: [
    ["ashley", "SrpZZmML", "c61a4be244711b5a"],
    ["andrew", "fsNTnb3r", "c1db28987323d0ff"],
    ["ashley", "DffhVzgD", "00dfc02ea4ecdd77"],
  ],
  failed: [["ashley", "C7DLKr2S", "9eae4abfe82d5cc2"]],
  tv: [
    ["andrew", "Y7d2MMj4"],
    ["ashley", "KQtQxx07"],
    ["andrew", "RzblwK1Y", "42f60877d70e2dc5"],
  ],
  couch: [
    ["ashley", "r9CCWNW5"],
    ["andrew", "79VLrdLx"],
  ],
  fridge: [
    ["andrew", "Y19k37kR"],
    ["ashley", "z62Z3WKS", "00dfc02ea4ecdd77"],
    ["ashley", "5JZj267W", "d730436f2a2b31a5"],
    ["andrew", "Slkwc75V", "c1db28987323d0ff"],
  ],
  door: [
    ["ashley", "JyySzG8v", "9baea3a9ab3cbc09"],
    ["andrew", "53kVkLnk", "42f60877d70e2dc5"],
  ],
  race: [["ashley", "0FZglcC0", "00dfc02ea4ecdd77"]],
  balcony: [
    ["andrew", "GGFgWtm5", "8ddc89c3cb615d12"],
    ["ashley", "0FZglcC0", "00dfc02ea4ecdd77"],
    ["andrew", "sNhtC9tH", "055fadd1e988e294"],
  ],
  praise: [
    ["ashley", "qdMcn5b2", "bc3f6c4a78c37314"],
    ["andrew", "C4VldH01", "6c40e35fcdd385a9"],
    ["ashley", "zYS3m2WK", "f3e25bcfc5cece64"],
  ],
  talk: [
    ["ashley", "TvZKXZfb"],
    ["andrew", "HGmdYhPn"],
    ["ashley", "1SQZG6Nn"],
    ["andrew", "S09zKGMM"],
  ],
};

const CLICKS = {
  ashley: [
    TALKS.praise,
    [
      ["ashley", "jv34wKks", "33b36b3ce42250e3"],
      ["andrew", "Q3Drq2ZN", "e916b6077d0a55a7"],
      ["ashley", "BVY0249d", "7bf9e1a7b3fc961a"],
    ],
    [
      ["ashley", "0kJcXC1C", "8437c57e159caae7"],
      ["andrew", "ng6TVmhG", "6e679186822ccbb6"],
    ],
    [["ashley", "150HmXgX", "34b15f34f103954d"]],
  ],
  andrew: [
    [
      ["andrew", "S09zKGMM"],
      ["andrew", "8vMBqY78", "497768a7223062f4"],
      ["ashley", "FR8P1xfm"],
    ],
    [["andrew", "08xVnPqM"]],
    [
      ["ashley", "rYS4WcJG", "eea509ea4222d93c"],
      ["andrew", "pK06S1qL", "ebb3e15de9e2290a"],
      ["ashley", "NYY1gkzY"],
    ],
    TALKS.talk,
  ],
};

const WORDS = { praise: "praise", pasta: "fridge", remote: "tv", couch: "couch", door: "door", balcony: "balcony", hello: "welcome" };

const HERO = {
  andrew: { left: 0, width: 36, balloon: [30, 2] },
  ashley: { left: 62, width: 38, balloon: [75, 8] },
};

function isTyping(target) {
  return target instanceof HTMLElement && (target.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName));
}

function cropUrl(bitmap, x, y, w, h) {
  const canvas = document.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  canvas.getContext("2d").drawImage(bitmap, x, y, w, h, 0, 0, w, h);
  return canvas.toDataURL();
}

function skinColors(bitmap) {
  const canvas = document.createElement("canvas");
  canvas.width = bitmap.width;
  canvas.height = bitmap.height;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  ctx.drawImage(bitmap, 0, 0);
  const colors = [];
  for (let n = 0; n < 32; n++) {
    const [r, g, b] = ctx.getImageData(96 + (n % 8) * 12 + 6, 144 + Math.floor(n / 8) * 12 + 6, 1, 1).data;
    colors.push(`rgb(${r} ${g} ${b})`);
  }
  return colors;
}

export class Theme {
  constructor() {
    this.awake = null;
    this.generation = 0;
    this.keys = [];
    this.letters = "";
    this.runners = new Map();
    this.hero = document.getElementById("hero");
    this.toggles = document.getElementById("toggles");
    this.soundToggle = document.getElementById("toggle-sound");
    this.musicToggle = document.getElementById("toggle-music");

    this.soundToggle.addEventListener("click", () => {
      const sounds = this.awake?.sounds;
      if (!sounds) return;
      sounds.setEnabled(!sounds.enabled);
      this.renderToggles();
    });
    this.musicToggle.addEventListener("click", () => {
      const sounds = this.awake?.sounds;
      if (!sounds) return;
      sounds.setMusic(!sounds.musicOn);
      this.renderToggles();
    });
    document.addEventListener("pointerover", (event) => this.hoverButton(event));
    document.addEventListener("focusin", (event) => this.focusButton(event));
    document.addEventListener("click", (event) => this.clickButton(event), true);
    window.addEventListener("keydown", (event) => this.secretKeys(event));
    for (const name of ["pointermove", "pointerdown", "keydown", "scroll", "wheel"]) {
      window.addEventListener(name, () => this.poke(), { passive: true });
    }
  }

  wake(files) {
    if (this.awake?.files === files) return Promise.resolve();
    const generation = ++this.generation;
    return this.load(files)
      .then((awake) => {
        if (generation !== this.generation) {
          awake.release();
          return;
        }
        this.teardown();
        this.awake = awake;
        this.show();
      })
      .catch((error) => console.warn("The page couldn't borrow the game's look:", error));
  }

  async load(files) {
    const assets = await openGameAssets(files);
    if (!assets) throw new Error("no readable System data");
    const { system } = assets;
    const actors = (await assets.json("data/Actors.json")) ?? [];
    const sheets = {};
    for (const [who, person] of Object.entries(PEOPLE)) {
      const actor = actors[person.actor];
      if (!actor?.characterName) throw new Error(`no sprite for ${who}`);
      sheets[who] = { image: await assets.image(`img/characters/${actor.characterName}.png`), index: actor.characterIndex ?? 0 };
    }
    const [skin, balloon, loading, title, strip, tapes] = await Promise.all([
      assets.image("img/system/Window.png"),
      assets.image("img/system/Balloon.png"),
      assets.image("img/system/Loading.png"),
      assets.image(`img/titles1/${system.title1Name}.png`),
      assets.image("img/system/msgimg_0.png"),
      assets.image("img/system/VNButtons.png"),
    ]);
    const icons = {};
    for (const [slot, name] of Object.entries(ICONS)) icons[slot] = await assets.image(`img/system/${name}.png`).catch(() => null);
    const font = new FontFace("Faustina Game", await assets.font("fonts/Faustina-Regular.ttf"));
    await font.load();
    const favicon = await assets.image("icon/icon.png").catch(() => null);
    const sounds = new Sounds(assets);
    sounds.preload(Object.values(SOUND).map((index) => sounds.systemPath(index)));
    const ticker = new Ticker();
    const box = new MessageBox({ onConfirm: () => sounds.play(sounds.systemPath(SOUND.ok), { volume: 0.35 }) });
    const faces = new Map();
    return {
      files,
      assets,
      sheets,
      skin,
      colors: skinColors(skin.bitmap),
      balloon,
      loading,
      title,
      strip,
      tapes,
      icons,
      font,
      favicon,
      sounds,
      ticker,
      box,
      faces,
      clicks: { ashley: 0, andrew: 0 },
      balloons: { ashley: null, andrew: null },
      release() {
        ticker.clear();
        box.release();
        sounds.release();
        document.fonts.delete(font);
        assets.release();
      },
    };
  }

  sleep() {
    this.generation++;
    this.teardown();
  }

  teardown() {
    const awake = this.awake;
    if (!awake) return;
    this.awake = null;
    clearTimeout(awake.idleTimer);
    awake.resize?.disconnect();
    awake.release();
    const root = document.documentElement;
    root.classList.remove("awake");
    for (const name of ["--strip", "--tape", "--tape-on", "--skin-pause", "--loading", "--text"]) root.style.removeProperty(name);
    for (const slot of Object.keys(ICONS)) root.style.removeProperty(`--icon-${slot}`);
    this.hero.replaceChildren();
    this.hero.hidden = true;
    this.toggles.hidden = true;
    for (const runner of this.runners.values()) runner.el.remove();
    this.runners.clear();
    document.querySelector('link[rel="icon"]')?.setAttribute("href", "data:,");
  }

  show() {
    const awake = this.awake;
    const root = document.documentElement;
    document.fonts.add(awake.font);
    root.style.setProperty("--strip", `url(${cropUrl(awake.strip.bitmap, STRIP.x, STRIP.y, STRIP.w, STRIP.h)})`);
    root.style.setProperty("--tape", `url(${cropUrl(awake.tapes.bitmap, TAPE.x, TAPE.off, TAPE.w, TAPE.h)})`);
    root.style.setProperty("--tape-on", `url(${cropUrl(awake.tapes.bitmap, TAPE.x, TAPE.on, TAPE.w, TAPE.h)})`);
    root.style.setProperty("--skin-pause", `url(${cropUrl(awake.skin.bitmap, 144, 96, 24, 24)})`);
    root.style.setProperty("--loading", `url(${awake.loading.url})`);
    root.style.setProperty("--text", awake.colors[0]);
    for (const [slot, icon] of Object.entries(awake.icons)) if (icon) root.style.setProperty(`--icon-${slot}`, `url(${icon.url})`);
    if (awake.favicon) document.querySelector('link[rel="icon"]')?.setAttribute("href", awake.favicon.url);
    root.classList.add("awake");

    this.buildHero();
    this.renderToggles();
    awake.sounds.setTrack(`audio/bgm/${awake.assets.system.titleBgm?.name}.ogg`);
    this.poke();
  }

  buildHero() {
    const awake = this.awake;
    const art = document.createElement("img");
    art.src = awake.title.url;
    art.alt = "";
    art.className = "hero-art";
    const people = Object.entries(HERO).map(([who, spot]) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "hero-who";
      button.setAttribute("aria-label", awake.assets.label(PEOPLE[who].label) ?? who);
      Object.assign(button.style, { left: `${spot.left}%`, width: `${spot.width}%` });
      button.addEventListener("click", () => this.clickPerson(who));
      return button;
    });
    this.hero.replaceChildren(art, ...people);
    this.hero.hidden = false;
    awake.resize = new ResizeObserver(() => {
      this.hero.style.setProperty("--balloon-scale", String((this.hero.clientWidth / TITLE_W) * 2));
    });
    awake.resize.observe(this.hero);
  }

  balloon(who, kind, { hold = false } = {}) {
    const awake = this.awake;
    if (!awake) return;
    awake.balloons[who]?.el.remove();
    const sprite = new Sprite(awake.balloon, { width: BALLOON, height: BALLOON, className: "balloon" });
    const [x, y] = HERO[who].balloon;
    Object.assign(sprite.el.style, { left: `${x}%`, top: `${y}%` });
    const row = BALLOONS[kind];
    sprite.frame(0, row);
    const entry = { el: sprite.el };
    awake.balloons[who] = entry;
    this.hero.append(sprite.el);
    if (prefersStill()) {
      sprite.frame(3, row);
      if (!hold) setTimeout(() => this.clearBalloon(who, entry), 1600);
      return;
    }
    const start = performance.now();
    awake.ticker.add((time) => {
      if (awake.balloons[who] !== entry) return false;
      const elapsed = time - start;
      const frame = Math.min(7, Math.floor(elapsed / 90));
      sprite.frame(hold ? Math.min(frame, 3 + (Math.floor(elapsed / 300) % 3)) : frame, row);
      if (hold || elapsed < 1500) return true;
      this.clearBalloon(who, entry);
      return false;
    });
  }

  clearBalloon(who, entry) {
    entry.el.remove();
    const awake = this.awake;
    if (awake?.balloons[who] === entry) awake.balloons[who] = null;
  }

  renderToggles() {
    const sounds = this.awake?.sounds;
    this.toggles.hidden = !sounds?.available;
    if (!sounds) return;
    this.soundToggle.textContent = sounds.enabled ? "sound on" : "sound off";
    this.soundToggle.setAttribute("aria-pressed", String(sounds.enabled));
    this.musicToggle.hidden = !sounds.canPlayMusic();
    this.musicToggle.textContent = sounds.musicOn ? "music on" : "music off";
    this.musicToggle.setAttribute("aria-pressed", String(sounds.musicOn));
  }

  async face(name) {
    const awake = this.awake;
    if (!awake.faces.has(name)) awake.faces.set(name, awake.assets.image(`img/faces/${name}[BUST]`).then((image) => image.url).catch(() => null));
    return awake.faces.get(name);
  }

  async talk(name) {
    const awake = this.awake;
    const lines = typeof name === "string" ? TALKS[name] : name;
    if (!awake || !lines || awake.box.open) return;
    const prepared = [];
    for (const [who, key, faceName] of lines) {
      const person = PEOPLE[who];
      const segments = awake.assets.line(key);
      if (!segments) continue;
      prepared.push({
        name: awake.assets.label(person.label),
        segments: segments.map((segment) => ({ ...segment, color: segment.color === null ? null : awake.colors[segment.color] })),
        color: awake.colors[person.color],
        bust: await this.face(faceName ?? person.face),
        side: person.side,
      });
    }
    if (prepared.length && this.awake === awake) await awake.box.conversation(prepared);
  }

  clickPerson(who) {
    const awake = this.awake;
    if (!awake || awake.box.open) return;
    this.balloon(who, "surprise");
    const options = CLICKS[who];
    const lines = options[awake.clicks[who] % options.length];
    awake.clicks[who]++;
    this.talk(lines);
  }

  poke() {
    const awake = this.awake;
    if (!awake) return;
    if (awake.sleeping) {
      awake.sleeping = false;
      for (const [who, entry] of Object.entries(awake.balloons)) if (entry) this.clearBalloon(who, entry);
    }
    clearTimeout(awake.idleTimer);
    awake.idleTimer = setTimeout(() => {
      if (this.awake !== awake || awake.box.open) return;
      awake.sleeping = true;
      this.balloon("ashley", "silence", { hold: true });
      this.balloon("andrew", "swirl", { hold: true });
    }, IDLE_MS);
  }

  secretKeys(event) {
    const awake = this.awake;
    if (!awake || awake.box.open || isTyping(event.target) || event.ctrlKey || event.metaKey || event.altKey) return;
    const key = event.key.length === 1 ? event.key.toLowerCase() : event.key;
    this.keys = [...this.keys, key].slice(-KONAMI.length);
    if (this.keys.join(",") === KONAMI.join(",")) {
      this.keys = [];
      this.race();
      return;
    }
    if (key.length !== 1) return;
    this.letters = (this.letters + key).slice(-12);
    for (const [word, talk] of Object.entries(WORDS)) {
      if (!this.letters.endsWith(word)) continue;
      this.letters = "";
      this.awake.sounds.play(this.awake.sounds.systemPath(SOUND.equip), { volume: 0.4 });
      this.talk(talk);
      return;
    }
  }

  race() {
    const awake = this.awake;
    if (!awake || awake.racing || prefersStill()) return;
    awake.racing = true;
    const lane = document.createElement("div");
    lane.className = "race";
    lane.setAttribute("aria-hidden", "true");
    const runners = ["ashley", "andrew"].map((who) => {
      const sheet = awake.sheets[who];
      const sprite = new Sprite(sheet.image, { width: CELL_W, height: CELL_H });
      sprite.character(sheet.index, 1, Direction.right);
      lane.append(sprite.el);
      return { sprite, index: sheet.index };
    });
    document.body.append(lane);
    this.talk("race");
    awake.sounds.play(awake.sounds.systemPath(SOUND.ok), { volume: 0.4 });
    const distance = window.innerWidth + 160;
    const duration = distance * 3.2;
    let start = null;
    awake.ticker.add((time) => {
      start ??= time;
      const elapsed = time - start;
      runners.forEach((runner, i) => {
        const lead = i === 0 ? 0 : 70 + Math.sin(elapsed / 300) * 30;
        runner.sprite.character(runner.index, [0, 1, 2, 1][Math.floor(elapsed / 110) % 4], Direction.right);
        runner.sprite.el.style.transform = `translateX(${(elapsed / duration) * distance - 60 - lead}px)`;
      });
      if (elapsed < duration && this.awake === awake) return true;
      lane.remove();
      awake.racing = false;
      return false;
    });
  }

  greet(fresh) {
    this.talk(fresh ? "imported" : "welcome");
  }

  celebrate() {
    const awake = this.awake;
    if (!awake) return;
    awake.sounds.play(awake.sounds.systemPath(SOUND.equip), { volume: 0.5 });
    this.balloon("ashley", "ashleyHeart");
    this.balloon("andrew", "andrewHeart");
    setTimeout(() => this.talk("done"), 700);
  }

  warn() {
    const awake = this.awake;
    if (!awake) return;
    awake.sounds.play(awake.sounds.systemPath(SOUND.cancel), { volume: 0.5 });
    this.balloon("ashley", "angry");
    this.talk("failed");
  }

  progress(status, fraction, running) {
    const awake = this.awake;
    if (!awake) return;
    let runner = this.runners.get(status);
    if (!runner) {
      const el = document.createElement("div");
      el.className = "runner";
      el.setAttribute("aria-hidden", "true");
      const sprites = ["andrew", "ashley"].map((who) => {
        const sheet = awake.sheets[who];
        const sprite = new Sprite(sheet.image, { width: CELL_W, height: CELL_H });
        el.append(sprite.el);
        return { sprite, index: sheet.index };
      });
      status.prepend(el);
      runner = { el, sprites, goal: 0, running: false, active: false, x: null, last: null, moved: -Infinity, dash: false };
      this.runners.set(status, runner);
    }
    runner.goal = Math.min(1, Math.max(0, fraction));
    runner.running = running;
    status.toggleAttribute("data-running", running);
    if (runner.active) return;
    runner.active = true;
    awake.ticker.add((time) => this.walk(status, runner, time));
  }

  walk(status, runner, time) {
    const width = status.clientWidth;
    const goal = Math.min(Math.max(0, width - PAIR_W), Math.max(0, runner.goal * width - LEAD_X));
    const elapsed = runner.last === null ? 0 : Math.min(100, time - runner.last);
    runner.last = time;
    if (runner.x === null || goal < runner.x - 1 || prefersStill()) {
      runner.x = goal;
    } else if (goal > runner.x) {
      runner.dash = goal - runner.x > DASH.gap;
      runner.x = Math.min(goal, runner.x + ((runner.dash ? DASH : WALK).speed * elapsed) / 1000);
      runner.moved = time;
    }
    runner.el.style.transform = `translateX(${runner.x}px)`;
    const pace = runner.dash ? DASH : WALK;
    const walking = time - runner.moved < pace.step;
    const pattern = walking ? [0, 1, 2, 1][Math.floor(time / pace.step) % 4] : 1;
    const direction = runner.running || walking ? Direction.right : Direction.down;
    runner.sprites.forEach(({ sprite, index }) => sprite.character(index, pattern, direction));
    if (runner.running || walking) return true;
    runner.active = false;
    runner.last = null;
    return false;
  }

  hoverButton(event) {
    const button = event.target.closest?.(".button, .choice, .hero-who");
    if (!button || button.disabled || button.contains(event.relatedTarget)) return;
    this.awake?.sounds.play(this.awake.sounds.systemPath(SOUND.cursor), { volume: 0.25 });
  }

  focusButton(event) {
    const button = event.target.closest?.(".button, .choice");
    if (!button || !event.target.matches(":focus-visible")) return;
    this.awake?.sounds.play(this.awake.sounds.systemPath(SOUND.cursor), { volume: 0.25 });
  }

  clickButton(event) {
    const button = event.target.closest?.(".button, #toggles button, .choice");
    if (!button || button.disabled || button.getAttribute("aria-disabled") === "true") return;
    const sound = button.classList.contains("subtle") ? SOUND.cancel : SOUND.ok;
    this.awake?.sounds.play(this.awake.sounds.systemPath(sound), { volume: 0.35 });
  }
}
