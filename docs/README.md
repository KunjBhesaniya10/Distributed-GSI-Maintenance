# Docs

| Doc | What it's for | Read when |
|---|---|---|
| [phase2_plan.md](phase2_plan.md) | Plan after checkpoint 1: cost model, async maintenance, online build, benchmarks, demo; open decisions `D<n>.<m>` | planning / choosing what to build |
| [phase2_results.md](phase2_results.md) | Phase 2 benchmark commands, results and observations | running or reading the experiments |
| [demo.md](demo.md) | The final-presentation demo, typed by hand; also the feature walkthrough | presenting, or seeing each feature work |
| [playbook.md](playbook.md) | Setup, tests, 1M-row load, benchmark commands, reset, troubleshooting | on a new laptop, or when something breaks |
| [progress.md](progress.md) | What was done, with verification evidence | after finishing any item |
| [architecture_plan.md](architecture_plan.md) | Why the design is the way it is (checkpoint 1); detailed designs for async (logical decoding) and online-build backfill | explaining a design choice |
| [implementation_plan.md](implementation_plan.md) | The locked checkpoint-1 build plan: invariants, interfaces, milestones M1-M8 | changing core write/commit/resolver code (check the invariants) |
| [gsi_fdw_guide.md](gsi_fdw_guide.md) | How PostgreSQL loads and calls the extension, file by file | first time reading the code |
| [code_changes.md](code_changes.md) | Every line we changed vs upstream postgres_fdw, with links | reviewing our diff against upstream |
| [call_graph.md](call_graph.md) | Mermaid diagrams of who calls whom | the big picture of a flow |
| [call_stacks.md](call_stacks.md) | Line-level call stacks per statement type | debugging a specific statement |
| [checkpoint1_report.pdf](checkpoint1_report.pdf) | Checkpoint 1 report (source: `.tex`) | — |

Rule: one topic, one place. A doc that needs another doc's content links to it instead of copying it.
