# Licence decision

**Decided: MIT (option B), 2026-11-11.** The project is now MIT-only. The
anti-996 text moved to `docs/community/anti-996.md` as a non-binding statement,
`LICENSE.MIT`/`LICENSE.996ICU` were removed so licence scanners see one clear
licence, `pyproject.toml` declares `license = "MIT"` with the MIT classifier,
and `LICENSE` carries the MIT text. Releases up to `v0.8.0` keep the terms they
were published under.

This note is kept as the record of why; the options below are the analysis that
led to the decision, not open questions.

## Why MIT rather than the alternatives

- Not OSI-approved: the combined licence was reported as `Other` by GitHub and
  PyPI, cannot be called open source in the OSI sense, and is refused by some
  distributions.
- Corporate review: most OSPOs run a scanner; "Other / custom AND custom" needs
  a legal review before a developer may even try a self-service dev tool.
- Enforcement: whether an employer "forced" overtime is a labour-law question,
  not a software-licence one; the clause was effectively unenforceable while
  deterring exactly the users the tool needs.
- The stance is still stated, just not as a condition — see
  `docs/community/anti-996.md`.

## Original analysis (kept for the record)

**Status at the time: open question.** Nothing in this branch changed the
licence; it only made the metadata match the files.

## Where we were

- `LICENSE` said: `MIT License AND "Anti 996" License Version 1.0 (Draft)`.
- `LICENSE.MIT` and `LICENSE.996ICU` held the two texts.
- `pyproject.toml` used to declare `license = {text = "MIT"}`, which contradicted
  the files above.
- GitHub reports the repository licence as **Other**; PyPI will show
  "MIT AND Anti 996…" rather than a recognised SPDX identifier.

## What the two licences actually require

MIT is the permissive part everyone expects. The Anti-996 licence adds three
obligations on top:

1. the licence text must be reproduced conspicuously in redistributions;
2. the user must obey the labour and employment law of the jurisdiction where
   they are located / registered (or Core International Labour Standards where
   none applies);
3. the user must not pressure employees or contractors into waiving labour
   rights, or retaliate against them for reporting violations.

Because the two are joined by `AND`, **all four requirements apply at once**.

## Why it matters for adoption

| Effect | Detail |
|---|---|
| Not OSI-approved | The project cannot honestly be called "open source" in the OSI sense; several distros and package mirrors will not carry it. |
| Corporate OSPO friction | Most companies run a licence scanner (FOSSA, Snyk, Black Duck). "Other / custom AND custom" typically needs legal review before a developer may use it — a self-service dev tool dies at that gate. |
| PyPI/GitHub presentation | License shows as "Other"; no SPDX filter match, worse discoverability. |
| Contributor terms | Contributors must accept both; some will decline. |
| The clause is hard to enforce | Whether a company "forced" overtime is a labour-law question, not a software-licence one; enforcement would be expensive and the ambiguity deters cautious users. |

## Options

**A. Keep `MIT AND Anti-996` (status quo).**
Maximum philosophical alignment, lowest adoption. Choose this only if the
project is meant as a statement plus internal tooling.

**B. MIT only; move the anti-996 text to a non-binding note (recommended).**
Replace `LICENSE` with `LICENSE.MIT`, keep the Anti-996 text under
`docs/community/anti-996.md` as a stated community value, and say in the README
that the project expects humane working conditions but does not make that a
licence condition. Result: OSI-approved, enterprise-scanner friendly, one line
in `pyproject.toml` (`license = "MIT"` with PEP 639, or keep `{file = ...}`).
Cost: the clause stops being enforceable — which, realistically, it never was.

**C. Apache-2.0.**
Same adoption profile as MIT, plus an explicit patent grant and a
contribution/notice regime. Slightly more attractive to large companies that
ship binaries, slightly more paperwork for contributors. Good fit if the C++
runtime is ever redistributed inside someone else's product.

**D. Open core: permissive core + commercial licence for the extras.**
MIT or Apache for the runtime and MCP server, paid (or separately licensed) for
the things enterprises ask for: audit logs, device farm, report generation,
signed builds. Only worth planning once there are users; do not announce it
before it exists.

## Recommendation

Take **B** now (it is a one-file change and unblocks PyPI), and revisit **D** if
commercial demand appears. If the anti-996 stance is non-negotiable, take **A**
and accept that the README must explicitly say "not OSI open source; check with
your legal team", because otherwise the first enterprise user will discover it
the hard way.

## Mechanics when a decision is made

1. Replace `LICENSE` (keep the old files under `docs/community/` if preserving
   history matters).
2. Update `pyproject.toml` licence field and add the matching classifier
   (`License :: OSI Approved :: MIT License`).
3. Note the change and its date in `CHANGELOG.md`; previous releases stay under
   the terms they were published with.
4. Confirm with the copyright holder — the licence of past releases cannot be
   retroactively changed.
