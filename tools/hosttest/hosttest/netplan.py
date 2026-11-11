"""Network plan model and validation for the configuration page.

The plan is a plain dict (JSON on disk):

    {"nodes":    [{"address": "0x0001", "role": "command" | "member"}],
     "groups":   [{"address": "0xF000", "ip": "239.1.1.1"}],
     "rf_heads": [{"name": "head0", "eui64": "00:11:22:33:44:55:66:77"}],
     "nets":     [{"net": "alpha", "rf_head": "head0", "net_id": "0x10"}]}

validate() returns every problem found, so the page can mark each field.
Nothing here touches the radio, the control socket or dataterm.
"""
from __future__ import annotations

import ipaddress
import re
from dataclasses import dataclass

# TODO: confirm these limits against the MAC frame spec. The repo MAC docs
# still show 32-bit node and group ids. Keep every limit in this table.
LIMITS = {
    "node_min": 0x0001,
    "node_max": 0xEFFF,
    "group_min": 0xF000,
    "group_max": 0xFFFD,
}

ROLES = ("command", "member")

_EUI64_RE = re.compile(r"^[0-9a-fA-F]{2}([:-]?[0-9a-fA-F]{2}){7}$")
_NAME_RE = re.compile(r"^[A-Za-z0-9_.-]{1,32}$")


@dataclass(frozen=True)
class Issue:
    """One validation problem. path names the field, e.g. nodes[2].address."""
    path: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}: {self.message}"


def empty_plan() -> dict:
    """Return a plan with no entries."""
    return {"nodes": [], "groups": [], "rf_heads": [], "nets": []}


def parse_int(value) -> int | None:
    """Parse a decimal or 0x hex integer from int or str. None if invalid."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        text = value.strip()
        if not re.fullmatch(r"0[xX][0-9a-fA-F]+|[0-9]+", text):
            return None
        return int(text, 0) if text[:2].lower() == "0x" else int(text, 10)
    return None


def normalise_eui64(value) -> str | None:
    """Return the EUI-64 as 16 upper-case hex digits, or None if invalid."""
    if not isinstance(value, str) or not _EUI64_RE.match(value.strip()):
        return None
    return re.sub(r"[:-]", "", value.strip()).upper()


def parse_endpoint(value) -> str | None:
    """Return 'a.b.c.d' or 'a.b.c.d:port' normalised, or None if invalid."""
    if not isinstance(value, str):
        return None
    host, sep, port = value.strip().partition(":")
    try:
        ip = ipaddress.IPv4Address(host)
    except ValueError:
        return None
    if not sep:
        return str(ip)
    if not port.isdigit() or not 1 <= int(port) <= 65535:
        return None
    return f"{ip}:{int(port)}"


def _rows(plan: dict, key: str, issues: list[Issue]) -> list:
    """Return plan[key] as a list, recording an issue when it is not one."""
    rows = plan.get(key, [])
    if not isinstance(rows, list):
        issues.append(Issue(key, "must be a list"))
        return []
    return rows


def _row(rows: list, i: int, key: str, issues: list[Issue]) -> dict | None:
    """Return rows[i] if it is an object, else record an issue."""
    if isinstance(rows[i], dict):
        return rows[i]
    issues.append(Issue(f"{key}[{i}]", "must be an object"))
    return None


def _check_nodes(plan: dict, issues: list[Issue]) -> None:
    """Node address range, uniqueness, role, and the command-radio rule."""
    seen: dict[int, int] = {}
    command: list[int] = []
    member: list[int] = []
    rows = _rows(plan, "nodes", issues)
    for i in range(len(rows)):
        row = _row(rows, i, "nodes", issues)
        if row is None:
            continue
        addr = parse_int(row.get("address"))
        if addr is None:
            issues.append(Issue(f"nodes[{i}].address", "not a number"))
        elif not LIMITS["node_min"] <= addr <= LIMITS["node_max"]:
            issues.append(Issue(
                f"nodes[{i}].address",
                f"0x{addr:X} outside 0x{LIMITS['node_min']:04X} to "
                f"0x{LIMITS['node_max']:04X}"))
        elif addr in seen:
            issues.append(Issue(f"nodes[{i}].address",
                                f"0x{addr:04X} already used by nodes[{seen[addr]}]"))
        else:
            seen[addr] = i
        if row.get("ip") not in (None, "") and parse_endpoint(row.get("ip")) is None:
            issues.append(Issue(f"nodes[{i}].ip", "want IPv4 or IPv4:port"))
        role = row.get("role")
        if role not in ROLES:
            issues.append(Issue(f"nodes[{i}].role", "must be command or member"))
        elif addr is not None and LIMITS["node_min"] <= addr <= LIMITS["node_max"]:
            (command if role == "command" else member).append(addr)
    # Break-in rule 13: command radios take the lowest addresses.
    if command and member and max(command) > min(member):
        issues.append(Issue(
            "nodes", f"command radios must have the lowest addresses: "
            f"0x{max(command):04X} is above member 0x{min(member):04X}"))


def _check_groups(plan: dict, issues: list[Issue]) -> None:
    """Group address range, multicast IP, and uniqueness of both."""
    addrs: dict[int, int] = {}
    ips: dict[str, int] = {}
    rows = _rows(plan, "groups", issues)
    for i in range(len(rows)):
        row = _row(rows, i, "groups", issues)
        if row is None:
            continue
        addr = parse_int(row.get("address"))
        if addr is None:
            issues.append(Issue(f"groups[{i}].address", "not a number"))
        elif not LIMITS["group_min"] <= addr <= LIMITS["group_max"]:
            issues.append(Issue(
                f"groups[{i}].address",
                f"0x{addr:X} outside 0x{LIMITS['group_min']:04X} to "
                f"0x{LIMITS['group_max']:04X}"))
        elif addr in addrs:
            issues.append(Issue(f"groups[{i}].address",
                                f"0x{addr:04X} already used by groups[{addrs[addr]}]"))
        else:
            addrs[addr] = i
        raw = row.get("ip")
        try:
            ip = ipaddress.ip_address(raw.strip() if isinstance(raw, str) else raw)
        except ValueError:
            issues.append(Issue(f"groups[{i}].ip", "not an IP address"))
            continue
        if not ip.is_multicast:
            issues.append(Issue(f"groups[{i}].ip", f"{ip} is not a multicast address"))
        elif str(ip) in ips:
            issues.append(Issue(f"groups[{i}].ip",
                                f"{ip} already used by groups[{ips[str(ip)]}]"))
        else:
            ips[str(ip)] = i


def _check_heads(plan: dict, issues: list[Issue]) -> set[str]:
    """RF head name and EUI-64 checks. Returns the valid head names."""
    names: dict[str, int] = {}
    euis: dict[str, int] = {}
    rows = _rows(plan, "rf_heads", issues)
    for i in range(len(rows)):
        row = _row(rows, i, "rf_heads", issues)
        if row is None:
            continue
        name = row.get("name")
        if not isinstance(name, str) or not _NAME_RE.match(name):
            issues.append(Issue(f"rf_heads[{i}].name",
                                "1 to 32 characters: letters, digits, _ . -"))
        elif name in names:
            issues.append(Issue(f"rf_heads[{i}].name",
                                f"{name} already used by rf_heads[{names[name]}]"))
        else:
            names[name] = i
        eui = normalise_eui64(row.get("eui64"))
        if eui is None:
            issues.append(Issue(f"rf_heads[{i}].eui64",
                                "need 8 hex bytes, e.g. 00:11:22:33:44:55:66:77"))
        elif eui in euis:
            issues.append(Issue(f"rf_heads[{i}].eui64",
                                f"already used by rf_heads[{euis[eui]}]"))
        else:
            euis[eui] = i
    return set(names)


def _check_nets(plan: dict, heads: set[str], issues: list[Issue]) -> None:
    """One Net ID per net per RF head; the head must exist."""
    pairs: dict[tuple[str, str], int] = {}
    rows = _rows(plan, "nets", issues)
    for i in range(len(rows)):
        row = _row(rows, i, "nets", issues)
        if row is None:
            continue
        net, head = row.get("net"), row.get("rf_head")
        if not isinstance(net, str) or not _NAME_RE.match(net):
            issues.append(Issue(f"nets[{i}].net",
                                "1 to 32 characters: letters, digits, _ . -"))
        if head not in heads:
            issues.append(Issue(f"nets[{i}].rf_head", "no such RF head"))
        # TODO: Net ID width is not defined yet; only "non-negative integer" is checked.
        net_id = parse_int(row.get("net_id"))
        if net_id is None or net_id < 0:
            issues.append(Issue(f"nets[{i}].net_id", "not a non-negative number"))
        if isinstance(net, str) and isinstance(head, str):
            key = (net, head)
            if key in pairs:
                issues.append(Issue(
                    f"nets[{i}]", f"net {net} already has a Net ID on {head} "
                    f"(nets[{pairs[key]}])"))
            else:
                pairs[key] = i


def validate(plan) -> list[Issue]:
    """Return every problem in the plan. An empty list means it is valid."""
    if not isinstance(plan, dict):
        return [Issue("plan", "must be an object")]
    issues: list[Issue] = []
    _check_nodes(plan, issues)
    _check_groups(plan, issues)
    heads = _check_heads(plan, issues)
    _check_nets(plan, heads, issues)
    return issues


def dataterm_args(plan: dict, node: int) -> list[str]:
    """Return the dataterm flags this plan can set for one node.

    Only the node id and the peers that have an ip are used: those are the
    only plan fields dataterm accepts today. Net ID, groups and EUI-64 have
    no dataterm input yet. Raises ValueError for an invalid plan or node.
    """
    if validate(plan):
        raise ValueError("plan is not valid")
    nodes = {parse_int(n["address"]): n for n in plan["nodes"]}
    if node not in nodes:
        raise ValueError(f"node 0x{node:04X} is not in the plan")
    args = ["-n", f"0x{node:04X}"]
    for addr, row in sorted(nodes.items()):
        endpoint = parse_endpoint(row.get("ip")) if row.get("ip") else None
        if addr != node and endpoint:
            args += ["-P", f"0x{addr:04X}={endpoint}"]
    return args


def load_plan(path: str) -> dict:
    """Read a plan file. Raises OSError or ValueError on a bad file."""
    import json
    with open(path, encoding="utf-8") as fh:
        plan = json.load(fh)
    if not isinstance(plan, dict):
        raise ValueError("plan file must hold a JSON object")
    return plan


def save_plan(path: str, plan: dict) -> None:
    """Write a valid plan. Raises ValueError if the plan has any issue."""
    import json
    if validate(plan):
        raise ValueError("plan is not valid")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(plan, fh, indent=2)
        fh.write("\n")
