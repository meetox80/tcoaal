const SOUND_PREF = "tcoaal-sound";
const MUSIC_PREF = "tcoaal-music";

function readPref(name, fallback) {
  try {
    const value = localStorage.getItem(name);
    return value === null ? fallback : value === "on";
  } catch {
    return fallback;
  }
}

function writePref(name, on) {
  try {
    localStorage.setItem(name, on ? "on" : "off");
  } catch {}
}

export class Sounds {
  constructor(assets) {
    this.assets = assets;
    this.buffers = new Map();
    this.enabled = readPref(SOUND_PREF, true);
    this.musicOn = readPref(MUSIC_PREF, false);
    this.music = null;
    this.musicPath = null;
    this.musicUrls = new Map();
    this.context = null;
    try {
      this.context = new AudioContext();
    } catch {
      this.context = null;
    }
    this.unlock = () => this.context?.resume().catch(() => {});
    window.addEventListener("pointerdown", this.unlock, true);
    window.addEventListener("keydown", this.unlock, true);
  }

  get available() {
    return Boolean(this.context);
  }

  systemPath(index) {
    const name = this.assets.system.sounds?.[index]?.name;
    return name ? `audio/se/${name}.ogg` : null;
  }

  buffer(path) {
    if (!this.buffers.has(path)) {
      this.buffers.set(
        path,
        this.assets
          .sound(path)
          .then((blob) => (blob ? blob.arrayBuffer() : null))
          .then((data) => (data && this.context ? this.context.decodeAudioData(data) : null))
          .catch(() => null),
      );
    }
    return this.buffers.get(path);
  }

  preload(paths) {
    for (const path of paths) if (path) this.buffer(path);
  }

  async play(path, { volume = 0.5, rate = 1 } = {}) {
    if (!path || !this.enabled || !this.context || this.context.state !== "running") return;
    const buffer = await this.buffer(path);
    if (!buffer) return;
    const source = this.context.createBufferSource();
    const gain = this.context.createGain();
    source.buffer = buffer;
    source.playbackRate.value = rate;
    gain.gain.value = volume;
    source.connect(gain).connect(this.context.destination);
    source.start();
  }

  setEnabled(on) {
    this.enabled = on;
    writePref(SOUND_PREF, on);
  }

  canPlayMusic() {
    return document.createElement("audio").canPlayType('audio/ogg; codecs="vorbis"') !== "";
  }

  musicUrl(path) {
    if (!this.musicUrls.has(path)) {
      this.musicUrls.set(
        path,
        this.assets.sound(path).then((blob) => (blob ? URL.createObjectURL(blob) : null)),
      );
    }
    return this.musicUrls.get(path);
  }

  async setTrack(path) {
    this.musicPath = path;
    await this.applyMusic();
  }

  async setMusic(on) {
    this.musicOn = on;
    writePref(MUSIC_PREF, on);
    await this.applyMusic();
  }

  async applyMusic() {
    const path = this.musicPath;
    if (!this.musicOn || !path) {
      this.music?.pause();
      return;
    }
    const url = await this.musicUrl(path);
    if (!url || path !== this.musicPath || !this.musicOn) return;
    if (!this.music) {
      this.music = new Audio();
      this.music.loop = true;
      this.music.volume = 0.4;
    }
    if (this.music.src !== url) this.music.src = url;
    this.music.play().catch(() => {});
  }

  release() {
    window.removeEventListener("pointerdown", this.unlock, true);
    window.removeEventListener("keydown", this.unlock, true);
    this.music?.pause();
    this.music = null;
    this.context?.close().catch(() => {});
    for (const promise of this.musicUrls.values()) promise.then((url) => url && URL.revokeObjectURL(url));
    this.musicUrls.clear();
  }
}
