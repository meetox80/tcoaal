const CHARACTER_DELAY = 26;
const CONFIRM_KEYS = new Set(["Enter", " ", "z", "Z"]);
const CANCEL_KEYS = new Set(["Escape", "x", "X"]);

export class MessageBox {
  constructor({ onConfirm } = {}) {
    this.onConfirm = onConfirm;
    this.layer = document.createElement("div");
    this.layer.className = "talk";
    this.layer.hidden = true;
    this.layer.innerHTML = `
      <img class="talk-bust" data-side="left" alt="" hidden>
      <img class="talk-bust" data-side="right" alt="" hidden>
      <div class="talk-box" role="dialog" aria-modal="true" aria-live="polite" tabindex="-1">
        <p class="talk-name"></p>
        <p class="talk-text"></p>
        <span class="talk-next" hidden></span>
      </div>`;
    document.body.append(this.layer);
    this.box = this.layer.querySelector(".talk-box");
    this.nameEl = this.layer.querySelector(".talk-name");
    this.textEl = this.layer.querySelector(".talk-text");
    this.nextEl = this.layer.querySelector(".talk-next");
    this.busts = {
      left: this.layer.querySelector('[data-side="left"]'),
      right: this.layer.querySelector('[data-side="right"]'),
    };
    this.active = null;
    this.busy = false;
    this.chain = Promise.resolve();

    this.layer.addEventListener("pointerdown", (event) => {
      event.preventDefault();
      this.confirm();
    });
    this.onKey = (event) => {
      if (!this.active) return;
      if (CONFIRM_KEYS.has(event.key)) this.confirm();
      else if (CANCEL_KEYS.has(event.key)) this.cancel();
      else return;
      event.preventDefault();
      event.stopPropagation();
    };
    window.addEventListener("keydown", this.onKey, true);
  }

  get open() {
    return Boolean(this.active) || this.busy;
  }

  conversation(lines) {
    const run = this.chain.then(async () => {
      this.busy = true;
      const previousFocus = document.activeElement;
      try {
        for (const line of lines) {
          const result = await this.show(line);
          if (result === "cancel") break;
        }
      } finally {
        this.busy = false;
        this.layer.hidden = true;
        for (const img of Object.values(this.busts)) img.hidden = true;
        previousFocus?.focus?.({ preventScroll: true });
      }
    });
    this.chain = run.catch(() => {});
    return run;
  }

  show({ name, segments, color, bust, side }) {
    return new Promise((resolve) => {
      this.layer.hidden = false;
      this.nameEl.textContent = name ?? "";
      this.nameEl.hidden = !name;
      this.textEl.style.color = color ?? "";
      const spans = segments.map((segment) => {
        const span = document.createElement("span");
        if (segment.color) span.style.color = segment.color;
        if (segment.italic) span.style.fontStyle = "italic";
        if (segment.bold) span.style.fontWeight = "700";
        return span;
      });
      this.textEl.replaceChildren(...spans);
      this.nextEl.hidden = true;
      for (const [where, img] of Object.entries(this.busts)) {
        const shown = Boolean(bust) && where === side;
        if (shown && img.src !== bust) img.src = bust;
        img.hidden = !shown;
        img.toggleAttribute("data-speaking", shown);
      }
      this.box.focus({ preventScroll: true });
      const state = { parts: segments.map((segment) => [...segment.text]), spans, part: 0, at: 0, timer: null, typing: true, resolve };
      this.active = state;
      this.typeNext(state);
    });
  }

  typeNext(state) {
    if (state !== this.active) return;
    while (state.part < state.parts.length && state.at >= state.parts[state.part].length) {
      state.part++;
      state.at = 0;
    }
    if (state.part >= state.parts.length) {
      this.finishTyping(state);
      return;
    }
    const ch = state.parts[state.part][state.at++];
    state.spans[state.part].textContent += ch;
    const pause = ch === "." || ch === "!" || ch === "?" ? CHARACTER_DELAY * 3 : CHARACTER_DELAY;
    state.timer = setTimeout(() => this.typeNext(state), pause);
  }

  finishTyping(state) {
    clearTimeout(state.timer);
    state.typing = false;
    state.spans.forEach((span, i) => {
      span.textContent = state.parts[i].join("");
    });
    this.nextEl.hidden = false;
  }

  confirm() {
    const state = this.active;
    if (!state) return;
    if (state.typing) {
      this.finishTyping(state);
      return;
    }
    this.onConfirm?.();
    this.close("next");
  }

  cancel() {
    const state = this.active;
    if (!state) return;
    this.close("cancel");
  }

  close(result) {
    const state = this.active;
    clearTimeout(state.timer);
    this.active = null;
    state.resolve(result);
  }

  release() {
    if (this.active) this.close("cancel");
    window.removeEventListener("keydown", this.onKey, true);
    this.layer.remove();
  }
}
