#pragma once

namespace gladius
{
    namespace nodes
    {
        class Assembly;
    }

    /**
     * @brief Re-sync every FunctionCall / FunctionGradient node against the model it references.
     *
     * Replacing a function's model (program snippets, graph conversion) changes the set of ports
     * the callee exposes, but the call nodes of the *calling* models keep their old ports and the
     * links that used them. That leaves references to outputs which no longer exist: the
     * GraphFlattener then fails with "Output &lt;name&gt; not found in end node" and the WGSL
     * generator with "Could not find port for input source: &lt;name&gt;".
     *
     * Unlike Assembly::updateInputsAndOutputs(), this also removes ports (and the parameters
     * sourcing them) which the callee does not expose anymore.
     */
    void syncFunctionCallPorts(nodes::Assembly& assembly);
}
