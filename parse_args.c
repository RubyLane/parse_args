/* TODO:
 *  - Better error messages
 */

#include <parse_argsInt.h>

#define NS "::" PACKAGE_NAME

static struct tracked_instances	g_tracked_instances;

// Process-global so that a spec_id identifies a spec even for Tcl_Objs shared
// between interps (option lookups are cached on the arg objs keyed by it)
static atomic_ulong				g_spec_id_seq;

struct option_info {
	Tcl_Obj*	param;			// What this param is called in the spec
	Tcl_Obj*	name;			// The name that will store this params value
	Tcl_Obj*	default_val;	// NULL if no default
	Tcl_Obj*	validator;		// NULL if no validator
	Tcl_Obj*	enum_choices;	// NULL if not an enum, also stores multi_choices for a multi
	Tcl_Obj*	comment;		// NULL if no comment
	int			arg_count;		// -1: store the option name in -name if present; -2: comsume all remaining args
	int			all_idx;		// >= 0 - collect all instances of this option as a list, accumulated in a list at this index
	int			multi_idx;		// If this option is part of a multi select
	int			decl_idx;		// Index of this entry in the argspec (options and positionals), -1 for spec->multi entries
	bool		is_args;		// args style processing - consume all remaining arguments
	bool		required;
	bool		alias;			// boolean
	bool		end_options;	// Treat seeing this option as if -- followed immediately after it
};

struct parse_spec {
	struct tracked_instance	inst;		// Must be first (so that parse_spec can be cast to tracked_instance)

	int						refcount;	// Held by the intrep, and by each bind in progress (a validator could shimmer the spec obj)
	unsigned long			spec_id;	// unique, monotonic (until overflow), assigned from g_spec_id_seq
	struct option_info*		option;		// Ordered for spec_find_option (currently: sorted in strcmp(param) collation order, for bsearch)
	int						option_count;
	int						decl_count;	// Number of entries in the argspec (options + positionals)
	bool					has_alias;
	Tcl_Obj*				usage_msg;
	struct option_info*		positional;
	int						positional_arg_count;
	struct option_info*		multi;
	int						multi_count;
	int						all_count;	// Number of -all accumulators a bind needs
	int						min_required_positional;	// sum over -required positionals of max(1, arg_count); is_args contributes 0
};

// Micro Tcl_ObjType - enum_choices <<<
static void free_enum_choices_intrep(Tcl_Obj* obj);
static void dup_drop_intrep(Tcl_Obj* src, Tcl_Obj* dst);

static Tcl_ObjType enum_choices_type = {
	.name			= "parse_spec_enum_choices",
	.freeIntRepProc	= free_enum_choices_intrep,
	.dupIntRepProc	= dup_drop_intrep,
};

/* Duplicates of our objtypes whose intreps own resources don't get the intrep:
 * they are regenerated on demand from the string rep, which these objtypes
 * ensure exists before storing their intreps */
static void dup_drop_intrep(Tcl_Obj* src, Tcl_Obj* dst)
{
	(void)src;
	(void)dst;
}

static void free_enum_choices_intrep(Tcl_Obj* obj)
{
	Tcl_ObjInternalRep*			ir = Tcl_FetchInternalRep(obj, &enum_choices_type);

	if (ir->otherValuePtr) {
		ckfree(ir->otherValuePtr);
		ir->otherValuePtr = NULL;
	}
	forget_instance(&g_tracked_instances, obj, NULL);
}

static int GetEnumChoicesFromObj(Tcl_Interp* interp, Tcl_Obj* obj, char*** res)
{
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(obj, &enum_choices_type);

	if (!ir) {
		const char**	table;
		Tcl_Size		len;

		TEST_OK(Tcl_SplitList(interp, Tcl_GetString(obj), &len, &table));

		Tcl_StoreInternalRep(obj, &enum_choices_type, &(Tcl_ObjInternalRep){ .otherValuePtr = table });
		register_instance(&g_tracked_instances, obj, NULL);
		ir = Tcl_FetchInternalRep(obj, &enum_choices_type);
	}

	*res = ir->otherValuePtr;

	return TCL_OK;
}
// Micro Tcl_ObjType - enum_choices >>>
// Micro Tcl_ObjType - option <<<
/* Using Tcl_GetIndexFromObj to look up arguments against the option name list (and cache those
   lookups) has a flaw: the objects are converted to a type that caches the found index for the
   table whose cache validity is based on the address of the table, but our tables are dynamic:
   they live as long as the parse spec obj, but Tcl (especially in optimized builds) will eagerly
   recycle Tcl_Objs and under heavy churn a new parse spec can re-use the address of a previous
   one, and so indicies looked up against the old spec (which doesn't match the new one) are returned
   without consulting the new which can lead to `-foo` resolving to `-bar` in the old map because
   it is in the same slot, or an out-of-bounds reference if `-foo` was at an index beyond the last
   element in the new spec.  Our performance requirements cannot afford to prevent this caching
   (using flag TCL_INDEX_TEMP_TABLE for Tcl_GetIndexFromObj), so we have to implement our own
   index caching lookup that is robust for our use case (include a unique global parse_spec monotonic
   id in the cache key)
*/
static void dup_option_intrep(Tcl_Obj* src, Tcl_Obj* dst);
static void free_option_intrep(Tcl_Obj* obj);

static Tcl_ObjType option_type = {
	.name			= "parse_spec_option",
	.freeIntRepProc	= free_option_intrep,
	.dupIntRepProc	= dup_option_intrep,
};

static void free_option_intrep(Tcl_Obj* obj)
{
	forget_instance(&g_tracked_instances, obj, NULL);
}

static void dup_option_intrep(Tcl_Obj* src, Tcl_Obj* dst)
{
	Tcl_StoreInternalRep(dst, &option_type, Tcl_FetchInternalRep(src, &option_type));
	register_instance(&g_tracked_instances, dst, NULL);
}

/* Option name lookup <<<
 * spec_index_options arranges spec->option at spec compile time, and
 * spec_find_option looks up an option by name in it.  These are the only places
 * that know how options are indexed (currently: sorted, with bsearch), so that
 * the strategy can be swapped out (a radish trie, for instance).  The order of
 * spec->option is otherwise irrelevant: everything that depends on the
 * declaration order uses option_info.decl_idx.
 */
static int cmp_opt(const void* a, const void* b)
{
	const struct option_info*	opt_a = a;
	const struct option_info*	opt_b = b;
	return strcmp(Tcl_GetString(opt_a->param), Tcl_GetString(opt_b->param));
}

static int cmp_opt_key(const void* key, const void* elem)
{
	const struct option_info*	opt = elem;
	return strcmp(key, Tcl_GetString(opt->param));
}

static void spec_index_options(struct parse_spec* spec)
{
	qsort(spec->option, spec->option_count, sizeof(spec->option[0]), cmp_opt);
}

static struct option_info* spec_find_option(struct parse_spec* spec, const char* str, Tcl_Size len)
{
	(void)len;
	return bsearch(str, spec->option, spec->option_count, sizeof(spec->option[0]), cmp_opt_key);
}

// Option name lookup >>>
static void spec_decl_order(const struct parse_spec* spec, const struct option_info* out[]) //<<<
{
	// out must have room for spec->decl_count entries
	for (int i=0; i<spec->option_count; i++)
		out[spec->option[i].decl_idx] = &spec->option[i];
	for (int i=0; i<spec->positional_arg_count; i++)
		out[spec->positional[i].decl_idx] = &spec->positional[i];
}

//>>>
static int bad_option(Tcl_Interp* interp, const struct parse_spec* spec, const char* str) //<<<
{
	// Same message and errorcode as the Tcl_GetIndexFromObj lookup this replaced
	if (!interp) return TCL_ERROR;

	const struct option_info*	decl[spec->decl_count ? spec->decl_count : 1];
	spec_decl_order(spec, decl);

	Tcl_Obj*	msg = Tcl_ObjPrintf("bad option \"%s\": ", str);
	if (spec->option_count == 0) {
		Tcl_AppendToObj(msg, "no valid options", -1);
	} else {
		Tcl_AppendToObj(msg, "must be ", -1);
		int	listed = 0;
		for (int d=0; d<spec->decl_count; d++) {
			if (Tcl_GetString(decl[d]->param)[0] != '-') continue;
			if (listed++)
				Tcl_AppendToObj(msg, listed < spec->option_count ? ", " : spec->option_count > 2 ? ", or " : " or ", -1);
			Tcl_AppendObjToObj(msg, decl[d]->param);
		}
	}
	Tcl_SetObjResult(interp, msg);
	Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "INDEX", "option", str, NULL);
	return TCL_ERROR;
}

//>>>
static int GetOptionFromObj(Tcl_Interp* interp, struct parse_spec* spec, Tcl_Obj* opt, struct option_info** option)
{
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(opt, &option_type);

	for (;;) {
		if (!ir) {
			Tcl_Size			len;
			const char*			str = Tcl_GetStringFromObj(opt, &len);
			struct option_info*	o = spec_find_option(spec, str, len);
			if (!o) return bad_option(interp, spec, str);
			Tcl_StoreInternalRep(opt, &option_type, &(Tcl_ObjInternalRep){
					.ptrAndLongRep = {
						.ptr	= o,
						.value	= spec->spec_id
					}
			});
			register_instance(&g_tracked_instances, opt, NULL);
			ir = Tcl_FetchInternalRep(opt, &option_type);
		}

		if (ir->ptrAndLongRep.value != spec->spec_id) {
			// Cached value is stale, force a new lookup
			ir = NULL;
			continue;
		}

		*option = ir->ptrAndLongRep.ptr;
		break;
	}

	return TCL_OK;
}
// Micro Tcl_ObjType - option >>>

static void free_internal_rep(Tcl_Obj* obj);

static Tcl_ObjType parse_spec_type = {
	.name			= "parse_spec",
	.freeIntRepProc	= free_internal_rep,
	.dupIntRepProc	= dup_drop_intrep,
};

/* Allocate static Tcl_Objs for these strings for each interp */
static const char* static_str[] = {
	"0",
	"1",
	"idx",
	"required",
	"default",
	"validate",
	"choices",
	"all",
	NULL
};
enum static_objs {
	L_FALSE=0,
	L_TRUE,
	L_IDX,
	L_REQUIRED,
	L_DEFAULT,
	L_VALIDATE,
	L_CHOICES,
	L_ALL,
	L_end
};

struct interp_cx {
	Tcl_Obj*		obj[L_end];
	Tcl_Obj*		enums;
};

// Fast unsigned int to string conversion from the talk by Alexandrescu: "Three Optimization Tips for C++" <<<
static uint32_t digits10(uint64_t v) //<<<
{
#define P01	10
#define P02	100
#define P03	1000
#define P04	10000
#define P05	100000
#define P06	1000000
#define P07	10000000
#define P08	100000000
#define P09	1000000000
#define P10	10000000000
#define P11	100000000000
#define P12	1000000000000
	if (v < P01) return 1;
	if (v < P02) return 2;
	if (v < P03) return 3;
	if (v < P12) {
		if (v < P08) {
			if (v < P06) {
				if (v < P04) return 4;
				return 5 + (v >= P05);
			}
			return 7 + (v >= P07);
		}
		if (v < P10) {
			return 9 + (v >= P09);
		}
		return 11 + (v >= P11);
	}
	return 12 + digits10(v / P12);
}

//>>>
static int u64toa(uint64_t value, char* restrict dst) //<<<
{
	// TODO: benchmark this against TclFormatInt and replace the latter with this if it's faster
	static const char digits[201] =
		"0001020304050607080910111213141516171819"
		"2021222324252627282930313233343536373839"
		"4041424344454647484950515253545556575859"
		"6061626364656667686970717273747576777879"
		"8081828384858687888990919293949596979899";
	const uint32_t length = digits10(value);
	uint32_t next = length-1;

	while (value >= 100) {
		const int i = (value % 100) * 2;
		value /= 100;
		memcpy(dst+next-1, digits+i, 2);
		//dst[next] = digits[i+1];
		//dst[next-1] = digits[i];
		next -= 2;
	}
	if (value < 10) {
		dst[next] = '0' + (uint32_t)value;
	} else {
		const int i = (uint32_t)value * 2;
		memcpy(dst+next-1, digits+i, 2);
		//dst[next] = digits[i + 1];
		//dst[next-1] = digits[i];
	}

	return length;
}

//>>>
//>>>
static const char* static_numstr(uint64_t v) //<<<
{
	static char	staticbuf[21];		// 21 - max length of decimal representation of uint64_t + null terminator
	staticbuf[u64toa(v, staticbuf)] = 0;
	return staticbuf;
}

//>>>
static void free_option_info(struct option_info* option) //<<<
{
	if (option) {
		replace_tclobj(&option->param,			NULL);
		replace_tclobj(&option->name,			NULL);
		replace_tclobj(&option->default_val,	NULL);
		replace_tclobj(&option->validator,		NULL);
		replace_tclobj(&option->enum_choices,	NULL);
		replace_tclobj(&option->comment,		NULL);
	}
}

//>>>
static void free_parse_spec(struct parse_spec** specPtr) //<<<
{
	struct parse_spec*	spec = *specPtr;

	if (spec) {
		// If NULL then we haven't registered the instance yet (or the intrep has been freed)
		if (spec->inst.self)
			forget_instance(&g_tracked_instances, spec->inst.self, &spec->inst);

		if (spec->option != NULL) {
			for (int i=0; i < spec->option_count; i++)
				free_option_info(&spec->option[i]);

			ckfree(spec->option);
			spec->option = NULL;
		}

		replace_tclobj(&spec->usage_msg, NULL);

		if (spec->positional) {
			for (int i=0; i < spec->positional_arg_count; i++)
				free_option_info(&spec->positional[i]);

			ckfree(spec->positional);
			spec->positional = NULL;
		}

		if (spec->multi) {
			for (int i=0; i < spec->multi_count; i++)
				free_option_info(&spec->multi[i]);

			ckfree(spec->multi);
			spec->multi = NULL;
		}

		ckfree(spec);
		*specPtr = NULL;
	}
}

//>>>
static inline void spec_incref(struct parse_spec* spec) //<<<
{
	spec->refcount++;
}

//>>>
static void spec_decref(struct parse_spec* spec) //<<<
{
	if (--spec->refcount <= 0)
		free_parse_spec(&spec);
}

//>>>
static void free_internal_rep(Tcl_Obj* obj) //<<<
{
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(obj, &parse_spec_type);
	struct parse_spec*	spec = ir->otherValuePtr;

	// The spec can outlive the intrep (if this happens during a bind)
	forget_instance(&g_tracked_instances, obj, &spec->inst);
	spec_decref(spec);
	ir->otherValuePtr = NULL;
}

//>>>
static Tcl_Obj* build_usage_msg(const struct parse_spec* spec) //<<<
{
	// Like a proc's usage: options in argspec order, then the positionals
	const struct option_info*	decl[spec->decl_count ? spec->decl_count : 1];
	spec_decl_order(spec, decl);

	Tcl_Obj*	msg = Tcl_NewObj();
#define APPEND(...) Tcl_AppendStringsToObj(msg, __VA_ARGS__, (char*)NULL)
	for (int pass=0; pass<2; pass++) {
		for (int d=0; d<spec->decl_count; d++) {
			const struct option_info*	o = decl[d];
			const char*					param = Tcl_GetString(o->param);
			const bool					is_option = param[0] == '-';
			if (is_option != (pass == 0)) continue;

			if (Tcl_GetCharLength(msg)) APPEND(" ");
			if (is_option) {
				// A value placeholder: the -enum choices, else the output name
				Tcl_Obj*	ph = NULL;
				defer { replace_tclobj(&ph, NULL); }
				if (o->enum_choices) {
					Tcl_Size	ec;
					Tcl_Obj**	ev;
					if (Tcl_ListObjGetElements(NULL, o->enum_choices, &ec, &ev) == TCL_OK && ec > 0) {
						replace_tclobj(&ph, Tcl_NewObj());
						for (Tcl_Size e=0; e<ec; e++)
							Tcl_AppendStringsToObj(ph, e ? "|" : "", Tcl_GetString(ev[e]), (char*)NULL);
					}
				}
				if (!ph) replace_tclobj(&ph, o->name);
				const char*	phs = Tcl_GetString(ph);

				if (!o->required) APPEND("?");
				APPEND(param);
				switch (o->arg_count) {
					case -2:	APPEND(" ", phs, " ..."); break;	// -args all
					case -1:	break;								// -multi flag
					case 0:		break;								// -boolean
					default:
						for (int n=0; n<o->arg_count; n++) APPEND(" ", phs);
				}
				if (!o->required) APPEND("?");
			} else if (o->is_args || o->arg_count == -2) {
				APPEND("?", param, " ...?");
			} else {
				const bool	optional = !o->required || o->default_val;
				const int	n = o->arg_count > 0 ? o->arg_count : 1;
				if (optional) APPEND("?");
				for (int i=0; i<n; i++) APPEND(i ? " " : "", param);
				if (optional) APPEND("?");
			}
		}
	}
#undef APPEND
	return msg;
}

//>>>
static int compile_parse_spec(Tcl_Interp* interp, Tcl_Obj* obj, struct parse_spec** res) //<<<
{
	struct interp_cx*	l = (struct interp_cx*)Tcl_GetAssocData(interp, "parse_args", NULL);

	Tcl_Obj*		multis = NULL;		defer { replace_tclobj(&multis, NULL); }
	replace_tclobj(&multis, Tcl_NewDictObj());

	Tcl_Size	oc;
	Tcl_Obj**	ov;
	TEST_OK(Tcl_ListObjGetElements(interp, obj, &oc, &ov));
	if (oc % 2 != 0) THROW_ERROR("argspec must be a dictionary");

	struct parse_spec*	spec = ckalloc(sizeof(struct parse_spec));
	*spec = (struct parse_spec){
		.refcount	= 1,
		.spec_id	= atomic_fetch_add(&g_spec_id_seq, 1) + 1,
		.decl_count	= oc / 2,
	};
	defer { if (spec) free_parse_spec(&spec); }

	{
		/* A repeated key would leave the earlier definition unreachable but
		 * still enforced (-required, -default), so reject it */
		Tcl_HashTable	seen;
		Tcl_InitHashTable(&seen, TCL_STRING_KEYS);
		defer { Tcl_DeleteHashTable(&seen); }

		for (int i=0; i<oc; i+=2) {
			const char*	str = Tcl_GetString(ov[i]);
			int			isnew;
			Tcl_CreateHashEntry(&seen, str, &isnew);
			if (!isnew) THROW_ERROR("Duplicate argspec entry \"", str, "\"");
			if (str[0] == '-')	spec->option_count++;
			else				spec->positional_arg_count++;
		}
	}

	spec->option		= ckalloc(sizeof(struct option_info) * spec->option_count);
	spec->positional	= ckalloc(sizeof(struct option_info) * spec->positional_arg_count);
	memset(spec->option,		0, sizeof(struct option_info) * spec->option_count);
	memset(spec->positional,	0, sizeof(struct option_info) * spec->positional_arg_count);

	int	o_i = 0;	// Number of options
	int	p_i = 0;	// Number of positional params
	for (int i=0; i<oc; i+=2) {
		Tcl_Size	str_len;
		const char*	str = Tcl_GetStringFromObj(ov[i], &str_len);

		struct option_info* option =
			str[0] == '-'
				? &spec->option[o_i++]
				: &spec->positional[p_i++];

		*option = (struct option_info){
			.arg_count	= 1,
			.multi_idx	= -1,
			.all_idx	= -1,
			.decl_idx	= i / 2,
		};
		replace_tclobj(&option->param, ov[i]);

		Tcl_Size	settingc;
		Tcl_Obj**	settingv;
		TEST_OK(Tcl_ListObjGetElements(interp, ov[i+1], &settingc, &settingv));
		bool	all_specified = false;
		for (int j=0; j<settingc; ) {
#			define SETTINGS \
				X( SETTING_DEFAULT,		"-default"	) \
				X( SETTING_REQUIRED,	"-required"	) \
				X( SETTING_VALIDATE,	"-validate"	) \
				X( SETTING_NAME,		"-name"		) \
				X( SETTING_BOOLEAN,		"-boolean"	) \
				X( SETTING_ARGS,		"-args"		) \
				X( SETTING_ENUM,		"-enum"		) \
				X( SETTING_COMMENT,		"-#"		) \
				X( SETTING_MULTI,		"-multi"	) \
				X( SETTING_ALIAS,		"-alias"	) \
				X( SETTING_ALL,			"-all"		) \
				X( SETTING_END,			"-end"		) \
				/* line intentionally left */
			static const char* settings[] = {
#				define X(e, s) s,
				SETTINGS
#				undef X
				(char*)NULL
			};
			enum setting {
#				define X(e, s) e,
				SETTINGS
#				undef X
			} setting;
#			undef SETTINGS

			int settingidx;
			TEST_OK(Tcl_GetIndexFromObj(interp, settingv[j], settings, "setting", TCL_EXACT, &settingidx));
			setting = settingidx;
			j++;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
			// Pre-validate that the settings which require a value have one
			switch (setting) {
				case SETTING_DEFAULT:
				case SETTING_VALIDATE:
				case SETTING_NAME:
				case SETTING_ARGS:
				case SETTING_ENUM:
				case SETTING_COMMENT:
					if (j >= settingc) THROW_ERROR(Tcl_GetString(settingv[j-1]), " needs a value");
					break;
			}
#pragma GCC diagnostic pop

			switch (setting) {
				case SETTING_DEFAULT:	replace_tclobj(&option->default_val, settingv[j++]); break;
				case SETTING_REQUIRED:	option->required = true; break;
				case SETTING_NAME:		replace_tclobj(&option->name, settingv[j++]); break;
				case SETTING_COMMENT:	replace_tclobj(&option->comment, settingv[j++]); break;
				case SETTING_ALIAS:		option->alias = spec->has_alias = true; break;
				case SETTING_ALL:		all_specified = true; break;	// Just raise a flag here to prevent multiple -all specs from excessively incrementing all_count
				case SETTING_END:		option->end_options = true; break;
				case SETTING_VALIDATE: //<<<
					{
						Tcl_Obj*	validatorobj = settingv[j++];
						Tcl_Obj**	vv;
						Tcl_Size	vc;
						TEST_OK(Tcl_ListObjGetElements(interp, validatorobj, &vc, &vv));
						if (vc) replace_tclobj(&option->validator, validatorobj);
					}
					break;
					//>>>
				case SETTING_BOOLEAN: //<<<
					if (str[0] != '-')
						THROW_ERROR("Cannot use -boolean on positional argument \"", Tcl_GetString(option->param), "\"");
					option->arg_count = 0;
					break;
					//>>>
				case SETTING_ARGS: //<<<
					if (strcmp("all", Tcl_GetString(settingv[j])) == 0) {
						option->arg_count = -2;
					} else {
						TEST_OK(Tcl_GetIntFromObj(interp, settingv[j], &option->arg_count));
						if (option->arg_count < 0)
							THROW_ERROR("-args cannot be negative");
						if (option->arg_count == 0 && str[0] != '-')
							THROW_ERROR("Cannot use -args 0 on positional argument \"", Tcl_GetString(option->param), "\"");
					}
					j++;
					break;
					//>>>
				case SETTING_ENUM: //<<<
					{
						/* enums are validated using Tcl_GetIndexFromObj, which
						 * shimmers its input obj to record the table and index
						 * info, so make an effort to unify enums across parse_specs
						 */
						if (!l->enums) return TCL_ERROR;

						Tcl_Obj*	enum_choices = settingv[j++];
						Tcl_Obj*	shared_enum_choices = NULL;
						TEST_OK(Tcl_DictObjGet(interp, l->enums, enum_choices, &shared_enum_choices));

						if (!shared_enum_choices) {
							if (Tcl_IsShared(l->enums))
								replace_tclobj(&l->enums, Tcl_DuplicateObj(l->enums));

							TEST_OK(Tcl_DictObjPut(interp, l->enums, enum_choices, shared_enum_choices = enum_choices));
						}

						Tcl_Size	size;
						TEST_OK(Tcl_DictObjSize(interp, l->enums, &size));

						// Paranoia - prevent the speculative enum cache from growing too large
						if (size > 1000) replace_tclobj(&l->enums, Tcl_NewDictObj());

						replace_tclobj(&option->enum_choices, shared_enum_choices);
					}
					break;
					//>>>
				case SETTING_MULTI: //<<<
					if (str[0] != '-')
						THROW_ERROR("Cannot use -multi on positional argument \"", Tcl_GetString(option->param), "\"");
					option->arg_count = -1;
					break;
					//>>>
				default: THROW_PRINTF("Invalid setting: %d", setting);
			}
		}

		if (!option->name) {
			replace_tclobj(&option->name,
				str[0] == '-'
					? Tcl_NewStringObj(str+1, str_len-1)
					: option->param
			);
		}

		if (option->arg_count == -1) {
			int			multi_idx;
			Tcl_Obj*	multi_choices;
			const char*	multi_val;
			Tcl_Size	multi_val_len;
			Tcl_Obj*	multi_config_loan = NULL;
			Tcl_Obj*	multi_all = NULL;

			TEST_OK(Tcl_DictObjGet(interp, multis, option->name, &multi_config_loan));
			if (!multi_config_loan) {
				multi_idx = spec->multi_count++;
				multi_config_loan = Tcl_NewDictObj();
				TEST_OK(Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_IDX], Tcl_NewIntObj(multi_idx)));
				TEST_OK(Tcl_DictObjPut(interp, multis, option->name, multi_config_loan));
				multi_choices = Tcl_NewListObj(0, NULL);
			} else {
				Tcl_Obj*	idx_obj = NULL;
				TEST_OK(Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_IDX], &idx_obj));
				TEST_OK(Tcl_GetIntFromObj(interp, idx_obj, &multi_idx));
				TEST_OK(Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_CHOICES], &multi_choices));
			}
			TEST_OK(Tcl_ListObjAppendElement(interp, multi_choices, option->param));
			TEST_OK(Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_CHOICES], multi_choices));

			option->multi_idx = multi_idx;

			// TODO: throw errors if -boolean or -args are mixed with -multi
			if (option->enum_choices) THROW_ERROR("Cannot use -multi and -enum together");

			if (option->required)
				TEST_OK(Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_REQUIRED], l->obj[L_TRUE]));
			if (option->default_val)
				TEST_OK(Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_DEFAULT],  option->default_val));
			if (option->validator)
				TEST_OK(Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_VALIDATE], option->validator));

			TEST_OK(Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_ALL], &multi_all));
			if (all_specified || multi_all) {
				int all_idx;
				if (multi_all) {
					TEST_OK(Tcl_GetIntFromObj(interp, multi_all, &all_idx));
				} else {
					all_idx = spec->all_count++;
				}
				option->all_idx = all_idx;
				if (!multi_all)
					TEST_OK(Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_ALL], Tcl_NewIntObj(all_idx)));
			}

			// Repurpose option->default_val to store the value this option will set in the multi output
			multi_val = Tcl_GetStringFromObj(option->param, &multi_val_len);
			replace_tclobj(&option->default_val, Tcl_NewStringObj(multi_val+1, multi_val_len-1));
		} else if (all_specified) {
			option->all_idx = spec->all_count++;
		}
	}

	spec_index_options(spec);

	// This is only known after parsing the options
	if (spec->multi_count > 0) {
		spec->multi = ckalloc(sizeof(struct option_info) * spec->multi_count);
		memset(spec->multi, 0, sizeof(struct option_info) * spec->multi_count);

		Tcl_DictSearch	search;
		int				done;
		Tcl_Obj*		multi_config_loan = NULL;
		Tcl_Obj*		name;
		TEST_OK(Tcl_DictObjFirst(interp, multis, &search, &name, &multi_config_loan, &done));
		defer { Tcl_DictObjDone(&search); }
		for (; !done; Tcl_DictObjNext(&search, &name, &multi_config_loan, &done)) {
			struct option_info* multi;

			Tcl_Obj*	idx_obj;
			TEST_OK(Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_IDX], &idx_obj));
			int idx;
			TEST_OK(Tcl_GetIntFromObj(interp, idx_obj, &idx));
			if (idx < 0 || idx >= spec->multi_count) {
				THROW_ERROR("Got out of bounds multi_count ", Tcl_GetString(idx_obj), " for option \"", Tcl_GetString(name));
			}
			multi = &spec->multi[idx];

			replace_tclobj(&multi->name, name);
			multi->arg_count = -1;
			multi->decl_idx = -1;

			Tcl_Obj*	val;

			// set -required
			TEST_OK(Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_REQUIRED], &val));
			if (val) multi->required = true;

			// set -default
			TEST_OK(Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_DEFAULT],  &val));
			replace_tclobj(&multi->default_val, val);

			// set -validate
			TEST_OK(Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_VALIDATE], &val));
			replace_tclobj(&multi->validator, val);

			// set multi choices (for error message if required and not set)
			TEST_OK(Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_CHOICES],  &val));
			replace_tclobj(&multi->enum_choices, val);

			TEST_OK(Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_ALL],  &val));
			if (val) {
				TEST_OK(Tcl_GetIntFromObj(interp, val, &multi->all_idx));
			} else {
				multi->all_idx = -1;
			}
		}
	}

	// Create special "args" behaviour for any positional param named "args".
	// Earlier this was restricted to the last positional, but now permitting
	// it at any position lets callers write specs like {args ... last_arg {-required}}.
	// strcmp is safe because Tcl_GetString always gives us a properly
	// \0 terminated string
	for (int i=0; i < spec->positional_arg_count; i++) {
		struct option_info*	p = &spec->positional[i];
		if (strcmp("args", Tcl_GetString(p->param)) == 0) {
			p->is_args = true;
			if (!p->default_val) replace_tclobj(&p->default_val, Tcl_NewObj());
		}
	}

	// Validate positional layout: only one is_args/-args all consumer, and any
	// positional that follows it must be -required (otherwise it could never
	// be reached, since the consumer is left-greedy modulo the suffix reservation).
	// Also compute min_required_positional = sum over positionals that need
	// args from the caller to succeed. A positional "needs args" iff it is
	// -required AND has no -default AND is not is_args/-args all.
	{
		bool saw_args_consumer = false;
		spec->min_required_positional = 0;
		for (int i=0; i < spec->positional_arg_count; i++) {
			struct option_info*	p = &spec->positional[i];
			int is_args_like = p->is_args || p->arg_count == -2;

			if (saw_args_consumer) {
				if (is_args_like)
					THROW_ERROR("Cannot have more than one args/-args all positional consumer (\"", Tcl_GetString(p->param), "\")");
				if (!p->required || p->default_val)
					THROW_ERROR("Positional \"", Tcl_GetString(p->param), "\" cannot follow an args/-args all consumer unless it is -required without a default");
			}
			if (is_args_like) saw_args_consumer = true;

			if (p->required && !is_args_like && !p->default_val)
				spec->min_required_positional += (p->arg_count > 0) ? p->arg_count : 1;
		}
	}

	replace_tclobj(&spec->usage_msg, build_usage_msg(spec));

	*res = spec;
	spec = NULL;	// Hand ownership to caller

	return TCL_OK;
}

//>>>

static int GetParseSpecFromObj(Tcl_Interp* interp, Tcl_Obj* spec, struct parse_spec** res) //<<<
{
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(spec, &parse_spec_type);

	if (!ir) {
		// Ensure the string rep exists before the intrep replaces whatever it
		// could be generated from (a spec built as a pure list, for instance)
		Tcl_GetString(spec);

		struct parse_spec*	compiled_spec = NULL;
		TEST_OK(compile_parse_spec(interp, spec, &compiled_spec));

		Tcl_StoreInternalRep(spec, &parse_spec_type, &(Tcl_ObjInternalRep){ .otherValuePtr = compiled_spec });
		ir = Tcl_FetchInternalRep(spec, &parse_spec_type);
		register_instance(&g_tracked_instances, spec, compiled_spec);
	}

	if (res)
		*res = ir->otherValuePtr;

	return TCL_OK;
}

//>>>

static int validate(Tcl_Interp* interp, struct option_info* option, Tcl_Obj* val) //<<<
{
	if (option->enum_choices) {
		int		dummy;
		char**	enum_table;

		TEST_OK(GetEnumChoicesFromObj(interp, option->enum_choices, &enum_table));
		TEST_OK(Tcl_GetIndexFromObj(interp, val, enum_table, Tcl_GetString(option->param), TCL_EXACT, &dummy));
	}

	if (option->validator) {
		Tcl_Size	dontcare;
		TEST_OK(Tcl_ListObjLength(interp, option->validator, &dontcare));	// Force to list before we dup (cache command lookup)

		Tcl_Obj*	valcmd = NULL;		defer { replace_tclobj(&valcmd, NULL); }
		replace_tclobj(&valcmd, Tcl_DuplicateObj(option->validator));
		TEST_OK(Tcl_ListObjAppendElement(interp, valcmd, val));
		const int res = Tcl_EvalObjEx(interp, valcmd, TCL_EVAL_DIRECT);
		Tcl_Obj*	verdict = NULL;		defer { replace_tclobj(&verdict, NULL); }
		replace_tclobj(&verdict, Tcl_GetObjResult(interp));

		int	passed;
		if (res == TCL_OK) {
			Tcl_ResetResult(interp);
			if (Tcl_GetCharLength(verdict) == 0) {
				// Accept a blank string as a pass
				passed = 1;
			} else {
				if (Tcl_GetBooleanFromObj(interp, verdict, &passed) != TCL_OK) {
					Tcl_ResetResult(interp);
					passed = 0;
				}
			}
		} else {
			passed = 0;
			Tcl_ResetResult(interp);
		}

		if (!passed) {
			Tcl_SetErrorCode(interp, "PARSE_ARGS", "VALIDATION", Tcl_GetString(option->param), NULL);
			THROW_PRINTF("Validation failed for \"%s\": %s",
						Tcl_GetString(option->param),
						Tcl_GetString(verdict)
			);
		}
	}

	return TCL_OK;
}

//>>>
/* Binding <<<
 * bind_args matches the args against a compiled spec and hands each resulting
 * value to a sink (bind_put), which stores it: as variables (the parse_args
 * command), into a dict (parse_args with a dict var, ParseArgs_BindDict), to a
 * C callback (ParseArgs_Bind), or converted into C slots (ParseArgs_BindSlots).
 * alias is true when the value is the name of a variable in the caller to alias
 * (an -alias option or positional that was supplied).
 */
typedef int (bind_put)(void* cx, Tcl_Interp* interp, struct option_info* option, Tcl_Obj* val, bool alias);

static int wrong_args(Tcl_Interp* interp, const struct parse_spec* spec) //<<<
{
	/* No command name: parse_args can't know which one is right (a proc's args,
	 * the values resumed into a coroutine, a script's argv, ...), callers can
	 * add that context if they want it */
	Tcl_WrongNumArgs(interp, 0, NULL, Tcl_GetString(spec->usage_msg));
	return TCL_ERROR;
}

static int bind_args(Tcl_Interp* interp, struct interp_cx* l, struct parse_spec* spec, Tcl_Size ac, Tcl_Obj*const av[], bind_put* put, void* cx) //<<<
{
	Tcl_Size	i;
	// Held, since a validator would otherwise free a fresh (refcount 0) list value
	Tcl_Obj*	val = NULL;		defer { replace_tclobj(&val, NULL); }

	// The spec must survive this bind, even if a validator shimmers the spec obj
	spec_incref(spec);
	defer { spec_decref(spec); }

	/* Per-bind state (kept off the spec so that a validator can re-enter a
	 * command that uses the same spec): which options and multis were supplied,
	 * and the -all accumulators */
	const size_t	all_bytes	= sizeof(Tcl_Obj*) * spec->all_count;
	const size_t	state_bytes	= all_bytes + spec->option_count + spec->multi_count;
	alignas(Tcl_Obj*) char	stackbuf[256];
	char*			state		= state_bytes <= sizeof(stackbuf) ? stackbuf : ckalloc(state_bytes);
	memset(state, 0, state_bytes);
	Tcl_Obj**		all				= (Tcl_Obj**)state;
	bool*			supplied		= (bool*)(state + all_bytes);
	bool*			multi_supplied	= supplied + spec->option_count;
	defer {
		for (int a=0; a<spec->all_count; a++)
			replace_tclobj(&all[a], NULL);
		if (state != stackbuf) ckfree(state);
	}

#define OUTPUT_DIRECT(option, val) \
	TEST_OK(put(cx, interp, (option), (val), false))

#define OUTPUT(option, val) \
	do { \
		if ((option)->all_idx >= 0) { \
			Tcl_Obj** _acc = &all[(option)->all_idx]; \
			if (*_acc == NULL) replace_tclobj(_acc, Tcl_NewListObj(1, NULL)); \
			TEST_OK(Tcl_ListObjAppendElement(interp, *_acc, (val))); \
		} else { \
			OUTPUT_DIRECT((option), (val)); \
		} \
	} while(0)

#define OUTPUT_ALIASABLE(option, val) \
	do { \
		if ((option)->alias) { \
			TEST_OK(put(cx, interp, (option), (val), true)); \
		} else { \
			OUTPUT((option), (val)); \
		} \
	} while(0)

#define VALIDATE(option, val) \
	if ((option)->validator != NULL || (option)->enum_choices != NULL) { \
		TEST_OK(validate(interp, (option), (val))); \
	}

	// Option phase: scan av[0..k) for options. Stop when:
	//   - we run out of args
	//   - the spec has no options at all
	//   - we reach a slot guaranteed-positional by the min-required-positional reservation
	//   - we hit "--", an arg not starting with '-', or an option marked -end
	// The reservation skips the Tcl_GetStringFromObj() for shimmer-hostile args
	// (e.g. large pure bytearrays) that must end up in a positional anyway.
	i = 0;
	bool check_options = (spec->option_count > 0);
	while (check_options && i < ac) {
		const char*	str;
		Tcl_Size	str_len;
		struct option_info*	option;
		struct option_info*	src_option;

		if (i >= ac - spec->min_required_positional) {
			check_options = false;
			break;
		}

		str = Tcl_GetStringFromObj(av[i], &str_len);

		if (str_len == 0 || str[0] != '-') {
			check_options = false;
			break;
		}

		if (str_len == 2 && str[1] == '-') {
			check_options = false;
			i++;
			break;
		}

		TEST_OK(GetOptionFromObj(interp, spec, av[i], &option));
		src_option = option;
		if (option->multi_idx >= 0) {
			option = &spec->multi[src_option->multi_idx];
			multi_supplied[src_option->multi_idx] = true;
		} else {
			supplied[option - spec->option] = true;
		}

		if (option->arg_count > 0 && ac - i - 1 < option->arg_count) {
			return wrong_args(interp, spec);
		}

		switch (option->arg_count) {
			case -2:	// -args all
				i++;
				replace_tclobj(&val, Tcl_NewListObj(ac-i, av+i));
				VALIDATE(option, val);
				OUTPUT(option, val);
				i = ac;
				break;

			case -1:	// -multi
				OUTPUT(option, src_option->default_val);
				i++;
				break;

			case 0:		// -boolean
				OUTPUT(option, l->obj[L_TRUE]);
				i++;
				break;

			case 1:
				replace_tclobj(&val, av[i+1]);
				VALIDATE(option, val);
				OUTPUT_ALIASABLE(option, val);
				i += 2;
				break;

			default:	// -args N, N > 1
				replace_tclobj(&val, Tcl_NewListObj(option->arg_count, av+i+1));
				VALIDATE(option, val);
				OUTPUT(option, val);
				i += option->arg_count + 1;
				break;
		}

		if (option->end_options) {
			check_options = false;
			break;
		}
	}

	// Positional phase: lookahead consumer. For each positional p:
	//   - is_args / -args all:          consume remaining - sum_later_req args;
	//                                   if 0 consumed and no default, leave unset.
	//   - -required, no default:        consume max(1, arg_count); error if short.
	//   - optional or -required+default: consume arg_count if remaining >= arg_count + sum_later_req;
	//                                   otherwise apply -default (or leave unset).
	// sum_later_req only counts positionals that *need* args from the caller
	// (-required AND no -default AND not is_args_like) — matching the
	// min_required_positional formula computed at spec-compile time.
	{
		Tcl_Size	remaining = ac - i;
		Tcl_Size	sum_remaining_req = spec->min_required_positional;

		for (int positional_arg = 0; positional_arg < spec->positional_arg_count; positional_arg++) {
			struct option_info*	p = &spec->positional[positional_arg];
			const int	is_args_like = p->is_args || p->arg_count == -2;
			const int	has_default = p->default_val != NULL;
			const int	needs_args = p->required && !is_args_like && !has_default;
			const int	p_contrib = needs_args ? ((p->arg_count > 0) ? p->arg_count : 1) : 0;
			const Tcl_Size	sum_later_req = sum_remaining_req - p_contrib;

			if (is_args_like) {
				Tcl_Size take_n = remaining - sum_later_req;
				if (take_n < 0) take_n = 0;	// later -required will report the shortage
				if (take_n > 0) {
					replace_tclobj(&val, Tcl_NewListObj(take_n, av+i));
					VALIDATE(p, val);
					OUTPUT_DIRECT(p, val);
					i += take_n;
					remaining -= take_n;
				} else if (has_default) {
					OUTPUT_DIRECT(p, p->default_val);
				}
				// else: leave unset (matches old behaviour for explicit -args all without a default)
			} else if (needs_args) {
				const int take_n = (p->arg_count > 0) ? p->arg_count : 1;
				if (remaining < take_n) {
					if (remaining == 0) {
						Tcl_SetErrorCode(interp, "PARSE_ARGS", "REQUIRED", Tcl_GetString(p->param), NULL);
						THROW_ERROR("argument ", Tcl_GetString(p->name), " is required");
					} else {
						Tcl_SetErrorCode(interp, "PARSE_ARGS", "WRONGARGS", Tcl_GetString(p->name), NULL);
						THROW_ERROR("Expecting ", static_numstr(take_n), " arguments for ", Tcl_GetString(p->name));
					}
				}
				if (take_n == 1) {
					VALIDATE(p, av[i]);
					if (p->alias) {
						TEST_OK(put(cx, interp, p, av[i], true));
					} else {
						OUTPUT_DIRECT(p, av[i]);
					}
				} else {
					replace_tclobj(&val, Tcl_NewListObj(take_n, av+i));
					VALIDATE(p, val);
					OUTPUT_DIRECT(p, val);
				}
				i += take_n;
				remaining -= take_n;
			} else {	// optional (incl. -required with -default)
				const int take_n = (p->arg_count > 0) ? p->arg_count : 1;
				if (p->arg_count > 0 && remaining >= take_n + sum_later_req) {
					if (take_n == 1) {
						VALIDATE(p, av[i]);
						if (p->alias) {
							TEST_OK(put(cx, interp, p, av[i], true));
						} else {
							OUTPUT_DIRECT(p, av[i]);
						}
					} else {
						replace_tclobj(&val, Tcl_NewListObj(take_n, av+i));
						VALIDATE(p, val);
						OUTPUT_DIRECT(p, val);
					}
					i += take_n;
					remaining -= take_n;
				} else if (has_default) {
					// Defaults baked in at spec compile time aren't
					// re-validated — they may intentionally be outside
					// the validator's domain.
					OUTPUT_DIRECT(p, p->default_val);
				}
				// else: leave unset
			}

			sum_remaining_req = sum_later_req;
		}

		if (remaining > 0) {
			return wrong_args(interp, spec);
		}
	}

	// Check -required, set -default for options that weren't specified, and set the -all lists
	for (int o=0; o < spec->option_count; o++) {
		struct option_info* option = &spec->option[o];

		if (option->all_idx >= 0) {
			const int	all_idx = option->all_idx;
			if (all[all_idx]) {
				OUTPUT_DIRECT(option, all[all_idx]);
				replace_tclobj(&all[all_idx], NULL);
			}
		}

		if (supplied[o]) continue;
		if (option->multi_idx >= 0) continue;

		if (option->default_val != NULL) {
			OUTPUT_DIRECT(option, option->default_val);
		} else if (option->arg_count == 0) {
			OUTPUT_DIRECT(option, l->obj[L_FALSE]);
		} else {
			if (option->required) {
				Tcl_SetErrorCode(interp, "PARSE_ARGS", "REQUIRED", Tcl_GetString(option->param), NULL);
				THROW_ERROR("option ", Tcl_GetString(option->param), " is required");
			}
		}
	}

	// Check -required and set -default for multi options that weren't specified
	for (int m=0; m < spec->multi_count; m++) {
		struct option_info* option = &spec->multi[m];

		if (multi_supplied[m]) continue;

		if (option->default_val) {
			// Direct: the -all accumulators have already been flushed, and
			// the default is used as given (as for an -all option's default)
			OUTPUT_DIRECT(option, option->default_val);
		} else if (option->required) {
			Tcl_SetErrorCode(interp, "PARSE_ARGS", "REQUIRED_ONE_OF", Tcl_GetString(option->enum_choices), NULL);
			THROW_ERROR("one of ", Tcl_GetString(option->enum_choices), " are required");
		}
	}

#undef OUTPUT_DIRECT
#undef OUTPUT
#undef OUTPUT_ALIASABLE
#undef VALIDATE

	return TCL_OK;
}

//>>>
static int put_var(void* cx, Tcl_Interp* interp, struct option_info* option, Tcl_Obj* val, bool alias) //<<<
{
	(void)cx;

	if (alias)
		return Tcl_UpVar(interp, "1", Tcl_GetString(val), Tcl_GetString(option->name), 0);

	if (Tcl_ObjSetVar2(interp, option->name, NULL, val, TCL_LEAVE_ERR_MSG) == NULL)
		return TCL_ERROR;

	return TCL_OK;
}

//>>>
static int put_dict(void* cx, Tcl_Interp* interp, struct option_info* option, Tcl_Obj* val, bool alias) //<<<
{
	Tcl_Obj*	dict = cx;

	// TODO: fetch the value from the variable called option->name in the parent callframe
	if (alias) THROW_ERROR("-alias is not yet supported in dictionary mode");

	return Tcl_DictObjPut(interp, dict, option->name, val);
}

//>>>
static int parse_args(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //<<<
{
	struct interp_cx*	l = (struct interp_cx*)cdata;
	Tcl_Obj**			av;
	Tcl_Size			ac;

	enum {A_cmd, A_ARGS, A_SPEC, A_args, A_DICT=A_args, A_objc};
	CHECK_RANGE_ARGS("args args_spec ?dict?");

	TEST_OK(Tcl_ListObjGetElements(interp, objv[A_ARGS], &ac, &av));
	struct parse_spec*	spec = NULL;
	TEST_OK(GetParseSpecFromObj(interp, objv[A_SPEC], &spec));

	if (objc > A_DICT) {
		Tcl_Obj*	res = NULL;		defer { replace_tclobj(&res, NULL); }
		replace_tclobj(&res, Tcl_NewDictObj());
		TEST_OK(bind_args(interp, l, spec, ac, av, put_dict, res));
		if (Tcl_ObjSetVar2(interp, objv[A_DICT], NULL, res, TCL_LEAVE_ERR_MSG) == NULL)
			return TCL_ERROR;
	} else {
		TEST_OK(bind_args(interp, l, spec, ac, av, put_var, NULL));
	}

	return TCL_OK;
}

//>>>
// Binding >>>

static void free_interp_cx(ClientData cdata, Tcl_Interp* interp) //<<<
{
	struct interp_cx*	l = cdata;
	(void)interp;

	if (l) {
		for (int i=0; i<L_end; i++)
			replace_tclobj(&l->obj[i], NULL);

		replace_tclobj(&l->enums, NULL);

		ckfree(l);
		l = NULL;
	}
}

//>>>

// Stubs API <<<
static int get_interp_cx(Tcl_Interp* interp, struct interp_cx** l) //<<<
{
	*l = Tcl_GetAssocData(interp, PACKAGE_NAME, NULL);
	if (!*l) THROW_ERROR(PACKAGE_NAME " is not loaded in this interp");
	return TCL_OK;
}

//>>>
struct callback_cx {
	ParseArgs_BindCallback*	cb;
	void*					cdata;
};
static int put_callback(void* cx, Tcl_Interp* interp, struct option_info* option, Tcl_Obj* val, bool alias) //<<<
{
	struct callback_cx*	c = cx;

	if (alias) THROW_ERROR("-alias is not supported by ParseArgs_Bind");

	return c->cb(c->cdata, interp, option->decl_idx, option->name, val);
}

//>>>
int ParseArgs_Bind(Tcl_Interp* interp, Tcl_Obj* argspec, Tcl_Size objc, Tcl_Obj*const objv[], ParseArgs_BindCallback* cb, void* cdata) //<<<
{
	struct interp_cx*	l = NULL;
	struct parse_spec*	spec = NULL;

	TEST_OK(get_interp_cx(interp, &l));
	TEST_OK(GetParseSpecFromObj(interp, argspec, &spec));

	return bind_args(interp, l, spec, objc, objv, put_callback, &(struct callback_cx){
		.cb		= cb,
		.cdata	= cdata,
	});
}

//>>>
int ParseArgs_BindDict(Tcl_Interp* interp, Tcl_Obj* argspec, Tcl_Size objc, Tcl_Obj*const objv[], Tcl_Obj** dict) //<<<
{
	struct interp_cx*	l = NULL;
	struct parse_spec*	spec = NULL;
	Tcl_Obj*			res = NULL;		defer { replace_tclobj(&res, NULL); }

	TEST_OK(get_interp_cx(interp, &l));
	TEST_OK(GetParseSpecFromObj(interp, argspec, &spec));

	replace_tclobj(&res, Tcl_NewDictObj());
	TEST_OK(bind_args(interp, l, spec, objc, objv, put_dict, res));
	replace_tclobj(dict, res);

	return TCL_OK;
}

//>>>
struct slots_cx {
	Tcl_Size				slotc;
	struct ParseArgs_Slot*	slots;
};
static int put_slot(void* cx, Tcl_Interp* interp, struct option_info* option, Tcl_Obj* val, bool alias) //<<<
{
	struct slots_cx*	c = cx;

	(void)alias;	// Rejected by ParseArgs_BindSlots before binding

	if (option->decl_idx < 0 || option->decl_idx >= c->slotc)
		THROW_PRINTF("No slot for \"%s\"", Tcl_GetString(option->name));

	struct ParseArgs_Slot*	slot = &c->slots[option->decl_idx];

	if (slot->type == PARSEARGS_SLOT_SKIP) return TCL_OK;

	// Hold a reference to the value so that the pointers we store into it stay valid
	replace_tclobj(&slot->hold, val);

	switch (slot->type) {
		case PARSEARGS_SLOT_SKIP:		break;
		case PARSEARGS_SLOT_OBJ:		*(Tcl_Obj**)slot->addr = val; break;
		case PARSEARGS_SLOT_INT:		TEST_OK(Tcl_GetIntFromObj(interp, val, (int*)slot->addr)); break;
		case PARSEARGS_SLOT_WIDEINT:	TEST_OK(Tcl_GetWideIntFromObj(interp, val, (Tcl_WideInt*)slot->addr)); break;
		case PARSEARGS_SLOT_DOUBLE:		TEST_OK(Tcl_GetDoubleFromObj(interp, val, (double*)slot->addr)); break;
		case PARSEARGS_SLOT_STRING:		*(const char**)slot->addr = Tcl_GetStringFromObj(val, slot->lenaddr); break;
		case PARSEARGS_SLOT_LIST:		TEST_OK(Tcl_ListObjGetElements(interp, val, slot->lenaddr, (Tcl_Obj***)slot->addr)); break;
		case PARSEARGS_SLOT_BOOL: //<<<
			{
				int	b;
				TEST_OK(Tcl_GetBooleanFromObj(interp, val, &b));
				*(bool*)slot->addr = b;
			}
			break;
			//>>>
		case PARSEARGS_SLOT_BYTES: //<<<
			{
				const unsigned char*	bytes = Tcl_GetBytesFromObj(interp, val, slot->lenaddr);
				if (!bytes) return TCL_ERROR;
				*(const unsigned char**)slot->addr = bytes;
			}
			break;
			//>>>
		case PARSEARGS_SLOT_INDEX: //<<<
			{
				if (!option->enum_choices)
					THROW_ERROR("INDEX output for \"", Tcl_GetString(option->param), "\" needs -enum");
				char**	enum_table;
				TEST_OK(GetEnumChoicesFromObj(interp, option->enum_choices, &enum_table));
				TEST_OK(Tcl_GetIndexFromObj(interp, val, (const char**)enum_table, Tcl_GetString(option->param), TCL_EXACT, (int*)slot->addr));
			}
			break;
			//>>>
		case PARSEARGS_SLOT_CUSTOM: //<<<
			if (!slot->convert)
				THROW_ERROR("No converter for \"", Tcl_GetString(option->param), "\"");
			TEST_OK(slot->convert(slot->convert_cdata, interp, val, slot->addr));
			break;
			//>>>
		default:
			THROW_PRINTF("Invalid slot type %d for \"%s\"", (int)slot->type, Tcl_GetString(option->param));
	}

	return TCL_OK;
}

//>>>
int ParseArgs_BindSlots(Tcl_Interp* interp, Tcl_Obj** cache, const char* specstr, Tcl_Size objc, Tcl_Obj*const objv[], Tcl_Size slotc, struct ParseArgs_Slot slots[]) //<<<
{
	struct interp_cx*	l = NULL;
	struct parse_spec*	spec = NULL;
	Tcl_Obj*			tmp = NULL;		defer { replace_tclobj(&tmp, NULL); }

	TEST_OK(get_interp_cx(interp, &l));

	if (!cache) cache = &tmp;	// No cache: build the spec just for this call
	if (!*cache) replace_tclobj(cache, Tcl_NewStringObj(specstr, -1));
	TEST_OK(GetParseSpecFromObj(interp, *cache, &spec));

	if (slotc != spec->decl_count)
		THROW_PRINTF("Got %" TCL_SIZE_MODIFIER "d slots for an argspec with %d entries", slotc, spec->decl_count);
	if (spec->multi_count)
		THROW_ERROR("-multi is not supported by ParseArgs_BindSlots");
	if (spec->has_alias)
		THROW_ERROR("-alias is not supported by ParseArgs_BindSlots");

	return bind_args(interp, l, spec, objc, objv, put_slot, &(struct slots_cx){
		.slotc	= slotc,
		.slots	= slots,
	});
}

//>>>
// Stubs API >>>

extern const Parse_argsStubs* const parse_argsConstStubsPtr;

DLLEXPORT int Parse_args_Init(Tcl_Interp* interp) //<<<
{
	/* Require 8.6 or later (9.0 also ok) */
#ifdef USE_TCL_STUBS
	if (!Tcl_InitStubs(interp, TCL_VERSION, 0)) return TCL_ERROR;
#else
	if (!Tcl_PkgRequire(interp, "Tcl", "8.6-", 0)) return TCL_ERROR;
#endif

	// Have to create the namespace before register_config() (or it will create it for us and ours will fail)
	Tcl_Namespace* ns = Tcl_CreateNamespace(interp, NS, NULL, NULL);
	if (!ns) return TCL_ERROR;

	// Register the pkgconfig (TIP 59) for jitc and other C consumers, when
	// loaded by pkgIndex.tcl (which runs Init in a frame where $dir is set)
	const char* dir = Tcl_GetVar(interp, "dir", 0);
	if (dir) register_config(interp, dir);

	init_instance_tracking(&g_tracked_instances);

	struct interp_cx* l = ckalloc(sizeof(struct interp_cx));
	*l = (struct interp_cx){};
	defer { if (l) free_interp_cx(l, interp); }

	for (int i=0; i<L_end; i++)
		replace_tclobj(&l->obj[i], Tcl_NewStringObj(static_str[i], -1));

	replace_tclobj(&l->enums, Tcl_NewDictObj());

	Tcl_SetAssocData(interp, PACKAGE_NAME, free_interp_cx, l);

	Tcl_CreateObjCommand(interp, NS "::parse_args", parse_args, l, NULL);
	TEST_OK(Tcl_Export(interp, ns, "parse_args", 0));

	l = NULL;

	TEST_OK(Tcl_PkgProvideEx(interp, PACKAGE_NAME, PACKAGE_VERSION, parse_argsConstStubsPtr));

	return TCL_OK;
}

//>>>
DLLEXPORT int Parse_args_SafeInit(Tcl_Interp* interp) //<<<
{
	// No unsafe features
	return Parse_args_Init(interp);
}

//>>>
DLLEXPORT int Parse_args_Unload(Tcl_Interp* interp, int flags) //<<<
{
	Tcl_DeleteAssocData(interp, PACKAGE_NAME);

	Tcl_Namespace*	ns = Tcl_FindNamespace(interp, NS, NULL, TCL_GLOBAL_ONLY);
	if (ns) Tcl_DeleteNamespace(ns);	// Also has the effect of reversing `register_config` (pkgconfig command is deleted)

	if (flags == TCL_UNLOAD_DETACH_FROM_PROCESS)
		finalize_instance_tracking(&g_tracked_instances);

	return TCL_OK;
}

//>>>
DLLEXPORT int Parse_args_SafeUnload(Tcl_Interp* interp, int flags) //<<<
{
	return Parse_args_Unload(interp, flags);
}

//>>>

// vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4 noexpandtab
