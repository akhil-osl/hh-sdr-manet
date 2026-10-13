# OpenCPI → SCA Descriptor Generator — Open Questions

Draft for discussion. Decisions needed before implementation starts.

**Goal:** generate SCA 2.2.2 descriptors (SPD / SCD / PRF) from OpenCPI
component metadata (OCS + OWD), validated against the SCA schema.
Scope: one component at a time. SAD deferred.

---

## Worked example

Everything OpenCPI gives us for the `bias` component:

```xml
<!-- specs/bias_spec.xml (OCS) -->
<ComponentSpec>
  <Property Name="biasValue" Initial="true"/>
  <DataInterfaceSpec Name="in">
    <include href="stream32_protocol.xml"/>
  </DataInterfaceSpec>
  <DataInterfaceSpec Name="out" Producer="true" buffersize='in'/>
</ComponentSpec>

<!-- bias.rcc/bias.xml (OWD) -->
<RccWorker name='bias' language='c' spec='bias_spec.xml'/>
```

What SCA needs (PRF extract):

```xml
<simple id="DCE:<uuid>" name="biasValue" type="ulong" mode="readwrite">
  <kind kindtype="configure"/>
</simple>
```

Note `type`, `id`, `mode` and `kindtype` are **not present in the input**. That
gap is the substance of the questions below.

---

## Q1 — How do we resolve implicit OpenCPI metadata?

`biasValue` has no `Type=`. OpenCPI's rule is that an untyped property defaults
to `ulong`. That rule lives in OpenCPI's code, not in our input files.

Same class of problem for: `spec=` lookup across the project search path,
`<include href=>` chasing, and `specproperty` merge (worker overrides spec).

**Options**

| | Approach | Trade-off |
|---|---|---|
| **A** | Reimplement OpenCPI's resolution rules in our parser | No OpenCPI dependency at runtime. Risk: a wrong or outdated rule fails silently. |
| **B** | Call OpenCPI's own tooling to resolve, then map its output | Correct by construction. Requires an OpenCPI install wherever the generator runs. |
| **C** | Reimplement (A), and for already-built components compare our resolved model against the artifact-description XML as a **test oracle** | Independent at runtime, proven correct in CI. Artifact is test data, never input. |

**Recommendation: C.**

---

## Q2 — Port `repid`: what do we emit?

SCA ports carry a CORBA interface repository ID:

```xml
<provides providesname="in" repid="IDL:???:1.0"/>
```

OpenCPI protocols (`stream32`, etc.) are **not** IDL interfaces. There is no
faithful mapping. Options:

1. Synthesize a repid from the protocol name — e.g.
   `IDL:ocpi/protocol/stream32:1.0`. Deterministic, but invents an IDL identity
   that no IDL file defines.
2. Map onto a standard interface set (e.g. BULKIO) where the data shape fits.
   Interoperable, but a lossy and sometimes wrong claim.
3. Emit a project-defined repid namespace and publish matching IDL separately.

**This is the weakest mapping in the whole design.** Whatever we choose is a
policy decision, not a derivation. It must be documented as such.

**Needs a decision.**

---

## Q3 — Property kind and mode mapping

OpenCPI's property attributes do not map cleanly onto SCA's `kindtype` / `mode`.

Proposed starting point:

| OpenCPI | SCA kindtype | SCA mode |
|---|---|---|
| `parameter='1'` | `execparam` | `readonly` |
| `initial='1'` | `configure` | `readwrite` |
| `volatile='1'` | `configure` | `readonly` |
| `hidden='1'` | *omitted entirely* | — |

Two points to confirm:

- Is `initial → configure/readwrite` right, or should it be `execparam`?
  OpenCPI `Initial` means "settable before start", which is arguably closer to
  `execparam`.
- Dropping `hidden` properties (`ocpi_debug`, `ocpi_endian`, `ocpi_version`, …)
  is a **policy choice**. They are OpenCPI implementation detail, but the
  alternative is emitting them as SCA properties. Confirm we want them gone.

---

## Q4 — Component identity and UUIDs

SCA requires DCE UUIDs for `softpkg id` and each property `id`. OpenCPI
identifies things by name (`ocpi.core.bias`).

- **Random UUID per generation** — regenerating a descriptor changes its ID,
  breaking any SAD that references it.
- **Deterministic from `specname`** (e.g. UUIDv5 over `ocpi.core.bias`) — stable
  across regeneration, reproducible builds.

**Recommendation: deterministic.** Confirm the namespace seed.

---

## Q5 — What exactly do we validate against?

SCA 2.2.2 defines descriptors with **DTDs**. REDHAWK also ships `.xsd` files,
but those are namespaced (`urn:mil:jpeojtrs:sca:spd`) and come from the SCA IDE
(EMF) tooling.

Verified locally: REDHAWK's own `GPP.spd.xml` **passes** DTD validation and
**fails** XSD validation —

```
Element 'softpkg': No matching global declaration available for the validation root.
```

because the DTD-form files are un-namespaced while the XSD expects a namespace.

Proposal — three independent levels, reported separately:

1. **DTD (SCA 2.2.2)** — the normative contract. **Must pass.**
2. **XSD (REDHAWK/IDE)** — informational only; REDHAWK's own files fail it.
3. **Semantic** — cross-file checks no schema covers: do `propertyfile` /
   `descriptor` hrefs resolve, do SCD ports agree with PRF, are IDs unique and
   DCE-shaped, do `refid`s point at real properties.

Level 3 catches the bugs that matter. A schema-valid descriptor with a dangling
`refid` is still broken.

**Confirm:** is DTD-valid our acceptance bar, with XSD as advisory?

---

## Q6 — Which implementation(s) does one SPD describe?

An SPD can carry multiple `<implementation>` elements (per OS / processor).
A single OpenCPI worker builds for several RCC platforms.

- One SPD per component with an implementation per built platform, or
- one SPD per platform?

Related: does the generator need to know about built binaries at all (to fill
`<code><localfile>`), or does it emit a placeholder for the packaging step to
fill in?

---

## Q7 — What do we do with OpenCPI constructs SCA has no slot for?

Examples seen in real components:

- `<slaves>` — proxy workers controlling HDL workers
  (`dgrdma_config_proxy` declares one)
- `buffersize='in'` — port buffer sizing expressions
- protocol detail: `dataValueWidth`, `maxMessageValues`, `zeroLengthMessages`

Options: drop silently, drop with a warning, or carry as non-standard
annotations. **Recommend: drop with a warning**, and keep a list of what was
dropped so nothing disappears unnoticed.

---

## Proposed order of work

1. Collect corpus: OpenCPI components (`bias`, `dgrdma_config_proxy`,
   `file_read`/`file_write`) plus hand-written SCA descriptors as golden
   references.
2. Write the mapping specification — Q2, Q3, Q4 resolved and justified.
3. Build reader (OCS/OWD → internal model).
4. Emit PRF, then SPD, then SCD.
5. Validation harness — the three levels in Q5.
6. Round-trip proof: generated descriptors load in REDHAWK.

Steps 1–2 come before any generator code. The mapping decisions are the
substance; emission is mechanical once they exist.

---

## Summary — decisions needed

| # | Question | Recommendation |
|---|---|---|
| Q1 | How to resolve implicit metadata | C — own resolver, artifact as test oracle |
| Q2 | Port `repid` | **open — no clean answer** |
| Q3 | Property kind/mode mapping | table above, confirm `initial` and `hidden` |
| Q4 | UUID strategy | deterministic from `specname` |
| Q5 | Validation target | DTD normative, XSD advisory |
| Q6 | Implementations per SPD | open |
| Q7 | Unmappable OpenCPI constructs | drop with warning, keep a list |

Q2 is the one with no correct answer. It needs a decision we can defend, not a
derivation.
