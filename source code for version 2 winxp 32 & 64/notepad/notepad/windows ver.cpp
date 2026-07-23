#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
//#include <VersionHelpers.h>  //windows xp
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdio>  
#include <cstdlib>
#include <cstring>  
#include <cctype>  
#include <sstream> /* Required for C++98 string formatting! */

using namespace std;

#define round(x) floor((x) + 0.5)
template <typename T>
string legacy_to_string(T value) {
    ostringstream os;
    os << value;
    return os.str();
}
#define to_string legacy_to_string

typedef vector<string> TextBuffer;

// --- Globals & State ---
int win_x_size = 80, win_y_size = 25;
int font_x_size = 11, font_y_size = 24;
int SCREEN_COLS = 80, SCREEN_ROWS = 25;
int MAX_ROWS = 19, MAX_COLS = 76;
int text_start_y = 3;

vector<pair<int, int>> buffer_log_map;

bool word_warp = true, v_scrollBar = true, h_scrollBar = false, is_hovering_hbar = false, use_mouse = true, respect_whole_words = true;
bool setting_read_only = false;
int h_scroll_offset = 0, max_line_length = 0, active_menu = 0;

int cursor_log_x = 0, cursor_log_y = 0, cursor_screen_x = 0, cursor_screen_y = 0;
bool has_selection = false, is_dragging = false;
int sel_start_log_x = 0, sel_start_log_y = 0;
bool is_dragging_vscroll = false, is_dragging_hscroll = false, is_dragging_browser_scroll = false;
int blink_x = 1, blink_y = 3;

// --- HYBRID PIECE TABLE & DISK STREAMING ENGINE ---
#define SOURCE_DISK 0
#define SOURCE_RAM  1

struct Piece {
    char source; long offset; long length;
    Piece(char s, long o, long l) : source(s), offset(o), length(l) {}
};

struct LineIndex {
    long global_offset;
    LineIndex(long o) : global_offset(o) {}
};
/* Core Piece Table Buffers */
vector<Piece> piece_table;
vector<char> add_buffer; /* The Append-Only RAM Bucket for new edits */

/* Navigation & Paging */
vector<LineIndex> stream_lines;
FILE* stream_file_handle = nullptr;
long current_chunk_start_log = 0;
const int CHUNK_LOG_SIZE = 6643;
char setting_disk_stream = 0; /* 0 = Vector RAM Mode, 1 = Piece Table Disk Mode */

/* Forward Declarations for Phase 3 */
void rebuild_stream_lines();
char get_stream_char(long global_offset);
long get_stream_logical_len(int log_y);
string get_streamed_segment(int line_idx, long start_col, int max_chars);
long get_global_offset(int log_x, int log_y);
void insert_into_stream(char c, long target_global);
void delete_from_stream(long target_global);

/* --- Prototype to fix Linker Errors --- */
void rebuild_stream_lines();
void execute_find_replace(int action);

/* --- CORE READ ENGINE --- */

/* Fetches a single character by calculating which Piece holds it */
char get_stream_char(long global_offset) {
    long current_offset = 0;
    for (size_t i = 0; i < piece_table.size(); i++) {
        if (global_offset >= current_offset && global_offset < current_offset + piece_table[i].length) {
            long local_offset = piece_table[i].offset + (global_offset - current_offset);
            if (piece_table[i].source == SOURCE_RAM) return add_buffer[local_offset];
            else if (piece_table[i].source == SOURCE_DISK && stream_file_handle) {
                fseek(stream_file_handle, local_offset, SEEK_SET);
                return fgetc(stream_file_handle);
            }
        }
        current_offset += piece_table[i].length;
    }
    return '\0';
}

/* Scans the entire Piece Table to map out where every Line ( \n ) begins */
void rebuild_stream_lines() {
    stream_lines.clear();
    stream_lines.push_back(LineIndex(0)); 

    long current_global = 0;
    for (size_t p = 0; p < piece_table.size(); p++) {
        if (piece_table[p].source == SOURCE_RAM) {
            for (long i = 0; i < piece_table[p].length; i++) {
                if (add_buffer[piece_table[p].offset + i] == '\n') stream_lines.push_back(LineIndex(current_global + i + 1));
            }
        } else if (piece_table[p].source == SOURCE_DISK && stream_file_handle) {
            fseek(stream_file_handle, piece_table[p].offset, SEEK_SET);
            const int BUF_SIZE = 4096; char buf[BUF_SIZE];
            long bytes_left = piece_table[p].length, piece_local = 0;

            while (bytes_left > 0) {
                long to_read = min((long)BUF_SIZE, bytes_left);
                long bytes_read = fread(buf, 1, to_read, stream_file_handle);
                if (bytes_read == 0) break;
                for (long i = 0; i < bytes_read; i++) {
                    if (buf[i] == '\n') stream_lines.push_back(LineIndex(current_global + piece_local + i + 1));
                }
                bytes_left -= bytes_read; piece_local += bytes_read;
            }
        }
        current_global += piece_table[p].length;
    }
}

/* Calculates the exact length of a specific line (stripping the hidden \r and \n bytes) */
long get_stream_logical_len(int log_y) {
    if (log_y < 0 || log_y >= (int)stream_lines.size()) return 0;
    long start_off = stream_lines[log_y].global_offset, end_off;
    
    if (log_y == (int)stream_lines.size() - 1) {
        long total_len = 0; 
        for (size_t i = 0; i < piece_table.size(); i++) total_len += piece_table[i].length;
        end_off = total_len;
    } else end_off = stream_lines[log_y + 1].global_offset;

    long len = end_off - start_off;
    if (len > 0) {
        if (get_stream_char(end_off - 1) == '\n') len--;
        if (len > 0 && get_stream_char(end_off - 2) == '\r') len--;
    }
    return len;
}

/* Extracts a stitched string to send to the UI Canvas */
string get_streamed_segment(int line_idx, long start_col, int max_chars) {
    if (line_idx < 0 || line_idx >= (int)stream_lines.size()) return "";
    long line_start = stream_lines[line_idx].global_offset;
    long logical_len = get_stream_logical_len(line_idx);

    if (start_col >= logical_len) return "";
    long read_len = min((long)max_chars, logical_len - start_col);

    string res = ""; res.reserve(read_len);
    long current_global = line_start + start_col;
    for (long i = 0; i < read_len; i++) res += get_stream_char(current_global + i);
    
    return res;
}

/* --- CORE EDIT ENGINE --- */

/* Calculates the exact global byte offset based on cursor position */
long get_global_offset(int log_x, int log_y) {
    if (log_y < 0 || log_y >= (int)stream_lines.size()) return 0;
    return stream_lines[log_y].global_offset + log_x;
}

/* Splits pieces and safely inserts a new character without moving RAM */
void insert_into_stream(char c, long target_global) {
    add_buffer.push_back(c);
    long new_offset = (long)add_buffer.size() - 1;

    if (piece_table.empty()) {
        piece_table.push_back(Piece(SOURCE_RAM, new_offset, 1));
        return;
    }

    long current_global = 0;
    for (vector<Piece>::iterator it = piece_table.begin(); it != piece_table.end(); ++it) {
        if (target_global >= current_global && target_global <= current_global + it->length) {
            if (target_global == current_global + it->length) {
                if (it->source == SOURCE_RAM && it->offset + it->length == new_offset) {
                    it->length++; return;
                }
                if (it + 1 == piece_table.end()) {
                    piece_table.push_back(Piece(SOURCE_RAM, new_offset, 1)); return;
                }
                current_global += it->length; continue;
            }
            if (target_global == current_global) {
                piece_table.insert(it, Piece(SOURCE_RAM, new_offset, 1)); return;
            }
            
            long left_len = target_global - current_global;
            long right_len = it->length - left_len;
            long right_offset = it->offset + left_len;
            char src = it->source;

            it->length = left_len;
            it = piece_table.insert(it + 1, Piece(SOURCE_RAM, new_offset, 1));
            piece_table.insert(it + 1, Piece(src, right_offset, right_len));
            return;
        }
        current_global += it->length;
    }
    piece_table.push_back(Piece(SOURCE_RAM, new_offset, 1));
}

void delete_from_stream(long target_global) {
    long current_global = 0;
    for (vector<Piece>::iterator it = piece_table.begin(); it != piece_table.end(); ++it) {
        if (target_global >= current_global && target_global < current_global + it->length) {
            if (it->length == 1) { piece_table.erase(it); return; }
            if (target_global == current_global) { it->offset++; it->length--; return; }
            if (target_global == current_global + it->length - 1) { it->length--; return; }

            long left_len = target_global - current_global;
            long right_len = it->length - left_len - 1;
            long right_offset = it->offset + left_len + 1;
            char src = it->source;

            it->length = left_len;
            piece_table.insert(it + 1, Piece(src, right_offset, right_len));
            return;
        }
        current_global += it->length;
    }
}
/* --- END HYBRID ENGINE --- */

string internal_clipboard = "";

bool is_context_menu = false;
int context_x = 0, context_y = 0;

bool show_settings = false, show_open_dialog = false, show_save_dialog = false, screen_too_small = false;
bool is_document_dirty = false, show_exit_dialog = false, last_was_typing = false;
bool show_find_dialog = false; /* FIX: Find & Replace State */
bool debug_red_all = false; /* FIX: Troubleshooting variable */
int pending_action = 0;

string find_text = "", replace_text = "";
int find_cursor = 0, replace_cursor = 0;

struct EditorState { 
    vector<Piece> p_table; int add_buf_size; int cx, cy; 
    EditorState(vector<Piece> p, int a, int x, int y) : p_table(p), add_buf_size(a), cx(x), cy(y) {}
};
vector<EditorState> undo_stack, redo_stack;

string dialog_input_text = "", dialog_search_text = "", dialog_address_text = "", dialog_status_msg = "";
int active_input_field = 0, dialog_input_cursor = 0, dialog_search_cursor = 0, dialog_address_cursor = 0;

int current_settings_tab = 0, setting_hover_effects = 2, setting_ansi_mode = 1;
bool setting_color = true, setting_internal_clipboard = true;

// Theme State Variables (Bitflags: Blue=1, Green=2, Red=4, White=7, Black=0)
int setting_bg_color = 0;
int setting_fg_color = 7;
int setting_sel_color = 1;
int setting_crit_color = 4;

int scroll_y = 0, total_visual_lines = 1; /* FIX: Restored smooth scrolling variables! */
string current_filename = "Untitled.txt";

struct FileItem { 
    string name; bool is_dir; DWORD size; 
    FileItem(string n, bool d, DWORD s) : name(n), is_dir(d), size(s) {}
};
vector<FileItem> all_browser_files, browser_files;
string current_browser_path = "";
int browser_scroll_offset = 0, browser_selected_index = -1, path_history_index = -1;
vector<string> path_history;

int current_hover_element = 0;
int active_tab_element = 0; // Tracks keyboard Tab navigation in dialogs
UINT original_cp = 0;
char* argv0_global = (char*)"";
string exe_dir = ""; /* FIX: Standalone Application Directory Tracking */


// --- Forward Declarations ---
bool IsLegacyConsole();
void gotoxy(int x, int y);
void replaceAll(string& str, const string& from, const string& to);
string apply_ansi(string text);
void print_at(int x, int y, string text);
void clear_screen_safe();
void font();
void scrnsz();
WORD get_fg_flag(int idx);
WORD get_bg_flag(int idx);
WORD get_default_attr();
WORD get_highlight_attr();
WORD get_critical_attr();
WORD get_invert_attr();
WORD get_dim_attr();
void save_state();
void perform_undo();
void perform_redo();
void filter_browser_files();
void load_directory(string path, bool update_history = true);
bool is_in_selection(int y, int x);
string get_selected_text();
void delete_selection();
void set_clipboard(const string& text);
string get_clipboard();
void insert_text(const string& text);
void save_config();
void load_config();
void init_app(char* argv0, bool clear_screen = true);
void draw_exit_dialog();
void draw_file_dialog(bool is_save);
void draw_file_menu();
void draw_edit_menu(int sx, int sy, bool ctxt);
void draw_view_menu();
void draw_settings();
void draw_vscrollbar(int current, int total);
void draw_bottom_bar();
void draw_title_at(int x, int w_limit = 0);
void render_hover_element(int id, bool is_hovered);
void displayTextBuffer(const TextBuffer& buffer, int viewport);
void close_active_menu(const TextBuffer& buffer);
TextBuffer wrap_text();
void update_cursor_from_screen(int mx, int my, const TextBuffer& buffer);
TextBuffer load_document(const string& filePath, const string& name);


// --- Console Helper Functions ---
bool IsLegacyConsole() { return IsWindowVisible(GetConsoleWindow()) != 0; }
void gotoxy(int x, int y) { 
    COORD c; 
    c.X = (SHORT)x; 
    c.Y = (SHORT)y; 
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), c); 
}

void replaceAll(string& str, const string& from, const string& to) {
    if (from.empty()) return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

string apply_ansi(string text) {
    if (setting_ansi_mode == 1) return text;
    const char* reps[][2] = { {"┌","+"},{"┐","+"},{"└","+"},{"┘","+"},{"│","|"},{"─","-"},{"☼","*"},{"▲","^"},{"▼","v"},{"◄","<"},{"►",">"},{"├","|"},{"┤","|"},{"┴","-"},{"┬","-"},{"█","|"},{"•","."},{"√","V"},{"←","<"},{"┼","+"} };
    for (int i = 0; i < 20; i++) replaceAll(text, reps[i][0], reps[i][1]);
    return text;
}

/* FIX: Native Win32 Kernel Printer. Bypasses C++ iostream buffering entirely for zero-latency drawing! */
void fast_print(const string& s) {
    DWORD written;
    WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), s.c_str(), (DWORD)s.length(), &written, NULL);
}

void print_at(int x, int y, string text) {
    gotoxy(x, y); 
    fast_print(apply_ansi(text)); 
}

// --- Dynamic Theme Engine ---
WORD get_fg_flag(int idx) {
    if (idx == 0) return 0;
    WORD flag = 0;
    if (idx & 1) flag |= FOREGROUND_BLUE;
    if (idx & 2) flag |= FOREGROUND_GREEN;
    if (idx & 4) flag |= FOREGROUND_RED;
    if (idx != 7) flag |= FOREGROUND_INTENSITY; // Brighten colors slightly, keep white standard
    return flag;
}
WORD get_bg_flag(int idx) {
    if (idx == 0) return 0;
    WORD flag = 0;
    if (idx & 1) flag |= BACKGROUND_BLUE;
    if (idx & 2) flag |= BACKGROUND_GREEN;
    if (idx & 4) flag |= BACKGROUND_RED;
    return flag;
}
WORD get_default_attr() { return get_fg_flag(setting_fg_color) | get_bg_flag(setting_bg_color); }

WORD get_highlight_attr() {
    if (!setting_color) return get_invert_attr(); // Uses the smart monochrome invert
    /* FIX: Force FOREGROUND_INTENSITY so the white text pops out as bright white just like DOS! */
    return get_bg_flag(setting_sel_color) | get_fg_flag(setting_sel_color == 7 ? 0 : 7) | (setting_sel_color == 7 ? 0 : FOREGROUND_INTENSITY);
}
WORD get_critical_attr() {
    if (!setting_color) return get_invert_attr(); // Uses the smart monochrome invert
    /* FIX: Force FOREGROUND_INTENSITY so the white text pops out as bright white just like DOS! */
    return get_bg_flag(setting_crit_color) | get_fg_flag(setting_crit_color == 7 ? 0 : 7) | (setting_crit_color == 7 ? 0 : FOREGROUND_INTENSITY);
}
WORD get_invert_attr() {
    // If the background is White (7), return White Text on a Black Background (0)
    if (setting_bg_color == 7) return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

    // Otherwise (Black background), return Black Text on a White Background
    return BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
}
WORD get_dim_attr() { return FOREGROUND_INTENSITY | get_bg_flag(setting_bg_color); } // Dark grey text on standard background

void clear_screen_safe() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, get_default_attr());
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi)) return;
    DWORD count, cellCount = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
    
    COORD origin; 
    origin.X = 0; 
    origin.Y = 0;
    
    FillConsoleOutputCharacter(hConsole, ' ', cellCount, origin, &count);
    FillConsoleOutputAttribute(hConsole, csbi.wAttributes, cellCount, origin, &count);
    SetConsoleCursorPosition(hConsole, origin);
}

void font() {
    /* Windows XP manages console fonts via the user's shortcut properties.
       No programmatic override is supported or required here! */
}

void scrnsz() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    DWORD outMode = 0;
    if (GetConsoleMode(hConsole, &outMode)) {
        /* XP Only: Simply disable line wrap */
        SetConsoleMode(hConsole, outMode & ~ENABLE_WRAP_AT_EOL_OUTPUT);
    }

    /* XP Only: Permanently lock the window borders to prevent scrollbar tearing! */
    HWND consoleWindow = GetConsoleWindow();
    LONG style = GetWindowLong(consoleWindow, GWL_STYLE);
    SetWindowLong(consoleWindow, GWL_STYLE, style & ~WS_MAXIMIZEBOX & ~WS_SIZEBOX);

    /* Set exact 1:1 Buffer and Window ratio */
    COORD newSize = { (SHORT)win_x_size, (SHORT)win_y_size };
    SMALL_RECT newRect = { 0, 0, (SHORT)(win_x_size - 1), (SHORT)(win_y_size - 1) };

    SMALL_RECT minWindow = { 0, 0, 1, 1 };
    SetConsoleWindowInfo(hConsole, TRUE, &minWindow);
    SetConsoleScreenBufferSize(hConsole, newSize);
    SetConsoleWindowInfo(hConsole, TRUE, &newRect);

    SetConsoleOutputCP(65001);
}

// --- Logic Engines ---
void save_state() {
    if (undo_stack.size() >= 100) undo_stack.erase(undo_stack.begin());
    undo_stack.push_back(EditorState(piece_table, (int)add_buffer.size(), cursor_log_x, cursor_log_y));
    redo_stack.clear();
}

void perform_undo() {
    if (undo_stack.empty()) return;
    redo_stack.push_back(EditorState(piece_table, (int)add_buffer.size(), cursor_log_x, cursor_log_y));
    auto state = undo_stack.back(); undo_stack.pop_back();
    piece_table = state.p_table; add_buffer.resize(state.add_buf_size); 
    cursor_log_x = state.cx; cursor_log_y = state.cy;
    rebuild_stream_lines(); is_document_dirty = true; last_was_typing = false;
}

void perform_redo() {
    if (redo_stack.empty()) return;
    undo_stack.push_back(EditorState(piece_table, (int)add_buffer.size(), cursor_log_x, cursor_log_y));
    auto state = redo_stack.back(); redo_stack.pop_back();
    piece_table = state.p_table; add_buffer.resize(state.add_buf_size); 
    cursor_log_x = state.cx; cursor_log_y = state.cy;
    rebuild_stream_lines(); is_document_dirty = true; last_was_typing = false;
}

void filter_browser_files() {
    browser_files.clear();
    string query = dialog_search_text;
    transform(query.begin(), query.end(), query.begin(), ::tolower);
    for (size_t i = 0; i < all_browser_files.size(); i++) {
        string lower_name = all_browser_files[i].name;
		transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        if (query.empty() || lower_name.find(query) != string::npos) browser_files.push_back(all_browser_files[i]);
    }
    browser_scroll_offset = 0; browser_selected_index = -1;
}

void load_directory(string path, bool update_history) {
    all_browser_files.clear(); browser_scroll_offset = 0; browser_selected_index = -1;
    dialog_search_text = ""; dialog_search_cursor = 0;
    
    /* FIX: Universally resolve absolute paths to clean up '..' from mouse clicks or typing! */
    char full_path[MAX_PATH];
    if (GetFullPathNameA(path.c_str(), MAX_PATH, full_path, nullptr)) {
        path = full_path;
    }
    
    if (path.length() > 0 && path.back() != '\\') path += "\\";
    current_browser_path = path; dialog_address_text = current_browser_path;
    dialog_address_cursor = (int)dialog_address_text.length();
    if (active_input_field == 3) active_input_field = 0;
    if (update_history) {
        if (path_history_index < (int)path_history.size() - 1) path_history.erase(path_history.begin() + path_history_index + 1, path_history.end());
        if (path_history.empty() || path_history.back() != path) { path_history.push_back(path); path_history_index++; }
    }
    WIN32_FIND_DATAA findFileData; HANDLE hFind = FindFirstFileA((path + "*").c_str(), &findFileData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            string fname = findFileData.cFileName; if (fname == ".") continue;
            bool isDir = (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDir) all_browser_files.push_back(FileItem(fname, true, 0));
            else if (fname.length() >= 4) {
                string ext = fname.substr(fname.length() - 4); for (size_t i = 0; i < ext.length(); i++) ext[i] = (char)tolower(ext[i]);
                if (ext == ".txt") all_browser_files.push_back(FileItem(fname, false, findFileData.nFileSizeLow));
            }
        } while (FindNextFileA(hFind, &findFileData) != 0);
        FindClose(hFind);
    }
	sort(all_browser_files.begin(), all_browser_files.end(), [](const FileItem& a, const FileItem& b) -> bool {
        if (a.name == "." || a.name == "..") {
            if (a.name == "." && b.name != ".") return true;
            if (a.name == ".." && b.name != "." && b.name != "..") return true;
            return false;
        }
        if (b.name == "." || b.name == "..") return false;
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        
        /* FIX: Case-insensitive sorting makes Windows truly alphabetical! */
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    filter_browser_files();
}

// --- Selection & Clipboard Logic ---
bool is_in_selection(int y, int x) {
    if (!has_selection) return false;
    int sy = sel_start_log_y, sx = sel_start_log_x, ey = cursor_log_y, ex = cursor_log_x;
    if (sy > ey || (sy == ey && sx > ex)) { swap(sy, ey); swap(sx, ex); }
    if (y < sy || y > ey) return false;
    if (sy == ey) return (x >= sx && x < ex);
    return (y == sy) ? x >= sx : (y == ey) ? x < ex : true;
}

string get_selected_text() {
    if (!has_selection) return "";
    int sy = sel_start_log_y, sx = sel_start_log_x, ey = cursor_log_y, ex = cursor_log_x;
    if (sy > ey || (sy == ey && sx > ex)) { swap(sy, ey); swap(sx, ex); }
    long start_global = get_global_offset(sx, sy);
    long end_global = get_global_offset(ex, ey);
    long len = end_global - start_global;
    string res = ""; res.reserve(len);
    for(long i = 0; i < len; i++) res += get_stream_char(start_global + i);
    return res;
}

void delete_selection() {
    if (!has_selection) return;
    int sy = sel_start_log_y, sx = sel_start_log_x, ey = cursor_log_y, ex = cursor_log_x;
    if (sy > ey || (sy == ey && sx > ex)) { swap(sy, ey); swap(sx, ex); }
    long start_global = get_global_offset(sx, sy);
    long end_global = get_global_offset(ex, ey);
    for (long i = end_global - 1; i >= start_global; i--) delete_from_stream(i);
    rebuild_stream_lines();
    cursor_log_x = sx; cursor_log_y = sy; has_selection = is_dragging = false; is_document_dirty = true;
}

void set_clipboard(const string& text) {
    if (setting_internal_clipboard) internal_clipboard = text;
    else if (OpenClipboard(nullptr)) {
        EmptyClipboard(); HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.length() + 1);
        if (hMem) {
            void* pLock = GlobalLock(hMem);
            if (pLock) {
                memcpy(pLock, text.c_str(), text.length() + 1);
                GlobalUnlock(hMem);
                SetClipboardData(CF_TEXT, hMem);
            }
        }
        CloseClipboard();
    }
}

string get_clipboard() {
    if (setting_internal_clipboard) return internal_clipboard;
    string text = "";
    if (OpenClipboard(nullptr)) {
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (hData) {
            char* pszText = static_cast<char*>(GlobalLock(hData));
            if (pszText) text = pszText;
            GlobalUnlock(hData);
        }
        CloseClipboard();
    }
    return text;
}

void insert_text(const string& text) {
    if (has_selection) delete_selection();
    long current_global = get_global_offset(cursor_log_x, cursor_log_y);
    for (size_t i = 0; i < text.length(); i++) { 
		char c = text[i];
        if (c == '\r') continue;
        insert_into_stream(c, current_global);
        current_global++;
        if (c == '\n') { cursor_log_y++; cursor_log_x = 0; }
        else cursor_log_x++;
    }
    rebuild_stream_lines();
    is_document_dirty = true;
}

/* FIX: High-Performance Stream Searching Engine for the Piece Table! */
void execute_find_replace(int action) {
    int f_len = (int)find_text.length();
    int r_len = (int)replace_text.length();
    int m_lines = (int)stream_lines.size();
    
    if (f_len == 0) return;

    if (action == 1) { /* Find / Next */
        int sy = cursor_log_y, sx = cursor_log_x;
        
        if (has_selection && sel_start_log_y == cursor_log_y) {
            int min_x = min(sel_start_log_x, cursor_log_x);
            string cur_line = get_streamed_segment(cursor_log_y, 0, get_stream_logical_len(cursor_log_y));
            if (min_x < (int)cur_line.length() && cur_line.substr(min_x, f_len) == find_text) sx = min_x + 1; 
        }

        for (int i = sy; i < m_lines; i++) {
            string line = get_streamed_segment(i, 0, get_stream_logical_len(i));
            if (line.empty() && i != sy) continue;
            size_t pos = (i == sy && sx < (int)line.length()) ? line.find(find_text, sx) : line.find(find_text);
            if (pos != string::npos) {
                cursor_log_y = i; cursor_log_x = (int)pos + f_len; sel_start_log_y = i; sel_start_log_x = (int)pos;
                has_selection = true; return;
            }
        }
        
        for (int i = 0; i <= sy && i < m_lines; i++) {
            string line = get_streamed_segment(i, 0, get_stream_logical_len(i));
            if (line.empty()) continue;
            size_t pos = line.find(find_text);
            if (pos != string::npos && (i < sy || (i == sy && (int)pos < sx))) {
                cursor_log_y = i; cursor_log_x = (int)pos + f_len; sel_start_log_y = i; sel_start_log_x = (int)pos;
                has_selection = true; return;
            }
        }
        has_selection = false; 
    }
    else if (action == 4) { /* Prev (Search Backwards) */
        int sy = cursor_log_y, sx = cursor_log_x;

        if (has_selection && sel_start_log_y == cursor_log_y) {
            int min_x = min(sel_start_log_x, cursor_log_x);
            string cur_line = get_streamed_segment(cursor_log_y, 0, get_stream_logical_len(cursor_log_y));
            if (min_x < (int)cur_line.length() && cur_line.substr(min_x, f_len) == find_text) sx = min_x - 1; 
            else sx--;
        } else sx--; 

        for (int i = sy; i >= 0; i--) {
            string line = get_streamed_segment(i, 0, get_stream_logical_len(i));
            if (line.empty() && i != sy) continue;
            size_t last_match = string::npos, current_match = line.find(find_text);
            while (current_match != string::npos) {
                if (i < sy || (i == sy && (int)current_match <= sx)) last_match = current_match;
                current_match = line.find(find_text, current_match + 1);
            }
            if (last_match != string::npos) {
                cursor_log_y = i; cursor_log_x = (int)last_match + f_len; sel_start_log_y = i; sel_start_log_x = (int)last_match;
                has_selection = true; return;
            }
            if (i > 0) sx = (int)get_stream_logical_len(i - 1); 
        }
        
        for (int i = m_lines - 1; i >= sy; i--) {
            string line = get_streamed_segment(i, 0, get_stream_logical_len(i));
            if (line.empty()) continue;
            size_t last_match = string::npos, current_match = line.find(find_text);
            while (current_match != string::npos) {
                if (i > sy || (i == sy && (int)current_match >= cursor_log_x)) last_match = current_match;
                current_match = line.find(find_text, current_match + 1);
            }
            if (last_match != string::npos) {
                cursor_log_y = i; cursor_log_x = (int)last_match + f_len; sel_start_log_y = i; sel_start_log_x = (int)last_match;
                has_selection = true; return;
            }
        }
        has_selection = false; 
    }
    else if (action == 2) { /* Replace */
        if (setting_read_only) return; 
        if (has_selection && sel_start_log_y == cursor_log_y) {
            int min_x = min(sel_start_log_x, cursor_log_x);
            string cur_line = get_streamed_segment(cursor_log_y, 0, get_stream_logical_len(cursor_log_y));
            if (min_x < (int)cur_line.length() && cur_line.substr(min_x, f_len) == find_text) {
                save_state(); last_was_typing = false;
                delete_selection();
                insert_text(replace_text);
                rebuild_stream_lines(); /* FIX: Critical safety refresh for Piece Table! */
                execute_find_replace(1); 
                return;
            }
        }
        execute_find_replace(1); 
    }
    else if (action == 3) { /* Replace All */
        debug_red_all = false; /* FIX: Turn off the troubleshooting color! */
        if (setting_read_only) return; 
        save_state(); last_was_typing = false;
        
        /* FIX: User's Macro Loop Design! Rapdily simulate 'Find Next' and 'Replace' sequentially! */
        int count = 0;
        
        /* 1. Reset to the top of the file to start a clean pass */
        cursor_log_y = 0; cursor_log_x = 0;
        has_selection = false;
        
        /* 2. Target the very first occurrence */
        execute_find_replace(1); 
        
        /* 3. Loop as long as matches exist */
        while (has_selection) {
            int prev_y = cursor_log_y, prev_x = cursor_log_x;
            
            delete_selection();
            insert_text(replace_text);
            count++;
            
            rebuild_stream_lines(); /* Required to keep memory synced between loops! */
            execute_find_replace(1); 
            
            /* Guard: Break the loop if the engine wraps around to the top of the file */
            if (cursor_log_y < prev_y || (cursor_log_y == prev_y && cursor_log_x <= prev_x)) {
                break; 
            }
        }
        
        has_selection = false;
        if (count > 0) is_document_dirty = true; 
    }
}

void save_document(const string& filePath) {
    /* FIX: Implement Atomic Save Protocol with Hidden Temporary File */
    string temp_path = filePath + ".tmp";
    FILE* f = fopen(temp_path.c_str(), "wb");
    if (f) {
        /* Hide the file immediately to protect it from the user! */
        SetFileAttributesA(temp_path.c_str(), FILE_ATTRIBUTE_HIDDEN);
        
        for (size_t p = 0; p < piece_table.size(); p++) { 
			const Piece& piece = piece_table[p];
            if (piece.source == SOURCE_RAM) fwrite(&add_buffer[piece.offset], 1, piece.length, f);
            else if (piece.source == SOURCE_DISK && stream_file_handle) {
                fseek(stream_file_handle, piece.offset, SEEK_SET);
                const int BUF_SIZE = 4096; char buf[BUF_SIZE]; long bytes_left = piece.length;
                while (bytes_left > 0) {
                    long to_read = min((long)BUF_SIZE, bytes_left);
                    long bytes_read = fread(buf, 1, to_read, stream_file_handle);
                    if (bytes_read == 0) break;
                    fwrite(buf, 1, bytes_read, f); bytes_left -= bytes_read;
                }
            }
        }
        fclose(f);
        
        /* FIX: Release the streaming lock before we manipulate the original file! */
        if (stream_file_handle) {
            fclose(stream_file_handle);
            stream_file_handle = nullptr;
        }

        /* FIX: Atomic Hot-Swap! Destroy the old, rename the new! */
        DeleteFileA(filePath.c_str());
        MoveFileA(temp_path.c_str(), filePath.c_str());
        
        /* FIX: Restore the stream lock now that the file is safely rebuilt! */
        if (setting_disk_stream == 1) {
            stream_file_handle = fopen(filePath.c_str(), "rb");
        }

        size_t pos = filePath.find_last_of("\\/");
        current_filename = (pos != string::npos) ? filePath.substr(pos + 1) : filePath;
        is_document_dirty = false;
    }
}

void save_config() {
    ofstream f(exe_dir + "config.txt");
    if (!f.is_open()) return;
    f << "word_warp=" << word_warp << "\n";
    f << "v_scrollBar=" << v_scrollBar << "\n";
    f << "h_scrollBar=" << h_scrollBar << "\n";
    f << "respect_whole_words=" << respect_whole_words << "\n";
    f << "setting_hover_effects=" << setting_hover_effects << "\n";
    f << "setting_color=" << setting_color << "\n";
    f << "setting_ansi_mode=" << setting_ansi_mode << "\n";
    f << "setting_internal_clipboard=" << setting_internal_clipboard << "\n";
    f << "setting_bg_color=" << setting_bg_color << "\n";
    f << "setting_fg_color=" << setting_fg_color << "\n";
    f << "setting_sel_color=" << setting_sel_color << "\n";
    f << "setting_crit_color=" << setting_crit_color << "\n";
    f << "setting_disk_stream=" << (int)setting_disk_stream << "\n";
    f.close();
}

void load_config() {
    ifstream f(exe_dir + "config.txt");
    if (!f.is_open()) return; // Fallback to hardcoded defaults
    string line;
    while (getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        string k = line.substr(0, eq);
        string v = line.substr(eq + 1);
        try {
            int val = stoi(v);
            if (k == "word_warp") word_warp = val;
            else if (k == "v_scrollBar") v_scrollBar = val;
            else if (k == "h_scrollBar") h_scrollBar = val;
            else if (k == "respect_whole_words") respect_whole_words = val;
            else if (k == "setting_hover_effects") setting_hover_effects = val;
            else if (k == "setting_color") setting_color = val;
            else if (k == "setting_ansi_mode") setting_ansi_mode = val;
            else if (k == "setting_internal_clipboard") setting_internal_clipboard = val;
            else if (k == "setting_bg_color") setting_bg_color = val;
            else if (k == "setting_fg_color") setting_fg_color = val;
            else if (k == "setting_sel_color") setting_sel_color = val;
            else if (k == "setting_crit_color") setting_crit_color = val;
            else if (k == "setting_disk_stream") setting_disk_stream = val;
        }
        catch (...) {} // Ignore corrupted configuration lines
    }
    f.close();
    MAX_COLS = v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2; // Re-sync boundary dependent on scrollbar state
}

// --- UI Components ---
void draw_exit_dialog() {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), get_default_attr());
    
    const char* arr80[] = {
        "┌──────────────────────────────────────────┐",
        "│             Unsaved Changes              │",
        "├──────────────────────────────────────────┤",
        "│ Do you want to save changes to           │", 
        "│                                          │", 
        "│                                          │",
        "│  ┌──────┐    ┌────────────┐    ┌──────┐  │", 
        "│  │ Save │    │ Don't Save │    │Cancel│  │", 
        "│  └──────┘    └────────────┘    └──────┘  │", 
        "└──────────────────────────────────────────┘" 
    };
    vector<string> d80(arr80, arr80 + 10);
    
    const char* arr40[] = {
        "┌────────────────────────────────────┐",
        "│          Unsaved Changes           │",
        "├────────────────────────────────────┤",
        "│ Do you want to save changes to     │", 
        "│                                    │", 
        "│                                    │",
        "│ ┌──────┐  ┌────────────┐  ┌──────┐ │", 
        "│ │ Save │  │ Don't Save │  │Cancel│ │", 
        "│ └──────┘  └────────────┘  └──────┘ │", 
        "└────────────────────────────────────┘" 
    };
    vector<string> d40(arr40, arr40 + 10);

    bool narrow = SCREEN_COLS < 80;
    int bx = narrow ? max(0, (SCREEN_COLS - 38) / 2) : max(0, (SCREEN_COLS - 44) / 2);
    int by = max(0, (SCREEN_ROWS - 10) / 2);

    if (narrow) {
        for (size_t i = 0; i < d40.size(); i++) print_at(bx, by + (int)i, d40[i]);
        string f = current_filename; 
        print_at(bx + 2, by + 4, f.length() > 34 ? f.substr(0, 31) + "..." : f + string(34 - f.length(), ' '));
    }
    else {
        for (size_t i = 0; i < d80.size(); i++) print_at(bx, by + (int)i, d80[i]);
        string f = current_filename; 
        print_at(bx + 2, by + 4, f.length() > 38 ? f.substr(0, 35) + "..." : f + string(38 - f.length(), ' '));
    }
}

/* Helper function to safely repeat multi-byte UTF-8 strings */
string repeat_str(string str, int times) { string result = ""; for (int i = 0; i < times; i++) result += str; return result; }

void init_app(char* argv0, bool clear_screen) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WORD def = get_default_attr();
    WORD hl = get_highlight_attr();
    WORD red = get_critical_attr();

    if (clear_screen) clear_screen_safe();
    SetConsoleTextAttribute(h, def);
    
    /* FIX: Prevent the Main UI from drawing in the background if a FULL-SCREEN dialog is open! */
    if (show_settings || show_open_dialog || show_save_dialog) return;
    
    /* FIX: IMGUI Architecture. Draw hover states immediately using fast_print kernel bypass! */
    string r2_left = "├──────┴──────┴──────┴"; int r2_len = 22;
    if (active_menu == 1) { r2_left = "├──────┴──────┴┬─────┴"; }
    else if (active_menu == 2 && !is_context_menu) { r2_left = "├─────┬┴──────┴──────┼"; }
    else if (active_menu == 3) { r2_left = "├──────┴─────┬┴──────┴─────────────┬"; r2_len = 36; }
    
    if (clear_screen) {
        gotoxy(0, 0); fast_print(apply_ansi("┌──────┬──────┬──────┬" + repeat_str("─", SCREEN_COLS - 27) + "┬───┐"));
        gotoxy(0, 1); fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (current_hover_element == 1 || active_menu == 1) ? hl : def); fast_print(" File "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (current_hover_element == 2 || active_menu == 2) ? hl : def); fast_print(" Edit "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (current_hover_element == 3 || active_menu == 3) ? hl : def); fast_print(" View "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        fast_print(string(SCREEN_COLS - 27, ' ') + apply_ansi("│"));
        SetConsoleTextAttribute(h, current_hover_element == 4 ? red : def); fast_print(" X "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        gotoxy(0, 2); fast_print(apply_ansi(r2_left + repeat_str("─", SCREEN_COLS - r2_len - 5) + (v_scrollBar ? "┴─┬─┤" : "┴───┤")));
    } else {
        gotoxy(0, 0); fast_print(apply_ansi("┌──────┬──────┬──────┬" + repeat_str("─", SCREEN_COLS - 27))); 
        gotoxy(SCREEN_COLS - 5, 0); fast_print(apply_ansi("┬───┐"));
        
        gotoxy(0, 1); fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (current_hover_element == 1 || active_menu == 1) ? hl : def); fast_print(" File "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (current_hover_element == 2 || active_menu == 2) ? hl : def); fast_print(" Edit "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (current_hover_element == 3 || active_menu == 3) ? hl : def); fast_print(" View "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        
        gotoxy(SCREEN_COLS - 5, 1); fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, current_hover_element == 4 ? red : def); fast_print(" X "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        
        gotoxy(0, 2); fast_print(apply_ansi(r2_left + repeat_str("─", SCREEN_COLS - r2_len - 5))); 
        gotoxy(SCREEN_COLS - 5, 2); fast_print(apply_ansi(v_scrollBar ? "┴─┬─┤" : "┴───┤"));
    }
    
    for (int y = 3; y <= SCREEN_ROWS - 4; y++) {
        /* FIX: Context-aware borders to completely stop T-Junction flickering! */
        string left_char = "│";
        if (active_menu == 1 && (y == 7 || y == 9 || y == 11 || y == 13)) left_char = "├";
        else if (show_find_dialog && y == 9) left_char = "├";
        
        gotoxy(0, y); fast_print(apply_ansi(left_char));
        
        string right_char = (show_find_dialog && y == 9) ? "┤" : "│";
        
        if (clear_screen) {
            fast_print(string(v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2, ' '));
            if (y == 3) {
                fast_print(apply_ansi("│"));
                if (v_scrollBar) { SetConsoleTextAttribute(h, current_hover_element == 61 ? hl : def); fast_print(apply_ansi("▲")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│")); }
            }
            else if (y == 4) fast_print(apply_ansi(v_scrollBar ? "├─┤" : "│"));
            else if (y == SCREEN_ROWS - 5) fast_print(apply_ansi(v_scrollBar ? "├─┤" : "│"));
            else if (y == SCREEN_ROWS - 4) {
                fast_print(apply_ansi("│"));
                if (v_scrollBar) { SetConsoleTextAttribute(h, current_hover_element == 62 ? hl : def); fast_print(apply_ansi("▼")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│")); }
            }
            else fast_print(apply_ansi(v_scrollBar ? right_char + " │" : right_char));
        } else {
            gotoxy(v_scrollBar ? SCREEN_COLS - 3 : SCREEN_COLS - 1, y);
            if (y == 3) {
                if (v_scrollBar) { fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 61 ? hl : def); fast_print(apply_ansi("▲")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│")); }
                else fast_print(apply_ansi("│"));
            }
            else if (y == 4) fast_print(apply_ansi(v_scrollBar ? "├─┤" : "│"));
            else if (y == SCREEN_ROWS - 5) fast_print(apply_ansi(v_scrollBar ? "├─┤" : "│"));
            else if (y == SCREEN_ROWS - 4) {
                if (v_scrollBar) { fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 62 ? hl : def); fast_print(apply_ansi("▼")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│")); }
                else fast_print(apply_ansi("│"));
            }
            else {
                if (v_scrollBar) {
                    fast_print(apply_ansi(right_char)); gotoxy(SCREEN_COLS - 1, y); fast_print(apply_ansi("│"));
                } else fast_print(apply_ansi(right_char));
            }
        }
    }
    
    gotoxy(0, SCREEN_ROWS - 3); fast_print(apply_ansi(h_scrollBar ? ("├─┬" + repeat_str("─", v_scrollBar ? SCREEN_COLS - 8 : SCREEN_COLS - 6) + (v_scrollBar ? "┬─┼─┤" : "┬─┬─┤")) : ("├" + repeat_str("─", v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2) + (v_scrollBar ? "┼─┤" : "┬─┤"))));
    
    if (clear_screen) {
        gotoxy(0, SCREEN_ROWS - 2); 
        if (h_scrollBar) {
            fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 63 ? hl : def); fast_print(apply_ansi("◄")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
            fast_print(string(v_scrollBar ? SCREEN_COLS - 8 : SCREEN_COLS - 6, ' '));
            fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 64 ? hl : def); fast_print(apply_ansi("►")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
            SetConsoleTextAttribute(h, current_hover_element == 60 ? hl : def); fast_print(apply_ansi("☼")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        } else {
            fast_print(apply_ansi("│"));
            fast_print(string(v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2, ' '));
            fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 60 ? hl : def); fast_print(apply_ansi("☼")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        }
    } else {
        gotoxy(0, SCREEN_ROWS - 2); 
        if (h_scrollBar) {
            fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 63 ? hl : def); fast_print(apply_ansi("◄")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
            gotoxy(v_scrollBar ? SCREEN_COLS - 5 : SCREEN_COLS - 3, SCREEN_ROWS - 2); 
            fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 64 ? hl : def); fast_print(apply_ansi("►")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
            SetConsoleTextAttribute(h, current_hover_element == 60 ? hl : def); fast_print(apply_ansi("☼")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        } else {
            fast_print(apply_ansi("│"));
            gotoxy(v_scrollBar ? SCREEN_COLS - 3 : SCREEN_COLS - 1, SCREEN_ROWS - 2);
            fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 60 ? hl : def); fast_print(apply_ansi("☼")); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
        }
    }
    
    gotoxy(0, SCREEN_ROWS - 1); fast_print(apply_ansi(h_scrollBar ? ("└─┴" + repeat_str("─", v_scrollBar ? SCREEN_COLS - 8 : SCREEN_COLS - 6) + "┴─┴─┘") : ("└" + repeat_str("─", v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2) + "┴─┘")));
}

void draw_file_dialog(bool is_save) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    
    /* FIX: Fully Dynamic Layout Generation for Both Modes! */
    if (SCREEN_COLS < 80) {
        for (int y = 0; y < SCREEN_ROWS; y++) {
            gotoxy(0, y);
            if      (y == 0) fast_print(apply_ansi("┌" + repeat_str("─", SCREEN_COLS - 6) + "┬───┐"));
            else if (y == 1) fast_print(apply_ansi("│" + repeat_str(" ", SCREEN_COLS - 6) + "│ X │"));
            else if (y == 2) fast_print(apply_ansi("├" + repeat_str("─", SCREEN_COLS - 6) + "┴───┤"));
            else if (y == 3) fast_print(apply_ansi("│ ┌─┐┌─┐ ┌─┐ ┌" + repeat_str("─", SCREEN_COLS - 26) + "┐ ┌──────┐ │"));
            else if (y == 4) fast_print(apply_ansi("│ │◄││►│ │▲│ │" + repeat_str(" ", SCREEN_COLS - 26) + "│ │Search│ │"));
            else if (y == 5) fast_print(apply_ansi("│ └─┘└─┘ └─┘ └" + repeat_str("─", SCREEN_COLS - 26) + "┘ └──────┘ │"));
            else if (y == 6) fast_print(apply_ansi("│ ┌────────┐ ┌" + repeat_str("─", SCREEN_COLS - 31) +       "────────────┬─┐ │"));
            else if (y == 7) fast_print(apply_ansi("│ │ Jmp to │ │ Title" + repeat_str(" ", SCREEN_COLS - 37) + "│ Typ │ Siz │▲│ │"));
            else if (y == 8) fast_print(apply_ansi("│ ├────────┤ ├" + repeat_str("─", SCREEN_COLS - 31) +       "────────────┼─┤ │"));
            else if (y >= 9 && y < SCREEN_ROWS - 7) {
                int d_idx = y - 9; string d_str = "        ";
                if (d_idx == 0) d_str = "   C:   "; 
                else if (d_idx == 1) d_str = "   D:   "; 
                else if (d_idx == 2) d_str = "   E:   ";
                else if (d_idx == 3) d_str = "   F:   "; 
                else if (d_idx == 4) d_str = "   G:   "; 
                else if (d_idx == 5) d_str = "   H:   ";
                string arr = (y == SCREEN_ROWS - 8) ? "▼" : " ";
                fast_print(apply_ansi("│ │" + d_str + "│ │" + repeat_str(" ", SCREEN_COLS - 19) + "│" + arr + "│ │"));
            }
            else if (y == SCREEN_ROWS - 7) fast_print(apply_ansi("│ └────────┘ └" + repeat_str("─", SCREEN_COLS - 19) + "┴─┘ │"));
            else if (y == SCREEN_ROWS - 6) fast_print(apply_ansi("│            ┌" + repeat_str("─", SCREEN_COLS - 33) + "┐ ┌────┐ ┌──────┐ │"));
            else if (y == SCREEN_ROWS - 5) fast_print(apply_ansi("│ File Name: │" + repeat_str(" ", SCREEN_COLS - 33) + "│ │ ok │ │Cancel│ │"));
            else if (y == SCREEN_ROWS - 4) fast_print(apply_ansi("│            └" + repeat_str("─", SCREEN_COLS - 33) + "┘ └────┘ └──────┘ │"));
            else if (y == SCREEN_ROWS - 3) fast_print(apply_ansi("├───────────┬" + repeat_str("─", SCREEN_COLS - 14) + "┤"));
            else if (y == SCREEN_ROWS - 2) fast_print(apply_ansi("│ Open File │" + repeat_str(" ", SCREEN_COLS - 14) + "│"));
            else if (y == SCREEN_ROWS - 1) fast_print(apply_ansi("└───────────┴" + repeat_str("─", SCREEN_COLS - 14) + "┘"));
        }
    } 
    else {
        for (int y = 0; y < SCREEN_ROWS; y++) {
            gotoxy(0, y);
            if      (y == 0) fast_print(apply_ansi("┌─────────────────────" + repeat_str("─", SCREEN_COLS - 27) +            "┬───┐"));
            else if (y == 1) fast_print(apply_ansi("│                     " + string(SCREEN_COLS - 27, ' ') +                "│ X │"));
            else if (y == 2) fast_print(apply_ansi("├─────────────────────" + repeat_str("─", SCREEN_COLS - 27) +            "┴───┤"));
            else if (y == 3) fast_print(apply_ansi("│ ┌──┐ ┌──┐ ┌─┐ ┌" + repeat_str("─", SCREEN_COLS - 37) +  "┐ ┌──────────────┐ │"));
            else if (y == 4) fast_print(apply_ansi("│ │◄─│ │─►│ │▲│ │" + string(SCREEN_COLS - 37, ' ') +      "│ │              │ │"));
            else if (y == 5) fast_print(apply_ansi("│ └──┘ └──┘ └─┘ └" + repeat_str("─", SCREEN_COLS - 37) +  "┘ └──────────────┘ │"));
            else if (y == 6) fast_print(apply_ansi("│ ┌───────────┐ ┌" + repeat_str("─", SCREEN_COLS - 38) + "┬────────┬──────┬─┐ │"));
            else if (y == 7) fast_print(apply_ansi("│ │  Jump to  │ │ Title"+string(SCREEN_COLS - 44, ' ') + "│  Type  │ Size │▲│ │"));
            else if (y == 8) fast_print(apply_ansi("│ ├───────────┤ ├" + repeat_str("─", SCREEN_COLS - 38) + "┼────────┼──────┼─┤ │"));
            else if (y >= 9 && y <= SCREEN_ROWS - 8) {
                string drive = "           ";
                if (y >= 9 && y <= 15) { drive = "    " + string(1, (char)('C' + (y - 9))) + ":     "; }
                if (y == SCREEN_ROWS - 8) fast_print(apply_ansi("│ │" + drive + "│ │" + string(SCREEN_COLS - 22, ' ') + "│▼│ │"));
                else fast_print(apply_ansi("│ │" + drive + "│ │" + string(SCREEN_COLS - 22, ' ') + "│ │ │"));
            }
            else if (y == SCREEN_ROWS - 7) fast_print(apply_ansi("│ └───────────┘ └" + repeat_str("─", SCREEN_COLS - 38) + "┴────────┴──────┴─┘ │"));
            else if (y == SCREEN_ROWS - 6) fast_print(apply_ansi("│               ┌" + repeat_str("─", SCREEN_COLS - 38) + "┐ ┌──────┐ ┌──────┐ │"));
            else if (y == SCREEN_ROWS - 5) fast_print(apply_ansi("│   File Name:  │" + string(SCREEN_COLS - 38, ' ') +     "│ │  Ok  │ │Cancel│ │"));
            else if (y == SCREEN_ROWS - 4) fast_print(apply_ansi("│               └" + repeat_str("─", SCREEN_COLS - 38) + "┘ └──────┘ └──────┘ │"));
            else if (y == SCREEN_ROWS - 3) fast_print(apply_ansi("├───────────┬" + repeat_str("─", SCREEN_COLS - 16) +       "──┤"));
            else if (y == SCREEN_ROWS - 2) fast_print(apply_ansi((is_save ?  "│ Save As   │" : "│ Open File │") + string(SCREEN_COLS - 16, ' ') + "  │"));
            else if (y == SCREEN_ROWS - 1) fast_print(apply_ansi("└───────────┴" + repeat_str("─", SCREEN_COLS - 16) + "──┘"));
        }
    }

    /* FIX: Unified Layout Positioning. The coordinate engine perfectly snaps to either mode automatically! */
    if (is_save) print_at(2, SCREEN_ROWS - 2, " Save As ");
    else print_at(2, SCREEN_ROWS - 2, "Open File");

    string d = current_filename; if (is_document_dirty) d = "*" + d;
    if (d.length() > (size_t)(SCREEN_COLS - 11)) d = d.substr(0, SCREEN_COLS - 14) + "...";
    
    /* FIX: Dynamic padding! 1 space chunk (X=2) for narrow, 2 space chunks (X=3) for wide! */
    int title_padding_x = (SCREEN_COLS < 80) ? 2 : 3;
    print_at(title_padding_x, 1, d);


    /* FIX: Ported DOS Sliding Camera to Address Bar! */
    string active_path = (active_input_field == 3) ? dialog_address_text : current_browser_path;
    string pdisp;
    int addr_len = (SCREEN_COLS < 80) ? SCREEN_COLS - 26 : SCREEN_COLS - 38;
    int addr_x = (SCREEN_COLS < 80) ? 14 : 18;
    if (active_input_field == 3 && dialog_address_cursor > addr_len - 1) {
        pdisp = active_path.substr(dialog_address_cursor - (addr_len - 1), addr_len); 
    } else { 
        if (active_path.length() > (size_t)addr_len) {
            if (active_input_field == 3) pdisp = active_path.substr(0, addr_len);
            else pdisp = active_path.substr(0, addr_len - 3) + "..."; 
        } else pdisp = active_path + string(addr_len - active_path.length(), ' '); 
    }
    print_at(addr_x, 4, pdisp);
    if (active_input_field == 3 && setting_hover_effects == 2) {
        int local_c = dialog_address_cursor - max(0, dialog_address_cursor - (addr_len - 1));
        char ch = (local_c >= 0 && local_c < (int)pdisp.length()) ? pdisp[local_c] : ' ';
        SetConsoleTextAttribute(h, BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE);
        print_at(addr_x + local_c, 4, string(1, ch)); SetConsoleTextAttribute(h, get_default_attr());
    }

    /* FIX: Standardized Sliding Camera for Search Bar! */
    string sdisp = dialog_search_text.empty() && active_input_field != 2 ? ((SCREEN_COLS < 80) ? "Search" : "Search here") : dialog_search_text;
    int search_len = (SCREEN_COLS < 80) ? 6 : 13;
    int search_x = (SCREEN_COLS < 80) ? SCREEN_COLS - 9 : SCREEN_COLS - 16;
    string final_sdisp;
    if (active_input_field == 2 && dialog_search_cursor > search_len - 1) {
        final_sdisp = dialog_search_text.substr(dialog_search_cursor - (search_len - 1), search_len);
    } else {
        if (sdisp.length() > (size_t)search_len) {
            if (active_input_field == 2) final_sdisp = sdisp.substr(0, search_len); 
            else final_sdisp = sdisp.substr(sdisp.length() - search_len);
        } else final_sdisp = sdisp + string(search_len - sdisp.length(), ' ');
    }
    print_at(search_x, 4, final_sdisp);
    if (active_input_field == 2 && setting_hover_effects == 2) {
        int local_c = dialog_search_cursor - max(0, dialog_search_cursor - (search_len - 1));
        char ch = (local_c >= 0 && local_c < (int)final_sdisp.length()) ? final_sdisp[local_c] : ' ';
        SetConsoleTextAttribute(h, BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE);
        print_at(search_x + local_c, 4, string(1, ch)); SetConsoleTextAttribute(h, get_default_attr());
    }

    int max_items = SCREEN_ROWS - 16;
    /* FIX: Reduce the track height by 1 so it stops exactly before the ▼ arrow! */
    int track_h = max_items - 1, sc_x = SCREEN_COLS - 4, lsize = (int)browser_files.size();
    int tsize = lsize > max_items ? max(1, (int)round((float)track_h * max_items / lsize)) : track_h;
    int tpos = lsize > max_items ? (int)round((float)browser_scroll_offset / (lsize - max_items) * (track_h - tsize)) : 0;
    string blk = setting_ansi_mode == 1 ? "█" : "|";

    WORD thumb_attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
    WORD def_attr = get_default_attr();
    for (int i = 0; i < track_h; i++) {
        if (lsize <= max_items || (i >= tpos && i < tpos + tsize)) { SetConsoleTextAttribute(h, thumb_attr); print_at(sc_x, 9 + i, blk); }
        else { SetConsoleTextAttribute(h, def_attr); print_at(sc_x, 9 + i, " "); }
    }
    SetConsoleTextAttribute(h, def_attr);

    /* FIX: Data loop perfectly distinguishes between Narrow (no lines) and Wide (lines) logic! */
    for (int i = 0; i < max_items; i++) {
        bool has_item = (browser_scroll_offset + i < lsize);
        int idx = browser_scroll_offset + i; 
        bool is_hl = (has_item && idx == browser_selected_index);
        
        if (SCREEN_COLS < 80) {
            if (has_item) {
                auto& item = browser_files[idx];
                string n = item.name; int name_len = SCREEN_COLS - 32;
                if (n.length() > (size_t)name_len) n = n.substr(0, name_len - 3) + "..."; else n = n + string(name_len - n.length(), ' ');
                
                string s = ""; if (item.is_dir) s = "    ";
                else {
                    long kb = item.size / 1024; if (kb == 0 && item.size > 0) kb = 1;
                    if (kb > 999) { long mb = kb / 1024; if (mb == 0) mb = 1; s = to_string(mb) + "M"; } else s = to_string(kb) + "K";
                    if (s.length() < 4) s = string(4 - s.length(), ' ') + s;
                }
                if (is_hl) {
                    SetConsoleTextAttribute(h, get_highlight_attr()); 
                    print_at(14, 9 + i, " " + n + " " + (item.is_dir ? " Fol " : " Txt ") + s + "  ");
                } else {
                    /* FIX: Reverted narrow mode back to NO separators for unhighlighted items! */
                    print_at(15, 9 + i, n); 
                    print_at(SCREEN_COLS - 16, 9 + i, item.is_dir ? " Fol " : " Txt "); 
                    print_at(SCREEN_COLS - 11, 9 + i, s + "  ");
                }
            } else {
                /* FIX: Empty rows in narrow mode are completely blank! */
                print_at(14, 9 + i, string(SCREEN_COLS - 19, ' '));
            }
        } else {
            if (has_item) {
                auto& item = browser_files[idx];
                string n = item.name; int name_len = SCREEN_COLS - 39;
                if (n.length() > (size_t)name_len) n = n.substr(0, name_len - 3) + "..."; else n = n + string(name_len - n.length(), ' ');
                
                string s = "";
                if (!item.is_dir) {
                    long kb = item.size / 1024; if (kb == 0 && item.size > 0) kb = 1;
                    if (kb > 999) { long mb = kb / 1024; if (mb == 0) mb = 1; s = to_string(mb) + "MB"; } else s = to_string(kb) + "KB";
                }
                s = s.length() > 5 ? s.substr(0, 5) : s + string(5 - s.length(), ' ');
                
                if (is_hl) SetConsoleTextAttribute(h, get_highlight_attr());
                /* FIX: Applied MB conversion to wide mode and synced the 5-char alignment! */
                print_at(17, 9 + i, " " + n + "│" + (item.is_dir ? " Folder " : " Text   ") + "│ " + s);
            } else {
                /* FIX: Empty rows in wide mode retain the vertical separators! */
                print_at(17, 9 + i, string(SCREEN_COLS - 38, ' ') + "│        │      ");
            }
        }
        if (is_hl) SetConsoleTextAttribute(h, get_default_attr());
    }

    /* FIX: Ported DOS Sliding Camera to File Name Box! */
    int max_in = (SCREEN_COLS < 80) ? SCREEN_COLS - 33 : SCREEN_COLS - 38;
    int in_x = (SCREEN_COLS < 80) ? 14 : 17;
    string idisp;
    if (active_input_field == 1 && dialog_input_cursor > max_in - 1) {
        idisp = dialog_input_text.substr(dialog_input_cursor - (max_in - 1), max_in);
    } else {
        if (dialog_input_text.length() > (size_t)max_in) {
            if (active_input_field == 1) idisp = dialog_input_text.substr(0, max_in);
            else idisp = dialog_input_text.substr(dialog_input_text.length() - max_in);
        } else idisp = dialog_input_text + string(max_in - dialog_input_text.length(), ' ');
    }
    print_at(in_x, SCREEN_ROWS - 5, idisp);
    if (active_input_field == 1 && setting_hover_effects == 2) {
        int local_c = dialog_input_cursor - max(0, dialog_input_cursor - (max_in - 1));
        char ch = (local_c >= 0 && local_c < (int)idisp.length()) ? idisp[local_c] : ' ';
        SetConsoleTextAttribute(h, BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE);
        print_at(in_x + local_c, SCREEN_ROWS - 5, string(1, ch)); SetConsoleTextAttribute(h, get_default_attr());
    }
    
    int msg_len = (SCREEN_COLS < 80) ? SCREEN_COLS - 16 : SCREEN_COLS - 18;
    print_at(14, SCREEN_ROWS - 2, dialog_status_msg.length() > (size_t)msg_len ? dialog_status_msg.substr(0, msg_len) : dialog_status_msg + string(msg_len - dialog_status_msg.length(), ' '));
}

void draw_file_menu() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WORD def = get_default_attr(), hl = get_highlight_attr(), red = get_critical_attr();
    
    /* FIX: IMGUI Menu Architecture. Draw borders and hover states atomically with fast_print! */
    gotoxy(0, 2); fast_print(apply_ansi("├──────┴──────┴┬"));
    gotoxy(0, 3); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 10 ? hl : def); fast_print(" New File     "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(0, 4); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 11 ? hl : def); fast_print(" Open         "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(0, 5); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 12 ? hl : def); fast_print(" Save         "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(0, 6); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 13 ? hl : def); fast_print(" Save As      "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(0, 7); fast_print(apply_ansi("├──────────────┤"));
    gotoxy(0, 8); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 14 ? hl : def); fast_print(" Clear Page   "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(0, 9); fast_print(apply_ansi("├──────────────┤"));
    gotoxy(0, 10); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 15 ? hl : def); fast_print(" Settings     "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(0, 11); fast_print(apply_ansi("├──────────────┤"));
    gotoxy(0, 12); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 16 ? red : def); fast_print(" Exit         "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(0, 13); fast_print(apply_ansi("├──────────────┘"));
}

void draw_edit_menu(int sx, int sy, bool ctxt) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WORD def = get_default_attr(), hl = get_highlight_attr(), red = get_critical_attr();

    gotoxy(sx, sy); fast_print(apply_ansi("┬┴──────┴──────┼"));
    gotoxy(sx, sy + 1); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 24 ? hl : def); fast_print(" Undo         "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 2); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 25 ? hl : def); fast_print(" Redo         "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 3); fast_print(apply_ansi("├──────────────┤"));
    gotoxy(sx, sy + 4); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 20 ? hl : def); fast_print(" Cut          "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 5); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 21 ? hl : def); fast_print(" Copy         "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 6); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 22 ? hl : def); fast_print(" Paste        "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 7); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 23 ? red : def); fast_print(" Delete       "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 8); fast_print(apply_ansi("├──────────────┤"));
    gotoxy(sx, sy + 9); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 26 ? hl : def); fast_print(" Select All   "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    /* FIX: Added Find/Replace to the Edit Menu! */
    gotoxy(sx, sy + 10); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 28 ? hl : def); fast_print(" Find/Replace "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    
    string ro_text = setting_read_only ? " Read Only [√]" : " Read Only [ ]";
    gotoxy(sx, sy + 11); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 27 ? hl : def); fast_print(ro_text); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 12); fast_print(apply_ansi("└──────────────┘"));
}

/* FIX: Standalone DOS-Style Context Menu! */
void draw_context_menu(int sx, int sy) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WORD def = get_default_attr(), hl = get_highlight_attr(), red = get_critical_attr();

    gotoxy(sx, sy); fast_print(apply_ansi("┌──────────────┐"));
    gotoxy(sx, sy + 1); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 20 ? hl : def); fast_print(" Cut          "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 2); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 21 ? hl : def); fast_print(" Copy         "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 3); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 22 ? hl : def); fast_print(" Paste        "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 4); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 23 ? red : def); fast_print(" Delete       "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 5); fast_print(apply_ansi("├──────────────┤"));
    gotoxy(sx, sy + 6); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 26 ? hl : def); fast_print(" Select All   "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    /* FIX: Add Read Only to the Context Menu */
    string ro_text = setting_read_only ? " Read Only [√]" : " Read Only [ ]";
    gotoxy(sx, sy + 7); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 27 ? hl : def); fast_print(ro_text); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(sx, sy + 8); fast_print(apply_ansi("└──────────────┘"));
}

void draw_view_menu() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WORD def = get_default_attr(), hl = get_highlight_attr();
    
    gotoxy(13, 2); fast_print(apply_ansi("┬┴──────┴─────────────┬"));
    gotoxy(13, 3); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 30 ? hl : def); fast_print(" Enable Horzizontal  "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(13, 4); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 30 ? hl : def); fast_print(" Scroll Bar          "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(13, 5); fast_print(apply_ansi("├─────────────────────┤"));
    gotoxy(13, 6); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 31 ? hl : def); fast_print(" Enable Vertical     "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(13, 7); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 31 ? hl : def); fast_print(" Scroll Bar          "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(13, 8); fast_print(apply_ansi("├─────────────────────┤"));
    gotoxy(13, 9); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 32 ? hl : def); fast_print(" Enable Word Wrap    "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(13, 10); fast_print(apply_ansi("├─────────────────────┤"));
    gotoxy(13, 11); fast_print(apply_ansi("│")); SetConsoleTextAttribute(h, current_hover_element == 33 ? hl : def); fast_print(" Respect Whole Words "); SetConsoleTextAttribute(h, def); fast_print(apply_ansi("│"));
    gotoxy(13, 12); fast_print(apply_ansi("└─────────────────────┘"));
}

void draw_find_dialog() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WORD bg = get_default_attr(), hl = get_highlight_attr(), red = get_invert_attr();
    
    int end_col = v_scrollBar ? SCREEN_COLS - 3 : SCREEN_COLS - 1;
    int inner_w = end_col - 1;

    /* FIX: Single-pass Kernel Printer. Zero destructive screen wipes! Spaces are baked into the strings. */
    if (SCREEN_COLS < 80) {
        string pad = string(max(0, inner_w - 34), ' ');
        int w = 9;
        string fdisp = find_text.length() > (size_t)w ? find_text.substr(max(0, find_cursor - w), w) : find_text + string(w - find_text.length(), ' ');
        string rdisp = replace_text.length() > (size_t)w ? replace_text.substr(max(0, replace_cursor - w), w) : replace_text + string(w - replace_text.length(), ' ');

        gotoxy(1, 3); fast_print(apply_ansi("       ┌─────────┐ ┌─────┐ ┌─────┐" + pad));
        
        gotoxy(1, 4); fast_print(" Find: "); fast_print(apply_ansi("│")); 
        for (int i=0; i<w; i++) {
            if (active_input_field == 4 && setting_hover_effects == 2 && i == find_cursor - max(0, find_cursor - w)) {
                SetConsoleTextAttribute(h, red);
                fast_print(string(1, fdisp[i])); SetConsoleTextAttribute(h, bg);
            } 
            else fast_print(string(1, fdisp[i]));
        }
        /* FIX: Added the missing right-wall for the input box and left-wall for the Nxt button! */
        fast_print(apply_ansi("│ "));
        fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (active_tab_element==21 || current_hover_element==90) ? hl : bg); fast_print(" Nxt ");
        SetConsoleTextAttribute(h, bg); fast_print(apply_ansi("│ "));
        fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (active_tab_element==22 || current_hover_element==91) ? hl : bg); fast_print(" Prv "); SetConsoleTextAttribute(h, bg); 
        fast_print(apply_ansi("│") + pad);
        
        gotoxy(1, 5); fast_print(apply_ansi("       └─────────┘ └─────┘ └─────┘" + pad));
        gotoxy(1, 6); fast_print(apply_ansi("       ┌─────────┐ ┌─────┐ ┌─────┐" + pad));
        
        gotoxy(1, 7); fast_print(" Rep:  "); fast_print(apply_ansi("│"));
        for (int i=0; i<w; i++) {
            if (active_input_field == 5 && setting_hover_effects == 2 && i == replace_cursor - max(0, replace_cursor - w)) {
                SetConsoleTextAttribute(h, red); 
                fast_print(string(1, rdisp[i])); SetConsoleTextAttribute(h, bg);
            } else fast_print(string(1, rdisp[i]));
        }
        /* FIX: Added the missing right-wall for the input box and left-wall for the Rep button! */
        fast_print(apply_ansi("│ "));
        fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (active_tab_element==23 || current_hover_element==92) ? hl : bg); fast_print(" Rep "); 
        SetConsoleTextAttribute(h, bg); fast_print(apply_ansi("│ "));
        fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (active_tab_element==24 || current_hover_element==93) ? hl : bg); fast_print(" All "); SetConsoleTextAttribute(h, bg); 
        fast_print(apply_ansi("│") + pad);
        
        gotoxy(1, 8); fast_print(apply_ansi("       └─────────┘ └─────┘ └─────┘" + pad));
        
    } else {
        string pad = string(max(0, inner_w - 75), ' ');
        int w = 39;
        string fdisp = find_text.length() > (size_t)w ? find_text.substr(max(0, find_cursor - w), w) : find_text + string(w - find_text.length(), ' ');
        string rdisp = replace_text.length() > (size_t)w ? replace_text.substr(max(0, replace_cursor - w), w) : replace_text + string(w - replace_text.length(), ' ');

        gotoxy(1, 3); fast_print(apply_ansi("          ┌───────────────────────────────────────┐ ┌─────────┐ ┌─────────┐" + pad));
        
        gotoxy(1, 4); fast_print("    Find: "); fast_print(apply_ansi("│"));
        for (int i=0; i<w; i++) {
            if (active_input_field == 4 && setting_hover_effects == 2 && i == find_cursor - max(0, find_cursor - w)) {
                SetConsoleTextAttribute(h, red); fast_print(string(1, fdisp[i])); SetConsoleTextAttribute(h, bg);
            } else fast_print(string(1, fdisp[i]));
        }
        fast_print(apply_ansi("│ "));
        fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (active_tab_element==21 || current_hover_element==90) ? hl : bg); fast_print("  Next   "); SetConsoleTextAttribute(h, bg); fast_print(apply_ansi("│ "));
        fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (active_tab_element==22 || current_hover_element==91) ? hl : bg); fast_print("  Prev   "); SetConsoleTextAttribute(h, bg); fast_print(apply_ansi("│") + pad);
        
        gotoxy(1, 5); fast_print(apply_ansi("          └───────────────────────────────────────┘ └─────────┘ └─────────┘" + pad));
        gotoxy(1, 6); fast_print(apply_ansi("          ┌───────────────────────────────────────┐ ┌─────────┐ ┌─────────┐" + pad));
        
        gotoxy(1, 7); fast_print(" Replace: "); fast_print(apply_ansi("│"));
        for (int i=0; i<w; i++) {
            if (active_input_field == 5 && setting_hover_effects == 2 && i == replace_cursor - max(0, replace_cursor - w)) {
                SetConsoleTextAttribute(h, red); fast_print(string(1, rdisp[i])); SetConsoleTextAttribute(h, bg);
            } else fast_print(string(1, rdisp[i]));
        }
        fast_print(apply_ansi("│ "));
        fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (active_tab_element==23 || current_hover_element==92) ? hl : bg); fast_print(" Replace "); SetConsoleTextAttribute(h, bg); fast_print(apply_ansi("│ "));
        fast_print(apply_ansi("│"));
        SetConsoleTextAttribute(h, (active_tab_element==24 || current_hover_element==93) ? hl : bg); fast_print("   All   "); SetConsoleTextAttribute(h, bg); fast_print(apply_ansi("│") + pad);
        
        gotoxy(1, 8); fast_print(apply_ansi("          └───────────────────────────────────────┘ └─────────┘ └─────────┘" + pad));
    }
    
    gotoxy(0, 9); fast_print(apply_ansi("├" + repeat_str("─", inner_w) + "┤"));
}

string build_theme_line(string label, int val, bool color_enabled) {
    if(SCREEN_COLS < 80){
        string s = "  " + label;
        s += string("Bk ") + (val == 0 ? "(•)" : "( )") + " ";
        s += string("Wh ") + (val == 7 ? "(•)" : "( )");
        if (color_enabled) {
            s += " B " + string((val != 0 && val != 7 && (val & 1)) ? "[√]" : "[ ]") + " ";
            s += string("G ") + ((val != 0 && val != 7 && (val & 2)) ? "[√]" : "[ ]") + " ";
            s += string("R ") + ((val != 0 && val != 7 && (val & 4)) ? "[√]" : "[ ]");
        }
        return s;
    }
    else {
        string s = "  " + label + " - ";
        s += string("Black ") + (val == 0 ? "(•)" : "( )") + ", ";
        s += string("White ") + (val == 7 ? "(•)" : "( )");
        if (color_enabled) {
            s += ", Blue " + string((val != 0 && val != 7 && (val & 1)) ? "[√]" : "[ ]") + ", ";
            s += string("Green ") + ((val != 0 && val != 7 && (val & 2)) ? "[√]" : "[ ]") + ", ";
            s += string("Red ") + ((val != 0 && val != 7 && (val & 4)) ? "[√]" : "[ ]");
        }
        return s;
    }
    
}

void draw_title_at(int x, int w_limit) { 
    gotoxy(x, 1); 
    int w = (w_limit > 0) ? w_limit : SCREEN_COLS;
    
    /* FIX: Prepend a space so the text doesn't touch the border, but we can physically start at index 22 */
    string d = " " + string(is_document_dirty ? "*" : "") + current_filename; 
    
    /* FIX: Dynamically calculate the exact gap to the X button to kill the 1-pixel dead zones! */
    int max_len = (w - 5) - x; 
    if (max_len < 5) max_len = 5;
    
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), get_default_attr());
    string final_str = (d.length() > max_len ? d.substr(0, max_len - 3) + "..." : d + string(max_len - d.length(), ' '));
    fast_print(final_str);
}

void draw_settings() {
    bool narrow = SCREEN_COLS < 80;
    /* FIX: Removed the destructive cout screen wipe. The border loop now handles the padding natively! */
    for (int y = 0; y < SCREEN_ROWS; y++) {
        if (narrow) {
            if (y == 0) print_at     (0, y, "┌──────────┬─" + repeat_str("─", SCREEN_COLS - 18) +                       "┬───┐");
            else if (y == 1) print_at(0, y, "│ Settings │ " + string(SCREEN_COLS - 18, ' ') +                           "│ X │");
            else if (y == 2) print_at(0, y, "├─────────┬┴─────────┬───────┬─" + repeat_str("─", SCREEN_COLS - 40) + "┬───┴───┤");
            else if (y == 3) print_at(0, y, "│ Display │ Graphics │ Other │ " + string(SCREEN_COLS - 40, ' ') +     "│← Back │");
            else if (y == 4) {
                if (current_settings_tab == 0) print_at(0, y,      "│         └──────────┴───────┴─" + repeat_str("─", SCREEN_COLS - 40) + "┴───────┤");
                else if (current_settings_tab == 1) print_at(0, y, "├─────────┘          └───────┴─" + repeat_str("─", SCREEN_COLS - 40) + "┴───────┤");
                else print_at(0, y,                                "├─────────┴──────────┘       └─" + repeat_str("─", SCREEN_COLS - 40) + "┴───────┤");
            }
            else if (y >= 5 && y <= SCREEN_ROWS - 2) print_at(0, y, "│" + string(SCREEN_COLS - 2, ' ') + "│");
            else if (y == SCREEN_ROWS - 1) print_at(0, y, "└" + repeat_str("─", SCREEN_COLS - 2) + "┘");
            
        } else {
            if (y == 0) print_at     (0, y, "┌──────────┬──────────" + repeat_str("─", SCREEN_COLS - 27) + "┬───┐");
            else if (y == 1) print_at(0, y, "│ Settings │          " + string(SCREEN_COLS - 27, ' ') + "│ X │");
            else if (y == 2) print_at(0, y, "├─────────┬┴─────────┬───────┬────────────────────────" + repeat_str("─", SCREEN_COLS - 63) + "┬───┴───┤");
            else if (y == 3) print_at(0, y, "│ Display │ Graphics │ Other │                        " + string(SCREEN_COLS - 63, ' ') + "│← Back │");
            else if (y == 4) {
                if (current_settings_tab == 0) print_at(0, y,      "│         └──────────┴───────┴────────────────────────" + repeat_str("─", SCREEN_COLS - 63) + "┴───────┤");
                else if (current_settings_tab == 1) print_at(0, y, "├─────────┘          └───────┴────────────────────────" + repeat_str("─", SCREEN_COLS - 63) + "┴───────┤");
                else print_at(0, y,                                "├─────────┴──────────┘       └────────────────────────" + repeat_str("─", SCREEN_COLS - 63) + "┴───────┤");
            }
            else if (y >= 5 && y <= SCREEN_ROWS - 2) print_at(0, y, "│" + string(SCREEN_COLS - 2, ' ') + "│");
            else if (y == SCREEN_ROWS - 1) print_at(0, y, "└" + repeat_str("─", SCREEN_COLS - 2) + "┘");
        }
    } 
    draw_title_at(13, narrow ? 40 : SCREEN_COLS);

    if (current_settings_tab == 0) {
        /* XP is permanently locked to Fixed Viewport */
        print_at(2, 6, narrow ? "Current Display Mode: Fixed View" : "Current Display Mode: Fixed Viewport");

        print_at(2, 8, "Terminal Resolution:");
        print_at(4, 9, string("Width:  ") + to_string(SCREEN_COLS) + " columns");
        print_at(4, 10, string("Height: ") + to_string(SCREEN_ROWS) + " rows");
    }
    else if (current_settings_tab == 1) {
        print_at(2, 6, "Hover Effects:"); print_at(4, 7, setting_hover_effects == 0 ? "(•) Off" : "( ) Off"); print_at(4, 8, setting_hover_effects == 1 ? "(•) Basic" : "( ) Basic"); print_at(4, 9, setting_hover_effects == 2 ? "(•) Enhanced" : "( ) Enhanced");
        print_at(2, 11, setting_color ? "Enable Color [√]" : "Enable Color [ ]");
        print_at(2, 13, "Ansi vs Unicode compatibility Mode:"); 
        print_at(4, 14, setting_ansi_mode == 0 ? "(•) Standard Ansi Mode"  : "( ) Standard Ansi Mode"); 
        print_at(4, 15, setting_ansi_mode == 1 ? "(•) Extended Ascii Mode" : "( ) Extended Ascii Mode");
        print_at(2, 17, "Theme Settings:");
        if (narrow) {
            print_at(0, 18, build_theme_line("BG: ", setting_bg_color, setting_color));
            print_at(0, 19, build_theme_line("FG: ", setting_fg_color, setting_color));
            if (setting_color) { 
                print_at(0, 20, build_theme_line("Sel:", setting_sel_color, true)); 
                print_at(0, 21, build_theme_line("Crt:", setting_crit_color, true)); 
            }
            print_at(0, 18, "│");
            print_at(0, 19, "│");
            print_at(0, 20, "│");
            print_at(0, 21, "│");
        } else {
            print_at(2, 18, build_theme_line("Background Color(BG)", setting_bg_color, setting_color));
            print_at(2, 19, build_theme_line("Foreground Color(FG)", setting_fg_color, setting_color));
            if (setting_color) { 
                print_at(2, 20, build_theme_line("Selection  Color(BG)", setting_sel_color, true)); 
                print_at(2, 21, build_theme_line("Critical   Color(BG)", setting_crit_color, true)); 
            }
        }
    }
    else {
        print_at(2, 6,  string("Mouse Support: ") + (use_mouse ? "Yes" : "No"));
        print_at(2, 8,  "Memory Mode:"); 
        print_at(5, 9,  setting_disk_stream == 0 ? "(•) Fast Ram Buffer" : "( ) Fast Ram Buffer"); 
        print_at(5, 10, setting_disk_stream == 1 ? "(•) Disk Streaming Mode" : "( ) Disk Streaming Mode");
        
        print_at(2, 12, "Clipboard Mode:"); 
        print_at(5, 13, setting_internal_clipboard ? "(•) Internal Clipboard" : "( ) Internal Clipboard"); 
        print_at(5, 14, !setting_internal_clipboard ? "(•) Global OS Clipboard" : "( ) Global OS Clipboard");
    }
}

void draw_vscrollbar(int sy, int total_vlines) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (total_vlines <= 0) total_vlines = 1;
    int track_len = SCREEN_ROWS - 10;
    
    int ts = max(1, min(track_len, (int)round((float)track_len * MAX_ROWS / max(1, total_vlines))));
    int tstart = 0;
    
    if (setting_disk_stream && stream_lines.size() > CHUNK_LOG_SIZE) {
        ts = max(1, (int)round((float)track_len * CHUNK_LOG_SIZE / stream_lines.size()));
        tstart = (int)round((float)cursor_log_y / stream_lines.size() * (track_len - ts));
    } else if (total_vlines > MAX_ROWS) {
        tstart = (int)round((float)sy / (total_vlines - MAX_ROWS) * (track_len - ts));
    }
    
    WORD thumb_attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
    WORD def_attr = get_default_attr();

    /* FIX: Single-pass render. If it's a thumb, draw a block. Otherwise, draw a space. Never double-draw! */
    for (int i = 0; i < track_len; i++) {
        if (i >= tstart && i < tstart + ts) {
            SetConsoleTextAttribute(h, thumb_attr);
            print_at(SCREEN_COLS - 2, 5 + i, "█");
        } else {
            SetConsoleTextAttribute(h, def_attr);
            print_at(SCREEN_COLS - 2, 5 + i, " ");
        }
    }
    SetConsoleTextAttribute(h, def_attr);
}

void draw_bottom_bar() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    
    /* FIX: Revert text shifting! The text core size ALWAYS stays identical to the horizontal scrollbar width. */
    int core_len = v_scrollBar ? SCREEN_COLS - 8 : SCREEN_COLS - 6;
    
    if (h_scrollBar && is_hovering_hbar) {
        gotoxy(3, SCREEN_ROWS - 2);
        int max_off = max(0, max_line_length - MAX_COLS), ts = max(1, min(core_len, (int)round((float)core_len * MAX_COLS / max_line_length)));
        int tp = max_off > 0 ? (int)round((float)h_scroll_offset / max_off * (core_len - ts)) : 0;
        WORD thumb_attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
        WORD def_attr = get_default_attr();
        
        string pre = string(tp, ' ');
        string thumb = repeat_str(setting_ansi_mode == 1 ? "█" : "|", ts);
        string post = string(max(0, core_len - (tp + ts)), ' ');
        
        SetConsoleTextAttribute(h, def_attr); if (!pre.empty()) fast_print(pre);
        SetConsoleTextAttribute(h, thumb_attr); if (!thumb.empty()) fast_print(thumb);
        SetConsoleTextAttribute(h, def_attr); if (!post.empty()) fast_print(post);
    }
    else {
        SetConsoleTextAttribute(h, get_default_attr());
        int pct = 0;
        if (setting_disk_stream && stream_lines.size() > CHUNK_LOG_SIZE) {
            if (stream_lines.size() > 0) pct = (int)((cursor_log_y * 100) / stream_lines.size());
        } else {
            if (total_visual_lines > MAX_ROWS) pct = (int)(((long)scroll_y * 100) / (total_visual_lines - MAX_ROWS));
        }
        string p_str = to_string(pct) + "%";
        int cc = 0; for (size_t i = 0; i < piece_table.size(); i++) cc += piece_table[i].length;
        string st = (SCREEN_COLS < 80 ? "" : "Chars: " + to_string(cc) + ", ") + "Ln " + to_string(cursor_log_y + 1) + ", Col " + to_string(cursor_log_x + 1);
        
        int pad_len = core_len - (int)p_str.length() - (int)st.length();
        if (pad_len < 0) pad_len = 0;
        
        string core_text = p_str + string(pad_len, ' ') + st;
        
        if (h_scrollBar) {
            gotoxy(3, SCREEN_ROWS - 2);
            fast_print(core_text);
        } else {
            /* FIX: Keep text rigidly locked at x=3, but pad the outer edges with spaces to natively erase the ghost arrows! */
            int total_fill = (v_scrollBar ? SCREEN_COLS - 3 : SCREEN_COLS - 1) - 1;
            string final_bar = "  " + core_text;
            int trailing = total_fill - (int)final_bar.length();
            if (trailing > 0) final_bar += string(trailing, ' ');
            
            gotoxy(1, SCREEN_ROWS - 2);
            fast_print(final_bar);
        }
    }
}
void render_hover_element(int id, bool is_hovered) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((id == 1 && active_menu == 1) || (id == 2 && active_menu == 2 && !is_context_menu) || (id == 3 && active_menu == 3)) is_hovered = true;
    WORD def = get_default_attr();
    WORD hl = get_highlight_attr();
    WORD red = get_critical_attr();
    SetConsoleTextAttribute(h, is_hovered ? hl : def);

    /* FIX: Force attribute first, then blast the pure text directly to the OS kernel! */
    if (is_hovered && (id == 4 || id == 16 || id == 23)) SetConsoleTextAttribute(h, red);
    else SetConsoleTextAttribute(h, is_hovered ? hl : def);

    if (id == 1) { gotoxy(1, 1); fast_print(" File "); } else if (id == 2) { gotoxy(8, 1); fast_print(" Edit "); } else if (id == 3) { gotoxy(15, 1); fast_print(" View "); }
    else if (id == 4) { gotoxy(SCREEN_COLS - 4, 1); fast_print(" X "); }
    else if (id == 5) { gotoxy(SCREEN_COLS - 18, SCREEN_ROWS - 5); fast_print("  Ok  "); } else if (id == 6) { gotoxy(SCREEN_COLS - 9, SCREEN_ROWS - 5); fast_print("Cancel"); }
    else if (id >= 80 && id <= 82) {
        bool narrow = SCREEN_COLS < 80;
        int bx = narrow ? max(0, (SCREEN_COLS - 38) / 2) : max(0, (SCREEN_COLS - 44) / 2);
        int by = max(0, (SCREEN_ROWS - 10) / 2);
        
        if (id == 80) { gotoxy(bx + (narrow ? 3 : 4), by + 7); fast_print(" Save "); } 
        else if (id == 81) { gotoxy(bx + (narrow ? 13 : 16), by + 7); fast_print(" Don't Save "); } 
        else if (id == 82) { SetConsoleTextAttribute(h, is_hovered ? hl : def); gotoxy(bx + (narrow ? 29 : 34), by + 7); fast_print("Cancel"); }
    }
    /* FIX: Deleted all menu hover logic from here! The fast_print draw_menu functions now handle this natively and atomically! */
    else if (id >= 40 && id <= 53 && show_settings) { //settings hover effects
        if (id == 40) print_at(1, 3, " Display "); 
        else if (id == 41) print_at(11, 3, " Graphics ");
        else if (id == 42) print_at(22, 3, " Other ");
        else if (id == 43) { SetConsoleTextAttribute(h, is_hovered ? red : def); print_at(SCREEN_COLS - 8, 3, "← Back "); }
        else if (current_settings_tab == 1) {
            if (id == 44) print_at(4, 7, setting_hover_effects == 0 ? "(•) Off" : "( ) Off"); else if (id == 45) print_at(4, 8, setting_hover_effects == 1 ? "(•) Basic" : "( ) Basic"); else if (id == 46) print_at(4, 9, setting_hover_effects == 2 ? "(•) Enhanced" : "( ) Enhanced");
            else if (id == 47) print_at(2, 11, setting_color ? "Enable Color [√]" : "Enable Color [ ]"); else if (id == 48) print_at(4, 14, setting_ansi_mode == 0 ? "(•) Standard Ansi Mode" : "( ) Standard Ansi Mode"); else if (id == 49) print_at(4, 15, setting_ansi_mode == 1 ? "(•) Extended Ascii Mode" : "( ) Extended Ascii Mode");
        }
        else if (current_settings_tab == 2) {
            if (id == 50)      print_at(5, 13, setting_internal_clipboard ? "(•) Internal Clipboard" : "( ) Internal Clipboard"); 
            else if (id == 51) print_at(5, 14, !setting_internal_clipboard ? "(•) Global OS Clipboard" : "( ) Global OS Clipboard");
            else if (id == 52) print_at(5, 9, setting_disk_stream == 0 ? "(•) Fast Ram Buffer" : "( ) Fast Ram Buffer"); 
            else if (id == 53) print_at(5, 10, setting_disk_stream == 1 ? "(•) Disk Streaming Mode" : "( ) Disk Streaming Mode");
        }
    }
    else if (id >= 100 && id < 140 && show_settings && current_settings_tab == 1) {
        int row = 18 + (id - 100) / 10; int col_idx = id % 10;
        if (!setting_color && (row >= 20 || col_idx > 1)) return;
        int val = (row == 18) ? setting_bg_color : ((row == 19) ? setting_fg_color : ((row == 20) ? setting_sel_color : setting_crit_color));
        int sx = (col_idx == 0) ? 27 : ((col_idx == 1) ? 38 : ((col_idx == 2) ? 49 : ((col_idx == 3) ? 59 : 70)));
        string t = (col_idx == 0) ? "Black " : ((col_idx == 1) ? "White " : ((col_idx == 2) ? "Blue " : ((col_idx == 3) ? "Green " : "Red ")));
        string box = "";
        if (col_idx == 0) box = (val == 0 ? "(•)" : "( )");
        else if (col_idx == 1) box = (val == 7 ? "(•)" : "( )");
        else if (col_idx == 2) box = ((val != 0 && val != 7 && (val & 1)) ? "[√]" : "[ ]");
        else if (col_idx == 3) box = ((val != 0 && val != 7 && (val & 2)) ? "[√]" : "[ ]");
        else if (col_idx == 4) box = ((val != 0 && val != 7 && (val & 4)) ? "[√]" : "[ ]");
        print_at(sx, row, t + box);
    }
    else if (id == 60) print_at(SCREEN_COLS - 2, SCREEN_ROWS - 2, "☼"); 
    else if (id == 61) print_at(SCREEN_COLS - 2, 3, "▲"); 
    else if (id == 62) print_at(SCREEN_COLS - 2, SCREEN_ROWS - 4, "▼");
    else if (id == 63) print_at(1, SCREEN_ROWS - 2, "◄"); 
    else if (id == 64) print_at(v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2, SCREEN_ROWS - 2, "►"); 
    else if (id == 65) print_at(SCREEN_COLS - 4, 7, "▲"); 
    else if (id == 66) print_at(SCREEN_COLS - 4, SCREEN_ROWS - 8, "▼");
    else if (id == 70) print_at(3, 4, "◄─"); 
    else if (id == 71) print_at(8, 4, "─►"); 
    else if (id == 72) print_at(13, 4, "▲");
    SetConsoleTextAttribute(h, def);
}

void displayTextBuffer(const TextBuffer& buffer) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE); 
    
    SetConsoleTitleA("Terminal Notepad");
    
    /* FIX: Stateful cursor tracking. Stops Windows from aggressively flashing the hardware cursor! */
    static bool last_cursor_state = true;
    CONSOLE_CURSOR_INFO cI; GetConsoleCursorInfo(h, &cI); 
    if (last_cursor_state) { cI.bVisible = FALSE; SetConsoleCursorInfo(h, &cI); last_cursor_state = false; }
    
    if (show_settings) {
        draw_settings();
        if ((setting_hover_effects == 2 || active_tab_element > 0) && current_hover_element != 0) render_hover_element(current_hover_element, true);
        bool want_vis = (setting_hover_effects == 1);
        if (last_cursor_state != want_vis) { cI.bVisible = want_vis; SetConsoleCursorInfo(h, &cI); last_cursor_state = want_vis; }
        if (want_vis) gotoxy(blink_x, blink_y);
        return;
    }
    if (show_open_dialog || show_save_dialog) {
        draw_file_dialog(show_save_dialog); 
        draw_title_at(SCREEN_COLS <80 ? 1 : 2);
        if (setting_hover_effects == 2) render_hover_element(4, false);
        if ((setting_hover_effects == 2 || active_tab_element > 0) && current_hover_element != 0) render_hover_element(current_hover_element, true);
        
        bool want_vis = ((active_input_field > 0 && setting_hover_effects != 2) || setting_hover_effects == 1); 
        if (last_cursor_state != want_vis) { cI.bVisible = want_vis ? TRUE : FALSE; SetConsoleCursorInfo(h, &cI); last_cursor_state = want_vis; }
        
        if (want_vis) {
            if (active_input_field == 1) gotoxy(min(SCREEN_COLS - 23, 18 + dialog_input_cursor), SCREEN_ROWS - 5); 
            else if (active_input_field == 2) gotoxy(min(SCREEN_COLS - 5, (SCREEN_COLS - 16) + dialog_search_cursor), 4);
            else if (active_input_field == 3) gotoxy(18 + dialog_address_cursor - (dialog_address_cursor >= SCREEN_COLS - 39 ? dialog_address_cursor - (SCREEN_COLS - 40) : 0), 4); 
            else if (setting_hover_effects == 1) gotoxy(blink_x, blink_y);
        }
        return;
    }
    scroll_y = max(0, min(scroll_y, max(0, total_visual_lines - MAX_ROWS)));
    for (int i = 0; i < MAX_ROWS; ++i) {
        int screen_y = text_start_y + i;
        int m_left = -1, m_right = -1;
        
        /* FIX: Dynamic Canvas Masking. Punch a hole in the text buffer so it NEVER overwrites active menus! */
        if (show_exit_dialog) {
            bool narrow = SCREEN_COLS < 80;
            int bx = narrow ? max(0, (SCREEN_COLS - 38) / 2) : max(0, (SCREEN_COLS - 44) / 2);
            int by = max(0, (SCREEN_ROWS - 10) / 2);
            if (screen_y >= by && screen_y <= by + 9) { m_left = bx; m_right = bx + (narrow ? 37 : 43); }
        }
        else if (active_menu == 1 && screen_y >= 2 && screen_y <= 13) { m_left = 0; m_right = 15; }
        /* FIX: Expanded Edit menu mask for Find/Replace */
        else if (active_menu == 2 && screen_y >= 2 && screen_y <= 14) { m_left = 6; m_right = 21; }
        else if (active_menu == 3 && screen_y >= 2 && screen_y <= 12) { m_left = 13; m_right = 35; }
        else if (active_menu == 4 && screen_y >= context_y && screen_y <= context_y + 8) { m_left = context_x; m_right = context_x + 15; }

        gotoxy(1, screen_y); int lIdx = scroll_y + i;
        if (lIdx < (int)buffer.size()) {
            int ly = buffer_log_map[lIdx].first, lx = buffer_log_map[lIdx].second, cp = 0;
            int cursor_target_y = text_start_y + (cursor_screen_y - scroll_y);
            int cursor_target_x = 1 + cursor_screen_x;
            bool is_cursor_row = (active_menu == 0 && setting_hover_effects == 2 && screen_y == cursor_target_y);
            
            WORD current_attr = 0xFFFF;
            string chunk = "";
            for (int c = 0; c < (int)buffer[lIdx].length(); c++) {
                int cx = 1 + cp;
                if (cx >= m_left && cx <= m_right) {
                    if (!chunk.empty()) { fast_print(chunk); chunk = ""; }
                    gotoxy(m_right + 1, screen_y);
                    cp++; continue; /* Jump the cursor! Skip the masked bounds! */
                }
                
                WORD target_attr = show_exit_dialog ? get_dim_attr() : (is_in_selection(ly, lx + c) ? get_highlight_attr() : get_default_attr());
                
                /* FIX: Debug mode turns text red to test button wiring */
                if (debug_red_all && !show_exit_dialog) target_attr = get_critical_attr();
                
                if (is_cursor_row && cx == cursor_target_x) target_attr = get_invert_attr(); /* FIX: Bake cursor directly into the string! */
                
                if (target_attr != current_attr) {
                    if (!chunk.empty()) { fast_print(chunk); chunk = ""; }
                    SetConsoleTextAttribute(h, target_attr);
                    current_attr = target_attr;
                }
                chunk += buffer[lIdx][c]; cp++;
            }
            
            while (cp < MAX_COLS) {
                int cx = 1 + cp;
                if (cx >= m_left && cx <= m_right) {
                    if (!chunk.empty()) { fast_print(chunk); chunk = ""; }
                    gotoxy(m_right + 1, screen_y);
                    cp = m_right;
                } else {
                    WORD target_attr = show_exit_dialog ? get_dim_attr() : get_default_attr();
                    if (is_cursor_row && cx == cursor_target_x) target_attr = get_invert_attr(); /* FIX: Bake cursor padding! */
                    
                    if (target_attr != current_attr) {
                        if (!chunk.empty()) { fast_print(chunk); chunk = ""; }
                        SetConsoleTextAttribute(h, target_attr);
                        current_attr = target_attr;
                    }
                    chunk += " ";
                }
                cp++;
            }
            if (!chunk.empty()) { fast_print(chunk); }
            if (current_attr != (show_exit_dialog ? get_dim_attr() : get_default_attr())) SetConsoleTextAttribute(h, show_exit_dialog ? get_dim_attr() : get_default_attr());
        }
        else { 
            SetConsoleTextAttribute(h, show_exit_dialog ? get_dim_attr() : get_default_attr()); 
            string chunk = "";
            for (int cp = 0; cp < MAX_COLS; cp++) {
                int cx = 1 + cp;
                if (cx >= m_left && cx <= m_right) {
                    if (!chunk.empty()) { fast_print(chunk); chunk = ""; }
                    gotoxy(m_right + 1, screen_y);
                    cp = m_right - 1; 
                } else chunk += " ";
            }
            if (!chunk.empty()) fast_print(chunk);
        }
    }
    SetConsoleTextAttribute(h, show_exit_dialog ? get_dim_attr() : get_default_attr());
    if (v_scrollBar) draw_vscrollbar(scroll_y, total_visual_lines); draw_bottom_bar(); draw_title_at(22);
    if (show_exit_dialog) {
        draw_exit_dialog();
        if ((setting_hover_effects == 2 || active_tab_element > 0) && current_hover_element != 0) render_hover_element(current_hover_element, true);
        return;
    }
    
    /* FIX: Apply the exact same Z-Index priority to the main render loop! 
       The Find dialog must yield its refresh to the active menu during a window resize event! */
    if (show_find_dialog && active_menu == 0) draw_find_dialog();
    
    if (active_menu == 1) draw_file_menu(); else if (active_menu == 2) draw_edit_menu(6, 2, false); else if (active_menu == 3) draw_view_menu(); else if (active_menu == 4) draw_context_menu(context_x, context_y);
    
    /* FIX: Deleted the old double-draw cursor block! It is now drawn flawlessly inline on a single pass. */
    
    if (setting_hover_effects == 2 || active_menu != 0) {
        render_hover_element(1, active_menu == 1); render_hover_element(2, active_menu == 2); render_hover_element(3, active_menu == 3);
        if (setting_hover_effects == 2) render_hover_element(4, false);
        if (current_hover_element != 0 && (setting_hover_effects == 2 || active_tab_element > 0)) render_hover_element(current_hover_element, true);
    }
    
    bool want_vis = (setting_hover_effects == 1 && active_menu == 0);
    if (last_cursor_state != want_vis) { cI.bVisible = want_vis ? TRUE : FALSE; SetConsoleCursorInfo(h, &cI); last_cursor_state = want_vis; }
    if (setting_hover_effects == 1) { /* FIX: Hardware cursor only jumps in Basic Mode! */
        if (active_input_field == 1) {
            int max_in = (SCREEN_COLS < 80) ? SCREEN_COLS - 35 : SCREEN_COLS - 40;
            int in_x = (SCREEN_COLS < 80) ? 14 : 17;
            gotoxy(in_x + min(dialog_input_cursor, max_in - 1), SCREEN_ROWS - 5);
        } 
        else if (active_input_field == 2) {
            int search_len = (SCREEN_COLS < 80) ? 6 : 12;
            int search_x = (SCREEN_COLS < 80) ? SCREEN_COLS - 9 : SCREEN_COLS - 16;
            gotoxy(search_x + min(dialog_search_cursor, search_len - 1), 4);
        }
        else if (active_input_field == 3) {
            int addr_len = (SCREEN_COLS < 80) ? SCREEN_COLS - 28 : SCREEN_COLS - 40;
            int addr_x = (SCREEN_COLS < 80) ? 14 : 18;
            gotoxy(addr_x + min(dialog_address_cursor, addr_len - 1), 4);
        } 
        else if (active_input_field == 4) gotoxy(SCREEN_COLS < 80 ? 9 + min(9, find_cursor) : 12 + min(39, find_cursor), 4);
        else if (active_input_field == 5) gotoxy(SCREEN_COLS < 80 ? 9 + min(9, replace_cursor) : 12 + min(39, replace_cursor), 7);
        else gotoxy(blink_x, blink_y);
    }
}

void close_active_menu(const TextBuffer& buffer) { active_menu = current_hover_element = active_tab_element = 0; is_context_menu = is_hovering_hbar = false;
    init_app(argv0_global, false); 
    displayTextBuffer(buffer); }

TextBuffer wrap_text() {
    TextBuffer buffer; buffer_log_map.clear(); int cml = 0, bli = 0;
    int num_lines = (int)stream_lines.size();
    
    int start_log = 0, end_log = num_lines;
    if (setting_disk_stream) {
        start_log = current_chunk_start_log;
        end_log = min(num_lines, start_log + CHUNK_LOG_SIZE);
        
        /* Pre-emptive Buffer Shift (YouTube Style) */
        if (cursor_log_y < start_log || cursor_log_y >= end_log) {
            current_chunk_start_log = max(0, cursor_log_y - (CHUNK_LOG_SIZE / 2));
            start_log = current_chunk_start_log;
            end_log = min(num_lines, start_log + CHUNK_LOG_SIZE);
        } else if (start_log > 0 && cursor_log_y < start_log + 200) {
            current_chunk_start_log = max(0, cursor_log_y - (CHUNK_LOG_SIZE / 2));
            start_log = current_chunk_start_log;
            end_log = min(num_lines, start_log + CHUNK_LOG_SIZE);
        } else if (end_log < num_lines && cursor_log_y > end_log - 200) {
            current_chunk_start_log = max(0, cursor_log_y - (CHUNK_LOG_SIZE / 2));
            start_log = current_chunk_start_log;
            end_log = min(num_lines, start_log + CHUNK_LOG_SIZE);
        }
    }
    
    for (int i = start_log; i < end_log; i++) {
        long log_len = get_stream_logical_len(i); cml = max(cml, (int)log_len);
        bool icl = (i == cursor_log_y), fc = false; int cp = 0;
        
        if (!word_warp) {
            if (icl) { cursor_screen_y = bli; cursor_screen_x = cursor_log_x - h_scroll_offset; }
            buffer.push_back(get_streamed_segment(i, h_scroll_offset, MAX_COLS));
            buffer_log_map.push_back(make_pair(i, h_scroll_offset)); bli++;
        }
        else {
            long chars_left = log_len; int current_start = 0;
            if (chars_left == 0) {
                if (icl && !fc) { cursor_screen_y = bli; cursor_screen_x = cursor_log_x; fc = true; }
                buffer.push_back(""); buffer_log_map.push_back(make_pair(i, 0)); bli++;
            }
            while (chars_left > 0) {
                int sp = (int)min((long)MAX_COLS, chars_left);
                if (respect_whole_words && chars_left > MAX_COLS) {
                    string chunk = get_streamed_segment(i, current_start, MAX_COLS);
                    int ls = (int)chunk.rfind(' '); if (ls > 0) sp = ls;
                }
                if (icl && !fc && cursor_log_x >= current_start && cursor_log_x <= current_start + sp) { cursor_screen_y = bli; cursor_screen_x = cursor_log_x - current_start; fc = true; }
                buffer.push_back(get_streamed_segment(i, current_start, sp)); buffer_log_map.push_back(make_pair(i, current_start));
                int adv = (respect_whole_words && sp != MAX_COLS && chars_left > MAX_COLS) ? 1 : 0; 
                current_start += sp + adv; chars_left -= (sp + adv); bli++;
            }
        }
    }
    if (buffer.empty()) { buffer.push_back(""); buffer_log_map.push_back(make_pair(0, 0)); }
    max_line_length = cml; total_visual_lines = (int)buffer.size();
    return buffer;
}

void update_cursor_from_screen(int mx, int my, const TextBuffer& buffer) {
    int ty = max(0, scroll_y + (my - text_start_y)), tx = mx - 1;
    if (ty >= (int)buffer.size()) { cursor_log_y = stream_lines.empty() ? 0 : (int)stream_lines.size() - 1; cursor_log_x = get_stream_logical_len(cursor_log_y); }
    else {
        if (word_warp) tx = max(0, min(tx, (int)buffer[ty].length()));
        cursor_log_y = buffer_log_map[ty].first; 
        cursor_log_x = max(0, min(buffer_log_map[ty].second + tx, (int)get_stream_logical_len(cursor_log_y)));
    }
}

TextBuffer load_document(const string& filePath, const string& name) {
    current_filename = name; 
    piece_table.clear(); add_buffer.clear(); stream_lines.clear();
    h_scroll_offset = cursor_log_x = cursor_log_y = 0; undo_stack.clear(); redo_stack.clear(); last_was_typing = false;
    
    /* FIX: Force C++ to surrender the hoarded RAM back to Windows! */
    piece_table.shrink_to_fit(); add_buffer.shrink_to_fit(); stream_lines.shrink_to_fit();
    undo_stack.shrink_to_fit(); redo_stack.shrink_to_fit();
    
    /* FIX: Ghost File Cleanup! Silently purge any left-over hidden .tmp files from previous crashes */
    // If filePath is "C:\MyFiles\diary.txt", it ONLY looks for "C:\MyFiles\diary.txt.tmp"
    string temp_path = filePath + ".tmp";
    if (GetFileAttributesA(temp_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(temp_path.c_str());
    }
    
    if (stream_file_handle) { fclose(stream_file_handle); stream_file_handle = nullptr; }
    
    if (setting_disk_stream == 1) {
        stream_file_handle = fopen(filePath.c_str(), "rb");
        if (stream_file_handle) {
            fseek(stream_file_handle, 0, SEEK_END); long fsize = ftell(stream_file_handle); fseek(stream_file_handle, 0, SEEK_SET);
            piece_table.push_back(Piece(SOURCE_DISK, 0, fsize));
        }
    } else {
        FILE* f = fopen(filePath.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
            if (fsize > 0) {
                long old_size = add_buffer.size(); add_buffer.resize(old_size + fsize);
                fread(&add_buffer[old_size], 1, fsize, f);
                piece_table.push_back(Piece(SOURCE_RAM, old_size, fsize));
            }
            fclose(f);
        }
    }
    if (piece_table.empty()) piece_table.push_back(Piece(SOURCE_RAM, 0, 0));
    rebuild_stream_lines(); is_document_dirty = false; return wrap_text();
}

// --- Main Engine ---
int main(int argc, char* argv[]) {
    /* FIX: Drastically speeds up cout to prevent screen tearing */
    ios_base::sync_with_stdio(false); cin.tie(NULL);
    
    /* FIX: Standalone Application Directory Tracking! Anchor to the .exe instead of the shell's working directory. */
    char exe_path_buf[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path_buf, MAX_PATH)) {
        string path_str(exe_path_buf);
        size_t last_slash = path_str.find_last_of("\\/");
        if (last_slash != string::npos) {
            exe_dir = path_str.substr(0, last_slash + 1);
        }
    }
    
    load_config();
    original_cp = GetConsoleOutputCP(); 
    argv0_global = argv[0]; 
    use_mouse = IsLegacyConsole();
    font(); scrnsz();
    if (!IsLegacyConsole()) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) && (csbi.srWindow.Right - csbi.srWindow.Left + 1 < win_x_size || csbi.srWindow.Bottom - csbi.srWindow.Top + 1 < win_y_size)) {
            screen_too_small = true; clear_screen_safe(); SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), get_default_attr());
            gotoxy(0, 0); cout << "Terminal size is too small!\nPlease resize the window to\nat least 80x25 to continue...\n";
        }
    }
    if (!screen_too_small) init_app(argv[0], true);
    TextBuffer buffer;
    
    /* FIX: CLI Path Initialization! Correctly separate the directory and the filename */
    char cwd[MAX_PATH];
    if (GetCurrentDirectoryA(MAX_PATH, cwd)) current_browser_path = string(cwd) + "\\";
    
    if (argc < 2) { 
        current_filename = "Untitled.txt"; piece_table.push_back(Piece(SOURCE_RAM, 0, 0)); stream_lines.push_back(LineIndex(0)); is_document_dirty = false; buffer = wrap_text(); scroll_y = 0; 
    } else { 
        string full_path = argv[1]; string f_name = full_path; char abs_path[MAX_PATH];
        if (GetFullPathNameA(full_path.c_str(), MAX_PATH, abs_path, nullptr)) { full_path = abs_path; f_name = full_path; }
        size_t pos = full_path.find_last_of("\\/");
        if (pos != string::npos) { current_browser_path = full_path.substr(0, pos + 1); f_name = full_path.substr(pos + 1); }
        buffer = load_document(full_path, f_name); scroll_y = 0; 
    }
    if (!screen_too_small) displayTextBuffer(buffer);

    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD fdwMode = ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
    if (use_mouse) { fdwMode |= ENABLE_MOUSE_INPUT; fdwMode &= ~ENABLE_QUICK_EDIT_MODE; }
    if (!SetConsoleMode(hStdin, fdwMode)) use_mouse = false;

    FlushConsoleInputBuffer(hStdin);
    DWORD app_start_time = GetTickCount();
    INPUT_RECORD irInBuf[128]; DWORD cNumRead; bool running = true;

    bool needs_ui_redraw = false;
    auto refresh = [&]() { buffer = wrap_text(); needs_ui_redraw = true; };

    auto refresh_follow_cursor = [&]() {
        buffer = wrap_text();
        if (cursor_screen_y < scroll_y) scroll_y = cursor_screen_y;
        else if (cursor_screen_y >= scroll_y + MAX_ROWS) scroll_y = cursor_screen_y - MAX_ROWS + 1;
        needs_ui_redraw = true;
    };

    auto execPending = [&]() {
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), get_default_attr());
        if (pending_action == 1) { clear_screen_safe(); running = false; }
        else if (pending_action == 2) {
            piece_table.clear(); add_buffer.clear(); stream_lines.clear();
            piece_table.push_back(Piece(SOURCE_RAM, 0, 0)); stream_lines.push_back(LineIndex(0));
            current_filename = "Untitled.txt"; cursor_log_x = cursor_log_y = h_scroll_offset = 0; is_document_dirty = last_was_typing = false; undo_stack.clear(); redo_stack.clear(); pending_action = 0; init_app(argv0_global, true); refresh_follow_cursor();
        }
        else if (pending_action == 3) {
            show_open_dialog = true; active_input_field = 1; dialog_input_text = dialog_search_text = dialog_address_text = ""; dialog_input_cursor = dialog_search_cursor = dialog_address_cursor = 0; dialog_status_msg = "Ready to select file..."; char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd); load_directory(string(cwd), true); pending_action = 0; init_app(argv0_global, true); refresh_follow_cursor();
        }
        pending_action = 0;
        };

    auto closeDialog = [&](bool save_action) {
        if (save_action) {
            dialog_status_msg = show_save_dialog ? "Ready to save file..." : "Ready to select file...";
            if (dialog_input_text.length() > 0) {
                string fn = dialog_input_text; if (fn.length() < 4 || fn.substr(fn.length() - 4) != ".txt") fn += ".txt";
                string fp = current_browser_path + fn;
                if (show_save_dialog) { save_document(fp); show_save_dialog = false; active_input_field = current_hover_element = active_tab_element = 0; }
                else {
                    ifstream cf(fp); if (!cf.is_open()) { dialog_status_msg = "Error: file do not exist"; refresh(); return; }
                    cf.close(); buffer = load_document(fp, fn); show_open_dialog = false; active_input_field = current_hover_element = active_tab_element = 0;
                }
            }
            else return;
        }
        else { show_open_dialog = show_save_dialog = false; active_input_field = current_hover_element = active_tab_element = 0; }

        if (pending_action != 0) {
            execPending();
        }
        else {
            init_app(argv0_global, true);
            refresh_follow_cursor();
        }
        };

    auto executeMenuAction = [&](int id) {
        bool sc = true;
        if (id == 10 || id == 11 || id == 14 || id == 16) {
            if (is_document_dirty) {
                show_exit_dialog = true; active_menu = current_hover_element = 0; pending_action = (id == 10 ? 2 : (id == 11 ? 3 : (id == 14 ? 2 : 1)));
                SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), get_dim_attr()); init_app(argv0_global, true); refresh(); return;
            }
            if (id == 16) { clear_screen_safe(); running = false; return; }
            if (id == 10 || id == 14) { 
                piece_table.clear(); add_buffer.clear(); stream_lines.clear();
                piece_table.push_back(Piece(SOURCE_RAM, 0, 0)); stream_lines.push_back(LineIndex(0));
                current_filename = "Untitled.txt"; cursor_log_x = cursor_log_y = h_scroll_offset = 0; undo_stack.clear(); redo_stack.clear(); last_was_typing = false; 
            }
            if (id == 11) { active_menu = 0; show_open_dialog = true; active_input_field = 1; dialog_input_text = dialog_search_text = dialog_address_text = ""; dialog_input_cursor = dialog_search_cursor = dialog_address_cursor = 0; dialog_status_msg = "Ready to select file..."; char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd); load_directory(string(cwd), true); sc = false; }
        }
        else if (id == 12) {
            if (current_filename == "Untitled.txt") { active_menu = 0; show_save_dialog = true; active_input_field = 1; dialog_input_text = dialog_search_text = dialog_address_text = ""; dialog_input_cursor = dialog_search_cursor = dialog_address_cursor = 0; dialog_status_msg = "Ready to save file..."; char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd); load_directory(string(cwd), true); sc = false; }
            else save_document(current_browser_path + current_filename);
        }
        else if (id == 13) { active_menu = 0; show_save_dialog = true; active_input_field = 1; dialog_input_text = current_filename != "Untitled.txt" ? current_filename : ""; dialog_input_cursor = (int)dialog_input_text.length(); dialog_search_text = dialog_address_text = ""; dialog_search_cursor = dialog_address_cursor = 0; dialog_status_msg = "Ready to save file..."; char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd); load_directory(string(cwd), true); sc = false; }
        else if (id == 15) { active_menu = 0; show_settings = true; draw_settings(); sc = false; }
        else if (id == 24) perform_undo(); else if (id == 25) perform_redo();
        else if (id == 20) { if (has_selection) { if (setting_read_only) return; save_state(); last_was_typing = false; set_clipboard(get_selected_text()); delete_selection(); } }
        else if (id == 21) { if (has_selection) set_clipboard(get_selected_text()); }
        else if (id == 22) { if (setting_read_only) return; save_state(); last_was_typing = false; insert_text(get_clipboard()); }
        else if (id == 23) { if (has_selection) { if (setting_read_only) return; save_state(); last_was_typing = false; delete_selection(); } }
        else if (id == 26) { sel_start_log_x = sel_start_log_y = 0; cursor_log_y = stream_lines.empty() ? 0 : (int)stream_lines.size() - 1; cursor_log_x = get_stream_logical_len(cursor_log_y); has_selection = true; last_was_typing = false; }
        else if (id == 27) { setting_read_only = !setting_read_only; }
        /* FIX: Wire up the Find/Replace action to mirror Ctrl+F */
        else if (id == 28) { 
            show_find_dialog = true; 
            MAX_ROWS = SCREEN_ROWS - 13; text_start_y = 10; 
            active_input_field = 4; if (has_selection) { find_text = get_selected_text(); find_cursor = find_text.length(); }
        }
        else if (id == 30) { h_scrollBar = !h_scrollBar; if (h_scrollBar) word_warp = false; else { word_warp = true; h_scroll_offset = 0; } save_config(); }
        else if (id == 31) { v_scrollBar = !v_scrollBar; MAX_COLS = v_scrollBar ? 76 : 78; save_config(); }
        else if (id == 32) { word_warp = !word_warp; if (word_warp) { h_scrollBar = false; h_scroll_offset = 0; } else h_scrollBar = true; save_config(); }
        else if (id == 33) respect_whole_words = !respect_whole_words; save_config();

        buffer = wrap_text(); // Always refresh logic buffer before redrawing
        if (sc) close_active_menu(buffer); else refresh();
        };

    /* --- DEBOUNCE STATE VARIABLES --- */
    bool pending_resize = false;

    while (running) {
        /* FIX: Use a standard debounce timer. Mouse drag events are hardware-pushed by Windows. 
           Polling at 0ms causes an infinite CPU-locking loop! */
        DWORD wait_result = WaitForSingleObject(hStdin, pending_resize ? 50 : 100);
        
        if (wait_result == WAIT_TIMEOUT) {
            if (pending_resize) {
                pending_resize = false;
                
                HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                GetConsoleScreenBufferInfo(hOut, &csbi);
            
            /* Measure the actual visual window viewport, NOT the underlying scrollback buffer! */
            int new_cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            int new_rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

                if (new_cols < 40 || new_rows < 25) {
                    if (!screen_too_small) { 
                        screen_too_small = true; clear_screen_safe(); 
                        SetConsoleTextAttribute(hOut, get_default_attr()); 
                        gotoxy(0, 0); cout << "Terminal size is too small!\nPlease resize the window to \natleast 40x25 to continue..\n"; 
                    }
                }
                else {
                    /* FIX: The Ultimate Win32 Scrollbar Destroyer (With Shock-Absorber) */
                    COORD newSize = { (SHORT)new_cols, (SHORT)new_rows }; 
                    SMALL_RECT newRect = { 0, 0, (SHORT)(new_cols - 1), (SHORT)(new_rows - 1) };
                    
                    /* Safely lock buffer. Shrink window first, then buffer, or vice versa */
                    if (newSize.X > csbi.dwSize.X || newSize.Y > csbi.dwSize.Y) {
                        SetConsoleScreenBufferSize(hOut, newSize);
                        SetConsoleWindowInfo(hOut, TRUE, &newRect);
                    } else {
                        SetConsoleWindowInfo(hOut, TRUE, &newRect);
                        SetConsoleScreenBufferSize(hOut, newSize);
                    }

                    SCREEN_COLS = new_cols; SCREEN_ROWS = new_rows;
                    /* FIX 1: Stop fighting the OS! Keep our base configuration synced with the dragged size. */
                    win_x_size = new_cols; win_y_size = new_rows; 
                    MAX_ROWS = show_find_dialog ? SCREEN_ROWS - 13 : SCREEN_ROWS - 6; 
                    MAX_COLS = v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2;
                    screen_too_small = false; 
                    
                    DWORD m = ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT; 
                    if (use_mouse) { m |= ENABLE_MOUSE_INPUT; m &= ~ENABLE_QUICK_EDIT_MODE; } 
                    SetConsoleMode(hStdin, m);
                    
                    DWORD outMode = 0;
                    if (GetConsoleMode(hOut, &outMode)) {
                        outMode &= ~ENABLE_WRAP_AT_EOL_OUTPUT;
                        SetConsoleMode(hOut, outMode);
                    }

                    /* FIX 2: No more screen wipes! No more snapping back to 80x25! */
                    /* Overpaint the new borders (false = no black flash), then paint the text! */
                    init_app(argv0_global, false);
                    buffer = wrap_text();
                    needs_ui_redraw = true;
                }
            }
            continue; /* Skip the rest of the loop since we just handled a timeout */
        }

        if (!ReadConsoleInput(hStdin, irInBuf, 128, &cNumRead)) break;
        
        for (DWORD i = 0; i < cNumRead; i++) {
            if (irInBuf[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
                if (GetTickCount() - app_start_time < 500) continue;
                pending_resize = true; /* Just flag it! The timeout will handle the math safely. */
            }
            else if (irInBuf[i].EventType == KEY_EVENT && !screen_too_small && irInBuf[i].Event.KeyEvent.bKeyDown) {
                WORD key = irInBuf[i].Event.KeyEvent.wVirtualKeyCode; DWORD ctrl = irInBuf[i].Event.KeyEvent.dwControlKeyState;
                bool isCtrlPressed = (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0, isShift = (ctrl & SHIFT_PRESSED) != 0, isAlt = (ctrl & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

                if (show_exit_dialog) {
                    if (key == VK_ESCAPE) { 
                        show_exit_dialog = false; 
                        pending_action = active_tab_element = 0; 
                        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), get_default_attr()); 
                        init_app(argv0_global, true); refresh(); 
                    }
                    else if (key == VK_TAB) {
                        active_tab_element = isShift ? (active_tab_element <= 1 ? 3 : active_tab_element - 1) : (active_tab_element >= 3 ? 1 : active_tab_element + 1);
                        current_hover_element = 79 + active_tab_element; // 1->80, 2->81, 3->82
                        refresh();
                    }
                    else if (key == VK_RETURN && active_tab_element > 0) {
                        if (active_tab_element == 1) { if (current_filename == "Untitled.txt") { show_exit_dialog = false; executeMenuAction(12); } else { save_document(current_browser_path + current_filename); show_exit_dialog = false; execPending(); } }
                        else if (active_tab_element == 2) { show_exit_dialog = false; execPending(); }
                        else if (active_tab_element == 3) { show_exit_dialog = false; pending_action = current_hover_element = active_tab_element = 0; SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), get_default_attr()); init_app(argv0_global, true); refresh(); }
                    }
                    continue;
                }

                /* FIX: Global Menu Shortcuts (Alt+F, Alt+E, Alt+V). 
                   Placed at the absolute top of the loop so textboxes don't swallow them! */
                if (isAlt && !show_settings && !show_open_dialog && !show_save_dialog) {
                    if (key == 'F') { active_menu = 1; current_hover_element = 10; is_context_menu = false; init_app(argv0_global, false); if (show_find_dialog) draw_find_dialog(); refresh(); continue; }
                    else if (key == 'E') { active_menu = 2; current_hover_element = 24; is_context_menu = false; init_app(argv0_global, false); if (show_find_dialog) draw_find_dialog(); refresh(); continue; }
                    else if (key == 'V') { active_menu = 3; current_hover_element = 30; is_context_menu = false; init_app(argv0_global, false); if (show_find_dialog) draw_find_dialog(); refresh(); continue; }
                }

                /* FIX: Native Keyboard Typing and Tab Switching for Find & Replace! */
                /* FIX: Added && active_menu == 0 to transfer keyboard focus to the menu when it opens! */
                if (show_find_dialog && active_input_field >= 4 && active_menu == 0) {
                    /* FIX: Whitelist Ctrl+F so it escapes the typing trap and closes the dialog! */
                    if (key == VK_ESCAPE || (isCtrlPressed && key == 'F')) { 
                        show_find_dialog = false; active_input_field = 0; 
                        MAX_ROWS = SCREEN_ROWS - 6; text_start_y = 3; /* FIX: Restore viewport! */
                        init_app(argv0_global, false); refresh_follow_cursor(); 
                    }
                    else if (key == VK_TAB) { 
                        active_input_field = (active_input_field == 4) ? 5 : 4;
                        refresh();
                    }
                    else if (key == VK_RETURN) { 
                        if (active_input_field == 4) execute_find_replace(1);
                        else if (active_input_field == 5) execute_find_replace(2);
                    }
                    /* FIX: Add F3 shortcut to trigger 'Find Next' directly from the textboxes! */
                    else if (key == VK_F3) {
                        execute_find_replace(1);
                        refresh_follow_cursor();
                    }
                    else {
                        int* cur = (active_input_field == 4) ? &find_cursor : &replace_cursor;
                        string* txt = (active_input_field == 4) ? &find_text : &replace_text;
                        
                        if (key == VK_LEFT && *cur > 0) (*cur)--; 
                        else if (key == VK_RIGHT && *cur < (int)txt->length()) (*cur)++; 
                        else if (key == VK_BACK && *cur > 0) { 
                            txt->erase(*cur - 1, 1); (*cur)--;
                        }
                        else if (key == VK_DELETE && *cur < (int)txt->length()) { 
                            txt->erase(*cur, 1);
                        }
                        else if (!isCtrlPressed) {
                            char c = irInBuf[i].Event.KeyEvent.uChar.AsciiChar;
                            if (c >= 32 && c <= 126 && txt->length() < 127) {
                                txt->insert(*cur, 1, c);
                                (*cur)++;
                            }
                        }
                        refresh_follow_cursor(); /* FIX: Force viewport snap when hitting enter to find! */
                    }
                    continue; 
                }

                if (show_settings) {
                    if (key == VK_ESCAPE) { 
                        show_settings = false; 
                        active_tab_element = current_hover_element = 0; 
                        init_app(argv0_global, true); 
                        refresh(); 
                    }
                    else if (key == VK_TAB) {
                        int t = active_tab_element;
                        if (!isShift) {
                            if (t == 0) t = 40; else if (t >= 40 && t < 43) t++; else if (t == 43) t = (current_settings_tab == 1 ? 44 : (current_settings_tab == 2 ? 50 : 40));
                            else if (t >= 44 && t < 46) t++; else if (t == 46) t = 47; else if (t == 47) t = 48; else if (t == 48) t = 49; else if (t == 49) t = 100;
                            else if (t >= 50 && t < 51) t++; else if (t == 51) t = 40;
                            else if (t == 100) t = 101; else if (t == 101) t = setting_color ? 102 : 110; else if (t >= 102 && t < 104) t++;
                            else if (t == 104) t = 110; else if (t == 110) t = 111; else if (t == 111) t = setting_color ? 112 : 120;
                            else if (t >= 112 && t < 114) t++; else if (t == 114) t = setting_color ? 120 : 40;
                            else if (t == 120) t = 121; else if (t >= 121 && t < 124) t++; else if (t == 124) t = 130;
                            else if (t == 130) t = 131; else if (t >= 131 && t < 134) t++; else if (t == 134) t = 40;
                        }
                        else t = 40; // Simplified Shift-Tab loop
                        active_tab_element = current_hover_element = t;
                        refresh();
                    }
                    else if (key == VK_RETURN && active_tab_element > 0) {
                        int t = active_tab_element;
                        if (t >= 40 && t <= 42) { current_settings_tab = t - 40; draw_settings(); }
                        else if (t == 43) { show_settings = false; active_tab_element = current_hover_element = 0; init_app(argv0_global, true); refresh(); }
                        else if (current_settings_tab == 1) {
                            if (t >= 44 && t <= 46) { setting_hover_effects = t - 44; save_config(); refresh(); }
                            else if (t == 47) { setting_color = !setting_color; if (!setting_color) { if (setting_bg_color != 0 && setting_bg_color != 7)setting_bg_color = 0; if (setting_fg_color != 0 && setting_fg_color != 7)setting_fg_color = 7; if (setting_bg_color == setting_fg_color)setting_fg_color = (setting_bg_color == 7 ? 0 : 7); } save_config(); init_app(argv0_global, true); refresh(); }
                            else if (t == 48 || t == 49) { setting_ansi_mode = t - 48; save_config(); refresh(); }
                            else if (t >= 100) {
                                int r = 18 + (t - 100) / 10, ci = t % 10, * tgt = (r == 18) ? &setting_bg_color : ((r == 19) ? &setting_fg_color : ((r == 20) ? &setting_sel_color : &setting_crit_color));
                                if (ci == 0)*tgt = 0; else if (ci == 1)*tgt = 7; else if (setting_color) { int m = (ci == 2) ? 1 : ((ci == 3) ? 2 : 4); if (*tgt == 0 || *tgt == 7)*tgt = m; else *tgt ^= m; }
                                if (setting_bg_color == setting_fg_color) { if (r == 18)setting_fg_color = setting_bg_color == 7 ? 0 : 7; else if (r == 19)setting_bg_color = setting_fg_color == 7 ? 0 : 7; }
                                save_config(); init_app(argv0_global, true); refresh();
                            }
                        }
                        else if (current_settings_tab == 2) {
                            if (t == 50) { setting_internal_clipboard = true; save_config(); refresh(); }
                            else if (t == 51) { setting_internal_clipboard = false; save_config(); refresh(); }
                        }
                    }
                    continue;
                }

                if (show_open_dialog || show_save_dialog) {
                    if (key == VK_ESCAPE) closeDialog(false);
                    else if (key == VK_TAB) {
                        active_tab_element = isShift ? (active_tab_element <= 1 ? 10 : active_tab_element - 1) : (active_tab_element >= 10 ? 1 : active_tab_element + 1);
                        active_input_field = (active_tab_element <= 3) ? active_tab_element : 0;
                        browser_selected_index = (active_tab_element == 7) ? browser_scroll_offset : -1;
                        int mapping[] = { 0, 0, 0, 0, 70, 71, 72, 0, 5, 6, 4 };
                        current_hover_element = mapping[active_tab_element];
                        refresh();
                    }
                    /* FIX: Global File Browser Arrow Navigation! Also dynamically updates the File Name box! */
                    else if (key == VK_UP || key == VK_DOWN) {
                        if (browser_files.size() > 0) {
                            if (browser_selected_index < 0) browser_selected_index = 0;
                            else if (key == VK_UP && browser_selected_index > 0) browser_selected_index--;
                            else if (key == VK_DOWN && browser_selected_index < (int)browser_files.size() - 1) browser_selected_index++;
                            
                            /* Dynamically adapt the scroll camera to the current window size */
                            int max_items = SCREEN_ROWS - 16;
                            if (browser_selected_index < browser_scroll_offset) browser_scroll_offset = browser_selected_index;
                            else if (browser_selected_index >= browser_scroll_offset + max_items) browser_scroll_offset = browser_selected_index - max_items + 1;
                            
                            /* Auto-fill the text box with the new selection */
                            if (!browser_files[browser_selected_index].is_dir) {
                                dialog_input_text = browser_files[browser_selected_index].name;
                                dialog_input_cursor = (int)dialog_input_text.length();
                                active_input_field = 1;
                            }
                            refresh();
                        }
                    }
                    else if (active_input_field > 0) {
                        if (active_input_field != 3) dialog_status_msg = show_save_dialog ? "Ready to save file..." : "Ready to select file...";
                        string* txt = active_input_field == 1 ? &dialog_input_text : (active_input_field == 2 ? &dialog_search_text : &dialog_address_text);
                        int* cur = active_input_field == 1 ? &dialog_input_cursor : (active_input_field == 2 ? &dialog_search_cursor : &dialog_address_cursor);
                        if (key == VK_LEFT) { if (*cur > 0) (*cur)--; refresh(); }
                        else if (key == VK_RIGHT) { if (*cur < (int)txt->length()) (*cur)++; refresh(); }
                        else if (key == VK_BACK) { if (*cur > 0) { txt->erase(*cur - 1, 1); (*cur)--; if (active_input_field == 2) filter_browser_files(); refresh(); } }
                        else if (key == VK_DELETE) { if (*cur < (int)txt->length()) { txt->erase(*cur, 1); if (active_input_field == 2) filter_browser_files(); refresh(); } }
                        else if (key == VK_RETURN) {
                            if (active_input_field == 1) closeDialog(true);
                            else if (active_input_field == 3) { DWORD a = GetFileAttributesA(dialog_address_text.c_str()); if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { load_directory(dialog_address_text, true); active_input_field = 0; dialog_status_msg = show_save_dialog ? "Ready to save file..." : "Ready to select file..."; } else dialog_status_msg = "Path does not exist"; refresh(); }
                        }
                        /* FIX: Expanded text limits to match DOS version (Address/Filename = 255, Search = 127)! */
                        else { 
                            char c = irInBuf[i].Event.KeyEvent.uChar.AsciiChar; 
                            int max_len = (active_input_field == 3) ? 255 : ((active_input_field == 2) ? 127 : 255);
                            if (c >= 32 && c <= 126 && (int)txt->length() < max_len) { 
                                txt->insert(*cur, 1, c); 
                                (*cur)++; 
                                if (active_input_field == 2) filter_browser_files(); 
                                refresh(); 
                            } 
                        }
                    }
                    else if (active_tab_element == 7) {
                        /* FIX: Arrow keys moved to global scope! Only handle Enter here. */
                        if (key == VK_RETURN && browser_selected_index >= 0) {
                            if (browser_files[browser_selected_index].is_dir) {
                                string np = current_browser_path;
                                if (browser_files[browser_selected_index].name == "..") { if (np.length() > 3) { np.pop_back(); size_t p = np.find_last_of("\\/"); if (p != string::npos) np = np.substr(0, p + 1); } }
                                else np += browser_files[browser_selected_index].name;
                                load_directory(np, true);
                            }
                            else {
                                dialog_input_text = browser_files[browser_selected_index].name;
                                dialog_input_cursor = (int)dialog_input_text.length();
                                active_input_field = active_tab_element = 1;
                            }
                            refresh();
                        }
                    }
                    else if (key == VK_RETURN && active_tab_element > 0) {
                        int t = active_tab_element;
                        if (t == 4 && path_history_index > 0) load_directory(path_history[--path_history_index], false);
                        else if (t == 5 && path_history_index < (int)path_history.size() - 1) load_directory(path_history[++path_history_index], false);
                        else if (t == 6) { string p = current_browser_path; if (p.length() > 3) { p.pop_back(); size_t pos = p.find_last_of("\\/"); if (pos != string::npos) load_directory(p.substr(0, pos + 1), true); } }
                        else if (t == 8) closeDialog(true);
                        else if (t == 9 || t == 10) closeDialog(false);
                        refresh();
                    }
                    continue;
                }

                if (active_menu != 0) {
                    /* FIX: Allow VK_APPS to act as an OFF toggle while the menu is actively open! */
                    if (key == VK_ESCAPE || key == VK_APPS) { 
                        close_active_menu(buffer); 
                        continue; 
                    }
                    /* FIX: Add '28' (Find) to e_id! */
                    int f_arr[] = { 10,11,12,13,14,15,16 }; vector<int> f_id(f_arr, f_arr + 7);
					int e_arr[] = { 24,25,20,21,22,23,26,28,27 }; vector<int> e_id(e_arr, e_arr + 9);
					int v_arr[] = { 30,31,32,33 }; vector<int> v_id(v_arr, v_arr + 4);
					int c_arr[] = { 20,21,22,23,26,27 }; vector<int> c_id(c_arr, c_arr + 6);
                    vector<int>* a_id = active_menu == 1 ? &f_id : (active_menu == 2 ? &e_id : (active_menu == 3 ? &v_id : &c_id));
					if (key == VK_DOWN || key == VK_UP) {
						vector<int>::iterator it = find(a_id->begin(), a_id->end(), current_hover_element); 
						int idx = it != a_id->end() ? (int)distance(a_id->begin(), it) : 0;
                        idx = key == VK_DOWN ? (idx + 1) % (int)a_id->size() : (idx - 1 + (int)a_id->size()) % (int)a_id->size();
                        current_hover_element = (*a_id)[idx];
                        
                        /* FIX: Keyboard Flicker Optimization! Repaint ONLY the active menu dropdown block. 
                           Bypasses full canvas/Find dialog refreshes so it stays completely solid! */
                        if (active_menu == 1) draw_file_menu();
                        else if (active_menu == 2) draw_edit_menu(6, 2, false);
                        else if (active_menu == 3) draw_view_menu();
                        else if (active_menu == 4) draw_context_menu(context_x, context_y);
                    }
                    else if (key == VK_LEFT || key == VK_RIGHT) {
                        if (active_menu == 4) close_active_menu(buffer); /* Context menu cleanly closes on left/right instead of sliding to Top Bar */
                        else {
                            active_menu = key == VK_RIGHT ? (active_menu % 3) + 1 : (active_menu == 1 ? 3 : active_menu - 1);
                            current_hover_element = active_menu == 1 ? 10 : (active_menu == 2 ? 24 : 30);
                            /* FIX: Redraw the base UI skeleton so the old menu's T-junction borders get erased! */
                            init_app(argv0_global, false); 
                            /* FIX: Repaint the Find/Replace dialog immediately to wipe out the old menu's ghost! */
                            if (show_find_dialog) draw_find_dialog();
                            refresh();
                        }
                    }
                    else if (key == VK_RETURN) { if (current_hover_element >= 10 && current_hover_element <= 33) executeMenuAction(current_hover_element); }
                    else if (active_menu == 1) { if (key == 'N') executeMenuAction(10); else if (key == 'O') executeMenuAction(11); else if (key == 'S') executeMenuAction(12); else if (key == 'A') executeMenuAction(13); else if (key == 'C') executeMenuAction(14); else if (key == 'X') executeMenuAction(16); }
                    else if (active_menu == 2) { if (key == 'U') executeMenuAction(24); else if (key == 'R') executeMenuAction(25); else if (key == 'T') executeMenuAction(20); else if (key == 'C') executeMenuAction(21); else if (key == 'P') executeMenuAction(22); else if (key == 'D') executeMenuAction(23); else if (key == 'A') executeMenuAction(26); }
                    else if (active_menu == 3) { if (key == 'H') executeMenuAction(30); else if (key == 'V') executeMenuAction(31); else if (key == 'W') executeMenuAction(32); else if (key == 'R') executeMenuAction(33); }
                    continue;
                }

                if (key == VK_ESCAPE) {
                    if (is_document_dirty) { show_exit_dialog = true; pending_action = 1; SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), get_dim_attr()); init_app(argv0_global, true); refresh(); }
                    else running = false;
                }
                else if (!show_open_dialog && !show_save_dialog && !show_settings) {
                    if (isCtrlPressed && key == 'Z') { perform_undo(); refresh_follow_cursor(); }
                    else if (isCtrlPressed && key == 'Y') { perform_redo(); refresh_follow_cursor(); }
                    else if (isCtrlPressed && key == 'A') { sel_start_log_x = sel_start_log_y = 0; cursor_log_y = stream_lines.empty() ? 0 : (int)stream_lines.size() - 1; cursor_log_x = get_stream_logical_len(cursor_log_y); has_selection = true; last_was_typing = false; refresh_follow_cursor(); }
                    else if (isCtrlPressed && key == 'C') { if (has_selection) set_clipboard(get_selected_text()); }
                    else if (isCtrlPressed && key == 'X') { if (has_selection) { if (setting_read_only) continue; save_state(); last_was_typing = false; set_clipboard(get_selected_text()); delete_selection(); refresh_follow_cursor(); } }
                    else if (isCtrlPressed && key == 'V') { if (setting_read_only) continue; save_state(); last_was_typing = false; insert_text(get_clipboard()); refresh_follow_cursor(); }
                    else if (isCtrlPressed && key == 'F') { 
                        show_find_dialog = !show_find_dialog; 
                        MAX_ROWS = show_find_dialog ? SCREEN_ROWS - 13 : SCREEN_ROWS - 6; /* FIX: Dynamically shrink viewport height! */
                        text_start_y = show_find_dialog ? 10 : 3; /* FIX: Push text below dialog! */
                        if (show_find_dialog) { active_input_field = 4; if (has_selection) { find_text = get_selected_text(); find_cursor = find_text.length(); } }
                        else active_input_field = 0; 
                        init_app(argv0_global, false); /* FIX: Force border redraw to wipe out the old T-junctions! */
                        refresh_follow_cursor(); 
                    }
                    /* FIX: Dynamic F3 shortcut! If open -> Find Next. If closed -> Open Dialog! */
                    else if (key == VK_F3) { 
                        if (show_find_dialog) {
                            if (!find_text.empty()) {
                                execute_find_replace(1);
                                refresh_follow_cursor();
                            }
                        } else {
                            show_find_dialog = true; 
                            MAX_ROWS = SCREEN_ROWS - 13; text_start_y = 10; 
                            active_input_field = 4; if (has_selection) { find_text = get_selected_text(); find_cursor = find_text.length(); }
                            init_app(argv0_global, false); 
                            refresh_follow_cursor(); 
                        }
                    }
                    else if (isCtrlPressed && key == 'N') { executeMenuAction(10); refresh(); }
                    /* FIX: Bind 'Context Menu' key (VK_APPS) to open our Edit context menu! */
                    else if (key == VK_APPS) {
                        /* FIX: Act as a toggle switch! Close if already open, otherwise open. */
                        if (active_menu == 4) {
                            close_active_menu(buffer);
                        } else {
                            active_menu = 4;
                            /* FIX: Clamps the menu coordinates so it never clips off the right or bottom edges! */
                            context_x = min(cursor_screen_x + 1, SCREEN_COLS - 17);
                            context_y = min(cursor_screen_y + text_start_y, SCREEN_ROWS - 10); /* FIX: Expanded height for Read Only */
                            current_hover_element = 20; /* FIX: Auto-select the first item (Cut) so arrow keys work instantly! */
                        }
                        refresh();
                    }
                    else if (isCtrlPressed && key == 'O') { executeMenuAction(11); refresh(); }
                    else if (isCtrlPressed && key == 'S') { if (current_filename == "Untitled.txt") executeMenuAction(12); else { save_document(current_browser_path + current_filename); refresh(); } }
                    else if (key == VK_NEXT || key == VK_PRIOR) { last_was_typing = false; if (key == VK_NEXT) scroll_y = min(max(0, total_visual_lines - MAX_ROWS), scroll_y + MAX_ROWS - 1); else if (key == VK_PRIOR) scroll_y = max(0, scroll_y - MAX_ROWS + 1); refresh(); }
                    else if (key == VK_HOME || key == VK_END) { last_was_typing = false; if (!word_warp) { int mo = max(0, max_line_length - MAX_COLS); if (key == VK_HOME) h_scroll_offset = isCtrlPressed ? max(0, h_scroll_offset - 15) : 0; else h_scroll_offset = isCtrlPressed ? min(mo, h_scroll_offset + 15) : mo; refresh(); } }
                    else if (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT) {
                        last_was_typing = false; 
                        if (!isShift) has_selection = is_dragging = false; 
                        else if (!has_selection) { sel_start_log_x = cursor_log_x; sel_start_log_y = cursor_log_y; has_selection = true; }

                        if (key == VK_UP) {
                            /* FIX: cursor_screen_y is ALREADY the absolute visual index! Do not add scroll_y! */
                            int current_vy = cursor_screen_y;
                            if (current_vy > 0 && current_vy <= (int)buffer_log_map.size()) {
                                current_vy--;
                                cursor_log_y = buffer_log_map[current_vy].first;
                                cursor_log_x = buffer_log_map[current_vy].second + min(cursor_screen_x, (int)buffer[current_vy].length());
                            } else { cursor_log_x = 0; cursor_log_y = 0; }
                        }
                        else if (key == VK_DOWN) {
                            /* FIX: cursor_screen_y is ALREADY the absolute visual index! Do not add scroll_y! */
                            int current_vy = cursor_screen_y;
                            if (current_vy >= 0 && current_vy < (int)buffer_log_map.size() - 1) {
                                current_vy++;
                                cursor_log_y = buffer_log_map[current_vy].first;
                                cursor_log_x = buffer_log_map[current_vy].second + min(cursor_screen_x, (int)buffer[current_vy].length());
                            } else {
                                cursor_log_y = stream_lines.empty() ? 0 : stream_lines.size() - 1;
                                cursor_log_x = get_stream_logical_len(cursor_log_y);
                            }
                        }
                        else if (key == VK_LEFT) { 
                            if (cursor_log_x > 0) cursor_log_x--; 
                            else if (cursor_log_y > 0) { cursor_log_y--; cursor_log_x = get_stream_logical_len(cursor_log_y); } 
                            if (!word_warp) h_scroll_offset = cursor_log_x < h_scroll_offset ? cursor_log_x : (cursor_log_x >= h_scroll_offset + MAX_COLS ? cursor_log_x - MAX_COLS + 1 : h_scroll_offset); 
                        }
                        else if (key == VK_RIGHT) { 
                            if (cursor_log_x < get_stream_logical_len(cursor_log_y)) cursor_log_x++; 
                            else if (cursor_log_y < (int)stream_lines.size() - 1) { cursor_log_y++; cursor_log_x = 0; } 
                            if (!word_warp) h_scroll_offset = cursor_log_x < h_scroll_offset ? cursor_log_x : (cursor_log_x >= h_scroll_offset + MAX_COLS ? cursor_log_x - MAX_COLS + 1 : h_scroll_offset); 
                        }
                        refresh_follow_cursor();
                    }
                    else if (key == VK_RETURN) { 
                        if (setting_read_only) continue;
                        save_state(); last_was_typing = false; if (has_selection) delete_selection(); 
                        long target = get_global_offset(cursor_log_x, cursor_log_y);
                        insert_into_stream('\n', target); rebuild_stream_lines();
                        cursor_log_y++; cursor_log_x = 0; if (!word_warp) h_scroll_offset = 0; 
                        is_document_dirty = true; refresh_follow_cursor(); 
                    }
                    else if (key == VK_BACK) {
                        if (setting_read_only) continue;
                        save_state(); last_was_typing = false; if (has_selection) delete_selection();
                        else {
                            long target = get_global_offset(cursor_log_x, cursor_log_y);
                            if (target > 0) {
                                delete_from_stream(target - 1); rebuild_stream_lines();
                                if (cursor_log_x > 0) cursor_log_x--;
                                else { cursor_log_y--; cursor_log_x = get_stream_logical_len(cursor_log_y); }
                            }
                        }
                        if (!word_warp && cursor_log_x < h_scroll_offset) h_scroll_offset = cursor_log_x; is_document_dirty = true; refresh_follow_cursor();
                    }
                    else if (key == VK_DELETE) {
                        if (setting_read_only) continue;
                        save_state(); last_was_typing = false; if (has_selection) delete_selection();
                        else {
                            long target = get_global_offset(cursor_log_x, cursor_log_y);
							long total_len = 0; for (size_t i = 0; i < piece_table.size(); i++) total_len += piece_table[i].length;
                            if (target < total_len) { delete_from_stream(target); rebuild_stream_lines(); }
                        }
                        is_document_dirty = true; refresh_follow_cursor();
                    }
                    else if (!isCtrlPressed) {
                        char c = irInBuf[i].Event.KeyEvent.uChar.AsciiChar;
                        if (c >= 32 && c <= 126) {
                            if (setting_read_only) continue;
                            if (!last_was_typing || c == ' ') save_state(); last_was_typing = true;
                            if (has_selection) delete_selection();
                            long target = get_global_offset(cursor_log_x, cursor_log_y);
                            insert_into_stream(c, target); rebuild_stream_lines();
                            cursor_log_x++;
                            if (!word_warp && cursor_log_x >= h_scroll_offset + MAX_COLS) h_scroll_offset = cursor_log_x - MAX_COLS + 1;
                            is_document_dirty = true; refresh_follow_cursor();
                        }
                    }
                }
            }
            else if (irInBuf[i].EventType == MOUSE_EVENT && use_mouse && !screen_too_small) {
                MOUSE_EVENT_RECORD mer = irInBuf[i].Event.MouseEvent; 
                int mx = mer.dwMousePosition.X, my = mer.dwMousePosition.Y;
                static int lmx = -1, lmy = -1; static DWORD lbtn = 0;
                if (mer.dwEventFlags == MOUSE_MOVED) { if (mx == lmx && my == lmy && mer.dwButtonState == lbtn) continue; lmx = mx; lmy = my; lbtn = mer.dwButtonState; }

                if (show_exit_dialog) {
                    bool narrow = SCREEN_COLS < 80;
                    int bx = narrow ? max(0, (SCREEN_COLS - 38) / 2) : max(0, (SCREEN_COLS - 44) / 2);
                    int by = max(0, (SCREEN_ROWS - 10) / 2);

                    if (mer.dwEventFlags == MOUSE_MOVED && setting_hover_effects == 2) {
                        int nh = 0;
                        if (my == by + 7) {
                            if (narrow) {
                                if (mx >= bx + 2 && mx <= bx + 9) nh = 80;
                                else if (mx >= bx + 12 && mx <= bx + 25) nh = 81;
                                else if (mx >= bx + 28 && mx <= bx + 35) nh = 82;
                            } else {
                                if (mx >= bx + 3 && mx <= bx + 10) nh = 80;
                                else if (mx >= bx + 15 && mx <= bx + 28) nh = 81;
                                else if (mx >= bx + 33 && mx <= bx + 40) nh = 82;
                            }
                        }
                        if (nh != current_hover_element) { if (current_hover_element != 0) render_hover_element(current_hover_element, false); if (nh != 0) render_hover_element(nh, true); current_hover_element = nh; }
                    }
                    else if (mer.dwButtonState == FROM_LEFT_1ST_BUTTON_PRESSED && my == by + 7) {
                        bool click_save = narrow ? (mx >= bx + 2 && mx <= bx + 9) : (mx >= bx + 3 && mx <= bx + 10);
                        bool click_dont = narrow ? (mx >= bx + 12 && mx <= bx + 25) : (mx >= bx + 15 && mx <= bx + 28);
                        bool click_cancel = narrow ? (mx >= bx + 28 && mx <= bx + 35) : (mx >= bx + 33 && mx <= bx + 40);

                        if (active_tab_element != 0) active_tab_element = 0;
                        
                        if (click_save) { if (current_filename == "Untitled.txt") { show_exit_dialog = false; executeMenuAction(12); } else { save_document(current_browser_path + current_filename); show_exit_dialog = false; execPending(); } }
                        else if (click_dont) { show_exit_dialog = false; execPending(); }
                        else if (click_cancel) { show_exit_dialog = false; pending_action = current_hover_element = 0; SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), get_default_attr()); init_app(argv0_global, true); refresh(); }
                    }
                    continue;
                }

                if (mer.dwEventFlags == MOUSE_MOVED) {
                    bool lbd = (mer.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
                    if (!lbd) is_dragging = is_dragging_vscroll = is_dragging_hscroll = is_dragging_browser_scroll = false;

                    if (lbd && is_dragging_vscroll) {
                        int track_h = SCREEN_ROWS - 10;
                        int cy = max(5, min(my, 5 + track_h - 1));
                        
                        if (setting_disk_stream && stream_lines.size() > CHUNK_LOG_SIZE) {
                            long new_log_y = (long)round((float)(cy - 5) / (track_h - 1) * (stream_lines.size() - 1));
                            if (new_log_y != cursor_log_y) {
                                cursor_log_y = max(0L, min((long)stream_lines.size() - 1, new_log_y));
                                cursor_log_x = 0; buffer = wrap_text(); needs_ui_redraw = true;
                            }
                        } else if (total_visual_lines > MAX_ROWS) {
                            int nsy = (int)round((float)(cy - 5) / (track_h - 1) * (total_visual_lines - MAX_ROWS));
                            if (nsy != scroll_y) { scroll_y = nsy; needs_ui_redraw = true; }
                        }
                        continue;
                    }
                    if (lbd && is_dragging_hscroll && max_line_length > MAX_COLS) { int cx = max(3, min(mx, 74)); int no = (int)round((float)(cx - 3) / 71.0f * (max_line_length - MAX_COLS)); if (no != h_scroll_offset) { h_scroll_offset = no; needs_ui_redraw = true; } continue; }
                    if (lbd && is_dragging_browser_scroll && (int)browser_files.size() > SCREEN_ROWS - 16) { int cy = max(9, min(my, SCREEN_ROWS - 9)); int no = (int)round((float)(cy - 9) / (float)(max(1, SCREEN_ROWS - 18)) * ((int)browser_files.size() - (SCREEN_ROWS - 16))); if (no != browser_scroll_offset) { browser_scroll_offset = no; needs_ui_redraw = true; } continue; }

                    if (!show_settings && !show_open_dialog && !show_save_dialog && active_menu == 0) { bool wh = is_hovering_hbar; is_hovering_hbar = (h_scrollBar && my == SCREEN_ROWS - 2 && mx >= 1 && mx <= (v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2)); if (wh != is_hovering_hbar) needs_ui_redraw = true; }
                    blink_x = mx; blink_y = my;

                    if (setting_hover_effects == 2) {
                        int nh = 0;
                        if (my == 1 && mx >= SCREEN_COLS - 4 && mx <= SCREEN_COLS - 2) nh = 4;
                        /* FIX: Layer Priority 1 - Menus! */
                        else if (active_menu == 1 && mx >= 1 && mx <= 14) {
                            if (my == 3) nh = 10; else if (my == 4) nh = 11; else if (my == 5) nh = 12; else if (my == 6) nh = 13; else if (my == 8) nh = 14; else if (my == 10) nh = 15; else if (my == 12) nh = 16;
                        }
                        /* FIX: Added hover map for Find/Replace (28) */
                        else if (active_menu == 2 && mx >= 7 && mx <= 20) {
                            if (my == 3) nh = 24; else if (my == 4) nh = 25; else if (my == 6) nh = 20; else if (my == 7) nh = 21; else if (my == 8) nh = 22; else if (my == 9) nh = 23; else if (my == 11) nh = 26; else if (my == 12) nh = 28; else if (my == 13) nh = 27;
                        }
                        else if (active_menu == 4 && mx >= context_x + 1 && mx <= context_x + 14) {
                            if (my == context_y + 1) nh = 20; else if (my == context_y + 2) nh = 21; else if (my == context_y + 3) nh = 22; else if (my == context_y + 4) nh = 23; else if (my == context_y + 6) nh = 26; else if (my == context_y + 7) nh = 27;
                        }
                        else if (active_menu == 3 && mx >= 14 && mx <= 33) {
                            if (my == 3 || my == 4) nh = 30; else if (my == 6 || my == 7) nh = 31; else if (my == 9) nh = 32; else if (my == 11) nh = 33;
                        }
                        /* FIX: Layer Priority 2 - Find Dialog! */
                        else if (show_find_dialog && my >= 3 && my <= 8) {
                            if (SCREEN_COLS < 80) {
                                /* FIX: Shifted narrow hover hitboxes +1 to the right to perfectly align with the new walls! */
                                if      (my == 4 && mx >= 20 && mx <= 26) nh = 90;
                                else if (my == 4 && mx >= 28 && mx <= 34) nh = 91;
                                else if (my == 7 && mx >= 20 && mx <= 26) nh = 92;
                                else if (my == 7 && mx >= 28 && mx <= 34) nh = 93;
                            } else {
                                if      (my == 4 && mx >= 53 && mx <= 63) nh = 90;
                                else if (my == 4 && mx >= 65 && mx <= 75) nh = 91;
                                else if (my == 7 && mx >= 53 && mx <= 63) nh = 92;
                                else if (my == 7 && mx >= 65 && mx <= 75) nh = 93;
                            }
                        }
                        /* FIX: Layer Priority 3 - Text Canvas & Base UI! */
                        else if (!show_settings && !show_open_dialog && !show_save_dialog && active_menu == 0 && (!show_find_dialog || my < 3 || my > 9)) {
                            if (my == 1) { if (mx >= 1 && mx <= 6) nh = 1; else if (mx >= 8 && mx <= 13) nh = 2; else if (mx >= 15 && mx <= 20) nh = 3; }
                            else if (mx == SCREEN_COLS - 2 && my == SCREEN_ROWS - 2) nh = 60; 
                            else if (v_scrollBar && mx == SCREEN_COLS - 2 && my == 3) nh = 61; 
                            else if (v_scrollBar && mx == SCREEN_COLS - 2 && my == SCREEN_ROWS - 4) nh = 62;
                            else if (h_scrollBar && mx == 1 && my == SCREEN_ROWS - 2) nh = 63; 
                            else if (h_scrollBar && mx == (v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2) && my == SCREEN_ROWS - 2) nh = 64;
                        }
                        else if (show_open_dialog || show_save_dialog) {
                            if (my == SCREEN_ROWS - 5 && mx >= SCREEN_COLS - 18 && mx <= SCREEN_COLS - 13) nh = 5; else if (my == SCREEN_ROWS - 5 && mx >= SCREEN_COLS - 9 && mx <= SCREEN_COLS - 4) nh = 6; else if (my == 7 && mx == SCREEN_COLS - 4) nh = 65; else if (my == SCREEN_ROWS - 8 && mx == SCREEN_COLS - 4) nh = 66;
                            else if (my == 4) { if (mx >= 2 && mx <= 5) nh = 70; else if (mx >= 7 && mx <= 10) nh = 71; else if (mx >= 12 && mx <= 14) nh = 72; }
                        }
                        else if (show_settings) { //hover
                            if (my == 3) { if (mx >= 1 && mx <= 9) nh = 40; //display tab
                            else if (mx >= 11 && mx <= 20) nh = 41; //graphics tab
                            else if (mx >= 22 && mx <= 28) nh = 42; //other tab
                            else if (mx >= SCREEN_COLS - 8 && mx <= SCREEN_COLS - 2) nh = 43; //back button
                        }
                            else if (current_settings_tab == 1) {
                                if (my == 7 && mx >= 2 && mx <= 20) nh = 44; else if (my == 8 && mx >= 2 && mx <= 20) nh = 45; else if (my == 9 && mx >= 2 && mx <= 20) nh = 46;
                                else if (my == 11 && mx >= 2 && mx <= 20) nh = 47; else if (my == 14 && mx >= 2 && mx <= 30) nh = 48; else if (my == 15 && mx >= 2 && mx <= 30) nh = 49;
                                else if (my >= 18 && my <= 21) {
                                    if (!setting_color && my >= 20) {}
                                    else {
                                        int base = 100 + (my - 18) * 10;
                                        if (mx >= 27 && mx <= 35) nh = base + 0; else if (mx >= 38 && mx <= 46) nh = base + 1;
                                        else if (setting_color) {
                                            if (mx >= 49 && mx <= 56) nh = base + 2; else if (mx >= 59 && mx <= 67) nh = base + 3; else if (mx >= 70 && mx <= 76) nh = base + 4;
                                        }
                                    }
                                }
                            }
                            else if (current_settings_tab == 2) { 
                                if (my == 13 && mx >= 5 && mx <= 27) nh = 50; else if (my == 14 && mx >= 5 && mx <= 27) nh = 51; 
                                else if (my == 9 && mx >= 5 && mx <= 27) nh = 52; else if (my == 10 && mx >= 5 && mx <= 27) nh = 53; 
                            }
                        }
                        
                        /* FIX: Decouple Dialog Repaint from Hover Repaint! */
                        if (nh != current_hover_element) { 
                            /* FIX: If a menu is active, only redraw if we hit a valid menu item. This stops empty-space hover tearing! */
                            if (active_menu != 0 && nh == 0) {
                                /* Do nothing. We are hovering over empty space inside or around the menu box. */
                            } else {
                                int old_hover = current_hover_element;
                                current_hover_element = nh; 
                                
                                /* FIX: Prevent hover flicker! Only redraw the Find Dialog if no menus are floating above it! */
                                if (show_find_dialog && active_menu == 0) draw_find_dialog();
                                
                                if (active_menu == 1) draw_file_menu();
                                else if (active_menu == 2) draw_edit_menu(6, 2, false);
                                else if (active_menu == 3) draw_view_menu();
                                else if (active_menu == 4) draw_context_menu(context_x, context_y);
                                
                                if (active_menu == 0) {
                                    if (old_hover != 0 && old_hover < 90) render_hover_element(old_hover, false); 
                                    if (nh != 0 && nh < 90) render_hover_element(nh, true); 
                                }
                            }
                        }
                    }

                    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
                    /* FIX: Dynamically sync visibility based on active mode! */
                    bool tcv = ((active_input_field > 0 && setting_hover_effects != 2) || setting_hover_effects == 1);
                    static bool csi = false, ccv = false;
                    if (!csi || ccv != tcv) { CONSOLE_CURSOR_INFO cI; GetConsoleCursorInfo(hConsole, &cI); cI.bVisible = tcv ? TRUE : FALSE; SetConsoleCursorInfo(hConsole, &cI); ccv = tcv; csi = true; }
                    if (tcv) {
                        if (active_input_field == 1) {
                            int max_in = (SCREEN_COLS < 80) ? SCREEN_COLS - 35 : SCREEN_COLS - 40;
                            int in_x = (SCREEN_COLS < 80) ? 14 : 17;
                            gotoxy(in_x + min(dialog_input_cursor, max_in - 1), SCREEN_ROWS - 5);
                        } 
                        else if (active_input_field == 2) {
                            int search_len = (SCREEN_COLS < 80) ? 6 : 12;
                            int search_x = (SCREEN_COLS < 80) ? SCREEN_COLS - 9 : SCREEN_COLS - 16;
                            gotoxy(search_x + min(dialog_search_cursor, search_len - 1), 4);
                        }
                        else if (active_input_field == 3) {
                            int addr_len = (SCREEN_COLS < 80) ? SCREEN_COLS - 28 : SCREEN_COLS - 40;
                            int addr_x = (SCREEN_COLS < 80) ? 14 : 18;
                            gotoxy(addr_x + min(dialog_address_cursor, addr_len - 1), 4);
                        } 
                        else if (setting_hover_effects == 1 && active_input_field == 0 && !show_open_dialog && !show_save_dialog) gotoxy(blink_x, blink_y);
                    }

                    if (lbd && is_dragging && active_menu == 0 && !show_settings && !show_open_dialog && !show_save_dialog) {
                        int olx = cursor_log_x, oly = cursor_log_y; update_cursor_from_screen(max(1, min(mx, MAX_COLS)), my, buffer);
                        if (cursor_log_x != olx || cursor_log_y != oly) {
                            if (cursor_log_x != sel_start_log_x || cursor_log_y != sel_start_log_y) has_selection = true;
                            if (!word_warp) h_scroll_offset = cursor_log_x < h_scroll_offset ? cursor_log_x : (cursor_log_x >= h_scroll_offset + MAX_COLS ? cursor_log_x - MAX_COLS + 1 : h_scroll_offset);
                            refresh_follow_cursor();
                        }
                    }
                }
                else if (mer.dwEventFlags == MOUSE_WHEELED) {
                    short wd = HIWORD(mer.dwButtonState);
                    if (show_open_dialog || show_save_dialog) { if (wd > 0 && browser_scroll_offset > 0) browser_scroll_offset--; else if (wd < 0 && browser_scroll_offset < (int)browser_files.size() - (SCREEN_ROWS - 16)) browser_scroll_offset++; refresh(); }
                    else if (!show_settings && active_menu == 0) { if (wd > 0) scroll_y = max(0, scroll_y - 3); else if (wd < 0) scroll_y = min(max(0, total_visual_lines - MAX_ROWS), scroll_y + 3); refresh(); }
                }
                else if (mer.dwButtonState == RIGHTMOST_BUTTON_PRESSED && !show_settings && !show_open_dialog && !show_save_dialog && active_menu == 0) {
                    /* FIX: Upgraded hardcoded 80x25 bounds to use dynamic SCREEN_COLS and SCREEN_ROWS */
                    if (mx >= 1 && mx <= MAX_COLS && my >= text_start_y && my <= SCREEN_ROWS - 4) { 
                        active_menu = 4; 
                        context_x = min(mx, SCREEN_COLS - 17); 
                        context_y = min(my, SCREEN_ROWS - 10); /* FIX: Expanded height for Read Only */
                        current_hover_element = 20; /* FIX: Auto-select the first item (Cut) so arrow keys work instantly! */
                        refresh(); 
                    }
                }
                else if (mer.dwButtonState == FROM_LEFT_1ST_BUTTON_PRESSED) {
                    if (active_tab_element != 0) { active_tab_element = 0; refresh(); }
                    
                    /* FIX: Layer Priority 1 - Menus intercept clicks BEFORE the Find dialog! */
                    if (active_menu == 1 && mx >= 0 && mx <= 15 && my >= 2 && my <= 13) { int id = (my == 3 ? 10 : (my == 4 ? 11 : (my == 5 ? 12 : (my == 6 ? 13 : (my == 8 ? 14 : (my == 10 ? 15 : (my == 12 ? 16 : 0))))))); if (id != 0) executeMenuAction(id); continue; }
                    /* FIX: Context Menu click bounds must be evaluated in the if-condition so misses correctly fall through to the close handler! */
                    else if (active_menu == 2 && mx >= 6 && mx <= 21 && my >= 2 && my <= 14) { 
                        int ly = my - 2, id = (ly == 1 ? 24 : (ly == 2 ? 25 : (ly == 4 ? 20 : (ly == 5 ? 21 : (ly == 6 ? 22 : (ly == 7 ? 23 : (ly == 9 ? 26 : (ly == 10 ? 28 : (ly == 11 ? 27 : 0))))))))); 
                        if (id != 0) executeMenuAction(id); continue; 
                    }
                    else if (active_menu == 3 && mx >= 13 && mx <= 34 && my >= 2 && my <= 12) { int id = (my == 3 || my == 4 ? 30 : (my == 6 || my == 7 ? 31 : (my == 9 ? 32 : (my == 11 ? 33 : 0)))); if (id != 0) executeMenuAction(id); continue; }
                    else if (active_menu == 4 && mx >= context_x && mx <= context_x + 15 && my >= context_y && my <= context_y + 8) { 
                        int ly = my - context_y, id = (ly == 1 ? 20 : (ly == 2 ? 21 : (ly == 3 ? 22 : (ly == 4 ? 23 : (ly == 6 ? 26 : (ly == 7 ? 27 : 0)))))); 
                        if (id != 0) executeMenuAction(id); continue; 
                    }
                    else if (active_menu != 0) { 
                        close_active_menu(buffer); 
                        continue; 
                    } // Click anywhere else -> Close Menu & Swallow click!

                    /* FIX: Layer Priority 2 - Find Dialog */
                    if (show_find_dialog && my >= 3 && my <= 8) {
                        /* FIX: Shifted narrow click hitboxes +1 to the right! */
                        int b1_s = (SCREEN_COLS < 80) ? 20 : 53; int b1_e = (SCREEN_COLS < 80) ? 26 : 63;
                        int b2_s = (SCREEN_COLS < 80) ? 28 : 65; int b2_e = (SCREEN_COLS < 80) ? 34 : 75; 
                        
                        if      (my == 4 && mx >= b1_s && mx <= b1_e) execute_find_replace(1);
                        else if (my == 4 && mx >= b2_s && mx <= b2_e) execute_find_replace(4);
                        else if (my == 7 && mx >= b1_s && mx <= b1_e) execute_find_replace(2);
                        else if (my == 7 && mx >= b2_s && mx <= b2_e) execute_find_replace(3);
                        else {
                            int input_end = (SCREEN_COLS < 80) ? 19 : 50; /* FIX: Expanded input boundary to catch the wall */
                            if (my >= 3 && my <= 5 && mx >= 8 && mx <= input_end) active_input_field = 4;
                            else if (my >= 6 && my <= 8 && mx >= 8 && mx <= input_end) active_input_field = 5;
                            else active_input_field = 0;
                        }
                        refresh_follow_cursor(); /* FIX: Force viewport snap when clicking Next/Prev! */
                        continue;
                    }

                    /* FIX: Ensure the global 'Exit Program' X button only triggers if dialogs are closed! */
                    if (!show_settings && !show_open_dialog && !show_save_dialog && my == 1 && mx >= SCREEN_COLS - 4 && mx <= SCREEN_COLS - 2) { 
                        if (is_document_dirty) { show_exit_dialog = true; pending_action = 1; active_menu = current_hover_element = 0; SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), get_dim_attr()); init_app(argv0_global, true); refresh(); } 
                        else { clear_screen_safe(); running = false; } 
                        continue; 
                    }
                    
                    if (show_settings) {
                        /* FIX: Safely route the X button click to close the Settings page! */
                        if (my == 1 && mx >= SCREEN_COLS - 4 && mx <= SCREEN_COLS - 2) { show_settings = false; current_hover_element = 0; init_app(argv0_global, true); refresh(); continue; }
                        
                        if (my == 3) { if (mx >= 1 && mx <= 9) { //display tab
                            current_settings_tab = 0; 
                            draw_settings(); 
                        } 
                        else if (mx >= 11 && mx <= 20) { //graphics tab
                            current_settings_tab = 1; 
                            draw_settings(); 
                        } 
                        else if (mx >= 22 && mx <= 28) { //others tab
                            current_settings_tab = 2; 
                            draw_settings(); 
                        } 
                        else if (mx >= SCREEN_COLS - 9 && mx <= SCREEN_COLS - 2) { //back button
                            show_settings = false; 
                            current_hover_element = 0; 
                            init_app(argv0_global, true); 
                            refresh(); 
                        } 
                    }
                        else if (current_settings_tab == 1) {
                            if (my == 7 && mx >= 2 && mx <= 20) { setting_hover_effects = 0; save_config(); draw_settings(); }
                            else if (my == 8 && mx >= 2 && mx <= 20) { setting_hover_effects = 1; save_config(); draw_settings(); }
                            else if (my == 9 && mx >= 2 && mx <= 20) { setting_hover_effects = 2; save_config(); draw_settings(); }
                            else if (my == 11 && mx >= 2 && mx <= 20) {
                                setting_color = !setting_color;
                                if (!setting_color) {
                                    if (setting_bg_color != 0 && setting_bg_color != 7) setting_bg_color = 0;
                                    if (setting_fg_color != 0 && setting_fg_color != 7) setting_fg_color = 7;
                                }
                                if (setting_bg_color == setting_fg_color) {
                                    if (setting_bg_color == 7) setting_fg_color = 0; else setting_fg_color = 7;
                                }
                                save_config(); init_app(argv0_global, true); draw_settings();
                            }
                            else if (my == 14 && mx >= 2 && mx <= 30) { setting_ansi_mode = 0; save_config(); draw_settings(); }
                            else if (my == 15 && mx >= 2 && mx <= 30) { setting_ansi_mode = 1; save_config(); draw_settings(); }
                            else if (my >= 18 && my <= 21) {
                                if (!setting_color && my >= 20) {}
                                else {
                                    int* target = (my == 18) ? &setting_bg_color : ((my == 19) ? &setting_fg_color : ((my == 20) ? &setting_sel_color : &setting_crit_color));
                                    if (mx >= 27 && mx <= 35) *target = 0; else if (mx >= 38 && mx <= 46) *target = 7;
                                    else if (setting_color) {
                                        if (mx >= 49 && mx <= 56) { if (*target == 0 || *target == 7) *target = 1; else *target ^= 1; }
                                        else if (mx >= 59 && mx <= 67) { if (*target == 0 || *target == 7) *target = 2; else *target ^= 2; }
                                        else if (mx >= 70 && mx <= 76) { if (*target == 0 || *target == 7) *target = 4; else *target ^= 4; }
                                    }

                                    if (setting_bg_color == setting_fg_color) {
                                        if (setting_bg_color == 7) setting_fg_color = 0; else setting_fg_color = 7;
                                    }

                                    save_config(); init_app(argv0_global, true); draw_settings();
                                }
                            }
                        }
                        else if (current_settings_tab == 2) { 
                            if (my == 13 && mx >= 5 && mx <= 27) { setting_internal_clipboard = true; save_config(); draw_settings(); } 
                            else if (my == 14 && mx >= 5 && mx <= 27) { setting_internal_clipboard = false; save_config(); draw_settings(); } 
                            else if (my == 9 && mx >= 5 && mx <= 27) { setting_disk_stream = 0; save_config(); draw_settings(); } 
                            else if (my == 10 && mx >= 5 && mx <= 27) { setting_disk_stream = 1; save_config(); draw_settings(); } 
                        }
                    }
                    else if (show_open_dialog || show_save_dialog) {
                        if ((mx >= SCREEN_COLS - 9 && mx <= SCREEN_COLS - 4 && my == SCREEN_ROWS - 5) || (my == 1 && mx >= SCREEN_COLS - 4 && mx <= SCREEN_COLS - 2)) closeDialog(false);
                        else if (mx >= SCREEN_COLS - 18 && mx <= SCREEN_COLS - 13 && my == SCREEN_ROWS - 5) closeDialog(true);
                        else if (my == 4) { if (mx >= 2 && mx <= 5 && path_history_index > 0) load_directory(path_history[--path_history_index], false); else if (mx >= 7 && mx <= 10 && path_history_index < (int)path_history.size() - 1) load_directory(path_history[++path_history_index], false); else if (mx >= 12 && mx <= 14) { string p = current_browser_path; if (p.length() > 0 && p.back() == '\\') p.pop_back(); size_t pos = p.find_last_of("\\/"); if (pos != string::npos) load_directory(p.substr(0, pos + 1), true); } else if (mx >= 18 && mx <= SCREEN_COLS - 22) { active_input_field = 3; dialog_address_text = current_browser_path; dialog_address_cursor = (int)dialog_address_text.length(); dialog_status_msg = "Editing address..."; } else if (mx >= SCREEN_COLS - 18 && mx <= SCREEN_COLS - 3) { active_input_field = 2; dialog_search_cursor = min((int)dialog_search_text.length(), max(0, mx - (SCREEN_COLS - 16))); } refresh(); }
                        else if (my >= 9 && my <= 14 && mx >= 2 && mx <= 12) { string dp = string(1, (char)('C' + (my - 9))) + ":\\"; DWORD a = GetFileAttributesA(dp.c_str()); if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) load_directory(dp, true); else dialog_status_msg = "drive not exist"; refresh(); }
                        else if (my == SCREEN_ROWS - 5 && mx >= 16 && mx <= SCREEN_COLS - 22) { dialog_status_msg = show_save_dialog ? "Ready to save file..." : "Ready to select file..."; active_input_field = 1; dialog_input_cursor = (mx >= 18 && mx <= 18 + (int)dialog_input_text.length()) ? mx - 18 : (mx > 18 + (int)dialog_input_text.length() ? (int)dialog_input_text.length() : 0); refresh(); }
                        else if (mx == SCREEN_COLS - 4 && my == 7) { if (browser_scroll_offset > 0) browser_scroll_offset--; refresh(); }
                        else if (mx == SCREEN_COLS - 4 && my == SCREEN_ROWS - 8) { if (browser_scroll_offset < (int)browser_files.size() - (SCREEN_ROWS - 16)) browser_scroll_offset++; refresh(); }
                        else if (mx == SCREEN_COLS - 4 && my >= 9 && my <= SCREEN_ROWS - 9) { int ls = (int)browser_files.size(), list_h = SCREEN_ROWS - 16, th = list_h - 1, ts = max(1, (int)round((float)th * list_h / ls)), tp = 9 + (int)round((float)browser_scroll_offset / max(1, ls - list_h) * (th - ts)); if (my >= tp && my < tp + ts) is_dragging_browser_scroll = true; else if (my < tp) { browser_scroll_offset = max(0, browser_scroll_offset - list_h); refresh(); } else { browser_scroll_offset = min(ls - list_h, browser_scroll_offset + list_h); refresh(); } }
                        else if (my >= 9 && my <= SCREEN_ROWS - 8 && mx >= 18 && mx <= SCREEN_COLS - 5) { int idx = browser_scroll_offset + (my - 9); if (idx < (int)browser_files.size()) { browser_selected_index = idx; if (browser_files[idx].is_dir) load_directory(current_browser_path + browser_files[idx].name, true); else { dialog_input_text = browser_files[idx].name; dialog_input_cursor = (int)dialog_input_text.length(); active_input_field = 1; dialog_status_msg = show_save_dialog ? "Ready to save file..." : "Ready to select file..."; } refresh(); } }
                        else { active_input_field = 0; refresh(); }
                    }
                    else {
                        if (my == 1) { if (mx >= 0 && mx <= 6) { active_menu = 1; current_hover_element = 10; refresh(); } else if (mx >= 8 && mx <= 14) { active_menu = 2; current_hover_element = 24; is_context_menu = false; refresh(); } else if (mx >= 15 && mx <= 21) { active_menu = 3; current_hover_element = 30; refresh(); } }
                        else if (mx == SCREEN_COLS - 2 && my == SCREEN_ROWS - 2) { show_settings = true; draw_settings(); }
                        else if (v_scrollBar && mx == SCREEN_COLS - 2 && my >= 3 && my <= SCREEN_ROWS - 4) {
                            if (my == 3) { if (scroll_y > 0) { scroll_y--; refresh(); } } /* Scroll UP 1 Line */
                            else if (my == SCREEN_ROWS - 4) { if (scroll_y < total_visual_lines - MAX_ROWS) { scroll_y++; refresh(); } } /* Scroll DOWN 1 Line */
                            else if (my >= 5 && my <= SCREEN_ROWS - 6) {
                                if (total_visual_lines > MAX_ROWS) {
                                    int track_len = SCREEN_ROWS - 10;
                                    int ts = max(1, min(track_len, (int)round((float)track_len * MAX_ROWS / max(1, total_visual_lines))));
                                    int tp = 5 + (int)round((float)scroll_y / (total_visual_lines - MAX_ROWS) * (track_len - ts));
                                    if (my >= tp && my < tp + ts) is_dragging_vscroll = true;
                                    else if (my < tp) { scroll_y = max(0, scroll_y - MAX_ROWS); refresh(); }
                                    else { scroll_y = min(total_visual_lines - MAX_ROWS, scroll_y + MAX_ROWS); refresh(); }
                                }
                            }
                        }
                        else if (h_scrollBar && my == SCREEN_ROWS - 2 && mx >= 1 && mx <= (v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2)) { int mo = max(0, max_line_length - MAX_COLS); int bar_len = v_scrollBar ? SCREEN_COLS - 8 : SCREEN_COLS - 6; if (mx == 1) { h_scroll_offset = max(0, h_scroll_offset - 15); refresh(); } else if (mx == (v_scrollBar ? SCREEN_COLS - 4 : SCREEN_COLS - 2)) { h_scroll_offset = min(mo, h_scroll_offset + 15); refresh(); } else if (mx >= 3 && mx <= 3 + bar_len && mo > 0) { int ts = max(1, min(bar_len, (int)round((float)bar_len * MAX_COLS / max_line_length))), tp = 3 + (int)round((float)h_scroll_offset / mo * (bar_len - ts)); if (mx >= tp && mx < tp + ts) is_dragging_hscroll = true; else if (mx < tp) { h_scroll_offset = max(0, h_scroll_offset - MAX_COLS); refresh(); } else { h_scroll_offset = min(mo, h_scroll_offset + MAX_COLS); refresh(); } } }
                        else if (mx >= 1 && mx <= MAX_COLS && my >= text_start_y && my <= SCREEN_ROWS - 4) { update_cursor_from_screen(mx, my, buffer); has_selection = false; is_dragging = true; sel_start_log_x = cursor_log_x; sel_start_log_y = cursor_log_y; last_was_typing = false; refresh_follow_cursor(); }
                    }
                }
            }
        }
        
        /* FIX: ATOMIC CORE BALANCER */
        /* Only blit graphics to the console when states actually change. */
        /* If no keys are pressed or mouse is completely static, rendering engine goes to sleep! */
        if (needs_ui_redraw && !screen_too_small) {
            displayTextBuffer(buffer);
            needs_ui_redraw = false;
        }
    }
    
    /* FIX: Final Exit Cleanup! Ensure no ghost .tmp files are left behind when the app closes! */
    if (current_filename != "Untitled.txt") {
        string final_temp_path = current_browser_path + current_filename + ".tmp";
        if (GetFileAttributesA(final_temp_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(final_temp_path.c_str());
        }
    }
    
    SetConsoleOutputCP(original_cp);
    return 0;
}