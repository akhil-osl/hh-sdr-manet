"""Network plan configuration window. Edits and checks a plan; no radio access."""
from __future__ import annotations

import re
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import netplan

PAD = 8
BAD_COLOUR = "#b71c1c"
OK_COLOUR = "#1b5e20"

# key in the plan, tab title, columns as (field, heading, width)
TABS = (
    ("nodes", "Nodes", (("address", "Address", 110), ("role", "Role", 100),
                        ("ip", "IP for dataterm (optional)", 200))),
    ("groups", "Groups", (("address", "Group address", 130),
                          ("ip", "Multicast IP", 220))),
    ("rf_heads", "RF heads", (("name", "Name", 130), ("eui64", "EUI-64", 260))),
    ("nets", "Nets", (("net", "Net", 130), ("rf_head", "RF head", 130),
                      ("net_id", "Net ID", 110))),
)
_PATH_RE = re.compile(r"^(\w+)\[(\d+)\]")


class Table:
    """One notebook tab: a row list and an entry form for one plan section."""

    def __init__(self, window: "NetPlanWindow", parent, key: str, columns) -> None:
        self.window, self.key, self.columns = window, key, columns
        self.frame = ttk.Frame(parent, padding=PAD)
        names = [c[0] for c in columns]
        self.tree = ttk.Treeview(self.frame, columns=names, show="headings",
                                 height=7, selectmode="browse")
        for name, heading, width in columns:
            self.tree.heading(name, text=heading)
            self.tree.column(name, width=width, anchor="w")
        self.tree.tag_configure("bad", foreground=BAD_COLOUR)
        self.tree.grid(row=0, column=0, columnspan=len(columns) + 1, sticky="nsew")
        self.frame.rowconfigure(0, weight=1)
        self.frame.columnconfigure(0, weight=1)
        self.vars: dict[str, tk.StringVar] = {}
        self.inputs: dict[str, ttk.Widget] = {}
        for col, (name, heading, _) in enumerate(columns):
            ttk.Label(self.frame, text=heading).grid(row=1, column=col, sticky="w",
                                                     pady=(PAD, 0))
            self.vars[name] = tk.StringVar()
            if name == "role":
                box = ttk.Combobox(self.frame, textvariable=self.vars[name],
                                   values=netplan.ROLES, state="readonly", width=10)
            elif name == "rf_head":
                box = ttk.Combobox(self.frame, textvariable=self.vars[name], width=12)
            else:
                box = ttk.Entry(self.frame, textvariable=self.vars[name])
            box.grid(row=2, column=col, sticky="ew", padx=(0, 4))
            self.inputs[name] = box
        buttons = ttk.Frame(self.frame)
        buttons.grid(row=2, column=len(columns), sticky="e")
        self.add_button = ttk.Button(buttons, text="Add", command=self.add)
        self.update_button = ttk.Button(buttons, text="Update", command=self.update)
        self.remove_button = ttk.Button(buttons, text="Remove", command=self.remove)
        for b in (self.add_button, self.update_button, self.remove_button):
            b.pack(side="left", padx=2)
        self.tree.bind("<<TreeviewSelect>>", self._load_form)

    def form_row(self) -> dict:
        """Return the form as a plan row; blank optional fields are left out."""
        row = {n: self.vars[n].get().strip() for n, _, _ in self.columns}
        if row.get("ip") == "":
            row.pop("ip")
        return row

    def _selected(self) -> int | None:
        """Return the selected row index, or None."""
        sel = self.tree.selection()
        return int(sel[0]) if sel else None

    def _load_form(self, _event=None) -> None:
        """Copy the selected row into the form."""
        i = self._selected()
        if i is None:
            return
        row = self.window.plan[self.key][i]
        for name, _, _ in self.columns:
            self.vars[name].set(str(row.get(name, "")) if isinstance(row, dict) else "")

    def add(self) -> None:
        """Append the form as a new row."""
        self.window.plan[self.key].append(self.form_row())
        self.window.changed()

    def update(self) -> None:
        """Replace the selected row with the form."""
        i = self._selected()
        if i is not None:
            self.window.plan[self.key][i] = self.form_row()
            self.window.changed(select=(self.key, i))

    def remove(self) -> None:
        """Delete the selected row."""
        i = self._selected()
        if i is not None:
            del self.window.plan[self.key][i]
            self.window.changed()

    def fill(self, rows: list, bad: set[int]) -> None:
        """Redraw the table from the plan rows; rows in bad are marked."""
        self.tree.delete(*self.tree.get_children())
        for i, row in enumerate(rows):
            row = row if isinstance(row, dict) else {}
            values = [row.get(n, "") for n, _, _ in self.columns]
            self.tree.insert("", "end", iid=str(i), values=values,
                             tags=("bad",) if i in bad else ())
        names = [r.get("name") for r in self.window.plan["rf_heads"]
                 if isinstance(r, dict) and isinstance(r.get("name"), str)]
        if "rf_head" in self.inputs:
            self.inputs["rf_head"].configure(values=names)


class NetPlanWindow(tk.Toplevel):
    """Edit the network plan. Save is possible only while the plan is valid."""

    def __init__(self, master) -> None:
        super().__init__(master)
        self.title("Network plan")
        self.plan: dict = netplan.empty_plan()
        self.issues: list[netplan.Issue] = []
        self.path: str | None = None
        self.status_var = tk.StringVar()
        self.command_var = tk.StringVar()
        self.node_var = tk.StringVar()
        self._build()
        self.changed()

    # -- construction ----------------------------------------------------

    def _build(self) -> None:
        """Lay out tabs, problem list, dataterm line and buttons."""
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        book = ttk.Notebook(self)
        book.grid(row=0, column=0, sticky="nsew", padx=PAD, pady=PAD)
        self.tables: dict[str, Table] = {}
        for key, title, columns in TABS:
            table = Table(self, book, key, columns)
            book.add(table.frame, text=title)
            self.tables[key] = table
        self.notebook = book

        box = ttk.LabelFrame(self, text="Problems", padding=PAD)
        box.grid(row=1, column=0, sticky="ew", padx=PAD)
        box.columnconfigure(0, weight=1)
        self.status_label = ttk.Label(box, textvariable=self.status_var)
        self.status_label.grid(row=0, column=0, sticky="w")
        self.problems = tk.Listbox(box, height=5, activestyle="none")
        self.problems.grid(row=1, column=0, sticky="ew")

        line = ttk.LabelFrame(self, text="dataterm flags for one node", padding=PAD)
        line.grid(row=2, column=0, sticky="ew", padx=PAD, pady=PAD)
        line.columnconfigure(1, weight=1)
        ttk.Label(line, text="Node").grid(row=0, column=0, padx=(0, 4))
        self.node_box = ttk.Combobox(line, textvariable=self.node_var, width=10,
                                     state="readonly")
        self.node_box.grid(row=0, column=1, sticky="w")
        self.node_box.bind("<<ComboboxSelected>>", lambda _e: self._update_command())
        ttk.Entry(line, textvariable=self.command_var, state="readonly").grid(
            row=1, column=0, columnspan=2, sticky="ew", pady=(4, 0))
        ttk.Label(line, text="Net ID, groups and EUI-64 are saved in the plan file "
                  "only: dataterm has no input for them yet.").grid(
            row=2, column=0, columnspan=2, sticky="w", pady=(4, 0))

        bar = ttk.Frame(self)
        bar.grid(row=3, column=0, sticky="e", padx=PAD, pady=(0, PAD))
        self.open_button = ttk.Button(bar, text="Open plan...", command=self.choose_open)
        self.save_button = ttk.Button(bar, text="Save plan...", command=self.choose_save)
        self.close_button = ttk.Button(bar, text="Close", command=self.destroy)
        for b in (self.open_button, self.save_button, self.close_button):
            b.pack(side="left", padx=2)

    # -- state -----------------------------------------------------------

    def changed(self, select: tuple[str, int] | None = None) -> None:
        """Recheck the plan and redraw every table, the problem list and buttons."""
        self.issues = netplan.validate(self.plan)
        bad: dict[str, set[int]] = {k: set() for k, _, _ in TABS}
        for issue in self.issues:
            m = _PATH_RE.match(issue.path)
            if m and m.group(1) in bad:
                bad[m.group(1)].add(int(m.group(2)))
        for key, table in self.tables.items():
            table.fill(self.plan[key], bad[key])
        if select and self.tables[select[0]].tree.exists(str(select[1])):
            self.tables[select[0]].tree.selection_set(str(select[1]))
        self.problems.delete(0, "end")
        for issue in self.issues:
            self.problems.insert("end", str(issue))
        if self.issues:
            self.status_var.set(f"{len(self.issues)} problem(s): fix them to save")
            self.status_label.configure(foreground=BAD_COLOUR)
        else:
            self.status_var.set("Plan is valid")
            self.status_label.configure(foreground=OK_COLOUR)
        self.save_button.state(["disabled" if self.issues else "!disabled"])
        self._update_nodes()

    def _update_nodes(self) -> None:
        """Refresh the node choice list and the dataterm line."""
        nodes = sorted(a for a in (netplan.parse_int(n.get("address"))
                                   for n in self.plan["nodes"] if isinstance(n, dict))
                       if a is not None)
        self.node_box.configure(values=[f"0x{a:04X}" for a in nodes])
        if self.node_var.get() not in self.node_box.cget("values"):
            self.node_var.set(f"0x{nodes[0]:04X}" if nodes else "")
        self._update_command()

    def _update_command(self) -> None:
        """Show the dataterm flags for the chosen node, or why there are none."""
        if self.issues:
            self.command_var.set("fix the problems above first")
            return
        node = netplan.parse_int(self.node_var.get())
        if node is None:
            self.command_var.set("add a node first")
            return
        self.command_var.set("dataterm " + " ".join(netplan.dataterm_args(self.plan, node)))

    def set_plan(self, plan: dict) -> None:
        """Replace the plan being edited; missing sections become empty."""
        merged = netplan.empty_plan()
        for key in merged:
            if isinstance(plan.get(key), list):
                merged[key] = plan[key]
        self.plan = merged
        self.changed()

    # -- files -----------------------------------------------------------

    def open_path(self, path: str) -> bool:
        """Load a plan file. Return False and tell the operator if it fails."""
        try:
            plan = netplan.load_plan(path)
        except (OSError, ValueError) as exc:
            messagebox.showerror("Network plan", f"Cannot open {path}\n{exc}", parent=self)
            return False
        self.path = path
        self.set_plan(plan)
        return True

    def save_path(self, path: str) -> bool:
        """Write the plan. Refuses while it has problems."""
        try:
            netplan.save_plan(path, self.plan)
        except (OSError, ValueError) as exc:
            messagebox.showerror("Network plan", f"Cannot save {path}\n{exc}", parent=self)
            return False
        self.path = path
        return True

    def choose_open(self) -> None:
        """Ask for a plan file and load it."""
        path = filedialog.askopenfilename(parent=self, title="Open network plan",
                                          filetypes=[("Plan", "*.json"), ("All", "*")])
        if path:
            self.open_path(path)

    def choose_save(self) -> None:
        """Ask for a file name and save the plan."""
        if self.issues:
            return
        path = filedialog.asksaveasfilename(parent=self, title="Save network plan",
                                            defaultextension=".json",
                                            initialfile=self.path or "netplan.json")
        if path:
            self.save_path(path)
