"""Link status window: the RF head link and its frame counters, with the age of each value.

A thin view over Controller.status. It never touches the board. Values that
were not just read are shown as STALE, in grey, never as live.
"""
from __future__ import annotations

import time
import tkinter as tk
from tkinter import ttk

from .controller import Busy, Controller, StatusNotConfigured
from .status import NEIGHBOURS_NOTE

PAD = 8
REFRESH_MS = 500
LIVE_COLOUR = "#1b5e20"
DOWN_COLOUR = "#b71c1c"
STALE_COLOUR = "#757575"

HEAD_COLUMNS = (("head", "RF head", 80), ("link", "Link", 80),
                ("good", "Good frames", 110), ("bad", "Bad frames", 110),
                ("lock", "Lock (link_status)", 130), ("age", "Age", 80),
                ("state", "State", 80))


def age_text(age_s: float) -> str:
    """Age of a value as shown, one decimal."""
    return f"{age_s:.1f} s"


def head_values(row) -> tuple:
    """Table cells for one RF head row."""
    return (f"head {row.head}", row.link, str(row.crc_ok), str(row.crc_bad),
            str(row.lock), age_text(row.age_s), "STALE" if row.stale else "live")


class StatusWindow(tk.Toplevel):
    """Start or stop reading the board and show what it said."""

    def __init__(self, master, controller: Controller) -> None:
        super().__init__(master)
        self.title("Link status")
        self.ctl = controller
        self.note = tk.StringVar()
        self._build()
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.refresh()

    def _build(self) -> None:
        """Lay out the buttons, the two tables and the error line."""
        self.columnconfigure(0, weight=1)
        bar = ttk.Frame(self, padding=PAD)
        bar.grid(row=0, column=0, sticky="ew")
        self.start_button = ttk.Button(bar, text="Start", command=self.start)
        self.start_button.grid(row=0, column=0)
        self.stop_button = ttk.Button(bar, text="Stop", command=self.ctl.stop_status)
        self.stop_button.grid(row=0, column=1, padx=PAD)
        ttk.Label(bar, textvariable=self.note).grid(row=0, column=2, sticky="w")
        self.heads = self._table("RF heads", 1, HEAD_COLUMNS, 2)
        box = ttk.LabelFrame(self, text="Neighbours", padding=PAD)
        box.grid(row=2, column=0, sticky="ew", padx=PAD, pady=(0, PAD))
        ttk.Label(box, text=NEIGHBOURS_NOTE).grid(row=0, column=0, sticky="w")

    def _table(self, title: str, row: int, columns, height: int) -> ttk.Treeview:
        box = ttk.LabelFrame(self, text=title, padding=PAD)
        box.grid(row=row, column=0, sticky="nsew", padx=PAD, pady=(0, PAD))
        tree = ttk.Treeview(box, columns=[c[0] for c in columns], show="headings",
                            height=height, selectmode="none")
        for key, heading, width in columns:
            tree.heading(key, text=heading)
            tree.column(key, width=width, anchor="w")
        tree.tag_configure("live", foreground=LIVE_COLOUR)
        tree.tag_configure("down", foreground=DOWN_COLOUR)
        tree.tag_configure("stale", foreground=STALE_COLOUR)
        tree.grid(row=0, column=0, sticky="nsew")
        return tree

    def start(self) -> None:
        """Begin reading. One task runs at a time."""
        try:
            self.ctl.start_status()
        except Busy:
            self.note.set("Another task is running")
        except StatusNotConfigured as exc:
            self.note.set(str(exc))
        self.refresh()

    def close(self) -> None:
        self.ctl.stop_status()
        self.destroy()

    def refresh(self) -> None:
        """Redraw from the controller's model. Runs on the GUI thread."""
        if not self.winfo_exists():
            return
        ctl = self.ctl
        view = ctl.status.view(time.monotonic())
        self._fill(self.heads, [(head_values(r), r.stale, r.link == "down")
                                for r in view.heads])
        if view.error:
            self.note.set(view.error)
        elif ctl.loaded and not ctl.status_configured:
            self.note.set("The config has no status section")
        else:
            self.note.set("Reading" if ctl.status_running else "Stopped")
        ready = ctl.status_configured and not ctl.running
        self.start_button.state(["!disabled" if ready else "disabled"])
        self.stop_button.state(["!disabled" if ctl.status_running else "disabled"])
        self.after(REFRESH_MS, self.refresh)

    @staticmethod
    def _fill(tree: ttk.Treeview, rows: list) -> None:
        tree.delete(*tree.get_children())
        for values, stale, down in rows:
            tree.insert("", "end", values=values,
                        tags=("stale" if stale else "down" if down else "live",))
