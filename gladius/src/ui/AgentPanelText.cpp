#include "AgentPanelText.h"

#include <cctype>

namespace gladius::ui::agentText
{
    std::string humaniseName(std::string const & toolName)
    {
        std::string out;
        bool atWordStart = true;
        for (char c : toolName)
        {
            if (c == '_')
            {
                atWordStart = true;
                out += ' ';
                continue;
            }
            out += atWordStart ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            atWordStart = false;
        }
        return out;
    }
} // namespace gladius::ui::agentText
