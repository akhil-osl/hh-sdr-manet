"""Operator window: Tkinter view over the controller. No board or file logic."""
from __future__ import annotations

import argparse
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from tkinter import font as tkfont

from .config import ConfigError
from .controller import CONNECTED, Busy, Controller

PAD = 8
POLL_MS = 100
PASS_COLOUR = "#1b5e20"
FAIL_COLOUR = "#b71c1c"
ARGS_SEP = " "


def short_path(path: str, limit: int = 72) -> str:
    return path if len(path) <= limit else "..." + path[-(limit - 3):]


def clock_text(record: dict) -> str:
    """HH:MM:SS.mmm from a record's started_utc."""
    return record["started_utc"][11:23]


class App:
    def __init__(self, root: tk.Tk, controller: Controller) -> None:
        self.root = root
        self.ctl = controller
        self._closing = False
        self._vars = {name: tk.StringVar(value="-") for name in (
            "config", "board", "address", "link", "board_id", "firmware",
            "status", "log", "counts")}

        root.title("Host test")
        root.minsize(900, 600)
        root.geometry("1000x680")
        self._style()
        self._build_menu()
        self._build_header()
        self._build_statusbar()
        self._build_body()
        self._bind_keys()
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        self._refresh()
        root.after(POLL_MS, self.tick)

    # -- construction ----------------------------------------------------

    def _style(self) -> None:
        style = ttk.Style(self.root)
        style.theme_use("clam")
        base = tkfont.nametofont("TkDefaultFont")
        self.bold = base.copy()
        self.bold.configure(weight="bold")
        self.mono = tkfont.nametofont("TkFixedFont")
        style.configure("Treeview", rowheight=base.metrics("linespace") + 6)
        style.configure("Heading.TLabel", font=self.bold)

    def _build_menu(self) -> None:
        bar = tk.Menu(self.root)
        file_menu = tk.Menu(bar, tearoff=False)
        file_menu.add_command(label="Load config...", command=self.choose_config)
        file_menu.add_separator()
        file_menu.add_command(label="Quit", command=self.on_close)
        run_menu = tk.Menu(bar, tearoff=False)
        run_menu.add_command(label="Run all", accelerator="Ctrl+R",
                             command=self.run_all)
        run_menu.add_command(label="Run selected", command=self.run_selected)
        run_menu.add_command(label="Cancel", accelerator="Esc", command=self.cancel)
        board_menu = tk.Menu(bar, tearoff=False)
        board_menu.add_command(label="Connect", command=self.connect)
        board_menu.add_command(label="Disconnect", command=self.disconnect)
        bar.add_cascade(label="File", menu=file_menu, underline=0)
        bar.add_cascade(label="Run", menu=run_menu, underline=0)
        bar.add_cascade(label="Board", menu=board_menu, underline=0)
        self.root.configure(menu=bar)
        self.menus = {"file": file_menu, "run": run_menu, "board": board_menu}

    def _build_header(self) -> None:
        frame = ttk.Frame(self.root, padding=PAD)
        frame.pack(fill="x")
        frame.columnconfigure(1, weight=1)
        frame.columnconfigure(3, weight=1)

        def pair(row, column, label, key, span=1):
            ttk.Label(frame, text=label, anchor="e", width=9).grid(
                row=row, column=column, sticky="e", padx=(0, PAD), pady=2)
            ttk.Label(frame, textvariable=self._vars[key], anchor="w").grid(
                row=row, column=column + 1, columnspan=span, sticky="w", pady=2)

        pair(0, 0, "Config", "config", span=3)
        self.load_button = ttk.Button(frame, text="Load...", command=self.choose_config)
        self.load_button.grid(row=0, column=4, sticky="e")
        pair(1, 0, "Board", "board")
        pair(1, 2, "Address", "address")
        pair(2, 0, "Link", "link")
        pair(2, 2, "Board ID", "board_id")
        pair(3, 0, "Status", "status")
        pair(3, 2, "Firmware", "firmware")
        ttk.Separator(self.root).pack(fill="x")

    def _build_body(self) -> None:
        body = ttk.Frame(self.root)
        body.pack(fill="both", expand=True, padx=PAD, pady=PAD)
        body.columnconfigure(0, weight=1)
        body.rowconfigure(0, weight=1)
        body.rowconfigure(1, weight=3)

        # Tests
        tests = ttk.Frame(body, padding=(0, 0, 0, PAD))
        ttk.Label(tests, text="Tests", style="Heading.TLabel").pack(anchor="w")
        columns = ("test", "command", "last", "time")
        self.tests = ttk.Treeview(tests, columns=columns, show="headings",
                                  selectmode="extended", height=4)
        for name, title, width in (("test", "Test", 180), ("command", "Command", 320),
                                   ("last", "Last result", 100), ("time", "Time", 90)):
            self.tests.heading(name, text=title, anchor="w")
            self.tests.column(name, width=width, anchor="w")
        self._tag_colours(self.tests)
        self.tests.pack(fill="both", expand=True, pady=(PAD // 2, PAD))
        buttons = ttk.Frame(tests)
        buttons.pack(fill="x")
        self.cancel_button = ttk.Button(buttons, text="Cancel", command=self.cancel)
        self.cancel_button.pack(side="right")
        self.run_button = ttk.Button(buttons, text="Run selected",
                                     command=self.run_selected)
        self.run_button.pack(side="right", padx=PAD)
        self.select_button = ttk.Button(buttons, text="Select all",
                                        command=self.select_all)
        self.select_button.pack(side="right")
        tests.grid(row=0, column=0, sticky="nsew")

        # Results
        results = ttk.Frame(body, padding=(0, 0, 0, PAD))
        ttk.Label(results, text="Results", style="Heading.TLabel").pack(anchor="w")
        holder = ttk.Frame(results)
        holder.pack(fill="both", expand=True, pady=(PAD // 2, 0))
        columns = ("time", "check", "result", "reason")
        self.results = ttk.Treeview(holder, columns=columns, show="headings",
                                    selectmode="browse", height=6)
        for name, title, width in (("time", "Time (UTC)", 110), ("check", "Check", 180),
                                   ("result", "Result", 70), ("reason", "Reason", 500)):
            self.results.heading(name, text=title, anchor="w")
            self.results.column(name, width=width, anchor="w")
        self._tag_colours(self.results)
        scroll = ttk.Scrollbar(holder, orient="vertical", command=self.results.yview)
        self.results.configure(yscrollcommand=scroll.set)
        self.results.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        self.results.bind("<<TreeviewSelect>>", self._show_detail)
        results.grid(row=1, column=0, sticky="nsew")

        # Detail
        detail = ttk.Frame(body)
        ttk.Label(detail, text="Selected result", style="Heading.TLabel").pack(anchor="w")
        self.detail = tk.Text(detail, height=4, wrap="word", font=self.mono,
                              relief="solid", borderwidth=1, state="disabled")
        self.detail.pack(fill="both", expand=True, pady=(PAD // 2, 0))
        detail.grid(row=2, column=0, sticky="nsew")

    def _build_statusbar(self) -> None:
        bar = ttk.Frame(self.root, padding=(PAD, 4))
        bar.pack(side="bottom", fill="x")
        ttk.Separator(self.root).pack(side="bottom", fill="x")
        ttk.Label(bar, textvariable=self._vars["log"], anchor="w").pack(side="left")
        ttk.Label(bar, textvariable=self._vars["counts"], anchor="e").pack(side="right")

    def _bind_keys(self) -> None:
        self.root.bind("<Control-r>", lambda event: self.run_all())
        self.root.bind("<Escape>", lambda event: self.cancel())

    @staticmethod
    def _tag_colours(tree: ttk.Treeview) -> None:
        tree.tag_configure("pass", foreground=PASS_COLOUR)
        tree.tag_configure("fail", foreground=FAIL_COLOUR)

    # -- actions ---------------------------------------------------------

    def choose_config(self) -> None:
        path = filedialog.askopenfilename(
            parent=self.root, title="Load config",
            filetypes=[("JSON", "*.json"), ("All files", "*")])
        if path:
            self.load_config(path)

    def load_config(self, path: str) -> bool:
        try:
            self.ctl.load(path)
        except ConfigError as exc:
            messagebox.showerror("Configuration error", str(exc), parent=self.root)
            return False
        except Busy:
            return False
        self.results.delete(*self.results.get_children())
        self.tests.delete(*self.tests.get_children())
        for test in self.ctl.session.config.tests:
            command = ARGS_SEP.join([test.command, *test.args])
            self.tests.insert("", "end", iid=test.id,
                              values=(test.id, command, "-", "-"))
        self.select_all()
        self._set_detail("")
        self._refresh()
        return True

    def select_all(self) -> None:
        self.tests.selection_set(self.tests.get_children())

    def run_all(self) -> None:
        self._start(self.ctl.run, None)

    def run_selected(self) -> None:
        chosen = set(self.tests.selection())
        if self.ctl.loaded and not chosen:
            messagebox.showwarning("Nothing to run", "Select at least one test.",
                                   parent=self.root)
            return
        ordered = [t.id for t in self.ctl.session.config.tests if t.id in chosen] \
            if self.ctl.loaded else []
        self._start(self.ctl.run, ordered)

    def connect(self) -> None:
        self._start(self.ctl.connect)

    def disconnect(self) -> None:
        try:
            self.ctl.disconnect()
        except Busy:
            pass
        self._refresh()

    def cancel(self) -> None:
        self.ctl.cancel()
        self._refresh()

    def _start(self, action, *args) -> None:
        if not self.ctl.loaded or self.ctl.running:
            return
        action(*args)
        self._refresh()

    def on_close(self) -> None:
        self._closing = True
        if self.ctl.running:
            self.ctl.cancel()
            self._vars["status"].set("Cancelling, closing when the board answers")
            return
        self._destroy()

    def _destroy(self) -> None:
        self.ctl.shutdown()
        self.root.destroy()

    # -- updates ---------------------------------------------------------

    def tick(self) -> None:
        """Apply worker events to the widgets. Runs on the GUI thread only."""
        for event in self.ctl.poll():
            if event[0] == "record":
                self._add_record(event[1])
        self._refresh()
        if self._closing and not self.ctl.running:
            self._destroy()
            return
        self.root.after(POLL_MS, self.tick)

    def _add_record(self, record: dict) -> None:
        index = len(self.results.get_children())
        tag = record["result"]
        self.results.insert("", "end", iid=str(index), tags=(tag,), values=(
            clock_text(record), record["check"], record["result"].upper(),
            record["reason"]))
        self.results.see(str(index))
        if record["check"] in self.tests.get_children():
            self.tests.item(record["check"], tags=(tag,), values=(
                record["check"], self.tests.set(record["check"], "command"),
                record["result"].upper(), f"{record['metrics']['elapsed_s'] * 1000:.1f} ms"))

    def _show_detail(self, event=None) -> None:
        chosen = self.results.selection()
        if not chosen:
            return
        record = self.ctl.records[int(chosen[0])]
        lines = [f"tx  {record['tx'] or '-'}", f"rx  {record['rx'] or '-'}"]
        if record["reason"]:
            lines.append(f"reason  {record['reason']}")
        discarded = record["metrics"].get("discarded_lines")
        if discarded:
            lines.append("discarded  " + " | ".join(discarded))
        self._set_detail("\n".join(lines))

    def _set_detail(self, text: str) -> None:
        self.detail.configure(state="normal")
        self.detail.delete("1.0", "end")
        self.detail.insert("1.0", text)
        self.detail.configure(state="disabled")

    def _refresh(self) -> None:
        ctl, v = self.ctl, self._vars
        if ctl.loaded:
            board = ctl.session.config.board
            v["config"].set(short_path(ctl.config_path))
            v["board"].set(board.name)
            v["address"].set(f"{board.host}:{board.port}")
            v["log"].set("Log: " + short_path(ctl.session.log_path))
        v["link"].set(ctl.link)
        v["board_id"].set(ctl.board_id if ctl.loaded else "-")
        v["firmware"].set(ctl.firmware if ctl.loaded else "-")
        if not self._closing:
            v["status"].set("Cancelling" if ctl.cancelling else
                            "Running" if ctl.running else "Idle")
        passed = sum(r["result"] == "pass" for r in ctl.records)
        v["counts"].set(f"Pass {passed}   Fail {len(ctl.records) - passed}")

        idle = ctl.loaded and not ctl.running
        connected = ctl.link == CONNECTED

        def enable(widget, on):
            widget.state(["!disabled" if on else "disabled"])

        for button, on in ((self.run_button, idle), (self.select_button, idle),
                           (self.load_button, not ctl.running),
                           (self.cancel_button, ctl.running and not ctl.cancelling)):
            enable(button, on)

        def entry(menu, label, on):
            self.menus[menu].entryconfigure(label, state="normal" if on else "disabled")

        entry("file", "Load config...", not ctl.running)
        entry("run", "Run all", idle)
        entry("run", "Run selected", idle)
        entry("run", "Cancel", ctl.running and not ctl.cancelling)
        entry("board", "Connect", idle and not connected)
        entry("board", "Disconnect", idle and connected)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="hosttest-gui")
    parser.add_argument("--config", help="load this config at start")
    args = parser.parse_args(argv)
    root = tk.Tk()
    app = App(root, Controller())
    if args.config:
        app.load_config(args.config)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
