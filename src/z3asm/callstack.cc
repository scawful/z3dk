#include "asar.h"
#include "platform/file-helpers.h"

// Shared callstack formatting and location helpers are split out of main.cpp
// to keep the core assembler pipeline code easier to navigate.

bool simple_callstacks = true;

// Shortens target_path to a relative path, but only if it resides
// within base_path or a child directory of it.
static string shorten_to_relative_path(const char* base_path, const char* target_path)
{
	if (stribegin(target_path, base_path)) target_path += strlen(base_path);
	return target_path;
}

static string get_top_level_directory()
{
	string top_level_file_dir;
	for (int i = 0; i < callstack.count; ++i)
	{
		if (callstack[i].type == callstack_entry_type::FILE)
		{
			top_level_file_dir = dir(callstack[i].content);
			break;
		}
	}
	return top_level_file_dir;
}

static string generate_call_details_string(const char* current_block, const char* current_call, int indentation, bool add_lines)
{
	string e;
	if (current_block != nullptr || current_call != nullptr)
	{
		string indent;
		if (add_lines) indent += "|";
		for (; indentation > 0; --indentation) indent += " ";

		if (current_block != nullptr) e += STR "\n"+indent+"in block: ["+current_block+"]";
		if (current_call != nullptr) e += STR "\n"+indent+"in macro call: [%"+current_call+"]";
	}
	return e;
}

static string get_pretty_filename(const char* current_file)
{
	// RPG Hacker: One could make an argument that we shouldn't shorten paths
	// here, since some IDEs support jumping to files by double-clicking their
	// paths. However, AFAIK, no IDE supports this for Asar yet, and if it's
	// ever desired, we could just make it a command line option. Until then,
	// I think it's more important to optimize for pretty command line display.
	return shorten_to_relative_path(get_top_level_directory(), current_file);
}

static string generate_filename_and_line(const char* current_file, int current_line_no)
{
	return STR current_file
		+ (current_line_no>=0?STR ":"+dec(current_line_no+1):STR "");
}

static string format_stack_line(const printable_callstack_entry& entry, int stack_frame_index)
{
	string indent = "\n|   ";
	indent += dec(stack_frame_index);
	indent += ": ";
	// RPG Hacker: We'll probably never have a call stack in the
	// hundreds even, so this very specific, lazy solution suffices.
	if (stack_frame_index < 100) indent += " ";
	if (stack_frame_index < 10) indent += " ";
	return indent
		+ generate_filename_and_line(entry.prettypath, entry.lineno)
		+ entry.details;
}

static void push_stack_line(autoarray<printable_callstack_entry>* out, const char* current_file, const char* current_block, const char* current_call, int current_line_no, int indentation, bool add_lines)
{
	printable_callstack_entry new_entry;
	new_entry.fullpath = current_file;
	new_entry.prettypath = get_pretty_filename(current_file);
	new_entry.lineno = current_line_no;
	new_entry.details = generate_call_details_string(current_block, current_call, indentation, add_lines).raw();
	out->append(new_entry);
}

void get_current_line_details(string* location, string* details, bool exclude_block)
{
	const char* current_file = nullptr;
	const char* current_block = nullptr;
	const char* current_call = nullptr;
	int current_line_no = -1;
	for (int i = callstack.count-1; i >= 0 ; --i)
	{
		switch (callstack[i].type)
		{
			case callstack_entry_type::FILE:
				current_file = callstack[i].content;
				if (exclude_block) current_block = nullptr;
				*location = generate_filename_and_line(get_pretty_filename(current_file), current_line_no);
				*details = generate_call_details_string(current_block, current_call, 4, false);
				return;
			case callstack_entry_type::MACRO_CALL:
				if (current_call == nullptr) current_call = callstack[i].content;
				break;
			case callstack_entry_type::LINE:
				if (current_block == nullptr && current_call == nullptr) current_block = callstack[i].content;
				if (current_line_no == -1) current_line_no = callstack[i].lineno;
				break;
			case callstack_entry_type::BLOCK:
				if (current_block == nullptr) current_block = callstack[i].content;
				break;
		}
	}
	*location = "";
	*details = "";
}

void get_full_printable_callstack(autoarray<printable_callstack_entry>* out, int indentation, bool add_lines)
{
	out->reset();
	const char* current_file = nullptr;
	const char* current_block = nullptr;
	const char* current_call = nullptr;
	int current_line_no = -1;
	for (int i = 0; i < callstack.count; ++i)
	{
		switch (callstack[i].type)
		{
			case callstack_entry_type::FILE:
				if (current_file != nullptr)
				{
					push_stack_line(out, current_file, current_block, current_call, current_line_no, indentation, add_lines);
				}
				current_file = callstack[i].content;
				current_block = nullptr;
				current_call = nullptr;
				current_line_no = -1;
				break;
			case callstack_entry_type::MACRO_CALL:
				current_block = nullptr;
				current_call = callstack[i].content;
				break;
			case callstack_entry_type::LINE:
				current_line_no = callstack[i].lineno;
				current_block = callstack[i].content;
				break;
			case callstack_entry_type::BLOCK:
				current_block = callstack[i].content;
				break;
		}
	}
}

static string get_full_callstack()
{
	autoarray<printable_callstack_entry> printable_stack;
	get_full_printable_callstack(&printable_stack, 12, true);

	string e;
	if (printable_stack.count > 0)
	{
		e += "\nFull call stack:";
		for (int i = printable_stack.count-1; i >= 0; --i)
		{
			e += format_stack_line(printable_stack[i], i);
		}
	}
	return e;
}

// RPG Hacker: This function essetially replicates classic Asar behavior
// of only printing a single macro call below the current level.
static string get_simple_callstack()
{
	int i;
	const char* current_call = nullptr;
	for (i = callstack.count-1; i >= 0 ; --i)
	{
		if (callstack[i].type == callstack_entry_type::MACRO_CALL)
		{
			current_call = callstack[i].content;
			break;
		}
	}

	const char* current_file = nullptr;
	int current_line_no = -1;
	if (current_call != nullptr)
	{
		bool stop = false;
		for (int j = i-1; j >= 0 ; --j)
		{
			switch (callstack[j].type)
			{
				case callstack_entry_type::FILE:
					if (current_file != nullptr)
					{
						stop = true;
						break;
					}
					current_file = callstack[j].content;
					break;
				case callstack_entry_type::MACRO_CALL:
					stop = true;
					break;
				case callstack_entry_type::LINE:
					if (current_line_no == -1) current_line_no = callstack[j].lineno;
					break;
				case callstack_entry_type::BLOCK:
					break;
			}

			if (current_file != nullptr && current_line_no != -1) stop = true;

			if (stop) break;
		}
	}

	string e;
	if (current_call != nullptr && current_file != nullptr)
	{
		e += STR "\n    called from: " + generate_filename_and_line(get_pretty_filename(current_file), current_line_no)
			+ ": [%" + current_call + "]";
	}
	return e;
}

string get_callstack()
{
	if (simple_callstacks)
		return get_simple_callstack();
	else
		return get_full_callstack();
}
