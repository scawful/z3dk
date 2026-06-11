#include "source_pipeline.h"

#include "asar.h"

#include "assembleblock.h"
#include "line_logic.h"
#include "macro.h"

struct sourcefile
{
	char* data;
	char** contents;
	int numlines;
};

static assocarr<sourcefile> filecontents;

#define cfree(x) free((void*)x)
static void clearfile(const string & key, sourcefile& filecontent)
{
	(void)key;
	cfree(filecontent.data);
	cfree(filecontent.contents);
}
#undef cfree

// Returns true if a file is protected via
// an "includeonce".
bool file_included_once(const char* file)
{
	for (int i = 0; i < includeonce.count; ++i)
	{
		if (includeonce[i] == file)
		{
			return true;
		}
	}

	return false;
}

void assemblefile(const char * filename)
{
	incsrcdepth++;
	string absolutepath = filesystem->create_absolute_path(get_current_file_name(), filename);

	if (file_included_once(absolutepath))
	{
		return;
	}

	callstack_push cs_push(callstack_entry_type::FILE, absolutepath);

	sourcefile file;
	file.contents = nullptr;
	file.numlines = 0;
	int startif=numif;
	if (!filecontents.exists(absolutepath))
	{
		char * temp = readfile(absolutepath, "");
		if (!temp)
		{
			asar_throw_error(0, error_type_null, vfile_error_to_error_id(asar_get_last_io_error()), filename);

			return;
		}
		sourcefile& newfile = filecontents.create(absolutepath);
		newfile.contents =split(temp, '\n');
		newfile.data = temp;
		for (int i=0;newfile.contents[i];i++)
		{
			newfile.numlines++;
			char * line = newfile.contents[i];
			int i_temp = i;
			char * comment;
			while((comment = strqchr(line, ';'))) {
				if(comment[1] == '[' && comment[2] == '[') {
					// block comment - find where it ends
					char* theline = comment + 3;
					char* comment_end = strstr(theline, "]]");
					while(comment_end == nullptr) {
						i_temp++;
						char* new_line = newfile.contents[i_temp];
						if(new_line == nullptr) {
							callstack_push cs_push(callstack_entry_type::LINE, line, i);
							asar_throw_error(0, error_type_null, error_id_unclosed_block_comment);
							// make sure this line is still parsed correctly
							*comment = 0;
							// but don't go looking at any other lines
							goto break_outer;
						}
						comment_end = strstr(new_line, "]]");
						// this line is itself part of the comment, so ignore it
						//new_line[0] = 0;
						// except not like that^, because that will break the
						// memmove below
						static char junk[]="";
						// using a static here should be fine, since if the line
						// doesn't contain ',' or '\' we won't go mutating it
						newfile.contents[i_temp] = junk;
					}
					// comment_end+2 is a valid pointer, since comment_end is
					// guaranteed to start with ]]
					comment_end += 2;
					// stitch together the part of the line before the comment,
					// and the part of the line after it
					memmove(comment, comment_end, strlen(comment_end) + 1);
					// and then recheck for ; in the line again...
				} else {
					*comment = 0;
				}
			}
		break_outer:
			if (!confirmquotes(line)) {
				callstack_push cs_push(callstack_entry_type::LINE, line, i);
				asar_throw_error(0, error_type_null, error_id_mismatched_quotes);
				line[0] = '\0';
			}
			newfile.contents[i] = strip_whitespace(line);
		}
		for(int i=0;newfile.contents[i];i++)
		{
			char* line = newfile.contents[i];
			if(!*line) continue;
			for (int j=1;line[strlen(line) - 1] == ',' && newfile.contents[i+j];j++)
			{
				// not using strcat because the source and dest overlap here
				char* otherline = newfile.contents[i+j];
				char* line_end = line + strlen(line);
				while(*otherline) *line_end++ = *otherline++;
				*line_end = '\0';
				static char nullstr[]="";
				newfile.contents[i+j]=nullstr;
			}
		}
		file = newfile;
	} else { // filecontents.exists(absolutepath)
		file = filecontents.find(absolutepath);
	}
	asarverallowed=true;
	for (int i=0;file.contents[i] && i<file.numlines;i++)
	{
		string connectedline;
		int skiplines = getconnectedlines<char**>(file.contents, i, connectedline);

		bool was_loop_end = do_line_logic(connectedline, absolutepath, i);
		i += skiplines;

		// if a loop ended on this line, should it run again?
		if (was_loop_end && whilestatus[numif].cond)
			i = whilestatus[numif].startline - 1;
	}
	while (in_macro_def > 0)
	{
		asar_throw_error(0, error_type_null, error_id_unclosed_macro, macro_defs[in_macro_def-1].data());
		if (!pass && in_macro_def == 1) endmacro(false);
		in_macro_def--;
		macro_defs.remove(in_macro_def);
	}
	while (in_hook_def > 0)
	{
		asar_throw_error(0, error_type_null, error_id_hook_without_endhook);
		in_hook_def--;
		hook_defs.remove(in_hook_def);
	}
	if (numif!=startif)
	{
		numif=startif;
		numtrue=startif;
		asar_throw_error(0, error_type_null, error_id_unclosed_if);
	}
	incsrcdepth--;
}

void reset_source_file_cache()
{
	filecontents.each(clearfile);
	filecontents.reset();
}
