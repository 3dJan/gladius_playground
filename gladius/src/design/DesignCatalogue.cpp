#include "DesignCatalogue.h"

#include <algorithm>

namespace gladius::design
{
    void DesignCatalogue::addEntry(CatalogueEntry entry)
    {
        m_entries[entry.id] = std::move(entry);
    }

    void DesignCatalogue::addCompatibilityRule(CompatibilityRule rule)
    {
        m_rules.push_back(std::move(rule));
    }

    bool DesignCatalogue::loadFromJson(nlohmann::json const & doc)
    {
        if (!doc.is_object())
        {
            return false;
        }
        if (doc.contains("entries") && doc["entries"].is_array())
        {
            for (auto const & item : doc["entries"])
            {
                CatalogueEntry e;
                e.id = item.value("id", "");
                e.name = item.value("name", e.id);
                if (e.id.empty())
                {
                    continue;
                }
                if (item.contains("functions") && item["functions"].is_array())
                {
                    for (auto const & f : item["functions"])
                    {
                        if (f.is_string())
                        {
                            e.functions.push_back(f.get<std::string>());
                        }
                    }
                }
                if (item.contains("parameters") && item["parameters"].is_object())
                {
                    for (auto const & [k, v] : item["parameters"].items())
                    {
                        CatalogueParameter p;
                        p.type = v.value("type", "length");
                        if (v.contains("minimum") && v["minimum"].is_number())
                        {
                            p.minimum = v["minimum"].get<double>();
                            p.hasMin = true;
                        }
                        if (v.contains("maximum") && v["maximum"].is_number())
                        {
                            p.maximum = v["maximum"].get<double>();
                            p.hasMax = true;
                        }
                        p.defaultValue = v.value("default", p.hasMin ? p.minimum : 0.0);
                        e.parameters[k] = p;
                    }
                }
                if (item.contains("manufacturing") && item["manufacturing"].is_object())
                {
                    auto const & mfg = item["manufacturing"];
                    e.supportsRequired = mfg.value("supportsRequired", false);
                    if (mfg.contains("processes") && mfg["processes"].is_array())
                    {
                        for (auto const & proc : mfg["processes"])
                        {
                            if (proc.is_string())
                            {
                                e.processes.push_back(proc.get<std::string>());
                            }
                        }
                    }
                }
                if (item.contains("interfaces") && item["interfaces"].is_array())
                {
                    for (auto const & iface : item["interfaces"])
                    {
                        if (iface.is_string())
                        {
                            e.interfaces.push_back(iface.get<std::string>());
                        }
                    }
                }
                e.generator = item.value("generator", "");
                e.snippetTemplate = item.value("snippetTemplate", "");
                addEntry(std::move(e));
            }
        }
        return true;
    }

    std::vector<CatalogueEntry> DesignCatalogue::findByFunction(std::string const & functionName) const
    {
        std::vector<CatalogueEntry> out;
        for (auto const & [id, entry] : m_entries)
        {
            if (std::find(entry.functions.begin(), entry.functions.end(), functionName) != entry.functions.end())
            {
                out.push_back(entry);
            }
        }
        return out;
    }

    bool DesignCatalogue::validateParameters(std::string const & entryId,
                                             nlohmann::json const & params,
                                             std::vector<std::string> & errors) const
    {
        auto it = m_entries.find(entryId);
        if (it == m_entries.end())
        {
            errors.push_back("Unknown catalogue entry: " + entryId);
            return false;
        }
        bool valid = true;
        for (auto const & [paramName, spec] : it->second.parameters)
        {
            if (!params.contains(paramName))
            {
                continue;
            }
            if (!params[paramName].is_number())
            {
                errors.push_back("Parameter '" + paramName + "' must be numeric");
                valid = false;
                continue;
            }
            double const val = params[paramName].get<double>();
            if (spec.hasMin && val < spec.minimum)
            {
                errors.push_back("Parameter '" + paramName + "' below minimum " + std::to_string(spec.minimum));
                valid = false;
            }
            if (spec.hasMax && val > spec.maximum)
            {
                errors.push_back("Parameter '" + paramName + "' above maximum " + std::to_string(spec.maximum));
                valid = false;
            }
        }
        return valid;
    }

    bool DesignCatalogue::checkCompatibility(std::unordered_map<std::string, std::string> const & selections,
                                             std::vector<std::string> & violations) const
    {
        bool ok = true;
        for (auto const & rule : m_rules)
        {
            if (rule.compatible || rule.conditions.empty())
            {
                continue;
            }
            bool allMatch = true;
            for (auto const & [nodeKey, varVal] : rule.conditions)
            {
                auto selIt = selections.find(nodeKey);
                if (selIt == selections.end() || selIt->second != varVal)
                {
                    allMatch = false;
                    break;
                }
            }
            if (allMatch)
            {
                ok = false;
                violations.push_back(rule.rationale.empty() ? "Incompatible variant combination" : rule.rationale);
            }
        }
        return ok;
    }

    std::vector<Configuration> DesignCatalogue::pruneBeam(std::vector<Configuration> const & candidates,
                                                          std::size_t beamWidth) const
    {
        std::vector<Configuration> compatible;
        for (auto const & cfg : candidates)
        {
            std::vector<std::string> violations;
            if (checkCompatibility(cfg.selectedVariants, violations))
            {
                compatible.push_back(cfg);
            }
        }
        std::stable_sort(compatible.begin(), compatible.end(), [](Configuration const & a, Configuration const & b) {
            return a.evaluation.pruningScore > b.evaluation.pruningScore;
        });
        if (compatible.size() > beamWidth)
        {
            compatible.resize(beamWidth);
        }
        return compatible;
    }
} // namespace gladius::design
