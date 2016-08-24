/* TODO:
 *  - Better error messages
 *  - Implement validators
 */

#include "main.h"

static void free_internal_rep(Tcl_Obj* obj);
static void dup_internal_rep(Tcl_Obj* src, Tcl_Obj* dest);
static int set_from_any(Tcl_Interp* interp, Tcl_Obj* obj);

// Micro Tcl_ObjType - enum_choices {{{
static void free_enum_choices_intrep(Tcl_Obj* obj);
static int set_enum_choices_from_any(Tcl_Interp* intrep, Tcl_Obj* obj);

Tcl_ObjType enum_choices_type = {
	"parse_spec_enum_choices",
	free_enum_choices_intrep,
	(Tcl_DupInternalRepProc*)NULL,
	(Tcl_UpdateStringProc*)NULL,
	set_enum_choices_from_any
};

static void free_enum_choices_intrep(Tcl_Obj* obj)
{
	if (obj->internalRep.otherValuePtr) {
		ckfree(obj->internalRep.otherValuePtr);
		obj->internalRep.otherValuePtr = NULL;
	}
}

static int set_enum_choices_from_any(Tcl_Interp* interp, Tcl_Obj* obj)
{
	int				dummy;
	const char**	table;

	TEST_OK(Tcl_SplitList(interp, Tcl_GetString(obj), &dummy, &table));

	if (obj->typePtr != NULL && obj->typePtr->freeIntRepProc != NULL)
		obj->typePtr->freeIntRepProc(obj);

	obj->typePtr = &enum_choices_type;
	obj->internalRep.otherValuePtr = table;

	return TCL_OK;
}

static int GetEnumChoicesFromObj(Tcl_Interp* interp, Tcl_Obj* obj, char*** res)
{
	if (obj->typePtr != &enum_choices_type)
		TEST_OK(set_enum_choices_from_any(interp, obj));

	*res = (char**)obj->internalRep.otherValuePtr;

	return TCL_OK;
}
// Micro Tcl_ObjType - enum_choices }}}

Tcl_ObjType parse_spec_type = {
	"parse_spec",
	free_internal_rep,
	dup_internal_rep,
	(Tcl_UpdateStringProc*)NULL, // update_string_rep - we never invalidate our string rep
	set_from_any
};

struct interp_data {
	Tcl_Obj*	true_obj;
	Tcl_Obj*	false_obj;
};

struct parse_spec {
	char**					options;
	struct option_info*		option;
	int						option_count;
	Tcl_Obj*				usage_msg;
	struct option_info*		positional;
	int						positional_arg_count;
};

struct option_info {
	int			arg_count;
	int			supplied;
	int			is_args;	// args style processing - consume all remaining arguments
	int			required;
	Tcl_Obj*	param;			// What this param is called in the spec
	Tcl_Obj*	name;			// The name that will store this params value
	Tcl_Obj*	default_val;	// NULL if no default
	Tcl_Obj*	validator;		// NULL if no validator
	Tcl_Obj*	enum_choices;	// NULL if not an enum, also stores multi_choices for a multi
	Tcl_Obj*	comment;		// NULL if no comment
};

static void free_option_info(struct option_info* option) //{{{
{
	if (option != NULL) {
		UNREF(option->param);
		UNREF(option->name);
		UNREF(option->default_val);
		UNREF(option->validator);
		UNREF(option->enum_choices);
		UNREF(option->comment);
	}
}

//}}}
static void free_parse_spec(struct parse_spec** specPtr) //{{{
{
	struct parse_spec*	spec = *specPtr;
	int		i;

	if (*specPtr != NULL) {
		//fprintf(stderr, "Freeing: %p\n", spec);
		if (spec->option_count > 0 && spec->options != NULL) {
			for (i=0; i < spec->option_count; i++) {
				if (spec->options[i] != NULL) {
					free(spec->options[i]);
					spec->options[i] = NULL;
				}
			}
			ckfree(spec->options);
			spec->options = NULL;
		}

		if (spec->option != NULL) {
			for (i=0; i < spec->option_count; i++) {
				free_option_info(&spec->option[i]);
			}
			ckfree(spec->option);
			spec->option = NULL;
		}

		UNREF(spec->usage_msg);

		if (spec->positional_arg_count > 0 && spec->positional != NULL) {
			for (i=0; i < spec->positional_arg_count; i++) {
				free_option_info(&spec->positional[i]);
			}
			ckfree(spec->positional);
			spec->positional = NULL;
		}

		ckfree(spec);
		*specPtr = NULL;
	}
}

//}}}
static void free_internal_rep(Tcl_Obj* obj) //{{{
{
	free_parse_spec((struct parse_spec**)&obj->internalRep.otherValuePtr);
}

//}}}
static void dup_internal_rep(Tcl_Obj* src, Tcl_Obj* dest) // This shouldn't actually ever be called I think {{{
{
	struct parse_spec*	spec = (struct parse_spec*)ckalloc(sizeof(struct parse_spec));
	struct parse_spec*	old = (struct parse_spec*)src->internalRep.otherValuePtr;
	int		i;

	//fprintf(stderr, "in dup_internal_rep\n");

	memset(spec, 0, sizeof(struct parse_spec));
	//fprintf(stderr, "Allocated spec: %p: \"%s\"\n", spec, Tcl_GetString(src));

	spec->option_count = old->option_count;
	spec->positional_arg_count = old->positional_arg_count;

	spec->options = ckalloc(sizeof(char*) * (spec->option_count+1));
	spec->option = ckalloc(sizeof(struct option_info) * spec->option_count);
	spec->positional = ckalloc(sizeof(struct option_info) * spec->positional_arg_count);
	memset(spec->options, 0, sizeof(char*) * (spec->option_count+1));
	memset(spec->option, 0, sizeof(struct option_info) * spec->option_count);
	memset(spec->positional, 0, sizeof(struct option_info) * spec->positional_arg_count);

	spec->usage_msg = old->usage_msg;
	if (spec->usage_msg != NULL)
		Tcl_IncrRefCount(spec->usage_msg);

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

	dest->typePtr = src->typePtr;
	dest->internalRep.otherValuePtr = spec;
}

//}}}
static int set_from_any(Tcl_Interp* interp, Tcl_Obj* obj) //{{{
{
	Tcl_Obj**	ov;
	int			oc, i, j, settingc, str_len, retcode=TCL_OK, index, o_i=0, p_i=0;
	const char*	str;
	Tcl_Obj*	name;
	Tcl_Obj**	settingv;
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
		SETTING_COMMENT
	};

	TEST_OK(Tcl_ListObjGetElements(interp, obj, &oc, &ov));

	if (oc % 2 != 0)
		THROW_ERROR("argspec must be a dictionary");

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

#define SET_TCLOBJ(dest, src) \
	if ((dest) != NULL) Tcl_DecrRefCount((dest)); \
	(dest) = (src); \
	if ((dest) != NULL) Tcl_IncrRefCount((dest));

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

		TEST_OK_LABEL(err, retcode, Tcl_ListObjGetElements(interp, ov[i+1], &settingc, &settingv));
		j = 0;
		//fprintf(stderr, "Checking %d setting elements: \"%s\"\n", settingc, Tcl_GetString(ov[i+1]));
		while (j<settingc) {
			//fprintf(stderr, "j: %d, checking setting \"%s\"\n", j, Tcl_GetString(settingv[j]));
			TEST_OK_LABEL(err, retcode, Tcl_GetIndexFromObj(interp, settingv[j], settings, "setting", TCL_EXACT, &index));
			j++;

			switch (index) {
				case SETTING_DEFAULT:
				case SETTING_VALIDATE:
				case SETTING_NAME:
				case SETTING_ARGS:
				case SETTING_ENUM:
				case SETTING_COMMENT:
					if (j >= settingc)
						THROW_ERROR_LABEL(err, retcode, Tcl_GetString(settingv[j-1]), " needs a value");
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

						TEST_OK(Tcl_ListObjGetElements(interp, validatorobj, &oc, &ov));
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
					TEST_OK_LABEL(err, retcode, Tcl_GetIntFromObj(interp, settingv[j++], &option->arg_count));
					if (option->arg_count < 0)
						THROW_ERROR("-args cannot be negative");
					break;
				case SETTING_ENUM:
					{
						/* enums are validated using Tcl_GetIndexFromObj, which
						 * shimmers its input obj to record the table and index
						 * info, so make an effort to unify enums across parse_specs
						 */
						Tcl_Obj*	varname = Tcl_NewStringObj("::parse_args::enums", 19);
						Tcl_Obj*	enums = Tcl_ObjGetVar2(interp, varname, NULL, TCL_LEAVE_ERR_MSG);
						Tcl_Obj*	enum_choices = settingv[j++];
						Tcl_Obj*	shared_enum_choices = NULL;
						int			size;

						TEST_OK_LABEL(err, retcode, Tcl_DictObjGet(interp, enums, enum_choices, &shared_enum_choices));
						if (shared_enum_choices == NULL)
							TEST_OK_LABEL(err, retcode, Tcl_DictObjPut(interp, enums, enum_choices, shared_enum_choices = enum_choices));
						TEST_OK_LABEL(err, retcode, Tcl_DictObjSize(interp, enums, &size));

						if (size > 1000) {
							// Paranoia - prevent the speculative enum cache from growing too large
							if (Tcl_ObjSetVar2(interp, varname, NULL, Tcl_NewDictObj(), TCL_LEAVE_ERR_MSG) == NULL) {
								retcode = TCL_ERROR;
								goto err;
							}
							enums = NULL;
						}

						Tcl_IncrRefCount(option->enum_choices = shared_enum_choices);
					}
					break;
				case SETTING_COMMENT:
					Tcl_IncrRefCount(option->comment = settingv[j++]);
					break;
				default:
					THROW_ERROR_LABEL(err, retcode, "Invalid setting: ", Tcl_GetString(Tcl_NewIntObj(index)));
			}
		}

		if (option->name == NULL) {
			if (str[0] == '-') {
				Tcl_IncrRefCount(option->name = Tcl_NewStringObj(str+1, str_len-1));
			} else {
				Tcl_IncrRefCount(option->name = name);
			}
		}
	}

	if (spec->positional_arg_count > 0) {
		struct option_info*	last = &spec->positional[spec->positional_arg_count - 1];
		// Create special "args" behaviour for last positional param named "args"
		// strcmp is safe because Tcl_GetString always gives us a properly
		// \0 terminated string
		if (strcmp("args", Tcl_GetString(last->param)) == 0) {
			last->is_args = 1;
			if (last->default_val == NULL)
				Tcl_IncrRefCount(last->default_val = Tcl_NewObj());
		}
	}

	// TODO: better usage_msg
	Tcl_IncrRefCount((spec->usage_msg = Tcl_ObjPrintf("Invalid args, should be ?-option ...? %s", "?arg ...?")));

	if (obj->typePtr != NULL && obj->typePtr->freeIntRepProc != NULL)
		obj->typePtr->freeIntRepProc(obj);

	obj->typePtr = &parse_spec_type;
	obj->internalRep.otherValuePtr = spec;

	//fprintf(stderr, "Set typePtr to parse_spec_type and otherValuePtr to %p\n", obj->internalRep.otherValuePtr);

	return TCL_OK;

err:
	//fprintf(stderr, "set_from_any failed, freeing spec\n");
	free_parse_spec(&spec);
	return retcode;
}

//}}}

static int GetParseSpecFromObj(Tcl_Interp* interp, Tcl_Obj* spec, struct parse_spec** res) //{{{
{
	if (spec->typePtr != &parse_spec_type)
		TEST_OK(set_from_any(interp, spec));

	*res = (struct parse_spec*)spec->internalRep.otherValuePtr;

	return TCL_OK;
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
			Tcl_Obj*	cmd[oc+1];
			int			i;

			for (i=0; i<oc; i++) Tcl_IncrRefCount(cmd[i] = ov[i]);
			Tcl_IncrRefCount(cmd[oc] = val);
			res = Tcl_EvalObjv(interp, oc+1, cmd, 0);
			Tcl_IncrRefCount(verdict = Tcl_GetObjResult(interp));
			for (i=0; i<oc+1; i++) Tcl_DecrRefCount(cmd[i]);
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
	struct interp_data*	local = (struct interp_data*)cdata;
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
				int	index;

				// strcmp is safe because Tcl_GetString always returns properly
				// \0 terminated strings
				if (str_len == 2 && strcmp(str, "--") == 0) {
					check_options = 0;
					continue;
				}

				TEST_OK(Tcl_GetIndexFromObj(interp, av[i], spec->options, "option", TCL_EXACT, &index));
				option = &spec->option[index];

				option->supplied = 1;
				//fprintf(stderr, "Option \"%s\" arg_count: %d\n",
				//		Tcl_GetString(option->name), option->arg_count);
				if (ac - i - 1 < option->arg_count) {
					// This option requires an args and not enough remain
					Tcl_WrongNumArgs(interp, 1, objv, Tcl_GetString(spec->usage_msg));
					return TCL_ERROR;
				}

				switch (option->arg_count) {
					case 0:
						OUTPUT(option->name, local->true_obj);
						break;

					case 1:
						val = av[++i];
						VALIDATE(option, val);
						OUTPUT(option->name, val);
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
			OUTPUT(spec->positional[positional_arg].name, av[i]);
		}

		positional_arg++;
	}

	// Check -required and set -default for options that weren't specified
	for (i=0; i < spec->option_count; i++) {
		struct option_info* option = &spec->option[i];

		if (option->supplied) continue;

		if (option->default_val != NULL) {
			OUTPUT(option->name, option->default_val);
		} else if (option->arg_count == 0) {
			OUTPUT(option->name, local->false_obj);
		} else {
			if (option->required) {
				Tcl_SetErrorCode(interp, "PARSE_ARGS", "REQUIRED", Tcl_GetString(option->param), NULL);
				THROW_ERROR("option ", Tcl_GetString(option->param), " is required");
			}
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

int Parse_args_Init(Tcl_Interp* interp) //{{{
{
	struct interp_data*	local;

	if (Tcl_InitStubs(interp, "8.5", 0) == NULL)
		return TCL_ERROR;

	if (Tcl_PkgRequire(interp, "Tcl", "8.5", 0) == NULL)
		return TCL_ERROR;

	if (Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION) != TCL_OK)
		return TCL_ERROR;

	local = ckalloc(sizeof(struct interp_data));
	if (local == NULL)
		THROW_ERROR("Couldn't allocate per-interp data");

	Tcl_IncrRefCount(local->true_obj = Tcl_NewBooleanObj(1));
	Tcl_IncrRefCount(local->false_obj = Tcl_NewBooleanObj(0));

	Tcl_RegisterObjType(&enum_choices_type);
	Tcl_RegisterObjType(&parse_spec_type);

	Tcl_CreateObjCommand(interp, "::parse_args::parse_args", parse_args,
				(ClientData *)local, NULL);

	if (Tcl_ObjSetVar2(interp, Tcl_NewStringObj("::parse_args::enums", 19), NULL, Tcl_NewDictObj(), TCL_LEAVE_ERR_MSG) == NULL)
		return TCL_ERROR;

	return TCL_OK;
}

//}}}

// vim: foldmethod=marker foldmarker={{{,}}} ts=4 shiftwidth=4
