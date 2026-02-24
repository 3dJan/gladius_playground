#include "CodeView.h"

#include "../ExpressionToGraphConverter.h"
#include "../ExpressionParser.h"
#include "../FunctionArgument.h"
#include "../nodes/Assembly.h"
#include "../nodes/Model.h"

#include <imgui.h>

namespace gladius::ui
{
    namespace
    {
        auto const kDefaultArgs = std::vector<FunctionArgument>{{"pos", ArgumentType::Vector}};
        auto const kDefaultOutput = FunctionOutput::defaultOutput();
    } // namespace

    static int inputTextCallback(ImGuiInputTextCallbackData * data)
    {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
        {
            auto * str = static_cast<std::string *>(data->UserData);
            str->resize(data->BufTextLen);
            data->Buf = str->data();
        }
        return 0;
    }

    void CodeView::setFunction(nodes::ResourceId functionId,
                               nodes::Model * model,
                               nodes::Assembly * assembly)
    {
        m_currentFunctionId = functionId;
        m_currentModel = model;
        m_currentAssembly = assembly;
    }

    bool CodeView::render()
    {
        if (!m_currentModel)
        {
            ImGui::TextUnformatted("No function selected.");
            return false;
        }

        auto & buf = m_buffers[m_currentFunctionId];

        // Regenerate snippet from graph when the buffer has no unsaved user edits.
        // This ensures graph changes made in the Graph tab are always reflected.
        bool const dirty = (buf.generated && buf.buffer != buf.syncedText);
        if (!dirty)
        {
            auto snippet = ExpressionToGraphConverter::convertGraphToSnippet(
              *m_currentModel, kDefaultArgs, kDefaultOutput, m_currentAssembly);
            if (snippet.empty())
            {
                snippet = "return 0;";
            }
            buf.buffer = snippet;
            buf.syncedText = snippet;
            buf.generated = true;
        }

        // Editable text area with dynamic resizing
        ImVec2 const available = ImGui::GetContentRegionAvail();
        float const buttonBarHeight = 35.f;
        ImGui::InputTextMultiline(
          "##codeview",
          buf.buffer.data(),
          buf.buffer.capacity() + 1,
          ImVec2(available.x, available.y - buttonBarHeight),
          ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput,
          inputTextCallback,
          &buf.buffer);

        bool synced = false;
        bool const hasUnsaved = (buf.buffer != buf.syncedText);

        // Sync button and status
        if (hasUnsaved)
        {
            if (ImGui::Button("Sync to Graph"))
            {
                synced = syncToGraph(buf);
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard"))
            {
                buf.buffer = buf.syncedText;
                m_lastError.clear();
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Unsaved changes");
        }

        if (!m_lastError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", m_lastError.c_str());
            ImGui::PopStyleColor();
        }

        return synced;
    }

    bool CodeView::syncToGraph(CodeBuffer & buf)
    {
        m_lastError.clear();

        // T046: Detect unsupported-node comments before parsing
        if (buf.buffer.find("/* unsupported:") != std::string::npos)
        {
            m_lastError =
              "Cannot sync: code contains unsupported node placeholders "
              "(/* unsupported: ... */). Remove or replace them first.";
            return false;
        }

        // Parse the snippet into a new graph
        ExpressionParser parser;
        nodes::Model tempModel;
        tempModel.createBeginEndWithDefaultInAndOuts();

        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          buf.buffer, tempModel, parser, kDefaultArgs, kDefaultOutput, m_currentAssembly);

        if (nodeId == 0)
        {
            m_lastError = "Syntax error: could not parse the code. Graph unchanged.";
            return false;
        }

        // Success: replace the current model's graph
        m_currentModel->clear();
        m_currentModel->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          buf.buffer, *m_currentModel, parser, kDefaultArgs, kDefaultOutput, m_currentAssembly);
        m_currentModel->updateGraphAndOrderIfNeeded();

        // Regenerate normalized snippet
        auto normalized = ExpressionToGraphConverter::convertGraphToSnippet(
          *m_currentModel, kDefaultArgs, kDefaultOutput, m_currentAssembly);
        if (!normalized.empty())
        {
            buf.buffer = normalized;
        }
        buf.syncedText = buf.buffer;

        return true;
    }

    bool CodeView::hasUnsavedChanges() const
    {
        auto it = m_buffers.find(m_currentFunctionId);
        if (it == m_buffers.end())
        {
            return false;
        }
        return it->second.buffer != it->second.syncedText;
    }

    void CodeView::discardChanges()
    {
        auto it = m_buffers.find(m_currentFunctionId);
        if (it != m_buffers.end())
        {
            it->second.buffer = it->second.syncedText;
            m_lastError.clear();
        }
    }

} // namespace gladius::ui
