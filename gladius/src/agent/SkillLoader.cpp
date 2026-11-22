#include "SkillLoader.h"

#include <sstream>

namespace gladius::agent
{
    SkillLoader::SkillLoader()
    {
        registerSkill(
          {"sdf-modeling",
           "Gladius implicit SDF snippet syntax, local variables, helper functions, and multi-material coloring.",
           "# Gladius Implicit SDF Modeling\n"
           "- `main` signature: `(vec3 color, float shape) main_3(vec3 pos)`\n"
           "- Locals: always use `float name = ...;` (never `vec3 name = ...;` for locals). Reference locals by plain name (`name`), NEVER `in_name`.\n"
           "- Function parameters: inside a function body, reference parameters as `in_paramName` only when editing normalized output; in new snippets, plain parameter names work.\n"
           "- Keep `main` concise: decompose repeated primitives, lattices, and fasteners into separate helper functions.\n"
           "- Remember `shape < 0` is inside the solid, `shape > 0` is outside, `min(a, b)` is union, `max(a, b)` is intersection, `max(a, -b)` is difference.\n"});

        registerSkill(
          {"library-reuse",
           "How to inventory required parts/functions, search the Gladius library, and import reusable items.",
           "# Library Reuse Workflow\n"
           "1. Call `add_function` to list every required part/feature before writing geometry.\n"
           "2. For each item in your list, call `list_library_items` with a relevant `query` or `category` (e.g. `primitives`, `lattices`, `mechanical`, `operations`, `patterns`).\n"
           "3. Decide explicitly: REUSE from library (`import_library_item`) or CREATE new.\n"
           "4. Call `import_library_item(category, name)` to merge reusable functions into the active document, then inspect the returned function signature and call it from `main` in `set_program_snippet`.\n"});

        registerSkill(
          {"morphological-design",
           "Systematic functional decomposition, morphological box variants, compatibility, and multi-fidelity refinement.",
           "# Morphological & Variant-Driven Design\n"
           "1. Capture requirements with `add_requirement`.\n"
           "2. Decompose into functions/components with `add_function`.\n"
           "3. Propose 2-3 solution principles per key function with `create_variant` at `envelope` fidelity first.\n"
           "4. Combine compatible variants into configurations (`create_configuration`, `activate_configuration`).\n"
           "5. Freeze finalized components (`freeze_component`), refine geometry with `set_program_snippet`, verify with `capture_viewport`, and call `finish_design`.\n"});
    }

    void SkillLoader::registerSkill(SkillInfo skill)
    {
        for (auto & existing : m_skills)
        {
            if (existing.name == skill.name)
            {
                existing = std::move(skill);
                return;
            }
        }
        m_skills.push_back(std::move(skill));
    }

    std::optional<SkillInfo> SkillLoader::findSkill(std::string const & name) const
    {
        for (auto const & s : m_skills)
        {
            if (s.name == name)
            {
                return s;
            }
        }
        return std::nullopt;
    }

    std::string SkillLoader::summarizeForPrompt() const
    {
        if (m_skills.empty())
        {
            return {};
        }
        std::ostringstream out;
        for (auto const & s : m_skills)
        {
            out << "- " << s.name;
            if (!s.description.empty())
            {
                out << ": " << s.description;
            }
            out << "\n";
        }
        return out.str();
    }
} // namespace gladius::agent
