// "because satanism is best defeated by summoning a bigger satan"
//   ~Alcaro, 2019 (discussing Asar)
#include "addr2line.h"
#include "asar.h"
#include "virtualfile.h"
#include "platform/file-helpers.h"
#include "assembleblock.h"
#include "asar_math.h"
#include "defines_pipeline.h"
#include "line_logic.h"
#include "macro.h"
#include "source_pipeline.h"
#include <ctime>
// randomdude999: remember to also update the .rc files (in res/windows/) when changing this.
// Couldn't find a way to automate this without shoving the version somewhere in the CMake files
const int asarver_maj=2;
const int asarver_min=0;
const int asarver_bug=0;
const bool asarver_beta=true;

#ifdef _I_RELEASE
extern char blockbetareleases[(!asarver_beta)?1:-1];
#endif
#ifdef _I_DEBUG
extern char blockreleasedebug[(asarver_beta)?1:-1];
#endif

unsigned const char * romdata_r;
int romlen_r;

int pass;

int optimizeforbank=-1;
int optimize_dp = optimize_dp_flag::ALWAYS;
int dp_base = 0;
int optimize_address = optimize_address_flag::MIRRORS;

autoarray<callstack_entry> callstack;

bool errored=false;
bool ignoretitleerrors=false;

volatile int recursioncount=0;

virtual_filesystem* filesystem = nullptr;

AddressToLineMapping addressToLineMapping;

int get_version_int()
{
	return asarver_maj * 10000 + asarver_min * 100 + asarver_bug;
}

bool setmapper()
{
	int maxscore=-99999;
	mapper_t bestmap=lorom;
	mapper_t maps[]={lorom, hirom, exlorom, exhirom};
	for (size_t mapid=0;mapid<sizeof(maps)/sizeof(maps[0]);mapid++)
	{
		mapper=maps[mapid];
		int score=0;
		int highbits=0;
		bool foundnull=false;
		for (int i=0;i<21;i++)
		{
			unsigned char c=romdata[snestopc(0x00FFC0+i)];
			if (foundnull && c) score-=4;//according to some documents, NUL terminated names are possible - but they shouldn't appear in the middle of the name
			if (c>=128) highbits++;
			else if (is_upper(c)) score+=3;
			else if (c==' ') score+=2;
			else if (is_digit(c)) score+=1;
			else if (is_lower(c)) score+=1;
			else if (c=='-') score+=1;
			else if (!c) foundnull=true;
			else score-=3;
		}
		if (highbits>0 && highbits<=14) score-=21;//high bits set on some, but not all, bytes = unlikely to be a ROM
		if ((romdata[snestopc(0x00FFDE)]^romdata[snestopc(0x00FFDC)])!=0xFF ||
				(romdata[snestopc(0x00FFDF)]^romdata[snestopc(0x00FFDD)])!=0xFF) score=-99999;//checksum doesn't match up to 0xFFFF? Not a ROM.
		//too lazy to check the real checksum
		if (score>maxscore)
		{
			maxscore=score;
			bestmap=mapper;
		}
	}
	mapper=bestmap;

	//detect oddball mappers
	int mapperbyte=romdata[snestopc(0x00FFD5)];
	int romtypebyte=romdata[snestopc(0x00FFD6)];
	if (mapper==lorom)
	{
		if (mapperbyte==0x23 && (romtypebyte==0x32 || romtypebyte==0x34 || romtypebyte==0x35)) mapper=sa1rom;
	}
	return (maxscore>=0);
}


// Callstack helpers moved to callstack.cc to keep this file focused on assembly flow.

asar_error_id vfile_error_to_error_id(virtual_file_error vfile_error)
{
	switch (vfile_error)
	{
	case vfe_doesnt_exist:
		return error_id_file_not_found;
	case vfe_access_denied:
		return error_id_failed_to_open_file_access_denied;
	case vfe_not_regular_file:
		return error_id_failed_to_open_not_regular_file;
	case vfe_unknown:
	case vfe_none:
	case vfe_num_errors:
		return error_id_failed_to_open_file;
	}

	return error_id_failed_to_open_file;
}

virtual_file_error asar_get_last_io_error()
{
	if (filesystem != nullptr)
	{
		return filesystem->get_last_error();
	}

	return vfe_unknown;
}

static bool freespaced;
static int getlenforlabel(snes_label thislabel, bool exists)
{
	unsigned int bank = thislabel.pos>>16;
	unsigned int word = thislabel.pos&0xFFFF;
	bool lblfreespace = thislabel.freespace_id > 0;
	unsigned int relaxed_bank;
	if(optimizeforbank >= 0) {
		relaxed_bank = optimizeforbank;
	} else {
		if(freespaceid == 0) {
			relaxed_bank = snespos >> 16;
		} else {
			int target_bank = freespaces[freespaceid].bank;
			if(target_bank == -2) relaxed_bank = 0;
			else if(target_bank == -1) relaxed_bank = 0x40;
			else relaxed_bank = target_bank;
		}
	}
	if (!exists)
	{
		return 2;
	}
	else if((optimize_dp == optimize_dp_flag::RAM) && bank == 0x7E && (word-dp_base < 0x100) && !lblfreespace)
	{
		return 1;
	}
	else if(optimize_dp == optimize_dp_flag::ALWAYS && (bank == 0x7E || !(bank & 0x40)) && (word-dp_base < 0x100) && !lblfreespace)
	{
		return 1;
	}
	else if (
		// if we should optimize ram accesses...
		(optimize_address == optimize_address_flag::RAM || optimize_address == optimize_address_flag::MIRRORS)
		// and we're in a bank with ram mirrors... (optimizeforbank=0x7E is checked later)
		&& !(relaxed_bank & 0x40)
		// and the label is in low RAM
		&& bank == 0x7E && word < 0x2000 && !lblfreespace)
	{
		return 2;
	}
	else if (
		// if we should optimize mirrors...
		optimize_address == optimize_address_flag::MIRRORS
		// we're in a bank with ram mirrors...
		&& !(relaxed_bank & 0x40)
		// and the label is in a mirrored section
		&& !(bank & 0x40) && word < 0x8000 && !lblfreespace)
	{
		return 2;
	}
	else if (optimizeforbank>=0)
	{
		// if optimizing for a specific bank:
		// if the label is in freespace, never optimize
		if (thislabel.freespace_id > 0) return 3;
		else if (bank==(unsigned int)optimizeforbank) return 2;
		else return 3;
	}
	else if (thislabel.freespace_id > 0 || freespaceid > 0)
	{
		// optimize only if the label is in the same freespace
		// TODO: check whether they're pinned to the same bank
		if (thislabel.freespace_id != freespaceid) return 3;
		else return 2;
	}
	else if ((int)bank != snespos >> 16){ return 3; }
	else { return 2;}
}


bool is_hex_constant(const char* str){
	if (*str=='$')
	{
		str++;
		while(is_xdigit(*str)) {
			str++;
		}
		if(*str=='\0'){
			return true;
		}
	}
	return false;
}

int getlen(const char * orgstr, bool optimizebankextraction)
{
	const char * str=orgstr;
	freespaced=false;

	const char* posneglabel = str;
	string posnegname = posneglabelname(&posneglabel, false);

	if (posnegname.length() > 0)
	{
		if (*posneglabel != '\0') goto notposneglabel;

		if (!pass) return 2;
		snes_label label_data;
		// RPG Hacker: Umm... what kind of magic constant is this?
		label_data.pos = 31415926;
		bool found = labelval(posnegname, &label_data);
		return getlenforlabel(label_data, found);
	}
notposneglabel:
	int len=0;
	while (*str)
	{
		int thislen=0;
		bool maybebankextraction=(str==orgstr);
		if (*str=='$')
		{
			str++;
			int i;
			for (i=0;is_xdigit(str[i]);i++);
			//if (i&1) warn(S dec(i)+"-digit hex value");//blocked in getnum instead
			thislen=(i+1)/2;
			str+=i;
		}
		else if (*str=='%')
		{
			str++;
			int i;
			for (i=0;str[i]=='0' || str[i]=='1';i++);
			//if (i&7) warn(S dec(i)+"-digit binary value");
			thislen=(i+7)/8;
			str+=i;
		}
		else if (str[0]=='\'' && str[2]=='\'')
		{
			thislen=1;
			str+=3;
		}
		else if (is_digit(*str))
		{
			int val=strtol(str, const_cast<char**>(&str), 10);
			if (val>=0) thislen=1;
			if (val>=256) thislen=2;
			if (val>=65536) thislen=3;
		}
		else if (is_ualpha(*str) || *str=='.' || *str=='?')
		{
			snes_label thislabel;
			bool exists=labelval(&str, &thislabel);
			thislen=getlenforlabel(thislabel, exists);
		}
		else str++;
		if (optimizebankextraction && maybebankextraction &&
				(!strcmp(str, ">>16") || !strcmp(str, "/65536") || !strcmp(str, "/$10000")))
					return 1;
		if (thislen>len) len=thislen;
	}
	return len;
}

struct strcompare {
	bool operator() (const char * lhs, const char * rhs) const
	{
		return strcmp(lhs, rhs)<0;
	}
};

struct stricompare {
	bool operator() (const char * lhs, const char * rhs) const
	{
		return stricmp(lhs, rhs)<0;
	}
};

assocarr<string> defines;
// needs to be separate because defines is reset between parsing arguments and patching
assocarr<string> clidefines;
assocarr<string> builtindefines;

bool moreonline;
bool asarverallowed = false;

void assembleline(const char * fname, int linenum, const char * line, int& single_line_for_tracker)
{
	recurseblock rec;
	bool moreonlinetmp=moreonline;
	// randomdude999: redundant, assemblefile already converted the path to absolute
	//string absolutepath = filesystem->create_absolute_path("", fname);
	string absolutepath = fname;
	single_line_for_tracker = 1;
	try
	{
		string out=line;
		autoptr<char**> blocks=qsplitstr(out.temp_raw(), " : ");
		moreonline=true;
		for (int block=0;moreonline;block++)
		{
			moreonline=(blocks[block+1] != nullptr);
			try
			{
				// it's possible that our input looks something like:
				// nop : : nop
				// nop : : : : : nop
				// also, it's possible that there were empty blocks at the start or end of the line:
				// : nop :
				// after qsplit, we still need to deal with possibly a single ": " from a preceding empty block,
				// and if it's the last block, possibly a following " :".
				string stripped_block = strip_whitespace(blocks[block]);
				int i = 0;
				// if the block starts with ": "
				if(stripped_block[i] == ':' && stripped_block[i+1] == ' ') {
					i++;
					while(stripped_block[i] == ' ') i++;
				}
				// if the block is a single :, skip that too.
				if(stripped_block[i] == ':' && stripped_block[i+1] == 0) i++;

				// last block - strip trailing " :" if present.
				if(!moreonline && stripped_block.length() >= 2 && stripped_block[stripped_block.length()-2] == ' ' && stripped_block[stripped_block.length()-1] == ':') {
					stripped_block.truncate(stripped_block.length()-2);
				}

				callstack_push cs_push(callstack_entry_type::BLOCK, stripped_block.data() + i);

				assembleblock(stripped_block.data() + i, single_line_for_tracker);
				checkbankcross();
			}
			catch (errblock&) {}
			if (blocks[block][0]!='\0') asarverallowed=false;
			if(single_line_for_tracker == 1) single_line_for_tracker = 0;
		}
	}
	catch (errline&) {}
	moreonline=moreonlinetmp;
}

int incsrcdepth=0;


void parse_std_includes(const char* textfile, autoarray<string>& outarray)
{
	char* content = readfilenative(textfile);

	if (content != nullptr)
	{
		char* pos = content;

		while (pos[0] != '\0')
		{
			string stdinclude;

			do
			{
				if (pos[0] != '\r' && pos[0] != '\n')
				{
					stdinclude += pos[0];
				}
				pos++;
			} while (pos[0] != '\0' && pos[0] != '\n');

			strip_whitespace(stdinclude);

			if (stdinclude != "")
			{
				if (!path_is_absolute(stdinclude))
				{
					stdinclude = dir(textfile) + stdinclude;
				}
				outarray.append(normalize_path(stdinclude));
			}
		}

		free(content);
	}
}

void parse_std_defines(const char* textfile)
{

	// RPG Hacker: add built-in defines.
	// (They're not really standard defines, but I was lazy and this was
	// one convenient place for doing it).
	builtindefines.create("assembler") = "asar";
	builtindefines.create("assembler_ver") = dec(get_version_int());
	builtindefines.create("assembler_time") = dec(time(nullptr));

	if(textfile == nullptr) return;

	char* content = readfilenative(textfile);

	if (content != nullptr)
	{
		char* pos = content;
		while (*pos != 0) {
			string define_name;
			string define_val;

			while (*pos != '=' && *pos != '\n') {
				if(*pos == '\r') { pos++; continue; }
				define_name += *pos;
				pos++;
			}
			if (*pos != 0 && *pos != '\r' && *pos != '\n') pos++; // skip =
			while (*pos != 0 && *pos != '\n') {
				if(*pos == '\r') { pos++; continue; }
				define_val += *pos;
				pos++;
			}
			if (*pos != 0)
				pos++; // skip \n
			// clean define_name
			strip_whitespace(define_name);
			define_name.strip_prefix('!'); // remove leading ! if present

			if (define_name == "")
			{
				if (define_val == "")
				{
					continue;
				}

				asar_throw_error(pass, error_type_null, error_id_stddefines_no_identifier);
			}

			if (!validatedefinename(define_name)) asar_throw_error(pass, error_type_null, error_id_cmdl_define_invalid, "stddefines.txt", define_name.data());

			// clean define_val
			const char* defval = define_val.data();
			string cleaned_defval;

			if (*defval == 0) {
				// no value
				if (clidefines.exists(define_name)) asar_throw_error(pass, error_type_null, error_id_cmdl_define_override, "Std define", define_name.data());
				clidefines.create(define_name) = "";
				continue;
			}

			while (*defval == ' ' || *defval == '\t') defval++; // skip whitespace in beginning
			if (*defval == '"') {
				defval++; // skip opening quote
				while (*defval != '"' && *defval != 0)
					cleaned_defval += *defval++;

				if (*defval == 0) {
					asar_throw_error(pass, error_type_null, error_id_mismatched_quotes);
				}
				defval++; // skip closing quote
				while (*defval == ' ' || *defval == '\t') defval++; // skip whitespace
				if (*defval != 0 && *defval != '\n')
					asar_throw_error(pass, error_type_null, error_id_stddefine_after_closing_quote);

				if (clidefines.exists(define_name)) asar_throw_error(pass, error_type_null, error_id_cmdl_define_override, "Std define", define_name.data());
				clidefines.create(define_name) = cleaned_defval;
				continue;
			}
			else
			{
				// slightly hacky way to remove trailing whitespace
				const char* defval_end = strchr(defval, '\n'); // slightly hacky way to get end of string or newline
				if (!defval_end) defval_end = strchr(defval, 0);
				defval_end--;
				while (*defval_end == ' ' || *defval_end == '\t') defval_end--;
				cleaned_defval = string(defval, (int)(defval_end - defval + 1));

				if (clidefines.exists(define_name)) asar_throw_error(pass, error_type_null, error_id_cmdl_define_override, "Std define", define_name.data());
				clidefines.create(define_name) = cleaned_defval;
				continue;
			}

		}
		free(content);
	}
}

bool checksum_fix_enabled = true;
bool force_checksum_fix = false;

#define cfree(x) free((void*)x)
static void clearmacro(const string & key, macrodata* & macro)
{
	(void)key;
	freemacro(macro);
}
#undef cfree

static void adddefine(const string & key, string & value)
{
	if (!defines.exists(key)) defines.create(key) = value;
}

static string symbolfile;

static void printsymbol_wla(const string& key, snes_label& label)
{
	string line = hex((label.pos & 0xFF0000)>>16, 2)+":"+hex(label.pos & 0xFFFF, 4)+" "+key+"\n";
	symbolfile += line;
}

static void printsymbol_nocash(const string& key, snes_label& label)
{
	string line = hex(label.pos & 0xFFFFFF, 8)+" "+key+"\n";
	symbolfile += line;
}

string create_symbols_file(string format, uint32_t romCrc){
	format = lower(format);
	symbolfile = "";
	if(format == "wla")
	{
		symbolfile =  "; wla symbolic information file\n";
		symbolfile += "; generated by asar\n";

		symbolfile += "\n[labels]\n";
		labels.each(printsymbol_wla);

		symbolfile += "\n[source files]\n";
		const autoarray<AddressToLineMapping::FileInfo>& addrToLineFileList = addressToLineMapping.getFileList();
		for (int i = 0; i < addrToLineFileList.count; ++i)
		{
			char addrToFileListStr[256];
			snprintf(addrToFileListStr, 256, "%.4x %.8x %s\n",
				i,
				addrToLineFileList[i].fileCrc,
				addrToLineFileList[i].filename.data()
			);
			symbolfile += addrToFileListStr;
		}

		symbolfile += "\n[rom checksum]\n";
		{
			char romCrcStr[32];
			snprintf(romCrcStr, 32, "%.8x\n",
				romCrc
			);
			symbolfile += romCrcStr;
		}

		symbolfile += "\n[addr-to-line mapping]\n";
		const autoarray<AddressToLineMapping::AddrToLineInfo>& addrToLineInfo = addressToLineMapping.getAddrToLineInfo();
		for (int i = 0; i < addrToLineInfo.count; ++i)
		{
			char addrToLineStr[32];
			snprintf(addrToLineStr, 32, "%.2x:%.4x %.4x:%.8x\n",
				(addrToLineInfo[i].addr & 0xFF0000) >> 16,
				addrToLineInfo[i].addr & 0xFFFF,
				addrToLineInfo[i].fileIdx & 0xFFFF,
				addrToLineInfo[i].line & 0xFFFFFFFF
			);
			symbolfile += addrToLineStr;
		}

	}
	else if (format == "nocash")
	{
		symbolfile = ";no$sns symbolic information file\n";
		symbolfile += ";generated by asar\n";
		symbolfile += "\n";
		labels.each(printsymbol_nocash);
	}
	return symbolfile;
}


bool in_top_level_file()
{
	int num_files = 0;
	for (int i = callstack.count-1; i >= 0; --i)
	{
		if (callstack[i].type == callstack_entry_type::FILE)
		{
			num_files++;
			if (num_files > 1) break;
		}
	}
	return (num_files <= 1);
}

const char* get_current_file_name()
{
	for (int i = callstack.count-1; i >= 0; --i)
	{
		if (callstack[i].type == callstack_entry_type::FILE)
			return callstack[i].content.raw();
	}
	return nullptr;
}

int get_current_line()
{
	for (int i = callstack.count-1; i >= 0; --i)
	{
		if (callstack[i].type == callstack_entry_type::LINE) return callstack[i].lineno;
	}
	return -1;
}

const char* get_current_block()
{
	for (int i = callstack.count-1; i >= 0; --i)
	{
		if (callstack[i].type == callstack_entry_type::LINE || callstack[i].type == callstack_entry_type::BLOCK) return callstack[i].content.raw();
	}
	return nullptr;
}


void reseteverything()
{
	string str;
	labels.reset();
	defines.reset();
	builtindefines.each(adddefine);
	clidefines.each(adddefine);
	structs.reset();

	macros.each(clearmacro);
	macros.reset();

	reset_source_file_cache();

	writtenblocks.reset();

	optimizeforbank=-1;
	optimize_dp = optimize_dp_flag::ALWAYS;
	dp_base = 0;
	optimize_address = optimize_address_flag::MIRRORS;

	closecachedfiles();

	incsrcdepth=0;
	label_counter = 0;
	errored = false;
	checksum_fix_enabled = true;
	force_checksum_fix = false;

	in_macro_def = 0;

#if !defined(ASAR_SHARED) && !defined(ASAR_STATIC)
		free(const_cast<unsigned char*>(romdata_r));
#endif

	callstack.reset();
	simple_callstacks = true;
#undef free
}
