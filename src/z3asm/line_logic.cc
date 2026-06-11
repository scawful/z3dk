#include "line_logic.h"

#include "assembleblock.h"
#include "asar_math.h"
#include "macro.h"

autoarray<string> macro_defs;
int in_macro_def=0;

autoarray<string> hook_defs;
int in_hook_def=0;

bool do_line_logic(const char* line, const char* filename, int lineno)
{
	int prevnumif = numif;
	int single_line_for_tracker = 1;
	try
	{
		string current_line;
		if (numif==numtrue || (numtrue+1==numif && stribegin(line, "elseif ")))
		{
			callstack_push cs_push(callstack_entry_type::LINE, line, lineno);
			string tmp=replace_macro_args(line);
			tmp.qnormalize();
			resolvedefines(current_line, tmp);
			if (!confirmquotes(current_line)) asar_throw_error(0, error_type_line, error_id_mismatched_quotes);
		}
		else current_line=line;

		callstack_push cs_push(callstack_entry_type::LINE, current_line, lineno);

		string trimmed_line = current_line;
		strip_whitespace(trimmed_line);

		if (stribegin(trimmed_line, "macro ") && numif==numtrue)
		{
			// RPG Hacker: Slight redundancy here with code that is
			// also in startmacro(). Could improve this for Asar 2.0.
			string macro_name = trimmed_line.data()+6;
			char * startpar=strqchr(macro_name.data(), '(');
			if (startpar) *startpar=0;
			macro_defs.append(macro_name);

			// RPG Hacker: I think it would make more logical sense
			// to have this ++ after the if, but hat breaks compatibility
			// with at least one test, and it generally leads to more
			// errors being output after a broken macro declaration.
			in_macro_def++;
			if (!pass)
			{
				if (in_macro_def == 1) startmacro(current_line.data()+6);
				else tomacro(current_line);
			}
		}
		else if (!stricmp(trimmed_line, "endmacro") && numif==numtrue)
		{
			if (in_macro_def == 0) asar_throw_error(0, error_type_line, error_id_misplaced_endmacro);
			else
			{
				in_macro_def--;
				macro_defs.remove(in_macro_def);
				if (!pass)
				{
					if (in_macro_def == 0) endmacro(true);
					else tomacro(current_line);
				}
			}
		}
		        else if (stribegin(trimmed_line, "hook ") && numif==numtrue)
		        {
		            in_hook_def++;
		            hook_defs.append(trimmed_line);
		            if (!pass)
		            {
		                if (in_hook_def == 1) 
		                {
		                    const char* p = trimmed_line.data() + 5;
		                    while (is_space(*p)) p++;
		                    string addr_str;
		                    while (*p && !is_space(*p) && *p != ',') addr_str += *p++;
		                    unsigned int addr = (unsigned int)getnum(addr_str.data());
		                    
		                    char macro_name[64];
		                    sprintf(macro_name, "___z3dk_hook_at_%06X", addr);
		                    startmacro(string(macro_name) + "()");
		                }
		                else tomacro(current_line);
		            }
		        }
		        else if (!stricmp(trimmed_line, "endhook") && numif==numtrue)
		        {
		            if (in_hook_def == 0) asar_throw_error(0, error_type_line, error_id_endhook_without_hook);
		            else
		            {
		                in_hook_def--;
		                string hook_cmd = hook_defs[in_hook_def];
		                hook_defs.remove(in_hook_def);
		                if (!pass)
		                {
		                    if (in_hook_def == 0)
		                    {
		                        endmacro(true);
		                        
		                        const char* p = hook_cmd.data() + 5;
		                        while (is_space(*p)) p++;
		                        string addr_str;
		                        while (*p && !is_space(*p) && *p != ',') addr_str += *p++;
		                        unsigned int addr = (unsigned int)getnum(addr_str.data());
		                        while (*p && (is_space(*p) || *p == ',')) p++;
		                        string type_str;
		                        while (*p && !is_space(*p)) type_str += *p++;
		                        if (type_str == "") type_str = "jsl";
		
		                        char macro_name[64];
		                        sprintf(macro_name, "___z3dk_hook_at_%06X", addr);
		                        string stripped_args = string(macro_name) + "|$" + hex(addr, 6).data() + "|" + type_str.data();
		                        string final_cmd = string("hook_internal ") + stripped_args.data();
		                        assembleblock(final_cmd.data(), single_line_for_tracker);
		                    }
		                    else tomacro(current_line);
		                }
		                else
		                {
		                    if (in_hook_def == 0)
		                    {
		                        const char* p = hook_cmd.data() + 5;
		                        while (is_space(*p)) p++;
		                        string addr_str;
		                        while (*p && !is_space(*p) && *p != ',') addr_str += *p++;
		                        unsigned int addr = (unsigned int)getnum(addr_str.data());
		                        while (*p && (is_space(*p) || *p == ',')) p++;
		                        string type_str;
		                        while (*p && !is_space(*p)) type_str += *p++;
		                        if (type_str == "") type_str = "jsl";
		
		                        char macro_name[64];
		                        sprintf(macro_name, "___z3dk_hook_at_%06X", addr);
		                        string stripped_args = string(macro_name) + "|$" + hex(addr, 6).data() + "|" + type_str.data();
		                        string final_cmd = string("hook_internal ") + stripped_args.data();
		                        assembleblock(final_cmd.data(), single_line_for_tracker);
		                    }
		                }
		            }
		        }		else if (in_macro_def > 0)
		{
			if (!pass) tomacro(current_line);
		}
		else if (in_hook_def > 0)
		{
			if (!pass) tomacro(current_line);
		}
		else
		{
			assembleline(filename, lineno, current_line, single_line_for_tracker);
		}
	}
	catch (errline&) {}
	return (numif != prevnumif || single_line_for_tracker == 3)
		&& (whilestatus[numif].iswhile || whilestatus[numif].is_for);
}
