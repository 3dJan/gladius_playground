#include "DesignCatalogue.h"

#include <algorithm>
#include <array>
#include <fstream>

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
        if (doc.contains("compatibilityRules") && doc["compatibilityRules"].is_array())
        {
            for (auto const & r : doc["compatibilityRules"])
            {
                CompatibilityRule rule;
                rule.compatible = r.value("compatible", false);
                rule.rationale = r.value("rationale", "");
                if (r.contains("conditions") && r["conditions"].is_array())
                {
                    for (auto const & cond : r["conditions"])
                    {
                        if (cond.is_array() && cond.size() == 2 && cond[0].is_string() && cond[1].is_string())
                        {
                            rule.conditions.emplace_back(cond[0].get<std::string>(), cond[1].get<std::string>());
                        }
                        else if (cond.is_object())
                        {
                            std::string const nodeKey = cond.value("node", "");
                            std::string const varVal = cond.value("variant", "");
                            if (!nodeKey.empty() && !varVal.empty())
                            {
                                rule.conditions.emplace_back(nodeKey, varVal);
                            }
                        }
                    }
                }
                if (!rule.conditions.empty())
                {
                    addCompatibilityRule(std::move(rule));
                }
            }
        }
        return true;
    }

    CatalogueEntry const * DesignCatalogue::findById(std::string const & entryId) const
    {
        auto it = m_entries.find(entryId);
        return it != m_entries.end() ? &it->second : nullptr;
    }

    bool DesignCatalogue::loadFromFile(std::filesystem::path const & path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            return false;
        }
        try
        {
            nlohmann::json doc;
            file >> doc;
            return loadFromJson(doc);
        }
        catch (...)
        {
            return false;
        }
    }

    bool DesignCatalogue::loadDefaultCatalogue()
    {
        static std::array<char const *, 4> const kCandidatePaths = {
          "resources/design_catalogue.json",
          "gladius/resources/design_catalogue.json",
          "../resources/design_catalogue.json",
          "../../resources/design_catalogue.json"};

        for (char const * candidate : kCandidatePaths)
        {
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec) && loadFromFile(candidate))
            {
                return true;
            }
        }

        // Embedded baseline fallback so catalogue queries and compatibility checks always work
        // even when launched from a working directory without resources/ next to the binary.
        nlohmann::json const fallback = {
          {"entries",
           {{{"id", "enclosure.shell.rounded_box"},
             {"name", "Rounded Box Shell Enclosure"},
             {"functions", {"Enclosure", "Housing", "ProtectiveShell"}},
             {"parameters",
              {{"width", {{"type", "length"}, {"minimum", 15.0}, {"maximum", 350.0}, {"default", 80.0}}},
               {"depth", {{"type", "length"}, {"minimum", 15.0}, {"maximum", 350.0}, {"default", 60.0}}},
               {"height", {{"type", "length"}, {"minimum", 10.0}, {"maximum", 300.0}, {"default", 40.0}}},
               {"wallThickness", {{"type", "length"}, {"minimum", 1.2}, {"maximum", 12.0}, {"default", 2.4}}}}},
             {"manufacturing", {{"processes", {"FDM", "SLA", "SLS"}}, {"supportsRequired", false}}},
             {"interfaces", {"planar_mount", "lid_seam"}}},
            {{"id", "enclosure.shell.cylindrical"},
             {"name", "Cylindrical Shell Enclosure"},
             {"functions", {"Enclosure", "Housing", "ProtectiveShell"}},
             {"parameters",
              {{"radius", {{"type", "length"}, {"minimum", 8.0}, {"maximum", 180.0}, {"default", 35.0}}},
               {"height", {{"type", "length"}, {"minimum", 10.0}, {"maximum", 300.0}, {"default", 60.0}}},
               {"wallThickness", {{"type", "length"}, {"minimum", 1.2}, {"maximum", 12.0}, {"default", 2.4}}}}},
             {"manufacturing", {{"processes", {"FDM", "SLA", "SLS"}}, {"supportsRequired", false}}},
             {"interfaces", {"circular_spigot", "axial_thread"}}},
            {{"id", "stand.pedestal.weighted"},
             {"name", "Weighted Pedestal Base"},
             {"functions", {"Stand", "Base", "Support"}},
             {"manufacturing", {{"processes", {"FDM", "SLA", "SLS"}}, {"supportsRequired", false}}},
             {"interfaces", {"circular_spigot", "planar_mount"}}},
            {{"id", "stand.tripod.spread"},
             {"name", "Three-Leg Tripod Stand"},
             {"functions", {"Stand", "Base", "Support"}},
             {"manufacturing", {{"processes", {"FDM", "SLS"}}, {"supportsRequired", true}}},
             {"interfaces", {"circular_spigot"}}},
            {{"id", "stand.clamp.desk"},
             {"name", "C-Clamp Desk Mount"},
             {"functions", {"Stand", "Mount", "Clamp"}},
             {"manufacturing", {{"processes", {"FDM", "SLS"}}, {"supportsRequired", false}}},
             {"interfaces", {"planar_mount", "m3_fastener_pattern"}}},
            {{"id", "mount.boss.m3_heatset"},
             {"name", "M3 Heat-Set Insert Boss"},
             {"functions", {"Fastening", "Mount", "Interface"}},
             {"manufacturing", {{"processes", {"FDM", "SLA", "SLS"}}, {"supportsRequired", false}}},
             {"interfaces", {"m3_fastener_pattern"}}},
            {{"id", "mount.snapfit.cantilever"},
             {"name", "Cantilever Snap-Fit Clip"},
             {"functions", {"Fastening", "Latch", "Interface"}},
             {"manufacturing", {{"processes", {"FDM", "SLS"}}, {"supportsRequired", false}}},
             {"interfaces", {"lid_seam"}}},
            {{"id", "core.lattice.gyroid"},
             {"name", "Gyroid TPMS Lightweight Core"},
             {"functions", {"Infill", "LightweightCore", "HeatDissipation"}},
             {"manufacturing", {{"processes", {"FDM", "SLA", "SLS"}}, {"supportsRequired", false}}},
             {"interfaces", {"volumetric_core"}}},
            {{"id", "routing.channel.rear_slot"},
             {"name", "Rear Cable Routing Slot"},
             {"functions", {"CableRoute", "Ventilation"}},
             {"manufacturing", {{"processes", {"FDM", "SLA", "SLS"}}, {"supportsRequired", false}}},
             {"interfaces", {"cable_exit"}}},
            {{"id", "shade.diffuser.cone"},
             {"name", "Conical Light Diffuser Shade"},
             {"functions", {"Shade", "Diffuser", "Cover"}},
             {"manufacturing", {{"processes", {"FDM", "SLA", "SLS"}}, {"supportsRequired", false}}},
             {"interfaces", {"circular_spigot"}}},
            {{"id", "shade.diffuser.ribbed"},
             {"name", "Ribbed Cylindrical Diffuser"},
             {"functions", {"Shade", "Diffuser"}},
             {"manufacturing", {{"processes", {"FDM", "SLA", "SLS"}}, {"supportsRequired", false}}},
             {"interfaces", {"circular_spigot"}}}}},
          {"compatibilityRules",
           {{{"conditions", {{{"node", "Stand"}, {"variant", "stand.clamp.desk"}}, {{"node", "Shade"}, {"variant", "shade.diffuser.ribbed"}}}},
             {"compatible", false},
             {"rationale", "Desk C-clamp lacks a concentric spigot collar for the ribbed cylindrical diffuser"}},
            {{"conditions", {{{"node", "Enclosure"}, {"variant", "enclosure.shell.cylindrical"}}, {{"node", "Fastening"}, {"variant", "mount.snapfit.cantilever"}}}},
             {"compatible", false},
             {"rationale", "Planar cantilever snap-fit requires a straight lid seam rather than a cylindrical shell"}}}}};
        return loadFromJson(fallback);
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
