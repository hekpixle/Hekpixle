---
name: creating-skills
description: When and how to save a reusable skill (SKILL.md) with skill_manage after solving a non-trivial task or learning a workflow.
---

# Creating and maintaining skills

Create a skill when you just completed a task that took 5+ tool calls, hit and fixed tricky errors, or the user taught you a workflow they will want repeated. Update an existing skill (`skill_manage patch`) when you find it was wrong or incomplete.

## Format
`skill_manage(action="create", name="kebab-case-name", content=...)` where content is a SKILL.md:

```
---
name: kebab-case-name
description: One line — what it does and when to use it (this line is what future-you sees in the index).
---
# Title
Steps, commands, pitfalls, verification. Be concrete and short.
```

## Rules
- One skill = one workflow. Keep it under ~80 lines; put long reference material in extra files (`skill_manage write_file`).
- Include exact commands/paths/flags that worked and the mistakes to avoid.
- Never store secrets (passwords, API keys) in a skill.
- Tell the user briefly when you created or changed a skill.
