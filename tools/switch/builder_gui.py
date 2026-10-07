"""Wind Waker HD NX: the window of the Switch builder (tools/switch/builder.py does the work).

Choose your game (a Cemu .wua archive or the extracted game folder), build wwhd.nro, then copy it and the
game to your SD card. Started by the builder program (WindWakerHDNX.exe, tools/switch/launcher.c) with
its bundled Python; `python3 tools/switch/builder_gui.py` works from a source checkout too.
"""
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import builder  # noqa: E402

APP = "Wind Waker HD NX"
REPO_URL = "https://github.com/NotARanger97/WindWakerHDNX"


def data_dir():
    """Working files (translated and compiled game code, ~1 GB while building): never next to the program."""
    base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "WindWakerHDNX", "work")


def sdk_dir():
    for d in (os.path.join(builder.REPO, "sdk"), os.path.join(builder.REPO, "sdk-switch")):
        if os.path.isfile(os.path.join(d, "manifest.json")):
            return d
    return os.path.join(builder.REPO, "sdk")


def open_folder(path):
    if sys.platform == "win32":
        subprocess.Popen(["explorer", "/select,", os.path.normpath(path)])
    elif sys.platform == "darwin":
        subprocess.Popen(["open", "-R", path])
    else:
        subprocess.Popen(["xdg-open", os.path.dirname(path)])


class App:
    def __init__(self, root):
        self.root = root
        self.events = queue.Queue()
        self.game = ""
        self.out = ""
        self.busy = False
        self.stop = False
        self.t0 = 0
        root.title(APP)
        root.minsize(620, 430)
        try:
            ttk.Style().theme_use("vista" if sys.platform == "win32" else "clam")
        except tk.TclError:
            pass
        pad = {"padx": 16}
        frame = ttk.Frame(root, padding=(0, 14, 0, 14))
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text=APP, font=("Segoe UI", 16, "bold")).pack(anchor="w", **pad)
        ttk.Label(frame, wraplength=580, justify="left",
                  text="Makes the Nintendo Switch homebrew of The Wind Waker HD from your own copy of the Wii U game "
                       "(Europe or USA, without the update). Nothing is installed; your game stays on this PC."
                  ).pack(anchor="w", pady=(4, 12), **pad)

        box = ttk.LabelFrame(frame, text=" 1. Your game ", padding=10)
        box.pack(fill="x", **pad)
        self.game_var = tk.StringVar(value="No game chosen yet")
        ttk.Label(box, textvariable=self.game_var, wraplength=560).pack(anchor="w")
        row = ttk.Frame(box)
        row.pack(anchor="w", pady=(8, 0))
        self.pick_wua = ttk.Button(row, text="Choose your .wua (Cemu archive)...", command=self.choose_wua)
        self.pick_wua.pack(side="left")
        self.pick_dir = ttk.Button(row, text="or a game folder...", command=self.choose_folder)
        self.pick_dir.pack(side="left", padx=(8, 0))

        box = ttk.LabelFrame(frame, text=" 2. Build ", padding=10)
        box.pack(fill="x", pady=(10, 0), **pad)
        row = ttk.Frame(box)
        row.pack(fill="x")
        self.build_btn = ttk.Button(row, text="Build wwhd.nro", command=self.start_build, state="disabled")
        self.build_btn.pack(side="left")
        self.status = tk.StringVar(value="About 5 minutes on a typical PC.")
        ttk.Label(row, textvariable=self.status).pack(side="left", padx=(12, 0))
        self.bar = ttk.Progressbar(box, mode="determinate", maximum=1000)
        self.bar.pack(fill="x", pady=(10, 0))

        box = ttk.LabelFrame(frame, text=" 3. Copy to your Switch ", padding=10)
        box.pack(fill="x", pady=(10, 0), **pad)
        self.done_var = tk.StringVar(value="Copies wwhd.nro and your game to the SD card (switch/wwhd/).")
        ttk.Label(box, textvariable=self.done_var, wraplength=560, justify="left").pack(anchor="w")
        row = ttk.Frame(box)
        row.pack(anchor="w", pady=(8, 0))
        self.copy_btn = ttk.Button(row, text="Copy to SD card...", command=self.copy_to_sd, state="disabled")
        self.copy_btn.pack(side="left")
        self.show_btn = ttk.Button(row, text="Show wwhd.nro", command=lambda: open_folder(self.out), state="disabled")
        self.show_btn.pack(side="left", padx=(8, 0))
        ttk.Button(row, text="How to play", command=self.how_to_play).pack(side="left", padx=(8, 0))

        self.details = tk.Text(frame, height=6, wrap="word", font=("Consolas", 9), relief="flat",
                               background=root.cget("background"))
        self.details.pack(fill="both", expand=True, pady=(10, 0), **pad)
        self.details.configure(state="disabled")
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(100, self.poll)

    # ---- choosing the game
    def set_game(self, path):
        self.game = path
        self.game_var.set(path)
        self.build_btn.configure(state="normal")
        self.out = os.path.join(os.path.dirname(path) if path.lower().endswith(".wua") else path, "wwhd.nro")

    def choose_wua(self):
        p = filedialog.askopenfilename(title="Your Wind Waker HD Cemu archive",
                                       filetypes=[("Cemu archive", "*.wua"), ("All files", "*.*")])
        if p:
            self.set_game(os.path.normpath(p))

    def choose_folder(self):
        p = filedialog.askdirectory(title="The extracted game folder (with code, content and meta)")
        if p:
            p = os.path.normpath(p)
            if not os.path.isfile(os.path.join(p, "code", "cking.rpx")):
                messagebox.showerror(APP, "This folder has no code\\cking.rpx. Choose the game's own folder "
                                          "(the one with code, content and meta).")
                return
            self.set_game(p)

    # ---- building (a worker thread; the window polls its events)
    def log(self, text):
        self.events.put(("log", text))

    def start_build(self):
        self.busy, self.stop, self.t0 = True, False, time.time()
        for b in (self.build_btn, self.pick_wua, self.pick_dir, self.copy_btn, self.show_btn):
            b.configure(state="disabled")
        self.bar.configure(value=0)
        threading.Thread(target=self.build_thread, daemon=True).start()

    def build_thread(self):
        try:
            builder.build(self.game, self.out, sdk_dir(), data_dir(), log=self.log,
                          progress=lambda step, done, total: self.events.put(("progress", step, done, total)),
                          cancel=lambda: self.stop)
            shutil.rmtree(data_dir(), ignore_errors=True)  # ~1 GB of working files
            self.events.put(("done",))
        except builder.BuildError as e:
            self.events.put(("error", str(e)))
        except Exception as e:  # noqa: BLE001 - shown to the player, with the details above
            self.events.put(("error", "%s: %s" % (type(e).__name__, e)))

    # steps' share of the bar: translating ~60 %, compiling ~38 %, the rest
    SPAN = {"prepare": (0, 10), "translate": (10, 600), "compile": (600, 980), "link": (980, 1000), "done": (1000, 1000)}

    def poll(self):
        try:
            while True:
                ev = self.events.get_nowait()
                kind = ev[0]
                if kind == "log":
                    self.details.configure(state="normal")
                    self.details.insert("end", ev[1] + "\n")
                    self.details.see("end")
                    self.details.configure(state="disabled")
                elif kind == "progress":
                    step, done, total = ev[1:]
                    lo, hi = self.SPAN.get(step, (0, 0))
                    self.step = step
                    self.bar.configure(value=lo + (hi - lo) * (done / total if total else 0))
                elif kind == "done":
                    self.busy = False
                    self.bar.configure(value=1000)
                    self.status.set("Done in %d:%02d." % divmod(int(time.time() - self.t0), 60))
                    self.done_var.set("Ready: %s\nNow copy it and your game to the SD card." % self.out)
                    for b in (self.build_btn, self.pick_wua, self.pick_dir, self.copy_btn, self.show_btn):
                        b.configure(state="normal")
                elif kind == "error":
                    self.busy = False
                    self.status.set("Stopped.")
                    for b in (self.build_btn, self.pick_wua, self.pick_dir):
                        b.configure(state="normal")
                    if ev[1] != "stopped":
                        messagebox.showerror(APP, ev[1])
                elif kind == "copied":
                    self.busy = False
                    self.status.set("")
                    for b in (self.build_btn, self.pick_wua, self.pick_dir, self.copy_btn, self.show_btn):
                        b.configure(state="normal")
                    messagebox.showinfo(APP, ev[1])
        except queue.Empty:
            pass
        if self.busy:
            elapsed = int(time.time() - self.t0)
            names = {"prepare": "Preparing", "translate": "Translating the game code", "compile": "Compiling",
                     "link": "Linking", "copy": "Copying to the SD card"}
            self.status.set("%s... %d:%02d" % (names.get(getattr(self, "step", ""), "Working"), elapsed // 60, elapsed % 60))
            if getattr(self, "step", "") == "translate":  # no fine-grained progress: creep towards its end
                lo, hi = self.SPAN["translate"]
                self.bar.configure(value=lo + (hi - lo) * min(0.97, elapsed / 200.0))
        self.root.after(200, self.poll)

    # ---- copying to the SD card
    def copy_to_sd(self):
        root = filedialog.askdirectory(title="Your Switch's SD card (the card itself, its top folder)")
        if not root:
            return
        root = os.path.normpath(root)
        if not os.path.isdir(os.path.join(root, "switch")):
            if not messagebox.askyesno(APP, "%s has no 'switch' folder, so it does not look like a Switch SD card "
                                             "with homebrew. Copy there anyway?" % root):
                return
        self.busy, self.t0, self.step = True, time.time(), "copy"
        for b in (self.build_btn, self.pick_wua, self.pick_dir, self.copy_btn, self.show_btn):
            b.configure(state="disabled")
        threading.Thread(target=self.copy_thread, args=(root,), daemon=True).start()

    def copy_thread(self, root):
        try:
            dest = os.path.join(root, "switch", "wwhd")
            os.makedirs(dest, exist_ok=True)
            shutil.copyfile(self.out, os.path.join(dest, "wwhd.nro"))
            if self.game.lower().endswith(".wua"):
                target = os.path.join(dest, "Wind Waker HD.wua")  # the runtime looks for "Wind Waker" in the name
                if not (os.path.isfile(target) and os.path.getsize(target) == os.path.getsize(self.game)):
                    self.copy_file(self.game, target)
            else:
                for part in ("code", "content", "meta"):
                    shutil.copytree(os.path.join(self.game, part), os.path.join(dest, "game", part), dirs_exist_ok=True)
            self.events.put(("copied", "Copied to %s.\n\nOn the Switch: start the homebrew menu in title takeover "
                                       "mode (hold R while starting any game) and choose Wind Waker HD." % dest))
        except OSError as e:
            self.events.put(("error", "Copying to the SD card failed: %s" % e))

    def copy_file(self, src, dst):
        total, done = os.path.getsize(src), 0
        with open(src, "rb") as fi, open(dst + ".part", "wb") as fo:
            while True:
                chunk = fi.read(8 << 20)
                if not chunk:
                    break
                fo.write(chunk)
                done += len(chunk)
                self.events.put(("progress", "copy", done, total))
        os.replace(dst + ".part", dst)

    def how_to_play(self):
        messagebox.showinfo(APP, "1. Copy wwhd.nro and your game to the SD card (button 'Copy to SD card').\n"
                                 "2. On the Switch (Atmosphere): hold R while starting any game to open the homebrew "
                                 "menu with full memory, then choose Wind Waker HD.\n"
                                 "3. For 30 fps the Switch needs raised clocks for this title (CPU 1785 MHz, memory "
                                 "1996 MHz) with sys-clk or horizon-oc.\n\nDetails: %s" % REPO_URL)

    def close(self):
        if self.busy and not messagebox.askyesno(APP, "Stop and quit?"):
            return
        self.stop = True
        self.root.destroy()


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
