# ha-config — Agent Routing

> Read at session start. Routes tasks to the right agent context.

## Routing Table

| Task type | Primary | Secondary |
|-----------|---------|-----------|
| HA YAML config / automations / scripts | ha-systems | — |
| Entity ID discovery / device wiring | ha-systems | — |
| Deploy to Pi / restart HA | release-manager | ha-systems |
| Cross-project change / KIS-wide pattern | release-manager | — |
| Trivial fix (per §6.6 TRIVIAL) | no agent — direct CC | — |
| Multi-session refactor | per scope above + read `kis-meta/refactors/<name>.md` | — |

## Session Start Checklist

1. Read this file
2. Read `CLAUDE.md` in this repo
3. Read `kis-meta/AGENTS.md` for KIS-wide conventions
4. Check `kis-meta/refactors/` for any active refactor touching this repo

## Cross-References

- **Deploy flow:** `kis-meta/agent_docs/deploy_flow.md`
- **Checkpoint format:** `kis-meta/agent_docs/checkpoint_format.md`
