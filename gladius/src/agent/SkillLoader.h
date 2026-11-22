#pragma once

#include <optional>
#include <string>
#include <vector>

namespace gladius::agent
{
    /// @brief Metadata and guidance for a built-in Gladius design skill
    struct SkillInfo
    {
        std::string name;
        std::string description;
        std::string content;
    };

    /// @brief Registry of curated domain skills for the Gladius Native Design Agent
    class SkillLoader
    {
      public:
        SkillLoader();

        void registerSkill(SkillInfo skill);

        [[nodiscard]] std::vector<SkillInfo> const & skills() const noexcept
        {
            return m_skills;
        }

        [[nodiscard]] std::optional<SkillInfo> findSkill(std::string const & name) const;
        [[nodiscard]] std::string summarizeForPrompt() const;

      private:
        std::vector<SkillInfo> m_skills;
    };
} // namespace gladius::agent
