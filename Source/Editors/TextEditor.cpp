// Source/Editors/TextEditor.cpp
// Refactored: clearer structure, smaller helper functions, consistent naming.
// Preserves original functionality: reads/writes binary .def format and converts
// between code page 1250 (Windows-1250 / CP1250) and UTF-8 so characters like
// ě š č ř ž ň ľ ů display correctly in ImGui text fields.

#include "Editors/TextEditor.h"

#include <imgui.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <limits>
#include <cstdint>
#include <optional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

using namespace te;

// ----------------------------- Constants & Types -----------------------------
static constexpr uint32_t RESERVED_EOF_ID = 99999999u;
static constexpr uint32_t RESERVED_MAX_NORMAL_ID = 99999998u;
// Removed MAX_ENTRIES_CAP — entry count limited only by 32?bit ID range and EOF sentinel
// (Removed MAX_STRING_BYTES — no artificial cap, strings rely only on null terminator and file size)
static const std::string EOF_TEXT_LITERAL = "EndOfFile!";

struct TableEntry {
    uint32_t id;
    uint32_t pos;
};

// ------------------------- Encoding helpers (CP1250 <-> UTF-8) -------------------------
#ifdef _WIN32

static std::string cp1250_to_utf8(const std::string& cp1250) {
    if(cp1250.empty()) return {};
    int wlen = MultiByteToWideChar(1250, 0, cp1250.data(), (int)cp1250.size(), nullptr, 0);
    if(wlen <= 0) return {};
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(1250, 0, cp1250.data(), (int)cp1250.size(), &w[0], wlen);

    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if(ulen <= 0) return {};
    std::string u(static_cast<size_t>(ulen), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, &u[0], ulen, nullptr, nullptr);
    return u;
}

// Convert UTF-8 -> CP1250. Returns (bytes, usedDefaultChar)
static std::pair<std::string, bool> utf8_to_cp1250_checked(const std::string& utf8) {
    if(utf8.empty()) return {std::string(), false};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if(wlen <= 0) return {{}, true};
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], wlen);

    BOOL usedDefault = FALSE;
    int blen = WideCharToMultiByte(1250, 0, w.data(), wlen, nullptr, 0, nullptr, &usedDefault);
    if(blen <= 0) return {{}, true};
    std::string b(static_cast<size_t>(blen), '\0');
    WideCharToMultiByte(1250, 0, w.data(), wlen, &b[0], blen, nullptr, &usedDefault);
    return {std::move(b), usedDefault != FALSE};
}

// Simpler conversion used for actual writing (no usedDefault reporting)
static std::string utf8_to_cp1250_simple(const std::string& utf8) {
    if(utf8.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if(wlen <= 0) return {};
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], wlen);

    int blen = WideCharToMultiByte(1250, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if(blen <= 0) return {};
    std::string b(static_cast<size_t>(blen), '\0');
    WideCharToMultiByte(1250, 0, w.data(), wlen, &b[0], blen, nullptr, nullptr);
    return b;
}

#else
// Non-Windows fallback: no CP1250 conversion available here; assume UTF-8 == CP1250 bytes
static std::string cp1250_to_utf8(const std::string& s) { return s; }
static std::pair<std::string, bool> utf8_to_cp1250_checked(const std::string& utf8) { return {utf8, false}; }
static std::string utf8_to_cp1250_simple(const std::string& s) { return s; }
#endif

// ----------------------------- Binary file helpers -----------------------------

// Read exactly a uint32_t from stream. Returns nullopt on failure.
static std::optional<uint32_t> read_u32(std::ifstream& in) {
    uint32_t v = 0;
    if(!in.read(reinterpret_cast<char*>(&v), sizeof(v))) return std::nullopt;
    return v;
}

// Read a null-terminated C string at given absolute offset. Respect fileSize limits.
static std::string read_cstring_at(std::ifstream& in, std::streamoff pos, std::streamoff fileSize) {
    // If the position is outside the file, return empty string.
    if(pos < 0 || pos >= fileSize) return {};
    in.seekg(pos);
    if(!in.good()) return {};

    std::string s;
    s.reserve(256);
    char c = 0;
    // Read bytes until NUL or until we reach the known file end. No artificial per-string cap.
    while(in.get(c)) {
        if(c == '\0') break;
        s.push_back(c);
        // If tellg is invalid or we've reached/passed fileSize, stop.
        auto cur = in.tellg();
        if(cur == -1 || cur >= fileSize) break;
    }
    return s;
}

// ----------------------------- DefModel I/O (refactored) -----------------------------

bool DefModel::LoadFromBinaryFile(const std::string& path) {
    m_entries.clear();
    unknownField = 0;

    std::ifstream in(path, std::ios::binary);
    if(!in) return false;

    in.seekg(0, std::ios::end);
    std::streamoff fileSize = in.tellg();
    if(fileSize <= 0) return false;
    in.seekg(0, std::ios::beg);

    const std::streamoff minHeader = static_cast<std::streamoff>(sizeof(uint32_t) * 2);
    if(fileSize < minHeader) return false;

    // read header
    auto optNum = read_u32(in);
    if(!optNum) return false;
    uint32_t numStrings = *optNum;

    auto optUnknown = read_u32(in);
    if(!optUnknown) return false;
    unknownField = *optUnknown;

    // No artificial cap on numStrings; rely on valid EOF sentinel and file bounds

    // check table size fits
    std::streamoff tableBytes = static_cast<std::streamoff>(numStrings) * static_cast<std::streamoff>(sizeof(uint32_t) * 2);
    auto curAfterHeader = in.tellg();
    if(curAfterHeader < 0) return false;
    if(curAfterHeader + tableBytes > fileSize) return false;

    // read table
    std::vector<TableEntry> table;
    table.reserve(numStrings);
    for(uint32_t i = 0; i < numStrings; ++i) {
        auto optId = read_u32(in);
        if(!optId) return false;
        auto optPos = read_u32(in);
        if(!optPos) return false;
        table.push_back({*optId, *optPos});
    }

    // read strings by table entries
    for(uint32_t i = 0; i < numStrings; ++i) {
        uint32_t pos = table[i].pos;
        if(static_cast<std::streamoff>(pos) >= fileSize) {
            DefEntry e;
            e.mTextId = table[i].id;
            e.text.clear();
            m_entries.push_back(std::move(e));
            continue;
        }

        std::string raw = read_cstring_at(in, static_cast<std::streamoff>(pos), fileSize);

        // convert CP1250 bytes into UTF-8 for UI editing
        std::string utf = cp1250_to_utf8(raw);

        DefEntry e;
        e.mTextId = table[i].id;
        e.text = std::move(utf);
        m_entries.push_back(std::move(e));
    }

    return true;
}

bool DefModel::SaveToBinaryFile(const std::string& path) const {
    const uint32_t numStrings = static_cast<uint32_t>(m_entries.size());
    const uint32_t headerSize = sizeof(uint32_t) * 2; // num + unknown
    const uint32_t tableSize = numStrings * (sizeof(uint32_t) * 2);
    uint32_t textStart = headerSize + tableSize;

    std::ofstream out(path, std::ios::binary);
    if(!out) return false;

    // write header
    uint32_t num = numStrings;
    out.write(reinterpret_cast<const char*>(&num), sizeof(num));
    uint32_t unk = unknownField;
    out.write(reinterpret_cast<const char*>(&unk), sizeof(unk));

    // compute positions (taking into account CP1250 encoding length)
    std::vector<uint32_t> positions;
    positions.reserve(numStrings);
    uint64_t curPos = textStart;
    for(const auto& e: m_entries) {
#ifdef _WIN32
        std::string cp = utf8_to_cp1250_simple(e.text);
        uint32_t len = static_cast<uint32_t>(cp.size());
#else
        uint32_t len = static_cast<uint32_t>(e.text.size());
#endif
        // guard overflow
        if(curPos + (uint64_t)len + 1u > (uint64_t)UINT32_MAX) return false;
        positions.push_back(static_cast<uint32_t>(curPos));
        curPos = curPos + (uint64_t)len + 1u;
    }

    // write table (id, pos)
    for(uint32_t i = 0; i < numStrings; ++i) {
        uint32_t idv = m_entries[i].mTextId;
        uint32_t posv = positions[i];
        out.write(reinterpret_cast<const char*>(&idv), sizeof(idv));
        out.write(reinterpret_cast<const char*>(&posv), sizeof(posv));
    }

    // write string bytes (CP1250) + nul
    for(uint32_t i = 0; i < numStrings; ++i) {
#ifdef _WIN32
        std::string cp = utf8_to_cp1250_simple(m_entries[i].text);
        out.write(cp.data(), cp.size());
#else
        out.write(m_entries[i].text.c_str(), m_entries[i].text.size());
#endif
        char nul = '\0';
        out.write(&nul, 1);
    }

    return true;
}

// ----------------------------- Model helpers (insert/remove/move) -----------------------------

void DefModel::Insert(size_t idx, const DefEntry& e) {
    if(idx >= m_entries.size()) m_entries.push_back(e);
    else
        m_entries.insert(m_entries.begin() + idx, e);
}
void DefModel::Remove(size_t idx) {
    if(idx < m_entries.size()) m_entries.erase(m_entries.begin() + idx);
}
bool DefModel::MoveUp(size_t idx) {
    if(idx == 0 || idx >= m_entries.size()) return false;
    std::swap(m_entries[idx - 1], m_entries[idx]);
    return true;
}
bool DefModel::MoveDown(size_t idx) {
    if(idx + 1 >= m_entries.size()) return false;
    std::swap(m_entries[idx], m_entries[idx + 1]);
    return true;
}

// ----------------------------- Sorting & EOF invariant -----------------------------

static void ResortAndEnsureEOF(std::vector<DefEntry>& entries, bool& dirtyFlag, int& selectedRow) {
    // stable sort by id while keeping reserved EOF at the end
    std::stable_sort(entries.begin(), entries.end(), [](const DefEntry& a, const DefEntry& b) {
        uint32_t ai = a.mTextId, bi = b.mTextId;
        if(ai == RESERVED_EOF_ID && bi == RESERVED_EOF_ID) return false;
        if(ai == RESERVED_EOF_ID) return false;
        if(bi == RESERVED_EOF_ID) return true;
        return ai < bi;
    });

    // find last valid EOF with exact text
    int lastGood = -1;
    for(int i = 0; i < (int)entries.size(); ++i) {
        if(entries[i].mTextId == RESERVED_EOF_ID && entries[i].text == EOF_TEXT_LITERAL) lastGood = i;
        else if(entries[i].mTextId == RESERVED_EOF_ID) {
            // wrong text for reserved id => clamp to near-max
            entries[i].mTextId = RESERVED_MAX_NORMAL_ID;
            dirtyFlag = true;
        }
    }

    if(lastGood == -1) {
        // append EOF sentinel
        DefEntry eof;
        eof.mTextId = RESERVED_EOF_ID;
        eof.text = EOF_TEXT_LITERAL;
        entries.push_back(std::move(eof));
        dirtyFlag = true;
    } else if(lastGood != (int)entries.size() - 1) {
        DefEntry eof = entries[lastGood];
        entries.erase(entries.begin() + lastGood);
        entries.push_back(std::move(eof));
        dirtyFlag = true;
        selectedRow = -1;
    }

    // remove duplicate EOF entries earlier
    for(int i = (int)entries.size() - 2; i >= 0; --i) {
        if(entries[i].mTextId == RESERVED_EOF_ID) {
            entries.erase(entries.begin() + i);
            dirtyFlag = true;
            if(selectedRow == i) selectedRow = -1;
            else if(selectedRow > i)
                --selectedRow;
        }
    }
}

// ----------------------------- ImGui helpers -----------------------------

// flashing map: row index -> remaining blink ticks (6 ticks -> 3 flashes)
static std::unordered_map<int, int> s_idFlashTicks;

// Callback resize helper for ImGui InputText to grow buffer stored in vector<char>
static int ImGui_InputText_CallbackResize_Vector(ImGuiInputTextCallbackData* data) {
    auto vec = reinterpret_cast<std::vector<char>*>(data->UserData);
    if(!vec) return 0;
    vec->resize((size_t)data->BufTextLen + 1); // +1 for nul
    data->Buf = vec->data();
    return 0;
}

// A compatibility helper that lets us edit std::string with ImGui InputText reliably.
static bool ImGuiInputText_EditStdString(const char* label, std::string& str, ImGuiInputTextFlags flags) {
    std::vector<char> buf;
    buf.resize(str.size() + 1);
    if(!str.empty()) std::memcpy(buf.data(), str.c_str(), str.size() + 1);
    else
        buf[0] = '\0';

    ImGuiInputTextFlags useFlags = flags | ImGuiInputTextFlags_CallbackResize;
    bool pressedEnter = ImGui::InputText(label, buf.data(), buf.size(), useFlags, ImGui_InputText_CallbackResize_Vector, (void*)&buf);
    if(pressedEnter || ImGui::IsItemDeactivatedAfterEdit()) {
        str.assign(buf.data());
        return pressedEnter;
    }
    return false;
}

// ----------------------------- TextEditor UI & Flow -----------------------------

TextEditor::TextEditor() = default;
TextEditor::~TextEditor() = default;

bool TextEditor::LoadFile(const std::string& path) {
    if(!m_model.LoadFromBinaryFile(path)) return false;
    m_currentPath = path;
    m_selectedRow = -1;
    m_dirty = false;

    auto& entries = m_model.Entries();
    ResortAndEnsureEOF(entries, m_dirty, m_selectedRow);
    s_idFlashTicks.clear();
    return true;
}

static inline std::string force_def_extension(std::string path) {
    if(path.size() >= 4) {
        std::string ext = path.substr(path.size() - 4);
        for(auto& c: ext)
            c = static_cast<char>(std::tolower((unsigned char)c));
        if(ext == ".def") return path;
    }
    path += ".def";
    return path;
}

bool TextEditor::ValidateBeforeSave(std::vector<uint32_t>& out_badIds, std::vector<std::string>& out_badTexts) const {
    out_badIds.clear();
    out_badTexts.clear();
#ifdef _WIN32
    for(const auto& e: m_model.Entries()) {
        if(e.mTextId == RESERVED_EOF_ID && e.text == EOF_TEXT_LITERAL) continue;
        auto res = utf8_to_cp1250_checked(e.text);
        if(res.second) {
            out_badIds.push_back(e.mTextId);
            out_badTexts.push_back(e.text);
        }
    }
    return out_badIds.empty();
#else
    (void)out_badIds;
    (void)out_badTexts;
    return true;
#endif
}

bool TextEditor::SaveFile(const std::string& path_in) {
    if(path_in.empty()) return false;
    std::string path = force_def_extension(path_in);

    std::vector<uint32_t> badIds;
    std::vector<std::string> badTexts;
    if(!ValidateBeforeSave(badIds, badTexts)) {
        m_showSaveWarning = true;
        m_lastBadIds = std::move(badIds);
        m_lastBadTexts = std::move(badTexts);
        return false;
    }

    if(!m_model.SaveToBinaryFile(path)) return false;
    m_currentPath = path;
    m_dirty = false;
    return true;
}

// Platform dialogs (Open/Save) - Windows implementation only.
std::string TextEditor::OpenFileDialog() {
#ifdef _WIN32
    char filePath[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "DEF Binary (*.def;*.bin)\0*.def;*.bin\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if(GetOpenFileNameA(&ofn)) return std::string(filePath);
#endif
    return {};
}

std::string TextEditor::SaveFileDialog() {
#ifdef _WIN32
    char filePath[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "DEF files (*.def)\0*.def\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if(GetSaveFileNameA(&ofn)) { return force_def_extension(std::string(filePath)); }
#endif
    return {};
}

void TextEditor::DrawToolbar() {
    ImGui::BeginChild("##toolbar", ImVec2(0, 28), false);

    if(ImGui::Button("Open")) {
        std::string p = OpenFileDialog();
        if(!p.empty()) LoadFile(p);
    }
    ImGui::SameLine();
    if(ImGui::Button("Save")) {
        std::string p = SaveFileDialog();
        if(!p.empty()) SaveFile(p);
    }

    ImGui::SameLine();
    if(ImGui::Button("Add")) {
        auto& entries = m_model.Entries();
        uint32_t maxId = 0;
        for(const auto& e: entries) {
            if(e.mTextId == RESERVED_EOF_ID) continue;
            if(e.mTextId > maxId) maxId = e.mTextId;
        }
        uint32_t newId = maxId + 1;
        if(newId > RESERVED_MAX_NORMAL_ID) newId = RESERVED_MAX_NORMAL_ID;
        DefEntry ne;
        ne.mTextId = newId;
        ne.text.clear();
        entries.push_back(std::move(ne));
        m_dirty = true;
        ResortAndEnsureEOF(entries, m_dirty, m_selectedRow);
        // select added row
        m_selectedRow = -1;
        for(int i = 0; i < (int)entries.size(); ++i) {
            if(entries[i].mTextId == newId && !(entries[i].mTextId == RESERVED_EOF_ID && entries[i].text == EOF_TEXT_LITERAL)) {
                m_selectedRow = i;
                break;
            }
        }
    }

    ImGui::SameLine();
    ImGui::Text("Entries: %u", (uint32_t)m_model.Entries().size());
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    ImGui::TextColored(m_dirty ? ImVec4(1, 0.6f, 0, 1) : ImVec4(0.6f, 1, 0.6f, 1), m_dirty ? "Modified" : "Saved");
    ImGui::EndChild();
}

void TextEditor::DrawTable() {
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV;
    if(!ImGui::BeginTable("def_table", 3, flags, ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) return;
    ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    auto& entries = m_model.Entries();
    std::vector<int> rowsToDelete;
    rowsToDelete.reserve(4);
    std::vector<std::tuple<int, uint32_t, uint32_t>> idEdits;
    idEdits.reserve(8);
    std::vector<std::pair<int, std::string>> textEdits;
    textEdits.reserve(8);

    bool needResortAfterLoop = false;
    uint32_t idToSelectAfterResort = 0;
    bool haveIdToSelect = false;

    ImGuiIO& io = ImGui::GetIO();
    const bool enterPressedGlobal = ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Enter));

    for(int row = 0; row < (int)entries.size(); ++row) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", row);

        ImGui::TableNextColumn();
        bool isProtected = (entries[row].mTextId == RESERVED_EOF_ID && entries[row].text == EOF_TEXT_LITERAL);
        uint32_t idv = entries[row].mTextId;
        std::string idLabel = std::string("ID##") + std::to_string(row);

        auto itFlash = s_idFlashTicks.find(row);
        bool shouldFlash = (itFlash != s_idFlashTicks.end() && itFlash->second > 0);
        if(shouldFlash) {
            bool phase = (itFlash->second % 2) == 0;
            ImGui::PushStyleColor(ImGuiCol_FrameBg, phase ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        }

        if(isProtected) {
            ImGui::BeginDisabled(true);
            ImGui::InputScalar(idLabel.c_str(), ImGuiDataType_U32, &idv, nullptr);
            ImGui::EndDisabled();
        } else {
            char idBuf[16];
            int written = snprintf(idBuf, sizeof(idBuf), "%u", idv);
            if(written < 0) idBuf[0] = '\\0';

            // commit when user presses Enter or when the widget is deactivated after edit
            bool pressedEnter = ImGui::InputText(idLabel.c_str(), idBuf, sizeof(idBuf), ImGuiInputTextFlags_EnterReturnsTrue);
            bool deactivated = ImGui::IsItemDeactivatedAfterEdit();

            if(pressedEnter || deactivated) {
                // reject empty or any non-digit characters (this also rejects '-')
                // trim leading/trailing whitespace (spaces, CR, LF) before validation
                int len = (int)strnlen(idBuf, sizeof(idBuf));
                int starti = 0;
                while(starti < len && (unsigned char)idBuf[starti] <= ' ')
                    ++starti;
                int endi = len - 1;
                while(endi >= starti && (unsigned char)idBuf[endi] <= ' ')
                    --endi;
                int trimmed_len = (starti <= endi) ? (endi - starti + 1) : 0;
                bool allDigits = (trimmed_len > 0);
                for(int k = 0; k < trimmed_len; ++k) {
                    char ch = idBuf[starti + k];
                    if(!std::isdigit(static_cast<unsigned char>(ch))) {
                        allDigits = false;
                        break;
                    }
                }
                if(!allDigits) {
                    s_idFlashTicks[row] = 6;
                } else {
                    unsigned long long val = 0;
                    try {
                        val = std::stoull(idBuf);
                    } catch(...) { val = 0; }
                    if(val > (unsigned long long)RESERVED_MAX_NORMAL_ID) val = RESERVED_MAX_NORMAL_ID;
                    uint32_t newId = static_cast<uint32_t>(val);
                    idEdits.emplace_back(row, entries[row].mTextId, newId);
                    m_dirty = true;
                    needResortAfterLoop = true;
                    idToSelectAfterResort = newId;
                    haveIdToSelect = true;
                }
            }
        }

        if(shouldFlash) ImGui::PopStyleColor();
        if(ImGui::IsItemActive()) m_selectedRow = row;
        ImGui::TableNextColumn();
        std::string& editRef = entries[row].text;
        std::string txtLabel = std::string("##txt") + std::to_string(row);

        if(isProtected) {
            ImGui::BeginDisabled(true);
            std::vector<char> robuf;
            robuf.resize(editRef.size() + 1);
            if(!editRef.empty()) std::memcpy(robuf.data(), editRef.c_str(), editRef.size() + 1);
            else
                robuf[0] = '\0';
            ImGui::InputText(txtLabel.c_str(), robuf.data(), robuf.size(), ImGuiInputTextFlags_ReadOnly);
            ImGui::EndDisabled();
        } else {
            ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
            bool committed = ImGuiInputText_EditStdString(txtLabel.c_str(), editRef, flags);
            if(committed || ImGui::IsItemDeactivatedAfterEdit()) {
                textEdits.emplace_back(row, editRef);
                m_dirty = true;
            }
        }

        ImGui::SameLine();
        std::string delLabel = "Del##del" + std::to_string(row);
        if(isProtected) {
            ImGui::BeginDisabled(true);
            ImGui::SmallButton(delLabel.c_str());
            ImGui::EndDisabled();
        } else {
            if(ImGui::SmallButton(delLabel.c_str())) rowsToDelete.push_back(row);
        }
    }

    ImGui::EndTable();

    // Apply text edits
    for(auto& te: textEdits) {
        int idx = te.first;
        if(idx >= 0 && idx < (int)m_model.Entries().size()) {
            if(!(m_model.Entries()[idx].mTextId == RESERVED_EOF_ID && m_model.Entries()[idx].text == EOF_TEXT_LITERAL)) m_model.Entries()[idx].text = te.second;
        }
    }

    // Apply ID edits with collision check
    for(auto& ie: idEdits) {
        int idx;
        uint32_t origId, newId;
        std::tie(idx, origId, newId) = ie;
        if(idx < 0 || idx >= (int)m_model.Entries().size()) continue;
        if(m_model.Entries()[idx].mTextId == RESERVED_EOF_ID && m_model.Entries()[idx].text == EOF_TEXT_LITERAL) continue;
        if(newId > RESERVED_MAX_NORMAL_ID) {
            s_idFlashTicks[idx] = 6;
            continue;
        }
        bool conflict = false;
        for(int j = 0; j < (int)m_model.Entries().size(); ++j) {
            if(j == idx) continue;
            if(m_model.Entries()[j].mTextId == RESERVED_EOF_ID) continue;
            if(m_model.Entries()[j].mTextId == newId) {
                conflict = true;
                break;
            }
        }
        if(!conflict) m_model.Entries()[idx].mTextId = newId;
        else {
            m_model.Entries()[idx].mTextId = origId;
            s_idFlashTicks[idx] = 6;
        }
    }

    // Apply deletions in reverse order
    for(int i = (int)rowsToDelete.size() - 1; i >= 0; --i) {
        int idx = rowsToDelete[i];
        if(idx >= 0 && idx < (int)m_model.Entries().size()) {
            if(m_model.Entries()[idx].mTextId == RESERVED_EOF_ID && m_model.Entries()[idx].text == EOF_TEXT_LITERAL) continue;
            m_model.Remove((size_t)idx);
            m_dirty = true;
            if(m_selectedRow == idx) m_selectedRow = -1;
            else if(m_selectedRow > idx)
                --m_selectedRow;
        }
    }

    if(needResortAfterLoop || !rowsToDelete.empty()) {
        auto& entries2 = m_model.Entries();
        ResortAndEnsureEOF(entries2, m_dirty, m_selectedRow);
        if(haveIdToSelect) {
            m_selectedRow = -1;
            for(int i = 0; i < (int)entries2.size(); ++i) {
                if(entries2[i].mTextId == idToSelectAfterResort && !(entries2[i].mTextId == RESERVED_EOF_ID && entries2[i].text == EOF_TEXT_LITERAL)) {
                    m_selectedRow = i;
                    break;
                }
            }
        }
    }

    // Update flash ticks
    if(!s_idFlashTicks.empty()) {
        std::vector<int> toErase;
        toErase.reserve(s_idFlashTicks.size());
        for(auto& p: s_idFlashTicks) {
            if(p.second > 0) p.second -= 1;
            if(p.second <= 0) toErase.push_back(p.first);
        }
        for(int k: toErase)
            s_idFlashTicks.erase(k);
    }
}

void te::TextEditor::Render(bool* pOpen) {
    if(!ImGui::Begin("DEF Editor###DefEditor", pOpen)) {
        ImGui::End();
        return;
    }
    DrawToolbar();
    ImGui::Separator();
    DrawTable();

    if(m_showSaveWarning) {
        ImGui::OpenPopup("Save failed");
        m_showSaveWarning = false;
    }

    if(ImGui::BeginPopupModal("Save failed", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("The following Text IDs contain characters that are invalid. The file will not be saved until the characters are removed.");
        ImGui::Separator();
        if(!m_lastBadIds.empty()) {
            ImGui::BeginChild("##badlist", ImVec2(600, 200), true);
            for(size_t i = 0; i < m_lastBadIds.size(); ++i) {
                uint32_t id = m_lastBadIds[i];
                const std::string& txt = (i < m_lastBadTexts.size()) ? m_lastBadTexts[i] : std::string();
                ImGui::Text("ID: %u", id);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", txt.c_str());
                ImGui::Separator();
            }
            ImGui::EndChild();
        } else {
            ImGui::Text("No invalid entries found (this should not happen). Please contact mEdit developer.");
        }
        if(ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::End();
}
