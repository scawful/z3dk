#include "defines_pipeline.h"

#include "asar.h"
#include "asar_math.h"

bool validatedefinename(const char * name)
{
	if (!name[0]) return false;
	for (int i = 0;name[i];i++)
	{
		if (!is_ualnum(name[i])) return false;
	}

	return true;
}

void resolvedefines(string& out, const char * start)
{
	recurseblock rec;
	const char * here=start;
	if (!strchr(here, '!'))
	{
		out += here;
		return;
	}
	while (*here)
	{
		if (here[0] == '\\' && here[1] == '\\')
		{
			// allow using \\ as escape sequence
			if (in_macro_def > 0) out += "\\";
			out += "\\";
			here += 2;
		}
		else if (here[0] == '\\' && here[1] == '!')
		{
			// allow using \! to escape !
			if (in_macro_def > 0) out += "\\";
			out+="!";
			here += 2;
		}
		else if (*here=='!')
		{
			bool first=(here==start || (here>=start+4 && here[-1]==' ' && here[-2]==':' && here[-3]==' '));//check if it's the start of a block
			string defname;
			here++;

			int depth = 0;
			for (const char* depth_str = here; *depth_str=='^'; depth_str++)
			{
				depth++;
			}
			here += depth;

			if (depth != in_macro_def)
			{
				out += '!';
				for (int i=0; i < depth; ++i) out += '^';
				if (depth > in_macro_def) asar_throw_error(0, error_type_line, error_id_invalid_depth_resolve, "define", "define", depth, in_macro_def);
				continue;
			}

			if (*here=='{')
			{
				here++;
				string unprocessedname;
				int braces=1;
				while (true)
				{
					if (*here=='{') braces++;
					if (*here=='}') braces--;
					if (!*here) asar_throw_error(0, error_type_line, error_id_mismatched_braces);
					if (!braces) break;
					unprocessedname+=*here++;
				}
				here++;
				resolvedefines(defname, unprocessedname);
				if (!validatedefinename(defname)) asar_throw_error(0, error_type_line, error_id_invalid_define_name);
			}
			else
			{
				while (is_ualnum(*here)) defname+=*here++;
			}

			if (first)
			{
				enum {
					null,
					append,
					expand,
					domath,
					setifnotset,
				} mode;
				if(0);
				else if (stribegin(here,  " = ")) { here+=3; mode=null; }
				else if (stribegin(here, " += ")) { here+=4; mode=append; }
				else if (stribegin(here, " := ")) { here+=4; mode=expand; }
				else if (stribegin(here, " #= ")) { here+=4; mode=domath; }
				else if (stribegin(here, " ?= ")) { here+=4; mode=setifnotset; }
				else goto notdefineset;
				string val;
				if (*here=='"')
				{
					here++;
					while (true)
					{
						if (*here=='"')
						{
							if (!here[1] || here[1]==' ') break;
							else if (here[1]=='"') here++;
							else asar_throw_error(0, error_type_line, error_id_broken_define_declaration);
						}
						val+=*here++;
					}
					here++;
				}
				else
				{
					while (*here && *here!=' ') val+=*here++;
				}
				//if (strqchr(val.data(), ';')) *strqchr(val.data(), ';')=0;
				if (*here && !stribegin(here, " : ")) asar_throw_error(0, error_type_line, error_id_broken_define_declaration);
				// RPG Hacker: Is it really a good idea to normalize
				// the content of defines? That kinda violates their
				// functionality as a string replacement mechanism.
				//val.qnormalize();

				// RPG Hacker: throw an error if we're trying to overwrite built-in defines.
				if (builtindefines.exists(defname))
				{
					asar_throw_error(0, error_type_line, error_id_overriding_builtin_define, defname.data());
				}

				switch (mode)
				{
					case null:
					{
						defines.create(defname) = val;
						break;
					}
					case append:
					{
						if (!defines.exists(defname)) asar_throw_error(0, error_type_line, error_id_define_not_found, defname.data());
						string oldval = defines.find(defname);
						val=oldval+val;
						defines.create(defname) = val;
						break;
					}
					case expand:
					{
						string newval;
						resolvedefines(newval, val);
						defines.create(defname) = newval;
						break;
					}
					case domath:
					{
						string newval;
						resolvedefines(newval, val);
						double num= getnumdouble(newval);
						if (foundlabel && !foundlabel_static) asar_throw_error(0, error_type_line, error_id_define_label_math);
						defines.create(defname) = ftostr(num);
						break;
					}
					case setifnotset:
					{
						if (!defines.exists(defname)) defines.create(defname) = val;
						break;
					}
				}
			}
			else
			{
			notdefineset:
				if (!defname) out+="!";
				else
				{
					if (!defines.exists(defname)) asar_throw_error(0, error_type_line, error_id_define_not_found, defname.data());
					else {
						string thisone = defines.find(defname);
						resolvedefines(out, thisone);
					}
				}
			}
		}
		else out+=*here++;
	}
	if (!confirmquotes(out)) { asar_throw_error(0, error_type_null, error_id_mismatched_quotes); out = ""; }
}
