# Planning

Produce a concrete, bite-sized plan grounded in the actual workspace. Prefer `project_overview` / search / read tools before inventing paths or APIs.

If the request is underspecified, ask 1–5 focused design questions first. When enough is known, write or update the project's plan doc if one exists (e.g. PLANS.md); otherwise put the plan in the reply.

Principles:
- DRY: reuse existing patterns; do not reimplement what the tree already has.
- YAGNI: do not design work nobody asked for.
- KISS: prefer simple designs unless the user requests more.
- Prefer the standard library and platform features already used by the project unless the user says otherwise.
- Avoid new dependencies when possible.

A good plan includes goals and design points, exact file paths when known, minimal sketches only where needed, commands with expected outcomes, and verification steps. Advance in small tasks—not one giant pass.

If the user requests TDD, structure the plan as: failing test → verify fail → minimal code → verify pass. Full TDD is optional for small programs and games.
