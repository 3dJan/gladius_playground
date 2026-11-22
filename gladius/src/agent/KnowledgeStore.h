#pragma once

#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gladius::agent
{
    /// @brief Concise, structured unit of agent experience or domain knowledge
    struct KnowledgeEntry
    {
        std::string id;
        std::string title;
        std::string category;
        std::vector<std::string> tags;
        std::string summary;
        std::string createdAt;
        std::string updatedAt;
        int useCount = 0;
    };

    /// @brief Cross-platform persistent knowledge store (desktop & Emscripten IDBFS)
    class KnowledgeStore
    {
      public:
        KnowledgeStore() = default;
        explicit KnowledgeStore(std::filesystem::path storagePath);

        void setStoragePath(std::filesystem::path storagePath);
        [[nodiscard]] std::filesystem::path storagePath() const;

        /// @brief Default cross-platform storage path (sago::getConfigHome()/gladius or /gladius on Web)
        [[nodiscard]] static std::filesystem::path defaultStoragePath();

        bool load();
        bool save() const;

        std::string store(std::string title,
                          std::string category,
                          std::vector<std::string> tags,
                          std::string summary);

        bool update(std::string const & id,
                    std::string const & title,
                    std::string const & category,
                    std::vector<std::string> const & tags,
                    std::string const & summary);

        bool remove(std::string const & id);

        [[nodiscard]] std::vector<KnowledgeEntry> search(std::string const & query,
                                                         std::string const & category = "",
                                                         std::size_t limit = 8);

        [[nodiscard]] std::vector<KnowledgeEntry> listAll() const;
        [[nodiscard]] std::string summarizeForPrompt(std::string const & query = "",
                                                     std::size_t limit = 5);

        void clear();

      private:
        mutable std::mutex m_mutex;
        std::filesystem::path m_storagePath;
        std::vector<KnowledgeEntry> m_entries;
        std::size_t m_nextId = 1;

        static std::string currentIsoTimestamp();
    };
} // namespace gladius::agent
