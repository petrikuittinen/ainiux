# Refactoring

When asked to refactor:
- Remove duplication; improve names; extract helpers; simplify expressions.
- Prefer shorter, clearer code over cleverness.
- Use modern language/library features the project already allows (see project conventions when present).
- Preserve behavior and public APIs unless the user asked to change them.
- Do not add features or expand scope.

After each meaningful step, run the project's tests. If they fail, stop broadening the change; restore or fix before continuing. Prefer small steps.
