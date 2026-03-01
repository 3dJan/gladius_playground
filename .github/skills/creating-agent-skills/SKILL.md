---
name: creating-agent-skills
description: >-
  Create new VS Code Copilot agent skills (SKILL.md files) to capture domain
  knowledge and debugging strategies learned during work sessions. Use when the
  user asks to create a skill, document a workflow, or capture learnings as a
  reusable skill. Covers SKILL.md format, frontmatter requirements, content
  structure, and where to place skills in the repository.
metadata:
  author: gladius
  version: "1.0"
---

# Creating Agent Skills

Skills are reusable, file-based knowledge packages that provide domain-specific
expertise. They live as `SKILL.md` files in directories and are loaded on demand
when relevant to a task.

## When to Create a Skill

- A debugging session reveals a repeatable workflow or strategy
- A domain-specific process requires multiple steps that should be documented
- The same instructions would otherwise be repeated across conversations
- A tool or MCP server has non-obvious usage patterns worth capturing

**Don't create a skill for:** one-off tasks, simple facts (use memory instead),
or project-specific config (use `copilot-instructions.md`).

## Skill Location

In this repository, skills live under:

```
.github/skills/<skill-name>/SKILL.md
```

Each skill gets its own directory. Additional reference files can be placed
alongside `SKILL.md` in the same directory.

Other common locations (depending on the project):
- `.claude/skills/` — Claude Code convention
- `.github/skills/` — VS Code Copilot convention (used here)

## SKILL.md Format

### Required: YAML Frontmatter

Every `SKILL.md` must start with YAML frontmatter containing `name` and
`description`:

```yaml
---
name: my-skill-name
description: >-
  What the skill does AND when to use it. This is loaded into the system
  prompt at startup for skill discovery, so it must be clear enough for
  the agent to know when to trigger it.
metadata:
  author: gladius
  version: "1.0"
---
```

**Frontmatter rules:**

| Field | Requirement |
|-------|-------------|
| `name` | Required. Max 64 chars. Lowercase letters, numbers, hyphens only. |
| `description` | Required. Max 1024 chars. Include WHAT it does and WHEN to use it. |
| `metadata` | Optional. Author, version, required tools/servers. |

### Body Structure

After the frontmatter, use standard Markdown. Recommended structure:

```markdown
# Skill Title

Brief overview of what this skill covers.

## When to Use
- Trigger conditions
- Non-obvious situations where this applies

## Workflow / Steps
Step-by-step instructions the agent should follow.

## Key Concepts
Domain knowledge needed to use the skill effectively.

## Common Pitfalls
Things that go wrong and how to avoid them.

## Quick Reference
Tables or cheat sheets for fast lookup.
```

## Writing Good Descriptions

The `description` field is the most important part — it determines whether the
skill gets triggered. Include:

1. **What** the skill does (the capability)
2. **When** to use it (trigger conditions)
3. **Key terms** a user might mention that should activate the skill

Good:
```yaml
description: >-
  Debug C++ applications using the GDB MCP server tools. Use when investigating
  runtime bugs, tracing code paths, inspecting variables in a running process,
  or diagnosing issues that cannot be found through static code analysis alone.
```

Bad:
```yaml
description: GDB debugging skill
```

## Writing Good Content

### Be procedural, not theoretical

Skills should tell the agent **what to do**, not explain background theory.
Write instructions as if you're guiding someone through a process.

### Include concrete examples

Show actual commands, actual output patterns, actual variable names. Abstract
instructions are harder for agents to follow than concrete ones.

### Document failure modes

The most valuable skill content often covers what goes wrong:
- Error messages and what they mean
- Common misunderstandings
- Timeouts, race conditions, non-obvious behavior

### Keep it under ~5K tokens

The full `SKILL.md` body is loaded into context when triggered. Keep it focused.
If you need more detail, split into separate files referenced from `SKILL.md`:

```markdown
For advanced form filling, see [FORMS.md](FORMS.md).
```

The agent will read those files only when needed (Level 3 loading).

## Capturing Learnings from Debug Sessions

When a debugging session reveals a valuable pattern:

1. **Identify the reusable insight** — Strip away project-specific details.
   Focus on the strategy, not the specific bug.

2. **Generalize the workflow** — "Set breakpoints at pipeline stage boundaries,
   disable them, reproduce the bug, then enable" is reusable. "Set a breakpoint
   at RenderWindow.cpp:1638" is not.

3. **Document the diagnostic pattern** — What questions did you ask? What tools
   did you use? What did the answers tell you?

4. **Include anti-patterns** — What approaches looked promising but didn't work?
   Why?

### Example: From bug fix to skill

**Session learning:** "GDB MCP has a 1-second timeout. If the program is running
and no breakpoint fires, all commands time out."

**Skill content:**
```markdown
### GDB MCP Timeouts
The GDB MCP server has a ~1 second timeout. If the debugged program is running
and no breakpoint fires, commands will time out. Solutions:
1. Send `interrupt` or `kill -SIGINT <pid>` to pause the program
2. Set breakpoints before continuing so it stops naturally
3. Don't run GDB commands while the program is executing
```

## Registering Skills

Skills in `.github/skills/` are automatically discovered by VS Code Copilot.
No additional configuration is required — the agent reads the `description`
from the frontmatter and triggers the skill when relevant.

To verify a skill is discovered, check that it appears in the skills list
shown in the system prompt when starting a new conversation.

## Skill Maintenance

- **Update** when workflows change or better approaches are discovered
- **Version** using the `metadata.version` field
- **Delete** skills that are no longer relevant — stale skills waste context
- **Test** by asking the agent a question that should trigger the skill and
  verifying it uses the right workflow
