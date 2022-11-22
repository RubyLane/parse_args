/* TODO:
 *  - Better error messages
 */

#include "parse_argsInt.h"

static void free_internal_rep(Tcl_Obj* obj);
static void dup_internal_rep(Tcl_Obj* src, Tcl_Obj* dest);

// Micro Tcl_ObjType - enum_choices {{{
static void free_enum_choices_intrep(Tcl_Obj* obj);

Tcl_ObjType enum_choices_type = {
	"parse_spec_enum_choices",
	free_enum_choices_intrep,
	NULL,
	NULL,
	NULL
};

static void free_enum_choices_intrep(Tcl_Obj* obj)
{
	Tcl_ObjInternalRep*		ir = Tcl_FetchInternalRep(obj, &enum_choices_type);

	if (ir->twoPtrValue.ptr1) {
		ckfree(ir->twoPtrValue.ptr1);
		ir->twoPtrValue.ptr1 = NULL;
	}
}

static int GetEnumChoicesFromObj(Tcl_Interp* interp, Tcl_Obj* obj, char*** res)
{
	int					code = TCL_OK;
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(obj, &enum_choices_type);

	if (ir == NULL) {
		const char**		table;
		int					len;
		Tcl_ObjInternalRep	newir;

		TEST_OK_LABEL(finally, code, Tcl_SplitList(interp, Tcl_GetString(obj), &len, &table));

		newir.twoPtrValue.ptr1 = table;
		Tcl_StoreInternalRep(obj, &enum_choices_type, &newir);
		ir = Tcl_FetchInternalRep(obj, &enum_choices_type);
	}

	*res = (char**)ir->twoPtrValue.ptr1;

finally:
	return code;
}
// Micro Tcl_ObjType - enum_choices }}}

Tcl_ObjType parse_spec_type = {
	"parse_spec",
	free_internal_rep,
	dup_internal_rep,
	NULL,	// update_string_rep - we never invalidate our string rep
	NULL	// set_from_any - we don't register this objtype
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
	L_end
};

struct interp_cx {
	Tcl_Obj*	obj[L_end];
	Tcl_Obj*	enums;
};

struct parse_spec {
	char**					options;
	struct option_info*		option;
	int						option_count;
	Tcl_Obj*				usage_msg;
	struct option_info*		positional;
	int						positional_arg_count;
	struct option_info*		multi;
	int						multi_count;
};

struct option_info {
	int			arg_count;	// -1: store the option name in -name if present
	int			supplied;
	int			is_args;	// args style processing - consume all remaining arguments
	int			required;
	int			multi_idx;		// If this option is part of a multi select
	Tcl_Obj*	param;			// What this param is called in the spec
	Tcl_Obj*	name;			// The name that will store this params value
	Tcl_Obj*	default_val;	// NULL if no default
	Tcl_Obj*	validator;		// NULL if no validator
	Tcl_Obj*	enum_choices;	// NULL if not an enum, also stores multi_choices for a multi
	Tcl_Obj*	comment;		// NULL if no comment
	int			alias;			// boolean
};

static void free_option_info(struct option_info* option) //{{{
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

//}}}
static void free_parse_spec(struct parse_spec** specPtr) //{{{
{
	struct parse_spec*	spec = *specPtr;
	int					i;

	if (*specPtr != NULL) {
		//fprintf(stderr, "Freeing: %p\n", spec);
		if (spec->options) {
			for (i=0; i < spec->option_count; i++) {
				if (spec->options[i]) {
					free(spec->options[i]);
					spec->options[i] = NULL;
				}
			}
			ckfree(spec->options);
			spec->options = NULL;
		}

		if (spec->option != NULL) {
			for (i=0; i < spec->option_count; i++)
				free_option_info(&spec->option[i]);

			ckfree(spec->option);
			spec->option = NULL;
		}

		replace_tclobj(&spec->usage_msg, NULL);

		if (spec->positional) {
			for (i=0; i < spec->positional_arg_count; i++)
				free_option_info(&spec->positional[i]);

			ckfree(spec->positional);
			spec->positional = NULL;
		}

		if (spec->multi) {
			for (i=0; i < spec->multi_count; i++)
				free_option_info(&spec->multi[i]);

			ckfree(spec->multi);
			spec->multi = NULL;
		}

		ckfree(spec);
		*specPtr = NULL;
	}
}

//}}}
static void free_internal_rep(Tcl_Obj* obj) //{{{
{
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(obj, &parse_spec_type);

	free_parse_spec((struct parse_spec**)&ir->twoPtrValue.ptr1);
}

//}}}
static void dup_internal_rep(Tcl_Obj* src, Tcl_Obj* dest) // This shouldn't actually ever be called I think {{{
{
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(src, &parse_spec_type);
	Tcl_ObjInternalRep	newir;
	struct parse_spec*	spec = (struct parse_spec*)ckalloc(sizeof(struct parse_spec));
	struct parse_spec*	old =  (struct parse_spec*)ir->twoPtrValue.ptr1;
	int		i;

	//fprintf(stderr, "in dup_internal_rep\n");

	memset(spec, 0, sizeof(struct parse_spec));
	//fprintf(stderr, "Allocated spec: %p: \"%s\"\n", spec, Tcl_GetString(src));

	spec->option_count = old->option_count;
	spec->positional_arg_count = old->positional_arg_count;
	spec->multi_count = old->positional_arg_count;

	spec->options = ckalloc(sizeof(char*) * (spec->option_count+1));
	spec->option = ckalloc(sizeof(struct option_info) * spec->option_count);
	spec->positional = ckalloc(sizeof(struct option_info) * spec->positional_arg_count);
	spec->multi = ckalloc(sizeof(struct option_info) * spec->multi_count);
	memset(spec->options, 0, sizeof(char*) * (spec->option_count+1));
	memset(spec->option, 0, sizeof(struct option_info) * spec->option_count);
	memset(spec->positional, 0, sizeof(struct option_info) * spec->positional_arg_count);
	memset(spec->multi, 0, sizeof(struct option_info) * spec->multi_count);

	replace_tclobj(&spec->usage_msg, old->usage_msg);

#define INCREF_OPTION(opt) \
	if ((opt).name != NULL)         Tcl_IncrRefCount((opt).name); \
	if ((opt).default_val != NULL)  Tcl_IncrRefCount((opt).default_val); \
	if ((opt).validator != NULL)    Tcl_IncrRefCount((opt).validator); \
	if ((opt).enum_choices != NULL) Tcl_IncrRefCount((opt).enum_choices);

	for (i=0; i < spec->option_count; i++) {
		// strdup is safe because the value came from Tcl_GetString, which
		// always returns properly \0 terminated strings
		spec->options[i] = strdup(old->options[i]);
		spec->option[i] = old->option[i];
		INCREF_OPTION(spec->option[i]);
	}

	for (i=0; i < spec->positional_arg_count; i++) {
		spec->positional[i] = old->positional[i];
		INCREF_OPTION(spec->positional[i]);
	}

	for (i=0; i < spec->multi_count; i++) {
		spec->multi[i] = old->multi[i];
		INCREF_OPTION(spec->multi[i]);
	}

	newir.twoPtrValue.ptr1 = spec;
	Tcl_StoreInternalRep(dest, &parse_spec_type, &newir);
}

//}}}
static int compile_parse_spec(Tcl_Interp* interp, Tcl_Obj* obj, struct parse_spec** res) //{{{
{
	struct interp_cx*	l = (struct interp_cx*)Tcl_GetAssocData(interp, "parse_args", NULL);
	Tcl_Obj**			ov;
	int					oc, i, j, settingc, str_len, code=TCL_OK, index, o_i=0, p_i=0;
	const char*			str;
	Tcl_Obj*			name;
	Tcl_Obj**			settingv;
	struct parse_spec*	spec = NULL;
	const char* settings[] = {
		"-default",
		"-required",
		"-validate",
		"-name",
		"-boolean",
		"-args",
		"-enum",
		"-#",
		"-multi",
		"-alias",
		(char*)NULL
	};
	enum {
		SETTING_DEFAULT,
		SETTING_REQUIRED,
		SETTING_VALIDATE,
		SETTING_NAME,
		SETTING_BOOLEAN,
		SETTING_ARGS,
		SETTING_ENUM,
		SETTING_COMMENT,
		SETTING_MULTI,
		SETTING_ALIAS
	};
	Tcl_DictSearch	search;
	Tcl_Obj*		multis = NULL;

	replace_tclobj(&multis, Tcl_NewDictObj());

	TEST_OK_LABEL(err, code, Tcl_ListObjGetElements(interp, obj, &oc, &ov));

	if (oc % 2 != 0)
		THROW_ERROR_LABEL(err, code, "argspec must be a dictionary");

	spec = ckalloc(sizeof(struct parse_spec));
	memset(spec, 0, sizeof(struct parse_spec));
	//fprintf(stderr, "Allocated spec: %p: \"%s\"\n", spec, Tcl_GetString(obj));

	for (i=0; i<oc; i+=2) {
		name = ov[i];
		str = Tcl_GetStringFromObj(name, &str_len);
		if (str_len > 0 && str[0] == '-') {
			spec->option_count++;
		} else {
			spec->positional_arg_count++;
		}
	}

	spec->options = ckalloc(sizeof(char*) * (spec->option_count+1));
	spec->option = ckalloc(sizeof(struct option_info) * spec->option_count);
	spec->positional = ckalloc(sizeof(struct option_info) * spec->positional_arg_count);
	memset(spec->options, 0, sizeof(char*) * (spec->option_count+1));
	memset(spec->option, 0, sizeof(struct option_info) * spec->option_count);
	memset(spec->positional, 0, sizeof(struct option_info) * spec->positional_arg_count);

	for (i=0; i<oc; i+=2) {
		struct option_info*	option;

		name = ov[i];
		str = Tcl_GetStringFromObj(name, &str_len);
		//fprintf(stderr, "spec element %d (name): \"%s\"\n\t%d (settings): \"%s\"\n", i, Tcl_GetString(name), i+1, Tcl_GetString(ov[i+1]));

		if (str_len > 0 && str[0] == '-') {
			// strdup is safe because Tcl_GetString always returns a properly
			// \0 terminated string
			spec->options[o_i] = strdup(str);
			//fprintf(stderr, "Storing option %d/%d: %p \"%s\"\n", o_i, spec->option_count, spec->options[o_i], str);
			option = &spec->option[o_i++];
		} else {
			//fprintf(stderr, "Storing positional param %d/%d: \"%s\"\n", p_i, spec->positional_arg_count, str);
			option = &spec->positional[p_i++];
		}

		Tcl_IncrRefCount(option->param = name);
		option->arg_count = 1;
		option->multi_idx = -1;

		TEST_OK_LABEL(err, code, Tcl_ListObjGetElements(interp, ov[i+1], &settingc, &settingv));
		j = 0;
		//fprintf(stderr, "Checking %d setting elements: \"%s\"\n", settingc, Tcl_GetString(ov[i+1]));
		while (j<settingc) {
			//fprintf(stderr, "j: %d, checking setting \"%s\"\n", j, Tcl_GetString(settingv[j]));
			TEST_OK_LABEL(err, code, Tcl_GetIndexFromObj(interp, settingv[j], settings, "setting", TCL_EXACT, &index));
			j++;

			switch (index) {
				case SETTING_DEFAULT:
				case SETTING_VALIDATE:
				case SETTING_NAME:
				case SETTING_ARGS:
				case SETTING_ENUM:
				case SETTING_COMMENT:
					if (j >= settingc)
						THROW_ERROR_LABEL(err, code, Tcl_GetString(settingv[j-1]), " needs a value");
					break;
			}

			switch (index) {
				case SETTING_DEFAULT:
					Tcl_IncrRefCount(option->default_val = settingv[j++]);
					break;
				case SETTING_REQUIRED:
					option->required = 1;
					break;
				case SETTING_VALIDATE:
					{
						Tcl_Obj*	validatorobj = settingv[j++];
						Tcl_Obj**	ov;
						int			oc;

						TEST_OK_LABEL(err, code, Tcl_ListObjGetElements(interp, validatorobj, &oc, &ov));
						if (oc > 0)
							Tcl_IncrRefCount(option->validator = validatorobj);
					}
					break;
				case SETTING_NAME:
					Tcl_IncrRefCount(option->name = settingv[j++]);
					break;
				case SETTING_BOOLEAN:
					option->arg_count = 0;
					break;
				case SETTING_ARGS:
					TEST_OK_LABEL(err, code, Tcl_GetIntFromObj(interp, settingv[j++], &option->arg_count));
					if (option->arg_count < 0)
						THROW_ERROR_LABEL(err, code, "-args cannot be negative");
					break;
				case SETTING_ENUM:
					{
						/* enums are validated using Tcl_GetIndexFromObj, which
						 * shimmers its input obj to record the table and index
						 * info, so make an effort to unify enums across parse_specs
						 */
						Tcl_Obj*	enum_choices = settingv[j++];
						Tcl_Obj*	shared_enum_choices = NULL;
						int			size;

						if (l->enums == NULL) {
							code = TCL_ERROR;
							goto err;
						}

						TEST_OK_LABEL(err, code, Tcl_DictObjGet(interp, l->enums, enum_choices, &shared_enum_choices));

						if (shared_enum_choices == NULL) {
							if (Tcl_IsShared(l->enums)) {
								Tcl_Obj*	tmp = NULL;
								replace_tclobj(&tmp, Tcl_DuplicateObj(l->enums));
								replace_tclobj(&l->enums, tmp);
								replace_tclobj(&tmp, NULL);
							}

							TEST_OK_LABEL(err, code, Tcl_DictObjPut(interp, l->enums, enum_choices, shared_enum_choices = enum_choices));
						}

						TEST_OK_LABEL(err, code, Tcl_DictObjSize(interp, l->enums, &size));

						if (size > 1000) {
							// Paranoia - prevent the speculative enum cache from growing too large
							replace_tclobj(&l->enums, Tcl_NewDictObj());
						}

						replace_tclobj(&option->enum_choices, shared_enum_choices);
					}
					break;
				case SETTING_COMMENT:
					replace_tclobj(&option->comment, settingv[j++]);
					break;
				case SETTING_MULTI:
					if (str[0] != '-')
						THROW_ERROR_LABEL(err, code, "Cannot use -multi on positional argument \"", Tcl_GetString(option->param), "\"");

					option->arg_count = -1;
					break;
				case SETTING_ALIAS:
					option->alias = 1;
					break;
				default:
					{
						char	buf[3*sizeof(index)+2];
						sprintf(buf, "%d", index);
						THROW_ERROR_LABEL(err, code, "Invalid setting: ", buf);
					}
			}
		}

		if (option->name == NULL) {
			if (str[0] == '-') {
				replace_tclobj(&option->name, Tcl_NewStringObj(str+1, str_len-1));
			} else {
				replace_tclobj(&option->name, name);
			}
		}

		if (option->arg_count == -1) {
			int			multi_idx;
			Tcl_Obj*	multi_choices;
			const char*	multi_val;
			int			multi_val_len;
			Tcl_Obj*	multi_config_loan = NULL;

			//fprintf(stderr, "multis: %s\n", Tcl_GetString(multis));
			TEST_OK_LABEL(err, code, Tcl_DictObjGet(interp, multis, option->name, &multi_config_loan));
			if (multi_config_loan == NULL) {
				multi_idx = spec->multi_count++;
				multi_config_loan = Tcl_NewDictObj();
				TEST_OK_LABEL(err, code, Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_IDX], Tcl_NewIntObj(multi_idx)));
				TEST_OK_LABEL(err, code, Tcl_DictObjPut(interp, multis, option->name, multi_config_loan));
				multi_choices = Tcl_NewListObj(0, NULL);
			} else {
				Tcl_Obj*	idx_obj = NULL;
				TEST_OK_LABEL(err, code, Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_IDX], &idx_obj));
				TEST_OK_LABEL(err, code, Tcl_GetIntFromObj(interp, idx_obj, &multi_idx));
				TEST_OK_LABEL(err, code, Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_CHOICES], &multi_choices));
			}
			TEST_OK_LABEL(err, code, Tcl_ListObjAppendElement(interp, multi_choices, option->param));
			TEST_OK_LABEL(err, code, Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_CHOICES], multi_choices));

			option->multi_idx = multi_idx;

			// TODO: throw errors if -boolean or -args are mixed with -multi
			if (option->enum_choices != NULL)
				THROW_ERROR_LABEL(err, code, "Cannot use -multi and -enum together");

			if (option->required)
				TEST_OK_LABEL(err, code, Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_REQUIRED], l->obj[L_TRUE]));
			if (option->default_val != NULL)
				TEST_OK_LABEL(err, code, Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_DEFAULT],  option->default_val));
			if (option->validator != NULL)
				TEST_OK_LABEL(err, code, Tcl_DictObjPut(interp, multi_config_loan, l->obj[L_VALIDATE], option->validator));

			// Repurpose option->default_val to store the value this option will set in the multi output
			multi_val = Tcl_GetStringFromObj(option->param, &multi_val_len);
			replace_tclobj(&option->default_val, Tcl_NewStringObj(multi_val+1, multi_val_len-1));
		}
	}

	// This is only known after parsing the options
	if (spec->multi_count > 0) {
		Tcl_Obj*	name;
		Tcl_Obj*	val;
		Tcl_Obj*	idx_obj;
		Tcl_Obj*	multi_config_loan = NULL;
		int			done, idx;

		spec->multi = ckalloc(sizeof(struct option_info) * spec->multi_count);
		memset(spec->multi, 0, sizeof(struct option_info) * spec->multi_count);

		TEST_OK_LABEL(err, code, Tcl_DictObjFirst(interp, multis, &search, &name, &multi_config_loan, &done));
		for (; !done; Tcl_DictObjNext(&search, &name, &multi_config_loan, &done)) {
			struct option_info* multi;

			//fprintf(stderr, "multi config for %s: %s\n", Tcl_GetString(name), Tcl_GetString(multi_config_loan));
			TEST_OK_LABEL(err_search, code, Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_IDX], &idx_obj));
			TEST_OK_LABEL(err_search, code, Tcl_GetIntFromObj(interp, idx_obj, &idx));
			if (idx < 0 || idx >= spec->multi_count) {
				THROW_ERROR_LABEL(err_search, code, "Got out of bounds multi_count ", Tcl_GetString(idx_obj), " for option \"", Tcl_GetString(name));
			}
			multi = &spec->multi[idx];

			//fprintf(stderr, "Setting multi %d name: \"%s\"\n", idx, Tcl_GetString(name));
			replace_tclobj(&multi->name, name);
			multi->arg_count = -1;

			// set -required
			TEST_OK_LABEL(err_search, code, Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_REQUIRED], &val));
			if (val != NULL) multi->required = 1;

			// set -default
			TEST_OK_LABEL(err_search, code, Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_DEFAULT],  &val));
			replace_tclobj(&multi->default_val, val);

			// set -validate
			TEST_OK_LABEL(err_search, code, Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_VALIDATE], &val));
			replace_tclobj(&multi->validator, val);

			// set multi choices (for error message if required and not set)
			TEST_OK_LABEL(err_search, code, Tcl_DictObjGet(interp, multi_config_loan, l->obj[L_CHOICES],  &val));
			replace_tclobj(&multi->enum_choices, val);
		}
		Tcl_DictObjDone(&search);
	}

	if (spec->positional_arg_count > 0) {
		struct option_info*	last = &spec->positional[spec->positional_arg_count - 1];
		// Create special "args" behaviour for last positional param named "args"
		// strcmp is safe because Tcl_GetString always gives us a properly
		// \0 terminated string
		if (strcmp("args", Tcl_GetString(last->param)) == 0) {
			last->is_args = 1;
			if (last->default_val == NULL)
				replace_tclobj(&last->default_val, Tcl_NewObj());
		}
	}

	// TODO: better usage_msg
	replace_tclobj(&spec->usage_msg, Tcl_ObjPrintf("Invalid args, should be ?-option ...? %s", "?arg ...?"));

	*res = spec;

	goto finally;

err_search:
	Tcl_DictObjDone(&search);
err:
	//fprintf(stderr, "compile_parse_spec failed, freeing spec\n");
	free_parse_spec(&spec);

finally:
	replace_tclobj(&multis, NULL);

	return code;
}

//}}}

static int GetParseSpecFromObj(Tcl_Interp* interp, Tcl_Obj* spec, struct parse_spec** res) //{{{
{
	int					code = TCL_OK;
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(spec, &parse_spec_type);

	if (ir == NULL) {
		Tcl_ObjInternalRep	newir;
		struct parse_spec*	compiled_spec = NULL;

		TEST_OK_LABEL(finally, code, compile_parse_spec(interp, spec, &compiled_spec));

		newir.twoPtrValue.ptr1 = compiled_spec;
		Tcl_StoreInternalRep(spec, &parse_spec_type, &newir);
		ir = Tcl_FetchInternalRep(spec, &parse_spec_type);
	}

	if (res)
		*res = (struct parse_spec*)ir->twoPtrValue.ptr1;

finally:
	return code;
}

//}}}

static int validate(Tcl_Interp* interp, struct option_info* option, Tcl_Obj* val) //{{{
{
	Tcl_Obj*	verdict = NULL;

	if (option->enum_choices != NULL) {
		int		dummy;
		char**	enum_table;

		TEST_OK(GetEnumChoicesFromObj(interp, option->enum_choices, &enum_table));
		TEST_OK(Tcl_GetIndexFromObj(interp, val, enum_table, Tcl_GetString(option->param), TCL_EXACT, &dummy));
	}

	if (option->validator != NULL) {
		int			res, passed;
		Tcl_Obj**	ov;
		int			oc;

		TEST_OK(Tcl_ListObjGetElements(interp, option->validator, &oc, &ov));
		{
#ifdef _MSC_VER
			/* 
			* VC++ does not support C99 varsize arrays. 
			* Use _malloca to allocate stack space 
			* (recommended over _alloca) instead. Note this will
			* die on stack overflow just like varsize arrays.
			* Also note the corresponding _freea to free memory.
                        */
			Tcl_Obj** cmd = _malloca((oc+1)*sizeof(*cmd));
#else
			Tcl_Obj*	cmd[oc+1];
#endif
			int			i;

			for (i=0; i<oc; i++) Tcl_IncrRefCount(cmd[i] = ov[i]);
			Tcl_IncrRefCount(cmd[oc] = val);
			res = Tcl_EvalObjv(interp, oc+1, cmd, 0);
			Tcl_IncrRefCount(verdict = Tcl_GetObjResult(interp));
			for (i=0; i<oc+1; i++) Tcl_DecrRefCount(cmd[i]);
#ifdef _MSC_VER
			_freea(cmd);
#endif
		}

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

		if (passed) {
			res = TCL_OK;
		} else {
			Tcl_SetObjResult(interp,
				Tcl_ObjPrintf("Validation failed for \"%s\": %s",
						Tcl_GetString(option->param),
						Tcl_GetString(verdict) ));
			Tcl_SetErrorCode(interp, "PARSE_ARGS", "VALIDATION", Tcl_GetString(option->param), NULL);
			res = TCL_ERROR;
		}

		Tcl_DecrRefCount(verdict); verdict = NULL;

		return res;
	}

	return TCL_OK;
}

//}}}
static int parse_args(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	struct interp_cx*	l = (struct interp_cx*)cdata;
	Tcl_Obj**	av;
	int			ac, i, check_options=1, positional_arg=0;
	struct parse_spec*	spec = NULL;
	Tcl_Obj*	res = NULL;
	Tcl_Obj*	val = NULL;
	const int	dictmode = objc >= 4;

	if (objc < 3 || objc > 4) {
		Tcl_WrongNumArgs(interp, 1, objv, "args args_spec ?dict?");
		return TCL_ERROR;
	}

	TEST_OK(Tcl_ListObjGetElements(interp, objv[1], &ac, &av));
	//fprintf(stderr, "Getting parse_spec from (%s)\n", Tcl_GetString(objv[2]));
	TEST_OK(GetParseSpecFromObj(interp, objv[2], &spec));

	for (i=0; i<spec->option_count; i++)
		spec->option[i].supplied = 0;
	for (i=0; i<spec->multi_count; i++)
		spec->multi[i].supplied = 0;

	if (dictmode)
		res = Tcl_NewDictObj();

#define OUTPUT(name, val) \
	if (dictmode) { \
		TEST_OK(Tcl_DictObjPut(interp, res, (name), (val))); \
	} else { \
		if (Tcl_ObjSetVar2(interp, (name), NULL, (val), TCL_LEAVE_ERR_MSG) == NULL) return TCL_ERROR; \
	}

#define VALIDATE(option, val) \
	if ((option)->validator != NULL || (option)->enum_choices != NULL) { \
		TEST_OK(validate(interp, (option), (val))); \
	}

	for (i=0; i<ac; i++) {
		if (check_options) {
			const char*	str;
			int str_len;

			str = Tcl_GetStringFromObj(av[i], &str_len);
			//fprintf(stderr, "Checking av[%d]: \"%s\"\n", i, str);
			if (str_len > 0 && str[0] == '-') {
				struct option_info*	option;
				struct option_info*	src_option;
				int	index;

				// strcmp is safe because Tcl_GetString always returns properly
				// \0 terminated strings
				if (str_len == 2 && strcmp(str, "--") == 0) {
					check_options = 0;
					continue;
				}

				TEST_OK(Tcl_GetIndexFromObj(interp, av[i], spec->options, "option", TCL_EXACT, &index));
				option = src_option = &spec->option[index];
				if (option->multi_idx >= 0)
					option = &spec->multi[src_option->multi_idx];

				option->supplied = 1;
				//fprintf(stderr, "Option \"%s\" arg_count: %d\n",
				//		Tcl_GetString(option->name), option->arg_count);
				if (option->arg_count > 0 && ac - i - 1 < option->arg_count) {
					// This option requires args and not enough remain
					Tcl_WrongNumArgs(interp, 1, objv, Tcl_GetString(spec->usage_msg));
					return TCL_ERROR;
				}

				switch (option->arg_count) {
					case -1:
						OUTPUT(option->name, src_option->default_val);
						break;

					case 0:
						OUTPUT(option->name, l->obj[L_TRUE]);
						break;

					case 1:
						val = av[++i];
						VALIDATE(option, val);
						if (option->alias == 0) {
							OUTPUT(option->name, val);
						} else {
							if (dictmode) {
								// TODO: fetch the value from the variable called option->name in the parent callframe
								THROW_ERROR("-alias is not yet supported in dictionary mode");
							} else {
								TEST_OK(Tcl_UpVar(interp, "1", Tcl_GetString(val), Tcl_GetString(option->name), 0));
							}
						}
						break;

					default:
						val = Tcl_NewListObj(option->arg_count, av+i+1);
						VALIDATE(option, val);
						OUTPUT(option->name, val);
						i += option->arg_count;
						break;
				}

				continue;
			} else {
				check_options = 0;
			}
		}

		if (positional_arg >= spec->positional_arg_count) {
			// Too many positional args
			Tcl_WrongNumArgs(interp, 1, objv, Tcl_GetString(spec->usage_msg));
			return TCL_ERROR;
		}

		if (spec->positional[positional_arg].is_args) {
			val = Tcl_NewListObj(ac-i, av+i);
			VALIDATE(&spec->positional[positional_arg], val);
			OUTPUT(spec->positional[positional_arg].name, val);
			i = ac;
		} else {
			VALIDATE(&spec->positional[positional_arg], av[i]);
			if (spec->positional[positional_arg].alias == 0) {
				OUTPUT(spec->positional[positional_arg].name, av[i]);
			} else {
				if (dictmode) {
					// TODO: fetch the value from the variable called option->name in the parent callframe
					THROW_ERROR("-alias is not yet supported in dictionary mode");
				} else {
					TEST_OK(Tcl_UpVar(interp, "1", Tcl_GetString(av[i]), Tcl_GetString(spec->positional[positional_arg].name), 0));
				}
			}
		}

		positional_arg++;
	}

	// Check -required and set -default for options that weren't specified
	for (i=0; i < spec->option_count; i++) {
		struct option_info* option = &spec->option[i];

		if (option->supplied) continue;
		if (option->multi_idx >= 0) continue;

		if (option->default_val != NULL) {
			OUTPUT(option->name, option->default_val);
		} else if (option->arg_count == 0) {
			OUTPUT(option->name, l->obj[L_FALSE]);
		} else {
			if (option->required) {
				Tcl_SetErrorCode(interp, "PARSE_ARGS", "REQUIRED", Tcl_GetString(option->param), NULL);
				THROW_ERROR("option ", Tcl_GetString(option->param), " is required");
			}
		}
	}

	// Check -required and set -default for multi options that weren't specified
	//fprintf(stderr, "-multi post check, count: %d\n", spec->multi_count);
	for (i=0; i < spec->multi_count; i++) {
		struct option_info* option = &spec->multi[i];

		//fprintf(stderr, "\t%d, %s supplied? %d, default_val: %p\n", i, Tcl_GetString(option->name), option->supplied, option->default_val);
		if (option->supplied) continue;

		if (option->default_val != NULL) {
			OUTPUT(option->name, option->default_val);
		} else if (option->required) {
			Tcl_SetErrorCode(interp, "PARSE_ARGS", "REQUIRED_ONE_OF", Tcl_GetString(option->enum_choices), NULL);
			THROW_ERROR("one of ", Tcl_GetString(option->enum_choices), " are required");
		}
	}

	// Set default values for positional params that need them
	for (; positional_arg < spec->positional_arg_count; positional_arg++) {
		struct option_info* option = &spec->positional[positional_arg];

		if (option->default_val != NULL) {
			// Don't need to validate here - the default was baked in at
			// definition time and it might be useful to allow it to be outside
			// the valid domain
			OUTPUT(option->name, option->default_val);
		} else {
			// Missing some positional args
			if (option->required) {
				Tcl_SetErrorCode(interp, "PARSE_ARGS", "REQUIRED", Tcl_GetString(option->param), NULL);
				THROW_ERROR("argument ", Tcl_GetString(option->name), " is required");
			}
		}
	}

	if (dictmode)
		if (Tcl_ObjSetVar2(interp, objv[3], NULL, res, TCL_LEAVE_ERR_MSG) == NULL)
			return TCL_ERROR;

	return TCL_OK;
}

//}}}

static void free_interp_cx(ClientData cdata, Tcl_Interp* interp) //<<<
{
	struct interp_cx*	l = (struct interp_cx*)cdata;
	int					i;

	if (l) {
		for (i=0; i<L_end; i++)
			replace_tclobj(&l->obj[i], NULL);

		replace_tclobj(&l->enums, NULL);

		ckfree(l);
		l = NULL;
	}
}

//>>>

#ifdef WIN32
extern DLLEXPORT
#endif
int Parse_args_Init(Tcl_Interp* interp) //{{{
{
	int					code = TCL_OK;
	struct interp_cx*	l = NULL;
	Tcl_Namespace*		ns = NULL;
	int					i;

	if (Tcl_InitStubs(interp, "8.5", 0) == NULL) {
		code = TCL_ERROR;
		goto finally;
	}

	if (Tcl_PkgRequire(interp, "Tcl", "8.5", 0) == NULL) {
		code = TCL_ERROR;
		goto finally;
	}

	l = ckalloc(sizeof(struct interp_cx));
	if (l == NULL)
		THROW_ERROR_LABEL(finally, code, "Couldn't allocate per-interp data");
	memset(l, 0, sizeof *l);

	for (i=0; i<L_end; i++)
		replace_tclobj(&l->obj[i], Tcl_NewStringObj(static_str[i], -1));

	replace_tclobj(&l->enums, Tcl_NewDictObj());

	Tcl_SetAssocData(interp, "parse_args", free_interp_cx, l);


	ns = Tcl_CreateNamespace(interp, "::parse_args", NULL, NULL);
	TEST_OK_LABEL(finally, code, Tcl_Export(interp, ns, "*", 0));

	Tcl_CreateObjCommand(interp, "::parse_args::parse_args", parse_args, l, NULL);

	TEST_OK_LABEL(finally, code, Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION));

finally:
	if (code != TCL_OK) {
		if (l) {
			free_interp_cx(l, interp);
			l = NULL;
		}
	}
	return code;
}

//}}}
int Parse_args_SafeInit(Tcl_Interp* interp) //{{{
{
	// No unsafe features
	return Parse_args_Init(interp);
}

//}}}

// vim: foldmethod=marker foldmarker={{{,}}} ts=4 shiftwidth=4
