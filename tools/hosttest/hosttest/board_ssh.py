"""Read link counters from a board over ssh, using the board's own radioctl.

Only commands that already exist on a board are used (docs/BOARD-RUNBOOK.md,
section 5): `radioctl opencpi get <worker> <property>`. Each prints key=value
lines: worker, property, value, type, access. No board change is needed.
"""
from __future__ import annotations

import shlex
import subprocess
from typing import Callable

from .config import StatusConfig



class StatusError(ValueError):
    """A board reply that does not hold the expected properties."""


READS = (("rx_ber", "crc_ok_count"), ("rx_ber", "crc_bad_count"),
         ("rx_fts", "link_status"))


def remote_command(cfg: StatusConfig) -> str:
    """One shell line for the board: the three property reads, stopping at the first failure."""
    radioctl = f"./radioctl -s {shlex.quote(cfg.radiod_socket)} opencpi get"
    gets = " && ".join(f"{radioctl} {w} {p}" for w, p in READS)
    return f"cd {shlex.quote(cfg.radioctl_dir)} && {gets}"


def parse_reads(text: str) -> dict[str, int]:
    """Values of the three properties, by property name. Raises StatusError."""
    blocks: list[dict] = []
    for line in text.splitlines():
        key, sep, value = line.strip().partition("=")
        if not sep:
            continue
        if key == "worker":
            blocks.append({})
        if blocks:
            blocks[-1][key] = value
    found: dict[str, int] = {}
    for worker, prop in READS:
        block = next((b for b in blocks
                      if b.get("worker") == worker and b.get("property") == prop), None)
        if block is None or "value" not in block:
            raise StatusError(f"no {worker} {prop} in the reply")
        try:
            found[prop] = int(block["value"])
        except ValueError:
            raise StatusError(f"{prop} is not a number: {block['value']!r}") from None
    return found


class SshStatusSource:
    """Runs the read on the board. `run` is subprocess.run, replaced in tests."""

    def __init__(self, cfg: StatusConfig, run: Callable = subprocess.run) -> None:
        self._cfg = cfg
        self._run = run

    def read(self) -> tuple[bool, dict, str]:
        """One read: (ok, counters by property name, reason). Never raises."""
        cfg = self._cfg
        argv = ["ssh", "-o", "BatchMode=yes",
                "-o", f"ConnectTimeout={max(1, int(cfg.timeout_s))}",
                "--", cfg.ssh_target, remote_command(cfg)]
        try:
            done = self._run(argv, capture_output=True, text=True,
                             timeout=cfg.timeout_s, check=False)
        except subprocess.TimeoutExpired:
            return False, {}, f"no reply within {cfg.timeout_s:g}s"
        except OSError as exc:
            return False, {}, f"cannot run ssh: {exc.strerror or exc}"
        if done.returncode != 0:
            detail = (done.stderr or done.stdout).strip().splitlines()
            return False, {}, f"ssh failed ({done.returncode}): {detail[0] if detail else 'no output'}"
        try:
            return True, parse_reads(done.stdout), ""
        except StatusError as exc:
            return False, {}, str(exc)
