// SPDX-FileCopyrightText: 2021 RizinOrg <info@rizin.re>
// SPDX-FileCopyrightText: 2021 deroad <wargio@libero.it>
// SPDX-FileCopyrightText: 2014-2016 jfrankowski <jody.frankowski@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-only
/* credits to IDA for the flirt tech */
/* original cpp code from Rheax <rheaxmascot@gmail.com> */
/* thanks LemonBoy for the improved research on rheax original work */
/* more information on flirt https://www.hex-rays.com/products/ida/tech/flirt/in_depth.shtml */

/*
   Flirt file format
   =================
   High level layout:
   After the v5 header, there might be two more header fields depending of the version.
   If version == 6 or version == 7, there is one more header field.
   If version == 8 or version == 9, there is two more header field.
   See idasig_v* structs for their description.
   Next there is the non null terminated library name of library_name_len length.
   Next see Parsing below.

   Endianness:
   All multi bytes values are stored in little endian form in the headers.
   For the rest of the file they are stored in big endian form.

   Parsing:
   - described headers
   - library name, not null terminated, length of library_name_len.

   parse_tree (cf. parse_tree):
   - read number of initial root nodes: 1 byte if strictly inferior to 127 otherwise 2 bytes,
   stored in big endian mode, and the most significant bit isn't used. cf. read_multiple_bytes().
   if 0, this is a leaf, goto leaf (cf. parse_leaf). else continue parsing (cf. parse_tree).

   - for number of root node do:
    - read node length, one unsigned byte (the pattern size in this node) (cf. read_node_length)
    - read node variant mask (bit array) (cf. read_node_variant_mask):
      if node length < 0x10 read up to two bytes. cf. read_max_2_bytes
      if node length < 0x20 read up to five bytes. cf. read_multiple_bytes
    - read non-variant bytes (cf. read_node_bytes)
    - goto parse_tree

   leaf (cf. parse_leaf):
   - read crc length, 1 byte
   - read crc value, 2 bytes
   module:
    - read total module length:
      if version >= 9 read up to five bytes, cf. read_multiple_bytes
      else read up to two bytes, cf. read_max_2_bytes
    - read module public functions (cf. read_module_public_functions):
    same crc:
      public function name:
	- read function offset:
	  if version >= 9 read up to five bytes, cf. read_multiple_bytes
	  else read up to two bytes, cf. read_max_2_bytes
	- if current byte < 0x20, read it : this is a function flag, see IDASIG_FUNCTION* defines
	- read function name until current byte < 0x20
	- read parsing flag, 1 byte
	- if flag & IDASIG__PARSE__MORE_PUBLIC_NAMES: goto public function name
	- if flag & IDASIG__PARSE__READ_TAIL_BYTES, read tail bytes, cf. read_module_tail_bytes:
	  - if version >= 8: read number of tail bytes, else suppose one
	  - for number of tail bytes do:
	    - read tail byte offset:
	      if version >= 9 read up to five bytes, cf. read_multiple_bytes
	      else read up to two bytes, cf. read_max_2_bytes
	    - read tail byte value, one byte

	- if flag & IDASIG__PARSE__READ_REFERENCED_FUNCTIONS, read referenced functions, cf. read_module_referenced_functions:
	  - if version >= 8: read number of referenced functions, else suppose one
	  - for number of referenced functions do:
	    - read referenced function offset:
	      if version >= 9 read up to five bytes, cf. read_multiple_bytes
	      else read up to two bytes, cf. read_max_2_bytes
	    - read referenced function name length, one byte:
	      - if name length == 0, read length up to five bytes, cf. read_multiple_bytes
	    - for name length, read name chars:
	      - if name is null terminated, it means the offset is negative

	- if flag & IDASIG__PARSE__MORE_MODULES_WITH_SAME_CRC, goto same crc, read function with same crc
	- if flag & IDASIG__PARSE__MORE_MODULES, goto module, to read another module


   More Information
   -----------------
   Function flags:
   - local functions ((l) with dumpsig) which are static ones.
   - collision functions ((!) with dumpsig) are the result of an unresolved collision.

   Tail bytes:
   When two modules have the same pattern, and same crc, flirt tries to identify
   a byte which is different in all the same modules.
   Their offset is from the first byte after the crc.
   They appear as "(XXXX: XX)" in dumpsig output

   Referenced functions:
   When two modules have the same pattern, and same crc, and are identical in
   non-variant bytes, they only differ by the functions they call. These functions are
   "referenced functions". They need to be identified first before the module can be
   identified.
   The offset is from the start of the function to the referenced function name.
   They appear as "(REF XXXX: NAME)" in dumpsig output
 */

#include <rz_lib.h>
#include <rz_flirt.h>
#include <signal.h>

#define DEBUG 0

/*file_types flags*/
#define IDASIG__FILE__DOS_EXE_OLD 0x00000001
#define IDASIG__FILE__DOS_COM_OLD 0x00000002
#define IDASIG__FILE__BIN         0x00000004
#define IDASIG__FILE__DOSDRV      0x00000008
#define IDASIG__FILE__NE          0x00000010
#define IDASIG__FILE__INTELHEX    0x00000020
#define IDASIG__FILE__MOSHEX      0x00000040
#define IDASIG__FILE__LX          0x00000080
#define IDASIG__FILE__LE          0x00000100
#define IDASIG__FILE__NLM         0x00000200
#define IDASIG__FILE__COFF        0x00000400
#define IDASIG__FILE__PE          0x00000800
#define IDASIG__FILE__OMF         0x00001000
#define IDASIG__FILE__SREC        0x00002000
#define IDASIG__FILE__ZIP         0x00004000
#define IDASIG__FILE__OMFLIB      0x00008000
#define IDASIG__FILE__AR          0x00010000
#define IDASIG__FILE__LOADER      0x00020000
#define IDASIG__FILE__ELF         0x00040000
#define IDASIG__FILE__W32RUN      0x00080000
#define IDASIG__FILE__AOUT        0x00100000
#define IDASIG__FILE__PILOT       0x00200000
#define IDASIG__FILE__DOS_EXE     0x00400000
#define IDASIG__FILE__DOS_COM     0x00800000
#define IDASIG__FILE__AIXAR       0x01000000

/*os_types flags*/
#define IDASIG__OS__MSDOS   0x01
#define IDASIG__OS__WIN     0x02
#define IDASIG__OS__OS2     0x04
#define IDASIG__OS__NETWARE 0x08
#define IDASIG__OS__UNIX    0x10
#define IDASIG__OS__OTHER   0x20

/*app types flags*/
#define IDASIG__APP__CONSOLE         0x0001
#define IDASIG__APP__GRAPHICS        0x0002
#define IDASIG__APP__EXE             0x0004
#define IDASIG__APP__DLL             0x0008
#define IDASIG__APP__DRV             0x0010
#define IDASIG__APP__SINGLE_THREADED 0x0020
#define IDASIG__APP__MULTI_THREADED  0x0040
#define IDASIG__APP__16_BIT          0x0080
#define IDASIG__APP__32_BIT          0x0100
#define IDASIG__APP__64_BIT          0x0200

/*feature flags*/
#define IDASIG__FEATURE__STARTUP       0x01
#define IDASIG__FEATURE__CTYPE_CRC     0x02
#define IDASIG__FEATURE__2BYTE_CTYPE   0x04
#define IDASIG__FEATURE__ALT_CTYPE_CRC 0x08
#define IDASIG__FEATURE__COMPRESSED    0x10

/*parsing flags*/
#define IDASIG__PARSE__MORE_PUBLIC_NAMES          0x01
#define IDASIG__PARSE__READ_TAIL_BYTES            0x02
#define IDASIG__PARSE__READ_REFERENCED_FUNCTIONS  0x04
#define IDASIG__PARSE__MORE_MODULES_WITH_SAME_CRC 0x08
#define IDASIG__PARSE__MORE_MODULES               0x10

/*functions flags*/
#define IDASIG__FUNCTION__LOCAL                0x02 // describes a static function
#define IDASIG__FUNCTION__UNRESOLVED_COLLISION 0x08 // describes a collision that wasn't resolved

typedef struct idasig_v5_t {
	/* newer header only add fields, that's why we'll always read a v5 header first */
	ut8 magic[6]; /* should be set to IDASGN */
	ut8 version; /*from 5 to 9*/
	ut8 arch;
	ut32 file_types;
	ut16 os_types;
	ut16 app_types;
	ut16 features;
	ut16 old_n_functions;
	ut16 crc16;
	ut8 ctype[12]; // XXX: how to use it
	ut8 library_name_len;
	ut16 ctypes_crc16;
} idasig_v5_t;

typedef struct idasig_v6_v7_t {
	ut32 n_functions;
} idasig_v6_v7_t;

typedef struct idasig_v8_v9_t {
	ut16 pattern_size;
} idasig_v8_v9_t;

typedef struct idasig_v10_t {
	ut16 unknown;
} idasig_v10_t;

typedef struct parse_status_t {
	RzBuffer *buffer;
	bool eof;
	bool error;
	ut8 version;
} ParseStatus;

typedef struct arch_id_t {
	const char *name;
	ut32 id;
} ArchId;

const ArchId arch_id_map[18] = {
	{ "6502", RZ_FLIRT_SIG_ARCH_6502 },
	{ "arm", RZ_FLIRT_SIG_ARCH_ARM },
	{ "avr", RZ_FLIRT_SIG_ARCH_AVR },
	{ "cr16", RZ_FLIRT_SIG_ARCH_CR16 },
	{ "dalvik", RZ_FLIRT_SIG_ARCH_DALVIK },
	{ "ebc", RZ_FLIRT_SIG_ARCH_EBC },
	{ "h8300", RZ_FLIRT_SIG_ARCH_H8 },
	{ "hppa", RZ_FLIRT_SIG_ARCH_HPPA },
	{ "java", RZ_FLIRT_SIG_ARCH_JAVA },
	{ "mips", RZ_FLIRT_SIG_ARCH_MIPS },
	{ "msp430", RZ_FLIRT_SIG_ARCH_MSP430 },
	{ "pic", RZ_FLIRT_SIG_ARCH_PIC },
	{ "ppc", RZ_FLIRT_SIG_ARCH_PPC },
	{ "sh", RZ_FLIRT_SIG_ARCH_SH },
	{ "sparc", RZ_FLIRT_SIG_ARCH_SPARC },
	{ "tricore", RZ_FLIRT_SIG_ARCH_TRICORE },
	{ "x86", RZ_FLIRT_SIG_ARCH_386 },
	{ "z80", RZ_FLIRT_SIG_ARCH_Z80 }
};

#define is_status_err_or_eof(p) (p->eof || p->error)

/* newer header only add fields, that's why we'll always read a v5 header first */
/*
   arch             : target architecture
   file_types       : files where we expect to find the functions (exe, coff, ...)
   os_types         : os where we expect to find the functions
   app_types        : applications in which we expect to find the functions
   features         : signature file features
   old_n_functions  : number of functions
   crc16            : certainly crc16 of the tree
   ctype[12]        : unknown field
   library_name_len : length of the library name, which is right after the header
   ctypes_crc16     : unknown field
   n_functions      : number of functions
   pattern_size     : number of the leading pattern bytes
 */

// This is from flair tools flair/crc16.cpp
#define POLY 0x8408
ut16 crc16(const unsigned char *data_p, size_t length) {
	ut8 i;
	ut32 data;
	ut32 crc = 0xFFFF;

	if (length == 0) {
		return 0;
	}
	do {
		data = *data_p++;
		for (i = 0; i < 8; i++) {
			if ((crc ^ data) & 1) {
				crc = (crc >> 1) ^ POLY;
			} else {
				crc >>= 1;
			}
			data >>= 1;
		}
	} while (--length > 0);

	crc = ~crc;
	data = crc;
	crc = (crc << 8) | ((data >> 8) & 0xff);
	return (ut16)(crc);
}

static ut8 read_byte(ParseStatus *b) {
	ut8 r = 0;
	int length;

	if (b->eof || b->error) {
		return 0;
	}
	if ((length = rz_buf_read(b->buffer, &r, 1)) != 1) {
		if (length == -1) {
			b->error = true;
		}
		if (length == 0) {
			b->eof = true;
		}
		return 0;
	}
	return r;
}

static ut16 read_short(ParseStatus *b) {
	ut16 r = (read_byte(b) << 8);
	r += read_byte(b);
	return r;
}

static ut32 read_word(ParseStatus *b) {
	ut32 r = ((ut32)(read_short(b)) << 16);
	r += read_short(b);
	return r;
}

static ut16 read_max_2_bytes(ParseStatus *b) {
	ut16 r = read_byte(b);
	return (r & 0x80)
		? ((r & 0x7f) << 8) + read_byte(b)
		: r;
}

static ut32 read_multiple_bytes(ParseStatus *b) {
	ut32 r = read_byte(b);
	if ((r & 0x80) != 0x80) {
		return r;
	}
	if ((r & 0xc0) != 0xc0) {
		return ((r & 0x7f) << 8) + read_byte(b);
	}
	if ((r & 0xe0) != 0xe0) {
		r = ((r & 0x3f) << 24) + (read_byte(b) << 16);
		r += read_short(b);
		return r;
	}
	return read_word(b);
}

void module_free(RzFlirtModule *module) {
	if (!module) {
		return;
	}
	rz_list_free(module->public_functions);
	rz_list_free(module->tail_bytes);
	rz_list_free(module->referenced_functions);
	free(module);
}

/**
 * \brief Frees an RzFlirtNode struct
 *
 * \param RzFlirtNode  The RzFlirtNode to be freed
 */
RZ_API void rz_sign_flirt_node_free(RZ_NULLABLE RzFlirtNode *node) {
	if (!node) {
		return;
	}
	free(node->pattern_mask);
	free(node->pattern_bytes);
	rz_list_free(node->module_list);
	rz_list_free(node->child_list);
	free(node);
}

/**
 * \brief Checks if a pattern does match the buffer data
 *
 * \param p_size   The pattern size
 * \param pattern  The pattern to check agains
 * \param mask     The pattern mask
 * \param b        Buffer to check
 * \param b_size   Size of the buffer to check
 *
 * \return True if pattern does match, false otherwise.
 */
static bool is_pattern_matching(ut32 p_size, const ut8 *pattern, const ut8 *mask, const ut8 *b, ut32 b_size) {
	if (b_size < p_size) {
		return false;
	}
	for (ut32 i = 0; i < p_size; i++) {
		if (!mask[i] && pattern[i] != b[i]) {
			return false;
		}
	}
	return true;
}

/**
 * \brief Checks if the module matches the buffer and renames the matched functions
 *
 * \param analysis  The RzAnalysis struct from where to fetch and modify the functions
 * \param module    The FLIRT module to match against the buffer
 * \param b         Buffer to check
 * \param address   Function address
 * \param buf_size  Size of the buffer to check
 *
 * \return True if pattern does match, false otherwise.
 */
static int module_match_buffer(RzAnalysis *analysis, const RzFlirtModule *module, ut8 *b, ut64 address, ut32 buf_size) {
	RzFlirtFunction *flirt_func;
	RzAnalysisFunction *next_module_function;
	RzListIter *tail_byte_it, *flirt_func_it;
	RzFlirtTailByte *tail_byte;

	if (32 + module->crc_length < buf_size &&
		module->crc16 != crc16(b + 32, module->crc_length)) {
		return false;
	}
	if (module->tail_bytes) {
		rz_list_foreach (module->tail_bytes, tail_byte_it, tail_byte) {
			if (32 + module->crc_length + tail_byte->offset < buf_size &&
				b[32 + module->crc_length + tail_byte->offset] != tail_byte->value) {
				return false;
			}
		}
	}

	rz_list_foreach (module->public_functions, flirt_func_it, flirt_func) {
		// Once the first module function is found, we need to go through the module->public_functions
		// list to identify the others. See flirt doc for more information

		next_module_function = rz_analysis_get_function_at((RzAnalysis *)analysis, address + flirt_func->offset);
		if (next_module_function) {
			char *name;
			int name_offs = 0;
			ut32 next_module_function_size;

			// get function size from flirt signature
			ut64 flirt_fcn_size = module->length - flirt_func->offset;
			RzFlirtFunction *next_flirt_func;
			RzListIter *next_flirt_func_it = flirt_func_it->n;
			while (next_flirt_func_it) {
				next_flirt_func = next_flirt_func_it->data;
				if (!next_flirt_func->is_local && !next_flirt_func->negative_offset) {
					flirt_fcn_size = next_flirt_func->offset - flirt_func->offset;
					break;
				}
				next_flirt_func_it = next_flirt_func_it->n;
			}
			// resize function if needed
			next_module_function_size = rz_analysis_function_linear_size(next_module_function);
			if (next_module_function_size < flirt_fcn_size) {
				RzListIter *iter;
				RzListIter *iter_tmp;
				RzAnalysisFunction *fcn;
				rz_list_foreach_safe (analysis->fcns, iter, iter_tmp, fcn) {
					if (fcn != next_module_function &&
						fcn->addr >= next_module_function->addr + next_module_function_size &&
						fcn->addr < next_module_function->addr + flirt_fcn_size) {
						RzListIter *iter_bb;
						RzAnalysisBlock *block;
						rz_list_foreach (fcn->bbs, iter_bb, block) {
							rz_analysis_function_add_block(next_module_function, block);
						}
						next_module_function->ninstr += fcn->ninstr;
						rz_analysis_function_delete(fcn);
					}
				}
				rz_analysis_function_resize(next_module_function, flirt_fcn_size);
				next_module_function_size = rz_analysis_function_linear_size(next_module_function);
				rz_analysis_trim_jmprefs((RzAnalysis *)analysis, next_module_function);
			}

			while (flirt_func->name[name_offs] == '?') { // skip '?' chars
				name_offs++;
			}
			if (!flirt_func->name[name_offs]) {
				continue;
			}
			name = rz_name_filter2(flirt_func->name + name_offs, true);
			free(next_module_function->name);
			next_module_function->name = rz_str_newf("flirt.%s", name);
			analysis->flb.set(analysis->flb.f, next_module_function->name,
				next_module_function->addr, next_module_function_size);
			RZ_LOG_INFO("FLIRT: Found %s\n", next_module_function->name);
			free(name);
		}
	}
	return true;
}

static int node_match_buffer(RzAnalysis *analysis, const RzFlirtNode *node, ut8 *b, ut64 address, ut32 buf_size, ut32 buf_idx) {
	RzListIter *node_child_it, *module_it;
	RzFlirtNode *child;
	RzFlirtModule *module;

	if (is_pattern_matching(node->length, node->pattern_bytes, node->pattern_mask, b + buf_idx, buf_size - buf_idx)) {
		if (node->child_list) {
			rz_list_foreach (node->child_list, node_child_it, child) {
				if (node_match_buffer(analysis, child, b, address, buf_size, buf_idx + node->length)) {
					return true;
				}
			}
		} else if (node->module_list) {
			rz_list_foreach (node->module_list, module_it, module) {
				if (module_match_buffer(analysis, module, b, address, buf_size)) {
					return true;
				}
			}
		}
	}

	return false;
}

/**
 * \brief Tries to find matching functions between the signature infos in root_node and the analyzed functions in analysis
 *
 * \param analysis   The analysis
 * \param root_node  The root node
 *
 * \return False on error, otherwise true
 */
static bool node_match_functions(RzAnalysis *analysis, const RzFlirtNode *root_node) {
	bool ret = true;

	if (rz_list_length(analysis->fcns) == 0) {
		RZ_LOG_ERROR("FLIRT: There are no analyzed functions. Have you run 'aa'?\n");
		return ret;
	}

	analysis->flb.push_fs(analysis->flb.f, "flirt");
	RzListIter *it_func;
	RzAnalysisFunction *func;
	rz_list_foreach (analysis->fcns, it_func, func) {
		if (func->type != RZ_ANALYSIS_FCN_TYPE_FCN && func->type != RZ_ANALYSIS_FCN_TYPE_LOC) { // scan only for unknown functions
			continue;
		}

		ut64 func_size = rz_analysis_function_linear_size(func);
		ut8 *func_buf = malloc(func_size);
		if (!func_buf) {
			ret = false;
			break;
		}
		if (!analysis->iob.read_at(analysis->iob.io, func->addr, func_buf, (int)func_size)) {
			RZ_LOG_ERROR("FLIRT: Couldn't read function %s at 0x%" PFMT64x "\n", func->name, func->addr);
			RZ_FREE(func_buf);
			ret = false;
			break;
		}
		RzListIter *node_child_it;
		RzFlirtNode *child;
		rz_list_foreach (root_node->child_list, node_child_it, child) {
			if (node_match_buffer(analysis, child, func_buf, func->addr, func_size, 0)) {
				break;
			}
		}
		RZ_FREE(func_buf);
	}
	analysis->flb.pop_fs(analysis->flb.f);

	return ret;
}

static ut8 read_module_tail_bytes(RzFlirtModule *module, ParseStatus *b) {
	/* parses a module tail bytes */
	/* returns false on parsing error */
	int i;
	ut8 number_of_tail_bytes;
	RzFlirtTailByte *tail_byte = NULL;
	if (!(module->tail_bytes = rz_list_newf((RzListFree)free))) {
		goto err_exit;
	}

	if (b->version >= 8) { // this counter was introduced in version 8
		number_of_tail_bytes = read_byte(b); // XXX are we sure it's not read_multiple_bytes?
		if (is_status_err_or_eof(b)) {
			goto err_exit;
		}
	} else { // suppose there's only one
		number_of_tail_bytes = 1;
	}
	for (i = 0; i < number_of_tail_bytes; i++) {
		tail_byte = RZ_NEW0(RzFlirtTailByte);
		if (!tail_byte) {
			return false;
		}
		if (b->version >= 9) {
			/*/!\ XXX don't trust ./zipsig output because it will write a version 9 header, but keep the old version offsets*/
			tail_byte->offset = read_multiple_bytes(b);
			if (is_status_err_or_eof(b)) {
				goto err_exit;
			}
		} else {
			tail_byte->offset = read_max_2_bytes(b);
			if (is_status_err_or_eof(b)) {
				goto err_exit;
			}
		}
		tail_byte->value = read_byte(b);
		if (is_status_err_or_eof(b)) {
			goto err_exit;
		}
		rz_list_append(module->tail_bytes, tail_byte);
#if DEBUG
		eprintf("READ TAIL BYTE: %04X: %02X\n", tail_byte->offset, tail_byte->value);
#endif
	}

	return true;

err_exit:
	free(tail_byte);
	rz_list_free(module->tail_bytes);
	return false;
}

static ut8 read_module_referenced_functions(RzFlirtModule *module, ParseStatus *b) {
	/* parses a module referenced functions */
	/* returns false on parsing error */
	int i, j;
	ut8 number_of_referenced_functions;
	ut32 ref_function_name_length;
	RzFlirtFunction *ref_function = NULL;

	module->referenced_functions = rz_list_newf((RzListFree)free);

	if (b->version >= 8) { // this counter was introduced in version 8
		number_of_referenced_functions = read_byte(b); // XXX are we sure it's not read_multiple_bytes?
		if (is_status_err_or_eof(b)) {
			goto err_exit;
		}
	} else { // suppose there's only one
		number_of_referenced_functions = 1;
	}

	for (i = 0; i < number_of_referenced_functions; i++) {
		ref_function = RZ_NEW0(RzFlirtFunction);
		if (!ref_function) {
			goto err_exit;
		}
		if (b->version >= 9) {
			ref_function->offset = read_multiple_bytes(b);
			if (is_status_err_or_eof(b)) {
				goto err_exit;
			}
		} else {
			ref_function->offset = read_max_2_bytes(b);
			if (is_status_err_or_eof(b)) {
				goto err_exit;
			}
		}
		ref_function_name_length = read_byte(b);
		if (is_status_err_or_eof(b)) {
			goto err_exit;
		}
		if (!ref_function_name_length) {
			// not sure why it's not read_multiple_bytes() in the first place
			ref_function_name_length = read_multiple_bytes(b); // XXX might be read_max_2_bytes, need more data
			if (is_status_err_or_eof(b)) {
				goto err_exit;
			}
		}
		if ((int)ref_function_name_length < 0 || ref_function_name_length >= RZ_FLIRT_NAME_MAX) {
			goto err_exit;
		}
		for (j = 0; j < ref_function_name_length; j++) {
			ref_function->name[j] = read_byte(b);
			if (is_status_err_or_eof(b)) {
				goto err_exit;
			}
		}
		if (!ref_function->name[ref_function_name_length]) {
			// if the last byte of the name is 0, it means the offset is negative
			ref_function->negative_offset = true;
		} else {
			ref_function->name[ref_function_name_length] = '\0';
		}
		rz_list_append(module->referenced_functions, ref_function);
#if DEBUG
		eprintf("(REF: %04X: %s)\n", ref_function->offset, ref_function->name);
#endif
	}

	return true;

err_exit:
	free(ref_function);
	return false;
}

static ut8 read_module_public_functions(RzFlirtModule *module, ParseStatus *b, ut8 *flags) {
	/* Reads and set the public functions names and offsets associated within a module */
	/* returns false on parsing error */
	int i;
	ut16 offset = 0;
	ut8 current_byte;
	RzFlirtFunction *function = NULL;

	module->public_functions = rz_list_newf((RzListFree)free);

	do {
		function = RZ_NEW0(RzFlirtFunction);
		if (b->version >= 9) { // seems like version 9 introduced some larger offsets
			offset += read_multiple_bytes(b); // offsets are dependent of the previous ones
			if (is_status_err_or_eof(b)) {
				goto err_exit;
			}
		} else {
			offset += read_max_2_bytes(b); // offsets are dependent of the previous ones
			if (is_status_err_or_eof(b)) {
				goto err_exit;
			}
		}
		function->offset = offset;

		current_byte = read_byte(b);
		if (is_status_err_or_eof(b)) {
			goto err_exit;
		}
		if (current_byte < 0x20) {
			if (current_byte & IDASIG__FUNCTION__LOCAL) { // static function
				function->is_local = true;
			}
			if (current_byte & IDASIG__FUNCTION__UNRESOLVED_COLLISION) {
				// unresolved collision (happens in *.exc while creating .sig from .pat)
				function->is_collision = true;
			}
#if DEBUG
			if (current_byte & 0x01 || current_byte & 0x04) { // appears as 'd' or '?' in dumpsig
				// XXX investigate
				eprintf("INVESTIGATE PUBLIC NAME FLAG: %02X @ %04X\n", current_byte,
					rz_buf_tell(b) + header_size);
			}
#endif
			current_byte = read_byte(b);
			if (is_status_err_or_eof(b)) {
				goto err_exit;
			}
		}

		for (i = 0; current_byte >= 0x20 && i < RZ_FLIRT_NAME_MAX; i++) {
			function->name[i] = current_byte;
			current_byte = read_byte(b);
			if (is_status_err_or_eof(b)) {
				goto err_exit;
			}
		}

		if (i == RZ_FLIRT_NAME_MAX) {
			eprintf("Function name too long\n");
			function->name[RZ_FLIRT_NAME_MAX - 1] = '\0';
		} else {
			function->name[i] = '\0';
		}

#if DEBUG
		eprintf("%04X:%s ", function->offset, function->name);
#endif
		*flags = current_byte;
		rz_list_append(module->public_functions, function);
	} while (*flags & IDASIG__PARSE__MORE_PUBLIC_NAMES);
#if DEBUG
	eprintf("\n");
#endif

	return true;

err_exit:
	free(function);
	return false;
}

static ut8 parse_leaf(ParseStatus *b, RzFlirtNode *node) {
	/* parses a signature leaf: modules with same leading pattern */
	/* returns false on parsing error */
	ut8 flags, crc_length;
	ut16 crc16;
	RzFlirtModule *module = NULL;

	node->module_list = rz_list_newf((RzListFree)module_free);
	do { // loop for all modules having the same prefix

		crc_length = read_byte(b);
		if (is_status_err_or_eof(b)) {
			goto err_exit;
		}
		crc16 = read_short(b);
		if (is_status_err_or_eof(b)) {
			goto err_exit;
		}
#if DEBUG
		if (crc_length == 0x00 && crc16 != 0x0000) {
			eprintf("WARNING non zero crc of zero length @ %04X\n",
				rz_buf_tell(b) + header_size);
		}
		eprintf("crc_len: %02X crc16: %04X\n", crc_length, crc16);
#endif

		do { // loop for all modules having the same crc
			module = RZ_NEW0(RzFlirtModule);
			if (!module) {
				goto err_exit;
			}

			module->crc_length = crc_length;
			module->crc16 = crc16;

			if (b->version >= 9) { // seems like version 9 introduced some larger length
				/*/!\ XXX don't trust ./zipsig output because it will write a version 9 header, but keep the old version offsets*/
				module->length = read_multiple_bytes(b); // should be < 0x8000
				if (is_status_err_or_eof(b)) {
					goto err_exit;
				}
			} else {
				module->length = read_max_2_bytes(b); // should be < 0x8000
				if (is_status_err_or_eof(b)) {
					goto err_exit;
				}
			}
#if DEBUG
			eprintf("module_length: %04X\n", module->length);
#endif

			if (!read_module_public_functions(module, b, &flags)) {
				goto err_exit;
			}

			if (flags & IDASIG__PARSE__READ_TAIL_BYTES) { // we need to read some tail bytes because in this leaf we have functions with same crc
				if (!read_module_tail_bytes(module, b)) {
					goto err_exit;
				}
			}
			if (flags & IDASIG__PARSE__READ_REFERENCED_FUNCTIONS) { // we need to read some referenced functions
				if (!read_module_referenced_functions(module, b)) {
					goto err_exit;
				}
			}

			rz_list_append(node->module_list, module);
		} while (flags & IDASIG__PARSE__MORE_MODULES_WITH_SAME_CRC);
	} while (flags & IDASIG__PARSE__MORE_MODULES); // same prefix but different crc

	return true;

err_exit:
	module_free(module);
	return false;
}

static ut8 read_node_length(RzFlirtNode *node, ParseStatus *b) {
	node->length = read_byte(b);
	if (is_status_err_or_eof(b)) {
		return false;
	}
#if DEBUG
	eprintf("node length: %02X\n", node->length);
#endif
	return true;
}

static ut8 read_node_variant_mask(RzFlirtNode *node, ParseStatus *b) {
	/* Reads and sets a node's variant bytes mask. This mask is then used to */
	/* read the non-variant bytes following. */
	/* returns false on parsing error */
	if (node->length < 0x10) {
		node->variant_mask = read_max_2_bytes(b);
		if (is_status_err_or_eof(b)) {
			return false;
		}
	} else if (node->length <= 0x20) {
		node->variant_mask = read_multiple_bytes(b);
		if (is_status_err_or_eof(b)) {
			return false;
		}
	} else if (node->length <= 0x40) { // it shouldn't be more than 64 bytes
		node->variant_mask = ((ut64)read_multiple_bytes(b) << 32) + read_multiple_bytes(b);
		if (is_status_err_or_eof(b)) {
			return false;
		}
	}

	return true;
}

static bool read_node_bytes(RzFlirtNode *node, ParseStatus *b) {
	/* Reads the node bytes, and also sets the variant bytes in pattern_mask */
	/* returns false on parsing error */
	int i;
	ut64 current_mask_bit = 0;
	if ((int)node->length < 0) {
		return false;
	}
	current_mask_bit = 1ULL << (node->length - 1);
	if (!(node->pattern_bytes = malloc(node->length))) {
		return false;
	}
	if (!(node->pattern_mask = malloc(node->length))) {
		return false;
	}
	for (i = 0; i < node->length; i++, current_mask_bit >>= 1) {
		node->pattern_mask[i] = (bool)(node->variant_mask & current_mask_bit);
		if (node->variant_mask & current_mask_bit) {
			node->pattern_bytes[i] = 0x00;
		} else {
			node->pattern_bytes[i] = read_byte(b);
			if (is_status_err_or_eof(b)) {
				return false;
			}
		}
	}
	return true;
}

static ut8 parse_tree(ParseStatus *b, RzFlirtNode *root_node) {
	/* parse a signature pattern tree or sub-tree */
	/* returns false on parsing error */
	RzFlirtNode *node = NULL;
	int i, tree_nodes = read_multiple_bytes(b); // confirmed it's not read_byte(), XXX could it be read_max_2_bytes() ???
	if (is_status_err_or_eof(b)) {
		return false;
	}
	if (tree_nodes == 0) { // if there's no tree nodes remaining, that means we are on the leaf
		return parse_leaf(b, root_node);
	}
	root_node->child_list = rz_list_newf((RzListFree)rz_sign_flirt_node_free);

	for (i = 0; i < tree_nodes; i++) {
		if (!(node = RZ_NEW0(RzFlirtNode))) {
			goto err_exit;
		}
		if (!read_node_length(node, b)) {
			goto err_exit;
		}
		if (!read_node_variant_mask(node, b)) {
			goto err_exit;
		}
		if (!read_node_bytes(node, b)) {
			goto err_exit;
		}
		rz_list_append(root_node->child_list, node);
		if (!parse_tree(b, node)) {
			goto err_exit; // parse child nodes
		}
	}
	return true;
err_exit:
	rz_sign_flirt_node_free(node);
	return false;
}

#if DEBUG
#define PRINT_ARCH(define, str) \
	if (arch == define) { \
		eprintf(" %s", str); \
		return; \
	}
static void print_arch(ut8 arch) {
	PRINT_ARCH(IDASIG__ARCH__386, "386");
	PRINT_ARCH(IDASIG__ARCH__Z80, "Z80");
	PRINT_ARCH(IDASIG__ARCH__I860, "I860");
	PRINT_ARCH(IDASIG__ARCH__8051, "8051");
	PRINT_ARCH(IDASIG__ARCH__TMS, "TMS");
	PRINT_ARCH(IDASIG__ARCH__6502, "6502");
	PRINT_ARCH(IDASIG__ARCH__PDP, "PDP");
	PRINT_ARCH(IDASIG__ARCH__68K, "68K");
	PRINT_ARCH(IDASIG__ARCH__JAVA, "JAVA");
	PRINT_ARCH(IDASIG__ARCH__6800, "6800");
	PRINT_ARCH(IDASIG__ARCH__ST7, "ST7");
	PRINT_ARCH(IDASIG__ARCH__MC6812, "MC6812");
	PRINT_ARCH(IDASIG__ARCH__MIPS, "MIPS");
	PRINT_ARCH(IDASIG__ARCH__ARM, "ARM");
	PRINT_ARCH(IDASIG__ARCH__TMSC6, "TMSC6");
	PRINT_ARCH(IDASIG__ARCH__PPC, "PPC");
	PRINT_ARCH(IDASIG__ARCH__80196, "80196");
	PRINT_ARCH(IDASIG__ARCH__Z8, "Z8");
	PRINT_ARCH(IDASIG__ARCH__SH, "SH");
	PRINT_ARCH(IDASIG__ARCH__NET, "NET");
	PRINT_ARCH(IDASIG__ARCH__AVR, "AVR");
	PRINT_ARCH(IDASIG__ARCH__H8, "H8");
	PRINT_ARCH(IDASIG__ARCH__PIC, "PIC");
	PRINT_ARCH(IDASIG__ARCH__SPARC, "SPARC");
	PRINT_ARCH(IDASIG__ARCH__ALPHA, "ALPHA");
	PRINT_ARCH(IDASIG__ARCH__HPPA, "HPPA");
	PRINT_ARCH(IDASIG__ARCH__H8500, "H8500");
	PRINT_ARCH(IDASIG__ARCH__TRICORE, "TRICORE");
	PRINT_ARCH(IDASIG__ARCH__DSP56K, "DSP56K");
	PRINT_ARCH(IDASIG__ARCH__C166, "C166");
	PRINT_ARCH(IDASIG__ARCH__ST20, "ST20");
	PRINT_ARCH(IDASIG__ARCH__IA64, "IA64");
	PRINT_ARCH(IDASIG__ARCH__I960, "I960");
	PRINT_ARCH(IDASIG__ARCH__F2MC, "F2MC");
	PRINT_ARCH(IDASIG__ARCH__TMS320C54, "TMS320C54");
	PRINT_ARCH(IDASIG__ARCH__TMS320C55, "TMS320C55");
	PRINT_ARCH(IDASIG__ARCH__TRIMEDIA, "TRIMEDIA");
	PRINT_ARCH(IDASIG__ARCH__M32R, "M32R");
	PRINT_ARCH(IDASIG__ARCH__NEC_78K0, "NEC_78K0");
	PRINT_ARCH(IDASIG__ARCH__NEC_78K0S, "NEC_78K0S");
	PRINT_ARCH(IDASIG__ARCH__M740, "M740");
	PRINT_ARCH(IDASIG__ARCH__M7700, "M7700");
	PRINT_ARCH(IDASIG__ARCH__ST9, "ST9");
	PRINT_ARCH(IDASIG__ARCH__FR, "FR");
	PRINT_ARCH(IDASIG__ARCH__MC6816, "MC6816");
	PRINT_ARCH(IDASIG__ARCH__M7900, "M7900");
	PRINT_ARCH(IDASIG__ARCH__TMS320C3, "TMS320C3");
	PRINT_ARCH(IDASIG__ARCH__KR1878, "KR1878");
	PRINT_ARCH(IDASIG__ARCH__AD218X, "AD218X");
	PRINT_ARCH(IDASIG__ARCH__OAKDSP, "OAKDSP");
	PRINT_ARCH(IDASIG__ARCH__TLCS900, "TLCS900");
	PRINT_ARCH(IDASIG__ARCH__C39, "C39");
	PRINT_ARCH(IDASIG__ARCH__CR16, "CR16");
	PRINT_ARCH(IDASIG__ARCH__MN102L00, "MN102L00");
	PRINT_ARCH(IDASIG__ARCH__TMS320C1X, "TMS320C1X");
	PRINT_ARCH(IDASIG__ARCH__NEC_V850X, "NEC_V850X");
	PRINT_ARCH(IDASIG__ARCH__SCR_ADPT, "SCR_ADPT");
	PRINT_ARCH(IDASIG__ARCH__EBC, "EBC");
	PRINT_ARCH(IDASIG__ARCH__MSP430, "MSP430");
	PRINT_ARCH(IDASIG__ARCH__SPU, "SPU");
	PRINT_ARCH(IDASIG__ARCH__DALVIK, "DALVIK");
}

#define PRINT_FLAG(define, str) \
	if (flags & define) { \
		eprintf(" %s", str); \
	}
static void print_file_types(ut32 flags) {
	PRINT_FLAG(IDASIG__FILE__DOS_EXE_OLD, "DOS_EXE_OLD");
	PRINT_FLAG(IDASIG__FILE__DOS_COM_OLD, "DOS_COM_OLD");
	PRINT_FLAG(IDASIG__FILE__BIN, "BIN");
	PRINT_FLAG(IDASIG__FILE__DOSDRV, "DOSDRV");
	PRINT_FLAG(IDASIG__FILE__NE, "NE");
	PRINT_FLAG(IDASIG__FILE__INTELHEX, "INTELHEX");
	PRINT_FLAG(IDASIG__FILE__MOSHEX, "MOSHEX");
	PRINT_FLAG(IDASIG__FILE__LX, "LX");
	PRINT_FLAG(IDASIG__FILE__LE, "LE");
	PRINT_FLAG(IDASIG__FILE__NLM, "NLM");
	PRINT_FLAG(IDASIG__FILE__COFF, "COFF");
	PRINT_FLAG(IDASIG__FILE__PE, "PE");
	PRINT_FLAG(IDASIG__FILE__OMF, "OMF");
	PRINT_FLAG(IDASIG__FILE__SREC, "SREC");
	PRINT_FLAG(IDASIG__FILE__ZIP, "ZIP");
	PRINT_FLAG(IDASIG__FILE__OMFLIB, "OMFLIB");
	PRINT_FLAG(IDASIG__FILE__AR, "AR");
	PRINT_FLAG(IDASIG__FILE__LOADER, "LOADER");
	PRINT_FLAG(IDASIG__FILE__ELF, "ELF");
	PRINT_FLAG(IDASIG__FILE__W32RUN, "W32RUN");
	PRINT_FLAG(IDASIG__FILE__AOUT, "AOUT");
	PRINT_FLAG(IDASIG__FILE__PILOT, "PILOT");
	PRINT_FLAG(IDASIG__FILE__DOS_EXE, "EXE");
	PRINT_FLAG(IDASIG__FILE__AIXAR, "AIXAR");
}

static void print_os_types(ut16 flags) {
	PRINT_FLAG(IDASIG__OS__MSDOS, "MSDOS");
	PRINT_FLAG(IDASIG__OS__WIN, "WIN");
	PRINT_FLAG(IDASIG__OS__OS2, "OS2");
	PRINT_FLAG(IDASIG__OS__NETWARE, "NETWARE");
	PRINT_FLAG(IDASIG__OS__UNIX, "UNIX");
}

static void print_app_types(ut16 flags) {
	PRINT_FLAG(IDASIG__APP__CONSOLE, "CONSOLE");
	PRINT_FLAG(IDASIG__APP__GRAPHICS, "GRAPHICS");
	PRINT_FLAG(IDASIG__APP__EXE, "EXE");
	PRINT_FLAG(IDASIG__APP__DLL, "DLL");
	PRINT_FLAG(IDASIG__APP__DRV, "DRV");
	PRINT_FLAG(IDASIG__APP__SINGLE_THREADED, "SINGLE_THREADED");
	PRINT_FLAG(IDASIG__APP__MULTI_THREADED, "MULTI_THREADED");
	PRINT_FLAG(IDASIG__APP__16_BIT, "16_BIT");
	PRINT_FLAG(IDASIG__APP__32_BIT, "32_BIT");
	PRINT_FLAG(IDASIG__APP__64_BIT, "64_BIT");
}

static void print_features(ut16 flags) {
	PRINT_FLAG(IDASIG__FEATURE__STARTUP, "STARTUP");
	PRINT_FLAG(IDASIG__FEATURE__CTYPE_CRC, "CTYPE_CRC");
	PRINT_FLAG(IDASIG__FEATURE__2BYTE_CTYPE, "2BYTE_CTYPE");
	PRINT_FLAG(IDASIG__FEATURE__ALT_CTYPE_CRC, "ALT_CTYPE_CRC");
	PRINT_FLAG(IDASIG__FEATURE__COMPRESSED, "COMPRESSED");
}

static void print_header(idasig_v5_t *header) {
	/*eprintf("magic: %s\n", header->magic);*/
	eprintf("version: %d\n", header->version);
	eprintf("arch:");
	print_arch(header->arch);
	eprintf("\n");
	eprintf("file_types:");
	print_file_types(header->file_types);
	eprintf("\n");
	eprintf("os_types:");
	print_os_types(header->os_types);
	eprintf("\n");
	eprintf("app_types:");
	print_app_types(header->app_types);
	eprintf("\n");
	eprintf("features:");
	print_features(header->features);
	eprintf("\n");
	eprintf("old_n_functions: %04x\n", header->old_n_functions);
	eprintf("crc16: %04x\n", header->crc16);
	eprintf("ctype: %s\n", header->ctype);
	eprintf("library_name_len: %d\n", header->library_name_len);
	eprintf("ctypes_crc16: %04x\n", header->ctypes_crc16);
}
#endif

static int parse_v5_header(RzBuffer *buf, idasig_v5_t *header) {
	rz_buf_seek(buf, 0, RZ_BUF_SET);
	if (rz_buf_read(buf, header->magic, sizeof(header->magic)) != sizeof(header->magic)) {
		return false;
	}
	if (rz_buf_read(buf, &header->version, sizeof(header->version)) != sizeof(header->version)) {
		return false;
	}
	if (rz_buf_read(buf, &header->arch, sizeof(header->arch)) != sizeof(header->arch)) {
		return false;
	}
	if (rz_buf_read(buf, (unsigned char *)&header->file_types, sizeof(header->file_types)) != sizeof(header->file_types)) {
		return false;
	}
	if (rz_buf_read(buf, (unsigned char *)&header->os_types, sizeof(header->os_types)) != sizeof(header->os_types)) {
		return false;
	}
	if (rz_buf_read(buf, (unsigned char *)&header->app_types, sizeof(header->app_types)) != sizeof(header->app_types)) {
		return false;
	}
	if (rz_buf_read(buf, (unsigned char *)&header->features, sizeof(header->features)) != sizeof(header->features)) {
		return false;
	}
	if (rz_buf_read(buf, (unsigned char *)&header->old_n_functions, sizeof(header->old_n_functions)) != sizeof(header->old_n_functions)) {
		return false;
	}
	if (rz_buf_read(buf, (unsigned char *)&header->crc16, sizeof(header->crc16)) != sizeof(header->crc16)) {
		return false;
	}
	if (rz_buf_read(buf, header->ctype, sizeof(header->ctype)) != sizeof(header->ctype)) {
		return false;
	}
	if (rz_buf_read(buf, (unsigned char *)&header->library_name_len, sizeof(header->library_name_len)) != sizeof(header->library_name_len)) {
		return false;
	}
	if (rz_buf_read(buf, (unsigned char *)&header->ctypes_crc16, sizeof(header->ctypes_crc16)) != sizeof(header->ctypes_crc16)) {
		return false;
	}

	return true;
}

static int parse_v6_v7_header(RzBuffer *buf, idasig_v6_v7_t *header) {
	if (rz_buf_read(buf, (unsigned char *)&header->n_functions, sizeof(header->n_functions)) != sizeof(header->n_functions)) {
		return false;
	}

	return true;
}

static int parse_v8_v9_header(RzBuffer *buf, idasig_v8_v9_t *header) {
	if (rz_buf_read(buf, (unsigned char *)&header->pattern_size, sizeof(header->pattern_size)) != sizeof(header->pattern_size)) {
		return false;
	}

	return true;
}

static int parse_v10_header(RzBuffer *buf, idasig_v10_t *header) {
	if (rz_buf_read(buf, (unsigned char *)&header->unknown, sizeof(header->unknown)) != sizeof(header->unknown)) {
		return false;
	}

	return true;
}

/**
 * \brief Parses the RzBuffer containing a FLIRT structure and returns an RzFlirtNode
 *
 * Parses the RzBuffer containing a FLIRT structure and returns an RzFlirtNode if expected_arch
 * matches the id or RZ_FLIRT_SIG_ARCH_ANY is set.
 *
 * \param  flirt_buf     The buffer to read
 * \param  expected_arch The expected arch to be used for the buffer
 * \return               Parsed FLIRT node
 */
RZ_API RZ_OWN RzFlirtNode *rz_sign_flirt_parse_compressed_buffer(RZ_NONNULL RzBuffer *flirt_buf, ut32 expected_arch) {
	rz_return_val_if_fail(flirt_buf, NULL);

	ut8 *name = NULL;
	ut8 *buf = NULL, *decompressed_buf = NULL;
	RzBuffer *rz_buf = NULL;
	int size, decompressed_size;
	RzFlirtNode *node = NULL;
	RzFlirtNode *ret = NULL;
	idasig_v5_t *header = NULL;
	idasig_v6_v7_t *v6_v7 = NULL;
	idasig_v8_v9_t *v8_v9 = NULL;
	idasig_v10_t *v10 = NULL;

	ParseStatus ps = { 0 };

	if (!(ps.version = rz_sign_flirt_get_version(flirt_buf))) {
		goto exit;
	}

	if (ps.version < 5 || ps.version > 10) {
		RZ_LOG_ERROR("FLIRT: Unsupported flirt signature version\n");
		goto exit;
	}

	if (!(header = RZ_NEW0(idasig_v5_t))) {
		goto exit;
	}

	parse_v5_header(flirt_buf, header);

	if (expected_arch != RZ_FLIRT_SIG_ARCH_ANY && header->arch != expected_arch) {
		goto exit;
	}

	if (ps.version >= 6) {
		if (!(v6_v7 = RZ_NEW0(idasig_v6_v7_t))) {
			goto exit;
		}
		if (!parse_v6_v7_header(flirt_buf, v6_v7)) {
			goto exit;
		}

		if (ps.version >= 8) {
			if (!(v8_v9 = RZ_NEW0(idasig_v8_v9_t))) {
				goto exit;
			}
			if (!parse_v8_v9_header(flirt_buf, v8_v9)) {
				goto exit;
			}

			if (ps.version >= 10) {
				if (!(v10 = RZ_NEW0(idasig_v10_t))) {
					goto exit;
				}
				if (!parse_v10_header(flirt_buf, v10)) {
					goto exit;
				}
			}
		}
	}

	name = malloc(header->library_name_len + 1);
	if (!name) {
		goto exit;
	}

	if (rz_buf_read(flirt_buf, name, header->library_name_len) != header->library_name_len) {
		goto exit;
	}

	name[header->library_name_len] = '\0';

	size = rz_buf_size(flirt_buf) - rz_buf_tell(flirt_buf);
	buf = malloc(size);
	if (rz_buf_read(flirt_buf, buf, size) != size) {
		goto exit;
	}

	if (header->features & IDASIG__FEATURE__COMPRESSED) {
		if (ps.version >= 5 && ps.version < 7) {
			if (!(decompressed_buf = rz_inflate_ignore_header(buf, size, NULL, &decompressed_size))) {
				RZ_LOG_ERROR("FLIRT: Failed to decompress buffer.\n");
				goto exit;
			}
		} else if (ps.version >= 7) {
			if (!(decompressed_buf = rz_inflate(buf, size, NULL, &decompressed_size))) {
				RZ_LOG_ERROR("FLIRT: Failed to decompress buffer.\n");
				goto exit;
			}
		} else {
			RZ_LOG_ERROR("FLIRT: Sorry we do not support compressed signatures with version %d.\n", ps.version);
			goto exit;
		}

		RZ_FREE(buf);
		buf = decompressed_buf;
		size = decompressed_size;
	}
	rz_buf = rz_buf_new_with_pointers(buf, size, false);
	if (!rz_buf) {
		goto exit;
	}
	ps.buffer = rz_buf;

	if (!(node = RZ_NEW0(RzFlirtNode))) {
		goto exit;
	}

	if (parse_tree(&ps, node)) {
		ret = node;
	} else {
		free(node);
	}

exit:
	free(buf);
	rz_buf_free(rz_buf);
	free(header);
	free(v6_v7);
	free(v8_v9);
	free(v10);
	free(name);
	return ret;
}

/**
 * \brief Returns the FLIRT file version read from the RzBuffer
 * This function returns the FLIRT file version, when it fails returns 0
 *
 * \param  buffer The buffer to read
 * \return        Parsed FLIRT version
 */
RZ_API ut8 rz_sign_flirt_get_version(RZ_NONNULL RzBuffer *buffer) {
	rz_return_val_if_fail(buffer, false);
	ut8 ret = 0;

	idasig_v5_t *header = RZ_NEW0(idasig_v5_t);
	if (!header) {
		goto exit;
	}

	if (rz_buf_read(buffer, header->magic, sizeof(header->magic)) != sizeof(header->magic)) {
		goto exit;
	}

	if (strncmp((const char *)header->magic, "IDASGN", 6)) {
		goto exit;
	}

	if (rz_buf_read(buffer, &header->version, sizeof(header->version)) != sizeof(header->version)) {
		goto exit;
	}

	ret = header->version;

exit:
	free(header);

	return ret;
}

/**
 * \brief Returns the FLIRT arch id from a given arch name
 * Returns RZ_FLIRT_SIG_ARCH_ANY if name is not found.
 *
 * \param  arch The arch to convert to id
 * \return      The FLIRT arch id.
 */
RZ_API ut32 rz_sign_flirt_id_from_name(RZ_NONNULL const char *arch) {
	rz_return_val_if_fail(RZ_STR_ISNOTEMPTY(arch), RZ_FLIRT_SIG_ARCH_ANY);

	for (ut32 i = 0; i < RZ_ARRAY_SIZE(arch_id_map); ++i) {
		if (!strcmp(arch, arch_id_map[i].name)) {
			return arch_id_map[i].id;
		}
	}

	return RZ_FLIRT_SIG_ARCH_ANY;
}

/**
 * \brief Parses the FLIRT file and applies the signatures
 *
 * \param analysis    The RzAnalysis structure
 * \param flirt_file  The FLIRT file to parse
 */
RZ_API void rz_sign_flirt_apply(RzAnalysis *analysis, const char *flirt_file, const char *arch) {
	rz_return_if_fail(analysis && RZ_STR_ISNOTEMPTY(flirt_file));
	RzBuffer *flirt_buf = NULL;
	RzFlirtNode *node = NULL;
	ut32 id = RZ_FLIRT_SIG_ARCH_ANY;

	const char *extension = rz_str_lchr(flirt_file, '.');
	if (RZ_STR_ISEMPTY(extension) || (strcmp(extension, ".sig") != 0 && strcmp(extension, ".pac") != 0)) {
		RZ_LOG_ERROR("FLIRT: unknown extension '%s'\n", extension);
		return;
	}

	if (!(flirt_buf = rz_buf_new_slurp(flirt_file))) {
		RZ_LOG_ERROR("FLIRT: Can't open %s\n", flirt_file);
		return;
	}

	if (RZ_STR_ISNOTEMPTY(arch)) {
		ut32 id = rz_sign_flirt_id_from_name(arch);
		if (id == RZ_FLIRT_SIG_ARCH_ANY) {
			RZ_LOG_ERROR("FLIRT: unknown arch %s\n", arch);
			return;
		}
	}

	if (!strcmp(extension, ".pac")) {
		node = rz_sign_flirt_parse_string_buffer(flirt_buf);
	} else {
		node = rz_sign_flirt_parse_compressed_buffer(flirt_buf, id);
	}

	rz_buf_free(flirt_buf);
	if (node) {
		if (!node_match_functions(analysis, node)) {
			RZ_LOG_ERROR("FLIRT: Error while scanning the file %s\n", flirt_file);
		}
		rz_sign_flirt_node_free(node);
		return;
	} else {
		RZ_LOG_ERROR("FLIRT: We encountered an error while parsing the file %s. Sorry.\n", flirt_file);
		return;
	}
}