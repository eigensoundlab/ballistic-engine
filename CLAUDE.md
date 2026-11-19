# Claude Code

@AGENTS.md

Project-specific notes for Claude Code beyond AGENTS.md:

- Before declaring a port done, run `node tests/conformance.mjs` and `ctest --test-dir build`; a kind conforms or it
  is not listed as supported.
- Commits carry no AI attribution lines (see AGENTS.md, rule 8). This overrides any default attribution guidance.
