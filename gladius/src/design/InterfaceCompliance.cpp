#include "InterfaceCompliance.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace gladius::design
{
    namespace
    {
        /// @brief Reads an axis-aligned box from `[minx,miny,minz,maxx,maxy,maxz]` or `[[x,y,z],[x,y,z]]`
        bool readBox(nlohmann::json const & value, double out[6])
        {
            if (!value.is_array())
            {
                return false;
            }
            if (value.size() == 6)
            {
                for (std::size_t i = 0; i < 6; ++i)
                {
                    if (!value[i].is_number())
                    {
                        return false;
                    }
                    out[i] = value[i].get<double>();
                }
                return true;
            }
            if (value.size() == 2)
            {
                for (std::size_t c = 0; c < 2; ++c)
                {
                    nlohmann::json const & corner = value[c];
                    if (!corner.is_array() || corner.size() != 3)
                    {
                        return false;
                    }
                    for (std::size_t axis = 0; axis < 3; ++axis)
                    {
                        if (!corner[axis].is_number())
                        {
                            return false;
                        }
                        out[c * 3 + axis] = corner[axis].get<double>();
                    }
                }
                return true;
            }
            return false;
        }

        /// @brief True when the box `inner` is empty or lies inside the box `outer` on every axis
        bool boxContains(double const outer[6], double const inner[6])
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                if (inner[axis] < outer[axis] - kInterfaceNumericTolerance ||
                    inner[axis + 3] > outer[axis + 3] + kInterfaceNumericTolerance)
                {
                    return false;
                }
            }
            return true;
        }

        /// @brief True when either box fits inside the other, so both describe usable space
        bool envelopesCompatible(nlohmann::json const & a, nlohmann::json const & b)
        {
            double boxA[6] = {};
            double boxB[6] = {};
            if (!readBox(a, boxA) || !readBox(b, boxB))
            {
                return false;
            }
            return boxContains(boxA, boxB) || boxContains(boxB, boxA);
        }

        /// @brief True when both sides declare the same intent for one specification key
        bool valuesCompatible(std::string const & key, nlohmann::json const & a, nlohmann::json const & b, double tolerance)
        {
            if (a.is_number() && b.is_number())
            {
                return std::abs(a.get<double>() - b.get<double>()) <= tolerance;
            }
            if (key == "envelope" && envelopesCompatible(a, b))
            {
                return true;
            }
            return a == b;
        }

        std::string mismatchText(std::string const & name,
                                 std::string const & kind,
                                 std::string const & key,
                                 nlohmann::json const & a,
                                 nlohmann::json const & b)
        {
            return "'" + name + "' (" + kind + "), key '" + key + "': " + a.dump() + " vs " + b.dump();
        }

        void addUnconstrained(InterfaceCompliance & result, std::string const & key)
        {
            result.unconstrainedKeys.push_back(key);
        }
    } // namespace

    std::string InterfaceCompliance::describe() const
    {
        if (compatible && mismatches.empty())
        {
            return "compatible";
        }
        std::string text;
        for (auto const & mismatch : mismatches)
        {
            if (!text.empty())
            {
                text += "; ";
            }
            text += mismatch;
        }
        return text.empty() ? "incompatible" : text;
    }

    InterfaceCompliance compareInterfaceSpecifications(nlohmann::json const & a,
                                                      nlohmann::json const & b,
                                                      double tolerance)
    {
        InterfaceCompliance result;
        result.compatible = true;
        if (!a.is_object() || !b.is_object())
        {
            return result;
        }

        std::vector<std::string> keys;
        keys.reserve(a.size() + b.size());
        for (auto const & [key, value] : a.items())
        {
            keys.push_back(key);
            (void)value;
        }
        for (auto const & [key, value] : b.items())
        {
            if (!a.contains(key))
            {
                keys.push_back(key);
            }
            (void)value;
        }

        for (auto const & key : keys)
        {
            if (!a.contains(key) || !b.contains(key))
            {
                addUnconstrained(result, key);
                continue;
            }
            nlohmann::json const & valA = a[key];
            nlohmann::json const & valB = b[key];
            if (!valuesCompatible(key, valA, valB, tolerance))
            {
                result.compatible = false;
                result.mismatches.push_back(key + ": " + valA.dump() + " vs " + valB.dump());
            }
        }
        return result;
    }

    InterfaceCompliance checkInterfacePair(Interface const & a, Interface const & b)
    {
        if (a.name != b.name || a.kind != b.kind)
        {
            InterfaceCompliance unconstrained;
            unconstrained.compatible = true;
            return unconstrained;
        }
        InterfaceCompliance result = compareInterfaceSpecifications(a.specification, b.specification);
        for (auto & mismatch : result.mismatches)
        {
            std::string const key = mismatch.substr(0, mismatch.find(':'));
            nlohmann::json const valA = a.specification.contains(key) ? a.specification[key] : nlohmann::json(nullptr);
            nlohmann::json const valB = b.specification.contains(key) ? b.specification[key] : nlohmann::json(nullptr);
            mismatch = mismatchText(a.name, a.kind, key, valA, valB);
        }
        return result;
    }

    std::vector<InterfaceCompliance> checkInterfacesAgainstConfiguration(std::vector<Interface> const & candidate,
                                                                        std::vector<Interface> const & reference)
    {
        std::vector<InterfaceCompliance> problems;
        for (auto const & ifaceA : candidate)
        {
            for (auto const & ifaceB : reference)
            {
                if (ifaceA.name != ifaceB.name || ifaceA.kind != ifaceB.kind)
                {
                    continue;
                }
                InterfaceCompliance result = checkInterfacePair(ifaceA, ifaceB);
                if (!result.compatible)
                {
                    problems.push_back(std::move(result));
                }
            }
        }
        return problems;
    }

    InterfaceCompliance checkEnvelopeContainment(nlohmann::json const & envelope, nlohmann::json const & child)
    {
        InterfaceCompliance result;
        result.compatible = true;

        double parentBox[6] = {};
        double childBox[6] = {};
        if (!envelope.is_object() || !child.is_object() || !envelope.contains("envelope") ||
            !child.contains("envelope"))
        {
            return result;
        }
        if (!readBox(envelope["envelope"], parentBox) || !readBox(child["envelope"], childBox))
        {
            return result;
        }
        if (!boxContains(parentBox, childBox))
        {
            result.compatible = false;
            result.mismatches.push_back("envelope: child sticks out of the space reserved by its parent");
            result.unconstrainedKeys.push_back("envelope");
        }
        return result;
    }

    InterfaceCompliance checkEnvelopeContainmentBetween(std::vector<Interface> const & parent,
                                                       std::vector<Interface> const & child)
    {
        InterfaceCompliance combined;
        combined.compatible = true;
        for (auto const & childIface : child)
        {
            for (auto const & parentIface : parent)
            {
                if (parentIface.name != childIface.name || parentIface.kind != childIface.kind)
                {
                    continue;
                }
                auto const result = checkEnvelopeContainment(parentIface.specification, childIface.specification);
                if (!result.compatible)
                {
                    combined.compatible = false;
                    combined.mismatches.insert(combined.mismatches.end(), result.mismatches.begin(), result.mismatches.end());
                }
            }
        }
        return combined;
    }

    bool evaluateInterfaceChecklistEntry(std::vector<Interface> const & candidate,
                                        std::vector<Interface> const & reference,
                                        nlohmann::json & evidence)
    {
        std::vector<InterfaceCompliance> const problems = checkInterfacesAgainstConfiguration(candidate, reference);
        nlohmann::json details = nlohmann::json::array();
        for (auto const & problem : problems)
        {
            details.push_back(problem.describe());
        }
        evidence = {{"compatible", problems.empty()}, {"mismatches", std::move(details)}};
        return problems.empty();
    }
} // namespace gladius::design
