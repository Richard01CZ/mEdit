#pragma once
// Include/Editors/TextEditor.h
// Minimal binary .def editor (010 Editor template) — saves only valid .def binary format

#include <string>
#include <vector>
#include <cstdint>

namespace te {

    struct DefEntry {
        uint32_t mTextId = 0;
        std::string text; // UTF-8 for UI
    };

    class DefModel {
      public:
        DefModel() = default;

        // Binary load/save following:
        // uint32 numStrings;
        // uint32 unknown;
        // struct { uint32 mTextId; uint32 mTextPos; } TextBlock[numStrings];
        // text bytes at mTextPos (null-terminated), encoded CP1250.
        bool LoadFromBinaryFile(const std::string& path); // loads CP1250 -> UTF-8
        bool SaveToBinaryFile(const std::string& path) const; // saves UTF-8 -> CP1250 (writes .def binary)

        std::vector<DefEntry>& Entries() { return m_entries; }
        const std::vector<DefEntry>& Entries() const { return m_entries; }

        void Clear() { m_entries.clear(); }
        void Insert(size_t idx, const DefEntry& e);
        void Remove(size_t idx);
        bool MoveUp(size_t idx);
        bool MoveDown(size_t idx);

        uint32_t unknownField = 0;

      private:
        std::vector<DefEntry> m_entries;
    };

    class TextEditor {
      public:
        TextEditor();
        ~TextEditor();

        // Render the editor (call each frame inside your ImGui loop)
        void Render(bool* pOpen = nullptr);

        // Programmatic load/save
        bool LoadFile(const std::string& path); // loads binary .def
        bool SaveFile(const std::string& path); // saves binary .def (forces .def ext)

      private:
        // UI parts
        void DrawToolbar();
        void DrawTable();

        // native dialog helpers (Win32)
        std::string OpenFileDialog(); // opens .def
        std::string SaveFileDialog(); // only .def, enforces extension

        // helper: validate entries are representable in CP1250; returns bad IDs/texts
        bool ValidateBeforeSave(std::vector<uint32_t>& out_badIds, std::vector<std::string>& out_badTexts) const;

        DefModel m_model;
        std::string m_currentPath;
        int m_selectedRow = -1;
        bool m_dirty = false;

        // Save warning modal state
        bool m_showSaveWarning = false;
        std::vector<uint32_t> m_lastBadIds;
        std::vector<std::string> m_lastBadTexts;
    };

} // namespace te
