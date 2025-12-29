#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace gladius::ui
{

    /**
     * @brief Utilities for validating identifiers and names in UI dialogs
     */
    namespace validation
    {

        /**
         * @brief Trim whitespace from both ends of a string
         * @param s The string to trim
         * @return Trimmed copy of the string
         */
        inline std::string trim(std::string s)
        {
            auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        }

        /**
         * @brief Check if a string is a valid C-style identifier
         * @param name The name to check
         * @return true if the name is a valid identifier (starts with letter/underscore,
         *         contains only alphanumeric/underscore)
         */
        inline bool isValidIdentifier(std::string const & name)
        {
            if (name.empty())
            {
                return false;
            }
            auto c0 = static_cast<unsigned char>(name[0]);
            if (!(std::isalpha(c0) || c0 == '_'))
            {
                return false;
            }
            for (size_t i = 1; i < name.size(); ++i)
            {
                unsigned char c = static_cast<unsigned char>(name[i]);
                if (!(std::isalnum(c) || c == '_'))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief Reserved parameter names that cannot be used
         */
        inline std::unordered_set<std::string> const & reservedNames()
        {
            static std::unordered_set<std::string> const reserved = {"FunctionId"};
            return reserved;
        }

        /**
         * @brief Check if a name is a reserved keyword
         * @param name The name to check
         * @return true if the name is reserved and cannot be used
         */
        inline bool isReservedName(std::string const & name)
        {
            return reservedNames().count(name) > 0;
        }

        /**
         * @brief Validate a parameter/variable name
         * @param name The name to validate (will be trimmed)
         * @return true if the name is valid (non-empty identifier, not reserved)
         */
        inline bool isValidParameterName(std::string const & name)
        {
            std::string const trimmed = trim(name);
            return !trimmed.empty() && isValidIdentifier(trimmed) && !isReservedName(trimmed);
        }

        /**
         * @brief Validate names for uniqueness and validity
         * @param names Vector of names to validate
         * @param outValidity Output vector of validity flags (resized to match names)
         * @return true if all names are valid and unique
         */
        inline bool validateUniqueNames(std::vector<std::string> const & names,
                                        std::vector<bool> & outValidity)
        {
            outValidity.resize(names.size(), true);
            std::unordered_set<std::string> seen;
            bool allValid = true;

            for (size_t i = 0; i < names.size(); ++i)
            {
                std::string const trimmed = trim(names[i]);
                bool valid = isValidParameterName(trimmed);

                if (valid)
                {
                    if (seen.count(trimmed) > 0)
                    {
                        valid = false; // duplicate
                    }
                    else
                    {
                        seen.insert(trimmed);
                    }
                }

                outValidity[i] = valid;
                allValid = allValid && valid;
            }

            return allValid;
        }

    } // namespace validation

} // namespace gladius::ui
