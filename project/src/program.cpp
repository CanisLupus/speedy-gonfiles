// =================================================
// Includes
// =================================================

// Comment this define to get a separate console and see prints.
#define PUBLIC_RELEASE

// Uncomment this to get file icons in the list, but it's extremely slow
// and there was no further research to try and make it faster.
// #define DRAW_ICONS

// Enable UI skinning.
#pragma comment(linker, "\"/manifestdependency:type='win32'\
	name='Microsoft.Windows.Common-Controls'\
	version='6.0.0.0'\
	processorArchitecture='*'\
	publicKeyToken='6595b64144ccf1df'\
	language='*'\"")

// Include windows.h but exclude rarely-used stuff.
#define WIN32_LEAN_AND_MEAN
#define WIN32_EXTRA_LEAN
#include <windows.h>

#include <shellapi.h>
#include <shlobj_core.h>

#include <assert.h>
#include <tchar.h>

#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <functional>		// std::function
#include <thread>
#include <mutex>
#include <queue>

#include "../res/resource.h"
#include "meow_hash_x64_aesni.h"

extern "C" int timsort(void* base, size_t nel, size_t width, int (*compar)(const void*, const void*));

#ifdef DRAW_ICONS
#pragma comment (lib, "comctl32")	// required to use ImageList functions
#endif

// =================================================
// Macros
// =================================================

LONGLONG perf_count_frequency;
LARGE_INTEGER start_clock;

#define START_CLOCK()\
	QueryPerformanceCounter(&start_clock);

#define END_CLOCK(msg)\
	{\
		LARGE_INTEGER end_clock;\
		QueryPerformanceCounter(&end_clock);\
		double time_spent = (end_clock.QuadPart - start_clock.QuadPart) / (double) perf_count_frequency;\
		printf("%s. [%0.3fs]\n", (msg), time_spent);\
	}

// =================================================
// Consts
// =================================================

const int VERSION = 7000;

const COLORREF LIST_EVEN_ROW_COLOR = RGB(245, 245, 245);
const COLORREF LIST_ROW_COLORS[] = { 0x0010d3ff, 0x0090cd59, 0x00fae2b5, 0x00b4c2ff, 0x00678ffb };

const size_t COULD_NOT_READ_FILE = -1;

// =================================================
// Types
// =================================================

enum MenuId {
	Window_ListView,
	Window_StatusBar,

	MenuBar_File_Clear,
	MenuBar_File_Exit,
	MenuBar_Help_About,

	Context_Open,
	Context_OpenFolder,
	Context_Delete,
	Context_Clear,
};

enum Column {
	OrderAdded,
	Path,
	Size,
	Hash,
	Group
};

struct file_info {
	wchar_t* Path;				// path of the file
	size_t Size;				// size of the file in bytes
	meow_u128 Hash;				// hash of the file's contents
	int DuplicateGroupNumber;	// number of the group of files with the same hash as this file (0 if not in a group)
};

struct hash_results {
	size_t TotalFileSize;
	size_t TotalDuplicates;
};

struct hashing_job {
	std::vector<wchar_t*> file_paths;
};

// hashing functor for unordered_set of wchar_t*
struct hash_wchar_t {
	// djb2 hash
	size_t operator()(const wchar_t* str) const {
		size_t hash = 5381;
		wchar_t c;
		while (c = *str++) {
			hash = (hash << 5) + hash + c;	// hash * 33 + c
		}
		return hash;
	}
};

// comparison functor for unordered_set of wchar_t*
struct compare_wchar_t {
	bool operator()(const wchar_t* a, const wchar_t* b) const {
		return _wcsicmp(a, b) == 0;
	}
};

// hashing functor for meow_u128
struct hash_meow_u128 {
	size_t operator()(meow_u128 val) const {
		return MeowU64From(val, 0);
	}
};

// comparison functor for meow_u128
struct compare_meow_u128 {
	bool operator()(meow_u128 a, meow_u128 b) const {
		return MeowHashesAreEqual(a, b);
	}
};

// =================================================
// Global variables
// =================================================

static HINSTANCE global_instance;
static HWND global_window;
static HWND global_list_view;
static HWND global_status_bar;
static HMENU global_menu_bar;
static HMENU global_context_menu;

#ifdef DRAW_ICONS
static HIMAGELIST global_list_view_image_list;
#endif

// The actual file_info structures. This vector is never reordered, only new elements added after dragging more files to the interface.
static std::vector<file_info> file_infos;
// Vector of indices that represent the current way file_infos are sorted (so we can sort without touching the original vector).
static std::vector<size_t> sorted_file_infos;
// Hash set of all the file paths, so that we can quickly test if a path was already processed.
static std::unordered_set<wchar_t*, hash_wchar_t, compare_wchar_t> file_paths_set;
// Hash map associating file sizes (in bytes) to the files that have those sizes.
static std::unordered_map<size_t, std::vector<size_t>> file_sizes_map;
// Hash map associating hashes to the files that have that hash.
static std::unordered_map<meow_u128, std::vector<size_t>, hash_meow_u128, compare_meow_u128> hashed_files_map;

// Next group number for files that have duplicates. All duplicates get the same group number.
static int next_duplicate_group_number = 1;

static Column last_sorted_column = Column::OrderAdded;
static bool is_column_sorted_ascending = true;

// Queue of jobs to be processed by the work thread and the respective mutexes/conditional variables.
static std::queue<hashing_job> hashing_jobs_queue;
static std::mutex jobs_exist_mutex;
static std::condition_variable jobs_exist_cond_var;

// =================================================
// Comparison functions for sorts
// =================================================

// special comparer for sorting purely paths
inline int compare_paths(const void* a, const void* b)
{
	return _wcsicmp(*((wchar_t**) a), *((wchar_t**) b));	// case insensitive
}

inline int compare_file_infos_by_path(const file_info* a, const file_info* b)
{
	return _wcsicmp(a->Path, b->Path);	// case insensitive
}

inline int compare_file_infos_by_size(const file_info* a, const file_info* b)
{
	if (a->Size < b->Size) return -1;
	if (a->Size > b->Size) return 1;
	return 0;
}

inline int compare_file_infos_by_hash(const file_info* a, const file_info* b)
{
	unsigned long long a_hash64 = MeowU64From(a->Hash, 1);
	unsigned long long b_hash64 = MeowU64From(b->Hash, 1);

	if (a_hash64 < b_hash64) return -1;
	if (a_hash64 > b_hash64) return 1;

	a_hash64 = MeowU64From(a->Hash, 0);
	b_hash64 = MeowU64From(b->Hash, 0);

	if (a_hash64 < b_hash64) return -1;
	if (a_hash64 > b_hash64) return 1;

	return 0;
}

inline int compare_file_infos_by_duplicate_group_number(const file_info* a, const file_info* b)
{
	if (a->DuplicateGroupNumber == 0 && b->DuplicateGroupNumber == 0) return 0;
	if (a->DuplicateGroupNumber == 0) return 1;
	if (b->DuplicateGroupNumber == 0) return -1;
	return a->DuplicateGroupNumber - b->DuplicateGroupNumber;
}

int compare_file_infos_by_order_added_ascending            (const void* _a, const void* _b) { return *(int*)_a - *(int*)_b; }
int compare_file_infos_by_order_added_descending           (const void* _a, const void* _b) { return *(int*)_b - *(int*)_a; }
int compare_file_infos_by_path_ascending                   (const void* _a, const void* _b) { return  compare_file_infos_by_path(&file_infos[*(int*)_a], &file_infos[*(int*)_b]); }
int compare_file_infos_by_path_descending                  (const void* _a, const void* _b) { return -compare_file_infos_by_path(&file_infos[*(int*)_a], &file_infos[*(int*)_b]); }
int compare_file_infos_by_size_ascending                   (const void* _a, const void* _b) { return  compare_file_infos_by_size(&file_infos[*(int*)_a], &file_infos[*(int*)_b]); }
int compare_file_infos_by_size_descending                  (const void* _a, const void* _b) { return -compare_file_infos_by_size(&file_infos[*(int*)_a], &file_infos[*(int*)_b]); }
int compare_file_infos_by_hash_ascending                   (const void* _a, const void* _b) { return  compare_file_infos_by_hash(&file_infos[*(int*)_a], &file_infos[*(int*)_b]); }
int compare_file_infos_by_hash_descending                  (const void* _a, const void* _b) { return -compare_file_infos_by_hash(&file_infos[*(int*)_a], &file_infos[*(int*)_b]); }
int compare_file_infos_by_duplicate_group_number_ascending (const void* _a, const void* _b) { return  compare_file_infos_by_duplicate_group_number(&file_infos[*(int*)_a], &file_infos[*(int*)_b]); }
int compare_file_infos_by_duplicate_group_number_descending(const void* _a, const void* _b) { return -compare_file_infos_by_duplicate_group_number(&file_infos[*(int*)_a], &file_infos[*(int*)_b]); }

// =================================================
// Sort
// =================================================

void sort_file_infos(Column column)
{
	int (*comparer)(const void* _a, const void* _b) = NULL;

	switch (column)
	{
		case Column::OrderAdded: comparer = is_column_sorted_ascending ? compare_file_infos_by_order_added_ascending            : compare_file_infos_by_order_added_descending;            break;
		case Column::Path:       comparer = is_column_sorted_ascending ? compare_file_infos_by_path_ascending                   : compare_file_infos_by_path_descending;                   break;
		case Column::Size:       comparer = is_column_sorted_ascending ? compare_file_infos_by_size_ascending                   : compare_file_infos_by_size_descending;                   break;
		case Column::Hash:       comparer = is_column_sorted_ascending ? compare_file_infos_by_hash_ascending                   : compare_file_infos_by_hash_descending;                   break;
		case Column::Group:      comparer = is_column_sorted_ascending ? compare_file_infos_by_duplicate_group_number_ascending : compare_file_infos_by_duplicate_group_number_descending; break;
	}

	assert(comparer);

	timsort(sorted_file_infos.data(), sorted_file_infos.size(), sizeof(size_t), comparer);

	last_sorted_column = column;

	ListView_SetItemState(global_list_view, -1, LVIF_STATE, LVIS_SELECTED);	// clear selections
}

// =================================================
// Collecting files
// =================================================

// NOTE: Function assumes it is called with the path of a directory (no trailing \).
void get_file_paths_recursively_in_directory(const wchar_t* dir_path, size_t path_len, std::vector<wchar_t*> &file_paths, size_t* symlink_count)
{
	// Create a new path equal to dir_path plus a trailing slash and asterisk (\*), for the FindFirstFile functions.

	wchar_t sub_path[512];
	memcpy(sub_path, dir_path, path_len * sizeof(wchar_t));
	*(sub_path + path_len) = L'\\';
	*(sub_path + path_len + 1) = L'*';
	*(sub_path + path_len + 2) = L'\0';

	// Iterate over contained files/folders.

	WIN32_FIND_DATA file;
	HANDLE file_search_handle = FindFirstFile(sub_path, &file);
	if (file_search_handle == INVALID_HANDLE_VALUE) return;

	do {
		// skip folders "." and ".."
		if (file.cFileName[0] == L'.') {
			if (file.cFileName[1] == L'\0') continue;
			if (file.cFileName[1] == L'.' && file.cFileName[2] == L'\0') continue;
		}

		if (file.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {	// skip symlinks
			(*symlink_count)++;
			continue;
		}

		size_t file_name_len = wcslen(file.cFileName);
		size_t sub_path_len = path_len + 1 + file_name_len;

		// path_len + 1 to write after '\\', file_name_len + 1 to also copy '\0'
		memcpy(sub_path + path_len + 1, file.cFileName, (file_name_len + 1) * sizeof(wchar_t));

		if ((file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			// directory
			get_file_paths_recursively_in_directory(sub_path, sub_path_len, file_paths, symlink_count);
		}
		else {
			// file
			wchar_t* file_path = (wchar_t*) malloc((sub_path_len + 1) * sizeof(wchar_t));
			assert(file_path);
			memcpy(file_path, sub_path, (sub_path_len + 1) * sizeof(wchar_t));
			file_paths.push_back(file_path);
		}
	} while (FindNextFile(file_search_handle, &file));

	FindClose(file_search_handle);
}

// Collects the files inside this path into a vector. If the path is a file, it just adds itself.
void get_file_paths_recursively(const wchar_t* path, size_t path_len, std::vector<wchar_t*> &file_paths, size_t* symlink_count)
{
	DWORD attributes = GetFileAttributes(path);

	// if over the path length limit or not a directory then it's a file(unsure?), so save it
	if (path_len > MAX_PATH || (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)) {
		wchar_t* file_path = (wchar_t*) malloc((path_len + 1) * sizeof(wchar_t));
		assert(file_path);
		memcpy(file_path, path, (path_len + 1) * sizeof(wchar_t));
		file_paths.push_back(file_path);
		return;
	}

	get_file_paths_recursively_in_directory(path, path_len, file_paths, symlink_count);
}

// Returns a filtered vector of file paths, removing already processed paths and duplicate dragged paths.
// Duplicate dragged paths is a weird concept but they can exist if the user dragged a file/folder and
// its container folder (or grandparent folder, etc) simultaneously, for example from the Windows folder
// search results.
std::vector<wchar_t*> filter_new_file_paths(std::vector<wchar_t*> &new_file_paths, std::unordered_set<wchar_t*, hash_wchar_t, compare_wchar_t> &file_paths_set)
{
	std::vector<wchar_t*> real_new_file_paths;
	real_new_file_paths.reserve(new_file_paths.size());	// guess that there will be no duplicates

	for (wchar_t* path : new_file_paths)
	{
		if (file_paths_set.find(path) == file_paths_set.end()) {
			real_new_file_paths.push_back(path);
			file_paths_set.insert(path);	// update set of file paths with new path
		}
	}

	return real_new_file_paths;
}

// =================================================
// File reading/hashing functions
// =================================================

std::vector<size_t> read_file_sizes(std::vector<wchar_t*> &paths)
{
	size_t n_paths = paths.size();
	std::vector<size_t> file_sizes(n_paths);

	for (size_t i = 0; i < n_paths; i++)
	{
		wchar_t* path = paths[i];

		WIN32_FIND_DATA find_data;
		HANDLE find_file = FindFirstFile(path, &find_data);

		if (find_file && find_file != INVALID_HANDLE_VALUE) {
			file_sizes[i] = ((size_t) find_data.nFileSizeHigh) << (sizeof(find_data.nFileSizeHigh) * 8) | find_data.nFileSizeLow;
			FindClose(find_file);
		} else {
			file_sizes[i] = COULD_NOT_READ_FILE;
		}
	}

	return file_sizes;
}

meow_u128 hash_file_contents(wchar_t* file_path, size_t size, std::function<void(size_t bytes_read, LONGLONG total_bytes)> &log_progress)
{
	HANDLE hFile = CreateFile(
		file_path,				// file to open
		GENERIC_READ,			// open for reading
		FILE_SHARE_READ,		// share for reading
		NULL,					// default security
		OPEN_EXISTING,			// existing file only
		FILE_ATTRIBUTE_NORMAL,	// normal file
		NULL);					// no attr. template

	if (hFile == INVALID_HANDLE_VALUE) return meow_u128();

	const size_t BUFFER_SIZE = 1024 * 1024 * 1;	// 1 MB
	static BYTE file_buffer[BUFFER_SIZE];

	meow_state state;
	MeowBegin(&state, MeowDefaultSeed);

	size_t total_bytes_read = 0;
	DWORD bytes_read = 0;

	for (;;)
	{
		if (!ReadFile(hFile, file_buffer, BUFFER_SIZE, &bytes_read, NULL)) break;
		if (bytes_read == 0) break;

		MeowAbsorb(&state, bytes_read, file_buffer);
		total_bytes_read += bytes_read;

		log_progress(total_bytes_read, size);
	}

	CloseHandle(hFile);

	return MeowEnd(&state, NULL);
}

// Prepares a function to be used by hash_file_contents() to report hashing progress on a file.
std::function<void(size_t bytes_read, LONGLONG total_bytes)> create_progress_logging_function(
	wchar_t* status_bar_text, size_t status_bar_text_max_len, int* n_chars_on_status_bar)
{
	LARGE_INTEGER status_bar_start_clock;
	QueryPerformanceCounter(&status_bar_start_clock);

	return [status_bar_text, status_bar_text_max_len, n_chars_on_status_bar, &status_bar_start_clock](size_t bytes_read, LONGLONG total_bytes)
	{
		const double TIME_BETWEEN_STATUS_BAR_UPDATES = 0.1;	// in seconds
		LARGE_INTEGER current_clock;
		QueryPerformanceCounter(&current_clock);

		if ((current_clock.QuadPart - status_bar_start_clock.QuadPart) / (double) perf_count_frequency > TIME_BETWEEN_STATUS_BAR_UPDATES)
		{
			status_bar_start_clock = current_clock;

			// write the end part of the status bar with the progress percent (unless it's 100%, in which case it doesn't mean much)
			if (bytes_read < total_bytes) {
				swprintf_s(status_bar_text + *n_chars_on_status_bar, status_bar_text_max_len - *n_chars_on_status_bar - 1,
					L" (%.0f%%)", (bytes_read / (double) total_bytes) * 100);
			}

			// ask the status bar to update its text
			SendMessage(global_status_bar, SB_SETTEXT, (WPARAM) 0, (LPARAM) status_bar_text);
		}
	};
}

hash_results hash_files_and_find_duplicates(
	std::vector<file_info> &file_infos, size_t new_file_infos_start_index, std::unordered_map<size_t, std::vector<size_t>> &file_sizes_map)
{
	wchar_t status_bar_text[512];
	int n_chars_on_status_bar;

	std::function<void(size_t bytes_read, LONGLONG total_bytes)> log_progress =
		create_progress_logging_function(status_bar_text, _countof(status_bar_text), &n_chars_on_status_bar);

	hash_results results = {0};

	for (size_t i = new_file_infos_start_index, n_files = file_infos.size(); i < n_files; i++)
	{
		file_info &info = file_infos[i];

		if (info.Size == COULD_NOT_READ_FILE) continue;
		// if there's only this file with this size then it's not a duplicate, so skip it
		if (file_sizes_map.find(info.Size)->second.size() == 1) continue;

		// write first part of status bar text, with file number and path
		n_chars_on_status_bar = swprintf_s(status_bar_text, L"[%zd/%zd] %ls", i+1, n_files, info.Path);

		info.Hash = hash_file_contents(info.Path, info.Size, log_progress);

		results.TotalFileSize += info.Size;

		auto it = hashed_files_map.find(info.Hash);

		if (it != hashed_files_map.end())
		{
			for (size_t index : it->second)
			{
				file_info &existing_file_info = file_infos[index];

				if (MeowHashesAreEqual(info.Hash, existing_file_info.Hash))
				{
					if (info.DuplicateGroupNumber == 0 && existing_file_info.DuplicateGroupNumber == 0) {
						info.DuplicateGroupNumber = next_duplicate_group_number;
						existing_file_info.DuplicateGroupNumber = next_duplicate_group_number;
					} else if (info.DuplicateGroupNumber == 0) {
						info.DuplicateGroupNumber = existing_file_info.DuplicateGroupNumber;
					} else {
						existing_file_info.DuplicateGroupNumber = info.DuplicateGroupNumber;
					}
				}
			}

			results.TotalDuplicates++;

			it->second.push_back(i);
		}
		else
		{
			hashed_files_map[info.Hash] = std::vector<size_t> { i };
		}

		if (info.DuplicateGroupNumber == next_duplicate_group_number) {
			next_duplicate_group_number++;
		}
	}

	return results;
}

hash_results hash_files(std::vector<file_info> &file_infos, std::vector<size_t> &file_indexes_to_hash)
{
	wchar_t status_bar_text[512];
	int n_chars_on_status_bar;

	std::function<void(size_t bytes_read, LONGLONG total_bytes)> log_progress =
		create_progress_logging_function(status_bar_text, _countof(status_bar_text), &n_chars_on_status_bar);

	hash_results results = {0};

	for (size_t i = 0, n_files = file_indexes_to_hash.size(); i < n_files; i++)
	{
		size_t index = file_indexes_to_hash[i];
		file_info &info = file_infos[index];

		if (info.Size == COULD_NOT_READ_FILE) continue;

		// write first part of status bar text, with file number and path
		n_chars_on_status_bar = swprintf_s(status_bar_text, L"[%zd/%zd] %ls", i+1, n_files, info.Path);

		info.Hash = hash_file_contents(info.Path, info.Size, log_progress);

		results.TotalFileSize += info.Size;

		hashed_files_map[info.Hash] = std::vector<size_t> { index };
	}

	return results;
}

// Updates the hash table associating file sizes to the indexes of the files of that size. It also determines the indexes of files
// that have to be hashed due to there being a new file with the same size.
std::vector<size_t> update_file_sizes_map(
	std::vector<size_t> &file_sizes, size_t previous_file_infos_count, std::unordered_map<size_t, std::vector<size_t>> &file_sizes_map)
{
	std::vector<size_t> file_indexes_needing_hashing;

	for (size_t i = 0, count = file_sizes.size(); i < count; i++)
	{
		size_t size = file_sizes[i];

		if (size == COULD_NOT_READ_FILE) continue;

		auto it = file_sizes_map.find(size);

		// if this is the first file of this size, create the vector to contain the index of this and future files of this size
		if (it == file_sizes_map.end()) {
			file_sizes_map[size] = std::vector<size_t> { previous_file_infos_count + i };
		}
		// if there were previous files of this size, add to the existing vector of indexes
		else {
			// if there was only one file of this size before this drag, it now needs hashing!
			if (it->second.size() == 1 && it->second[0] < previous_file_infos_count) {
				file_indexes_needing_hashing.push_back(it->second[0]);
			}

			it->second.push_back(previous_file_infos_count + i);
		}
	}

	return file_indexes_needing_hashing;
}

void process_dragged_files(std::vector<wchar_t*> &dragged_file_paths)
{
	printf("\n");

	LARGE_INTEGER processing_start_clock;
	QueryPerformanceCounter(&processing_start_clock);

	// ---------------------------------------------------------
	// Recurse through every dragged folder to get all actual dragged files.
	// ---------------------------------------------------------

	SendMessage(global_status_bar, SB_SETTEXT, (WPARAM) 0, (LPARAM) L"Gathering all dragged files...");

	START_CLOCK();

	std::vector<wchar_t*> all_recursive_file_paths;

	size_t symlink_count = 0;

	for (size_t i = 0, n_dragged_files = dragged_file_paths.size(); i < n_dragged_files; ++i)
	{
		wchar_t* path = dragged_file_paths[i];
		get_file_paths_recursively(path, wcslen(path), all_recursive_file_paths, &symlink_count);
	}

	END_CLOCK("Recursively get all dragged files");

	printf("\t# dragged files: %zd\n", all_recursive_file_paths.size());

	// ---------------------------------------------------------
	// Remove all duplicates of previously processed files.
	// ---------------------------------------------------------

	SendMessage(global_status_bar, SB_SETTEXT, (WPARAM) 0, (LPARAM) L"Checking for already processed files...");

	START_CLOCK();

	std::vector<wchar_t*> new_paths = filter_new_file_paths(all_recursive_file_paths, file_paths_set);

	// Sorting is unnecessary, but this way we have always the same duplicate numbers for the same bunch of dragged files.
	timsort(new_paths.data(), new_paths.size(), sizeof(wchar_t*), compare_paths);

	END_CLOCK("Filter down to new file paths only");

	printf("\t# new paths: %zd (existing: %zd)\n", new_paths.size(), all_recursive_file_paths.size() - new_paths.size());

	// ---------------------------------------------------------
	// Read all file sizes.
	// ---------------------------------------------------------

	SendMessage(global_status_bar, SB_SETTEXT, (WPARAM) 0, (LPARAM) L"Reading sizes of all dragged files...");

	START_CLOCK();

	size_t previous_file_infos_count = file_infos.size();
	std::vector<size_t> file_sizes = read_file_sizes(new_paths);
	std::vector<size_t> file_indexes_needing_hashing = update_file_sizes_map(file_sizes, previous_file_infos_count, file_sizes_map);

	// we know how many elements we will add
	file_infos.resize(file_infos.size() + new_paths.size());

	size_t total_size = 0;

	// fill file_infos with the paths and sizes
	for (size_t i = 0, count = new_paths.size(); i < count; i++)
	{
		file_info &file_info = file_infos[previous_file_infos_count + i];
		file_info.Path = new_paths[i];
		file_info.Size = file_sizes[i];
		total_size += file_sizes[i];
	}

	END_CLOCK("Read all file sizes");

	printf("\tTotal file size: %0.2f MB\n", total_size / 1024.0 / 1024.0);

	// ---------------------------------------------------------
	// Process old files that now need to be hashed.
	// ---------------------------------------------------------

	if (!file_indexes_needing_hashing.empty())
	{
		SendMessage(global_status_bar, SB_SETTEXT, (WPARAM) 0, (LPARAM) L"Hashing old files that now have non-unique sizes...");

		START_CLOCK();

		hash_results results_for_old_files = hash_files(file_infos, file_indexes_needing_hashing);

		END_CLOCK("Read and hash previous files that now have non-unique sizes");

		printf("\t# files hashed: %zd\n", file_indexes_needing_hashing.size());
		printf("\tTotal file size read and hashed: %0.2f MB\n", results_for_old_files.TotalFileSize / 1024.0 / 1024.0);
	}

	// ---------------------------------------------------------
	// Process all new files (read and hash).
	// ---------------------------------------------------------

	SendMessage(global_status_bar, SB_SETTEXT, (WPARAM) 0, (LPARAM) L"Hashing dragged files...");

	START_CLOCK();

	hash_results results = hash_files_and_find_duplicates(file_infos, previous_file_infos_count, file_sizes_map);

	END_CLOCK("Read files and compare hashes");

	printf("\tDuplicate files: %zd\n", results.TotalDuplicates);
	printf("\tTotal file size read: %0.2f MB\n", results.TotalFileSize / 1024.0 / 1024.0);

	// ---------------------------------------------------------
	// Sort the results according to the current active sort.
	// ---------------------------------------------------------

	START_CLOCK();

	size_t n_existing_files = sorted_file_infos.size();
	size_t n_added_files = new_paths.size();

	sorted_file_infos.reserve(n_existing_files + n_added_files);

	for (size_t i = n_existing_files; i < n_existing_files + n_added_files; i++) {
		sorted_file_infos.push_back(i);
	}

	sort_file_infos(last_sorted_column);

	END_CLOCK("Sort results");

	// ---------------------------------------------------------
	// Load file icons.
	// ---------------------------------------------------------

#ifdef DRAW_ICONS

	START_CLOCK();

	ImageList_SetImageCount(global_list_view_image_list, n_existing_files + n_added_files);

	for (size_t i = n_existing_files; i < n_existing_files + n_added_files; i++)
	{
		SHFILEINFO shInfo;
		SHGetFileInfo(file_infos[i].Path, FILE_ATTRIBUTE_NORMAL, &shInfo, sizeof(SHFILEINFO), SHGFI_ICON | SHGFI_USEFILEATTRIBUTES);

		ImageList_ReplaceIcon(global_list_view_image_list, i, shInfo.hIcon);
		DestroyIcon(shInfo.hIcon);
	}

	END_CLOCK("Load icons");

#endif

	// ---------------------------------------------------------
	// Finish processing.
	// ---------------------------------------------------------

	ListView_SetItemCount(global_list_view, file_infos.size());
	ListView_RedrawItems(global_list_view, 0, file_infos.size());

	LARGE_INTEGER processing_end_clock;
	QueryPerformanceCounter(&processing_end_clock);
	double time_spent = (processing_end_clock.QuadPart - processing_start_clock.QuadPart) / (double) perf_count_frequency;

	if (n_added_files == 0) {
		SendMessage(global_status_bar, SB_SETTEXT, (WPARAM) 0, (LPARAM) L"No new files were processed.");
	} else {
		wchar_t status_bar_text[512];
		if (n_added_files == 1) {
			swprintf_s(status_bar_text, L"Finished processing 1 file. Took %0.3fs.", time_spent);
		} else {
			swprintf_s(status_bar_text, L"Finished processing %zd files. Took %0.3fs.", n_added_files, time_spent);
		}
		SendMessage(global_status_bar, SB_SETTEXT, (WPARAM) 0, (LPARAM) status_bar_text);
	}

	if (symlink_count > 0)
	{
		wchar_t buffer[512];
		swprintf_s(buffer, L"Skipped %zd symbolic link(s) when processing."
			"\n\nNOTE: This is usually what you want when searching for duplicate files.", symlink_count);

		MessageBox(
			NULL,
			buffer,
			L"Symbolic links skipped",
			MB_ICONINFORMATION | MB_OK
		);
	}
}

void set_state_of_menus_during_processing(bool enable)
{
	UINT state = enable ? MF_ENABLED : MF_DISABLED;
	EnableMenuItem(global_menu_bar, MenuId::MenuBar_File_Clear, state);
	EnableMenuItem(global_context_menu, MenuId::Context_Clear, state);
}

// function ran by the thread that processes jobs
void process_hashing_jobs_loop()
{
	for (;;)
	{
		std::unique_lock<std::mutex> lock(jobs_exist_mutex);
		while (hashing_jobs_queue.empty()) {
			jobs_exist_cond_var.wait(lock);
		}

		hashing_job job = hashing_jobs_queue.front();
		lock.unlock();

		set_state_of_menus_during_processing(false);

		process_dragged_files(job.file_paths);

		for (size_t i = 0, count = job.file_paths.size(); i < count; ++i)
		{
			free(job.file_paths[i]);
		}

		set_state_of_menus_during_processing(true);

		lock.lock();
		hashing_jobs_queue.pop();	// removed at the end so that other places can check if this thread is processing jobs
		lock.unlock();
	}
}

// =================================================
// Program Window - Context menu
// =================================================

void set_state_of_menus_based_on_selections()
{
	// enable/disable context menu entries based on if there are selections currently
	UINT state = ListView_GetNextItem(global_list_view, -1, LVNI_SELECTED) != -1 ? MF_ENABLED : MF_DISABLED;
	EnableMenuItem(global_context_menu, MenuId::Context_Open, state);
	EnableMenuItem(global_context_menu, MenuId::Context_OpenFolder, state);
	EnableMenuItem(global_context_menu, MenuId::Context_Delete, state);
}

void ContextMenu_OpenFiles(std::vector<int> &selected_indexes)
{
	for (int index : selected_indexes)
	{
		const wchar_t* path = file_infos[sorted_file_infos[index]].Path;
		ShellExecute(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
	}
}

void ContextMenu_OpenInExplorer(std::vector<int> &selected_indexes)
{
	// Step 1: Get the paths according to the selected indexes and sort.

	std::vector<const wchar_t*> paths;

	for (int index : selected_indexes)
	{
		const wchar_t* path = file_infos[sorted_file_infos[index]].Path;
		paths.push_back(path);
	}

	timsort(paths.data(), paths.size(), sizeof(wchar_t*), compare_paths);

	// Step 2: Since paths are sorted, paths in the same parent folder are adjacent, so open the explorer folders as we go.

	wchar_t dir_path[512];

	size_t i = 0;
	while (i < paths.size())
	{
		// find the parent folder path from the first path in the group

		const wchar_t* path = paths[i];

		size_t dir_path_len = wcsrchr(path, '\\') - path;	// get length from last position of slash
		memcpy(dir_path, path, dir_path_len * sizeof(wchar_t));
		dir_path[dir_path_len] = L'\0';

		ITEMIDLIST* dir = ILCreateFromPath(dir_path);

		std::vector<ITEMIDLIST*> selected_paths;
		selected_paths.push_back(ILCreateFromPath(path));

		// add all consecutive paths with the same parent folder

		i++;
		for (; i < paths.size(); i++)
		{
			path = paths[i];
			size_t index_of_last_separator = wcsrchr(path, '\\') - path;	// get length from last position of slash

			if (index_of_last_separator != dir_path_len) break;
			if (memcmp(path, dir_path, index_of_last_separator) != 0) break;

			selected_paths.push_back(ILCreateFromPath(path));
		}

		// open the group of files with the same parent folder

		SHOpenFolderAndSelectItems(dir, (UINT) selected_paths.size(), (LPCITEMIDLIST*) selected_paths.data(), 0);

		// free memory

		for (ITEMIDLIST* item : selected_paths) {
			ILFree(item);
		}

		ILFree(dir);
	}
}

void ContextMenu_DeleteFiles(std::vector<int> &selected_indexes)
{
	// As per SHFileOperation specification, we join all paths into a single array of chars
	// where each path is null terminated and the whole array is double-null terminated.

	std::vector<wchar_t> buffer;

	for (int index : selected_indexes)
	{
		const wchar_t* path = file_infos[sorted_file_infos[index]].Path;
		buffer.insert(buffer.end(), path, path + wcslen(path) + 1);	// plus \0
	}

	buffer.push_back('\0');	// double-null terminated

	SHFILEOPSTRUCT sfo = {0};
	sfo.wFunc = FO_DELETE;
	sfo.fFlags = FOF_ALLOWUNDO | FOF_WANTNUKEWARNING;
	sfo.pFrom = buffer.data();

	SHFileOperation(&sfo);
}

// =================================================
// Program Window - List View
// =================================================

void ListView_GetDisplayInfo(HWND hWnd, LPARAM lParam)
{
	LV_DISPINFO* lpdi = (LV_DISPINFO*) lParam;

	if (lpdi->item.mask & LVIF_TEXT)
	{
		file_info file_info = file_infos[sorted_file_infos[lpdi->item.iItem]];

		switch (lpdi->item.iSubItem)
		{
			case Column::OrderAdded: {
				_sntprintf_s(lpdi->item.pszText, lpdi->item.cchTextMax, _TRUNCATE, L"%d", lpdi->item.iItem + 1);
				break;
			}

			case Column::Path: {
				_sntprintf_s(lpdi->item.pszText, lpdi->item.cchTextMax, _TRUNCATE, L"%ls", file_info.Path);
				break;
			}

			case Column::Size: {
				if (file_info.Size == COULD_NOT_READ_FILE) {
					_sntprintf_s(lpdi->item.pszText, lpdi->item.cchTextMax, _TRUNCATE, L"<invalid>");
					break;
				}

				double size = (double) file_info.Size;
				int unit = 0;

				while (unit < 3 && size >= 1024) {
					size /= 1024.0;
					unit++;
				}

				const wchar_t* unit_txt;
				switch (unit)
				{
					case 0:  unit_txt = L"B"; break;
					case 1:  unit_txt = L"KB"; break;
					case 2:  unit_txt = L"MB"; break;
					default: unit_txt = L"GB"; break;
				}

				const wchar_t* text_template = unit == 0 ? L"%0.f %ls" : L"%0.2f %ls";	// don't show decimal places for units in bytes
				_sntprintf_s(lpdi->item.pszText, lpdi->item.cchTextMax, _TRUNCATE, text_template, size, unit_txt);
				break;
			}

			case Column::Hash: {
				if (file_info.Size == COULD_NOT_READ_FILE) {
					_sntprintf_s(lpdi->item.pszText, lpdi->item.cchTextMax, _TRUNCATE, L"<couldn't read file>");
					break;
				}

				meow_u128 hash = file_info.Hash;

				if (MeowU64From(hash, 0) == 0 && MeowU64From(hash, 1) == 0) {
					break;
				}

				_sntprintf_s(lpdi->item.pszText, lpdi->item.cchTextMax, _TRUNCATE, L"%08X-%08X-%08X-%08X",
					MeowU32From(hash, 3), MeowU32From(hash, 2), MeowU32From(hash, 1), MeowU32From(hash, 0));
				break;
			}

			case Column::Group: {
				int duplicate_group_number = file_info.DuplicateGroupNumber;
				if (duplicate_group_number != 0) {
					_sntprintf_s(lpdi->item.pszText, lpdi->item.cchTextMax, _TRUNCATE, L"%d", duplicate_group_number);
				}
				break;
			}
		}
	}

#ifdef DRAW_ICONS
	if (lpdi->item.mask & LVIF_IMAGE)
	{
		lpdi->item.iImage = sorted_file_infos[lpdi->item.iItem];
	}
#endif
}

void ListView_ColumnClick(HWND hWnd, LPARAM lParam)
{
	NMLISTVIEW* list_view = (NMLISTVIEW*) lParam;

	if (list_view->iSubItem == last_sorted_column) {
		is_column_sorted_ascending = !is_column_sorted_ascending;
	} else {
		is_column_sorted_ascending = true;
	}

	START_CLOCK();

	sort_file_infos((Column) list_view->iSubItem);

	END_CLOCK("Sort results");

	ListView_RedrawItems(global_list_view, 0, file_infos.size());
}

LRESULT ListView_CustomDraw(HWND hWnd, LPARAM lParam)
{
	LPNMLVCUSTOMDRAW lplvcd = (LPNMLVCUSTOMDRAW) lParam;

	switch(lplvcd->nmcd.dwDrawStage)
	{
		case CDDS_PREPAINT:	// Before the paint cycle begins request notifications for individual listview items
			return CDRF_NOTIFYITEMDRAW;

		case CDDS_ITEMPREPAINT: // Before an item is drawn
		{
			if (file_infos.empty()) return CDRF_DODEFAULT;

			DWORD_PTR index = lplvcd->nmcd.dwItemSpec;
			int duplicate_group_number = file_infos[sorted_file_infos[index]].DuplicateGroupNumber;

			if (duplicate_group_number != 0) {
				lplvcd->clrTextBk = LIST_ROW_COLORS[duplicate_group_number % _countof(LIST_ROW_COLORS)];
			} else {
				if (lplvcd->nmcd.dwItemSpec % 2 == 0) {
					lplvcd->clrTextBk = LIST_EVEN_ROW_COLOR;
				}
			}

			return CDRF_NEWFONT;
		}
	}

	return CDRF_DODEFAULT;
}

std::vector<int> ListView_GetSelectedIndexes()
{
	std::vector<int> selected_indexes;

	int index = ListView_GetNextItem(global_list_view, -1, LVNI_SELECTED);	// gets the first selected item

	while (index != -1)
	{
		selected_indexes.push_back(index);
		index = ListView_GetNextItem(global_list_view, index, LVNI_SELECTED);
	}

	return selected_indexes;
}

void ListView_DoubleClick(HWND hWnd, LPARAM lParam)
{
	LPNMITEMACTIVATE lpnmitem = (LPNMITEMACTIVATE) lParam;
	std::vector<int> selected_indexes { lpnmitem->iItem };
	ContextMenu_OpenInExplorer(selected_indexes);
}

LRESULT ListViewNotify(HWND hWnd, LPARAM lParam)
{
	LPNMHDR lpnmh = (LPNMHDR) lParam;

	// https://docs.microsoft.com/en-us/cpp/mfc/virtual-list-controls?view=msvc-160
	switch(lpnmh->code)
	{
		case LVN_GETDISPINFO:
			ListView_GetDisplayInfo(hWnd, lParam);
			break;

		case LVN_COLUMNCLICK:
			ListView_ColumnClick(hWnd, lParam);
			break;

		case NM_CUSTOMDRAW:
			return ListView_CustomDraw(hWnd, lParam);

		case NM_DBLCLK:
			ListView_DoubleClick(hWnd, lParam);
			break;

		case NM_RCLICK:
			set_state_of_menus_based_on_selections();	// must be done before TrackPopupMenu

			POINT cursor;
			GetCursorPos(&cursor);
			TrackPopupMenu(global_context_menu, 0, cursor.x, cursor.y, 0, hWnd, NULL);
			break;
	}

	return 0;
}

void ResizeListView(HWND hwndListView, HWND hwndParent)
{
	RECT rc;
	GetClientRect(hwndParent, &rc);

	RECT rc_status_bar;
	GetClientRect(global_status_bar, &rc_status_bar);
	UINT status_bar_height = rc_status_bar.bottom - rc_status_bar.top;

	MoveWindow(hwndListView,
		rc.left,
		rc.top,
		rc.right - rc.left,
		rc.bottom - rc.top - status_bar_height,
		TRUE
	);
}

HWND CreateListView(HWND hwndParent)
{
	// https://docs.microsoft.com/en-us/windows/win32/controls/list-view-controls-overview
	// Create the list-view window in report view with label editing enabled.
	HWND hWndListView = CreateWindow(
		WC_LISTVIEW,
		L"",
		WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA
			| LVS_SHOWSELALWAYS,	// keep selections visible (greyed out) when Window loses focus
		0, 0,
		0,
		0,
		hwndParent,
		(HMENU) MenuId::Window_ListView,
		global_instance,
		NULL
	);

	ResizeListView(hWndListView, hwndParent);

	ListView_SetExtendedListViewStyle(
		hWndListView,
		  LVS_EX_FULLROWSELECT	// show a full row item selection (instead of just showing the first column selected)
		| LVS_EX_DOUBLEBUFFER	// scrolling the list fast no longer shows items pop-in; also enables the translucent selection rectangle
		| LVS_EX_HEADERDRAGDROP	// makes column headers draggable to allow reordering
	);

	ListView_DeleteAllItems(hWndListView);	// empty the list

#ifdef DRAW_ICONS
	// prepare to use file icons in the list view

	global_list_view_image_list = ImageList_Create(16, 16, ILC_COLOR32, 0, 0);
	ListView_SetImageList(hWndListView, global_list_view_image_list, LVSIL_SMALL);
#endif

	// initialize the columns
	// https://docs.microsoft.com/en-us/windows/win32/api/commctrl/ns-commctrl-lvcolumna

	UINT column_mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;

	// NOTE: cast to non-const wchar_t* was needed but is undesirable...
	LVCOLUMN column_infos[] = {
		// mask,       fmt,           cx, pszText,                       cchTextMax, iSubItem, iImage, iOrder
		{ column_mask, LVCFMT_RIGHT,  60, (wchar_t*) L"#",               0,          0,        0,      0 },
		{ column_mask, LVCFMT_LEFT,  600, (wchar_t*) L"File path",       0,          0,        0,      0 },
		{ column_mask, LVCFMT_LEFT,   80, (wchar_t*) L"Size",            0,          0,        0,      0 },
		{ column_mask, LVCFMT_LEFT,  300, (wchar_t*) L"Hash",            0,          0,        0,      0 },
		{ column_mask, LVCFMT_LEFT,  100, (wchar_t*) L"Duplicate Group", 0,          0,        0,      0 },
	};

	for (int i = 0; i < _countof(column_infos); i++)
	{
		ListView_InsertColumn(hWndListView, i, &column_infos[i]);
	}

	ListView_SetItemCount(hWndListView, file_infos.size());	// set the number of items in the list

	return (hWndListView);
}

// =================================================
// Program Window - Status Bar
// =================================================

HWND CreateStatusBar(HWND hwndParent)
{
	HWND hwnd = CreateWindowEx(
		0,
		STATUSCLASSNAME,
		(PCTSTR) NULL,
		WS_CHILD | WS_VISIBLE,
		0, 0, 0, 0,
		hwndParent,
		(HMENU) MenuId::Window_StatusBar,
		global_instance,
		NULL
	);

	return hwnd;
}

// =================================================
// Application Window
// =================================================

std::vector<wchar_t*> CollectDroppedFilePaths(HWND hWnd, WPARAM wParam)
{
	// https://docs.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-dragqueryfilew

	HDROP drop = (HDROP) wParam;
	UINT n_dragged_files = DragQueryFile(drop, 0xFFFFFFFF, NULL, 0);

	std::vector<wchar_t*> dragged_file_paths;

	for (UINT i = 0; i < n_dragged_files; ++i)
	{
		UINT path_len = DragQueryFile(drop, i, NULL, 0);
		wchar_t* file_path = (wchar_t*) malloc((path_len + 1) * sizeof(wchar_t));

		DragQueryFile(drop, i, file_path, path_len+1);
		dragged_file_paths.push_back(file_path);
	}

	DragFinish(drop);

	return dragged_file_paths;
}

void ResetStatusBar()
{
	SendMessage(global_status_bar, SB_SETTEXT, (WPARAM) 0, (LPARAM) L"Drag files/folders to the interface to process them.");
}

void ClearList()
{
	size_t count = file_infos.size();

	for (size_t i = 0; i < count; i++) {
		free(file_infos[i].Path);
	}

#ifdef DRAW_ICONS
	for (int i = 0; i < count; i++) {
		HICON icon = ImageList_GetIcon(global_list_view_image_list, i, 0);
		DestroyIcon(icon);
	}

	ImageList_SetImageCount(global_list_view_image_list, 0);
#endif

	file_infos.clear();
	sorted_file_infos.clear();
	file_paths_set.clear();
	file_sizes_map.clear();
	hashed_files_map.clear();

	next_duplicate_group_number = 1;

	ListView_SetItemCount(global_list_view, file_infos.size());
	ListView_RedrawItems(global_list_view, 0, file_infos.size());

	ResetStatusBar();
}

HMENU CreateMenuBar(HWND window)
{
	HMENU menu_bar = CreateMenu();

	HMENU menu_bar_file = CreateMenu();
	AppendMenu(menu_bar, MF_POPUP, (UINT_PTR) menu_bar_file, L"&File");
	AppendMenu(menu_bar_file, MF_STRING, MenuId::MenuBar_File_Clear, L"&Clear");
	AppendMenu(menu_bar_file, MF_STRING, MenuId::MenuBar_File_Exit, L"&Exit");

	HMENU menu_bar_help = CreateMenu();
	AppendMenu(menu_bar, MF_POPUP, (UINT_PTR) menu_bar_help, L"&Help");
	AppendMenu(menu_bar_help, MF_STRING, MenuId::MenuBar_Help_About, L"&About");

	SetMenu(window, menu_bar);
	return menu_bar;
}

HMENU CreateContextMenu()
{
	HMENU menu = CreatePopupMenu();
	AppendMenu(menu, MF_STRING, MenuId::Context_OpenFolder, L"Open containing folder");
	AppendMenu(menu, MF_STRING, MenuId::Context_Open, L"Open");
	AppendMenu(menu, MF_SEPARATOR, 0, 0);
	AppendMenu(menu, MF_STRING, MenuId::Context_Delete, L"Delete");
	AppendMenu(menu, MF_SEPARATOR, 0, 0);
	AppendMenu(menu, MF_STRING, MenuId::Context_Clear, L"Clear");

	SetMenuDefaultItem(menu, MenuId::Context_OpenFolder, FALSE);

	return menu;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
		case WM_CREATE:
			global_menu_bar = CreateMenuBar(hWnd);
			global_context_menu = CreateContextMenu();
			global_status_bar = CreateStatusBar(hWnd);
			global_list_view = CreateListView(hWnd);
			ResetStatusBar();
			break;

		case WM_COMMAND:
		{
			int wmId = LOWORD(wParam);

			switch (wmId)
			{
				case MenuId::MenuBar_File_Clear: {
					std::unique_lock<std::mutex> lock(jobs_exist_mutex);
					bool are_there_jobs = hashing_jobs_queue.empty();
					lock.unlock();

					if (are_there_jobs) {
						ClearList();
					} else {
						// Shouldn't happen now that the Clear menu is disabled during processing, but...
						MessageBox(
							NULL,
							L"Sorry, you can't clear the list while processing files.",
							L"Can't do that!",
							MB_ICONINFORMATION | MB_OK
						);
					}

					break;
				}

				case MenuId::MenuBar_File_Exit:
					DestroyWindow(hWnd);
					break;

				case MenuId::MenuBar_Help_About:
					wchar_t text[512];
					swprintf_s(text, L"Speedy GonFiles, Version %d.%02d.%02d\nCopyright (c) Daniel Lobo 2021.",
						VERSION/10000, (VERSION/100) % 100, VERSION % 100);

					MessageBox(
						NULL,
						text,
						L"About",
						MB_ICONINFORMATION | MB_OK
					);
					break;

				case MenuId::Context_Open: {
					std::vector<int> selected_indexes = ListView_GetSelectedIndexes();
					if (selected_indexes.size() > 0) {
						ContextMenu_OpenFiles(selected_indexes);
					}
					break;
				}

				case MenuId::Context_OpenFolder: {
					std::vector<int> selected_indexes = ListView_GetSelectedIndexes();
					if (selected_indexes.size() > 0) {
						ContextMenu_OpenInExplorer(selected_indexes);
					}
					break;
				}

				case MenuId::Context_Delete: {
					int result = MessageBox(
						NULL,
						L"Send these files to the Recycle Bin?",
						L"Delete files",
						MB_ICONWARNING | MB_YESNO
					);

					if (result == IDYES) {
						std::vector<int> selected_indexes = ListView_GetSelectedIndexes();
						if (selected_indexes.size() > 0) {
							ContextMenu_DeleteFiles(selected_indexes);
						}
					}
					break;
				}

				case MenuId::Context_Clear: {
					ClearList();
					break;
				}

				default:
					return DefWindowProc(hWnd, message, wParam, lParam);
			}
			break;
		}

		case WM_SIZE:
			SendMessage(global_status_bar, message, wParam, lParam);	// status bar resizes itself
			ResizeListView(global_list_view, hWnd);
			break;

		case WM_NOTIFY:
			return ListViewNotify(hWnd, lParam);

		case WM_DESTROY:
			PostQuitMessage(0);
			break;

		case WM_ACTIVATE :
			SetFocus(global_list_view);
			break;

		case WM_DROPFILES: {
			// collect the dragged files into a job for later

			hashing_job job;
			job.file_paths = CollectDroppedFilePaths(hWnd, wParam);

			std::unique_lock<std::mutex> lock(jobs_exist_mutex);
			hashing_jobs_queue.push(job);
			lock.unlock();

			jobs_exist_cond_var.notify_one();	// notify worker thread of the new job
			break;
		}

		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
	}

	return 0;
}

static bool CreateApplicationWindow(HINSTANCE hInstance, int nCmdShow)
{
	WNDCLASSEXW wcex = {0};
	wcex.cbSize         = sizeof(WNDCLASSEX);
	wcex.style          = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc    = WndProc;
	wcex.cbClsExtra     = 0;
	wcex.cbWndExtra     = 0;
	wcex.hInstance      = hInstance;
	wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPLICATION_ICON));
	wcex.hCursor        = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName   = NULL;	// menu bar is created later
	wcex.lpszClassName  = L"Speedy GonFiles' main window",
	wcex.hIconSm        = NULL;

	RegisterClassEx(&wcex);

	// Perform application initialization:

	global_window = CreateWindow(
		wcex.lpszClassName,
		L"Speedy GonFiles",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
		NULL,
		NULL,
		hInstance,
		NULL);

	if (!global_window) return FALSE;

	DragAcceptFiles(global_window, TRUE);

	if (IsUserAnAdmin()) {
		// If the application is running as administrator, drag-and-drop from a non-elevated process such as the explorer
		// doesn't work because the relevant messages are filtered. To fix that, we have to let the needed messages through.
		ChangeWindowMessageFilterEx(global_window, WM_DROPFILES, MSGFLT_ALLOW, NULL);
		//ChangeWindowMessageFilterEx(global_window, WM_COPYDATA, MSGFLT_ALLOW, NULL);	// not needed if we have WM_COPYGLOBALDATA
		ChangeWindowMessageFilterEx(global_window, 0x0049, MSGFLT_ALLOW, NULL);	// WM_COPYGLOBALDATA
	}

	ShowWindow(global_window, nCmdShow);
	UpdateWindow(global_window);

	return TRUE;
}

// =================================================
// Debug functions
// =================================================

#ifndef PUBLIC_RELEASE

void InitializeOutputConsole()
{
	AllocConsole();
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	SetConsoleTitle(L"Speedy GonFiles: Output Console");
	FILE* file;
	freopen_s(&file, "CONOUT$", "w", stdout);
	freopen_s(&file, "CONOUT$", "w", stderr);
}

#endif

// =================================================
// Main
// =================================================

int APIENTRY wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nCmdShow)
{
	// ---------- Setup ----------

	// prepare things for timers
	LARGE_INTEGER perfCountFrequencyResult;
	QueryPerformanceFrequency(&perfCountFrequencyResult);
	perf_count_frequency = perfCountFrequencyResult.QuadPart;

	// ---------- Debug ----------

	#ifndef PUBLIC_RELEASE
	InitializeOutputConsole();
	#endif

	// ---------- Application ----------

	sort_file_infos(Column::Group);

	if (!CreateApplicationWindow(hInstance, nCmdShow)) return FALSE;

	global_instance = hInstance; // Store instance handle in our global variable

	// Create jobs thread and detach it to avoid the program crashing on exit due to not joining thread.
	// (we don't want to join it since we'd have to wait for its completion).
	std::thread hashing_jobs_thread(process_hashing_jobs_loop);
	hashing_jobs_thread.detach();

	// Main message loop

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return (int) msg.wParam;
}

// =================================================
// =================================================
// =================================================
