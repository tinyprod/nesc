/* This file is part of the nesC compiler.
   Copyright (C) 2002 Intel Corporation

The attached "nesC" software is provided to you under the terms and
conditions of the GNU General Public License Version 2 as published by the
Free Software Foundation.

nesC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with nesC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

#include "parser.h"
#include "nesc-semantics.h"
#include "semantics.h"
#include "nesc-decls.h"
#include "c-parse.h"
#include "input.h"
#include "AST_utils.h"
#include "nesc-paths.h"
#include "nesc-cpp.h"
#include "nesc-env.h"
#include "nesc-component.h"
#include "nesc-interface.h"
#include "edit.h"
#include "c-parse.h"
#include "semantics.h"
#include "attributes.h"

#include <errno.h>

#ifdef HAVE_BASENAME
#include <libgen.h>
#else
/* A trivial version, which should work for unix and cygwin, and for 
   our purposes.
   Does NOT handle trailing / properly (we're using it for files only)
   (returns "" in that case)
*/
char *basename(const char *path)
{
  char *end;

  if (!path || !*path)
    return ".";

  end = path + strlen(path);
  while (end > path)
    if (*--end == '/' || *end == '\\')
      return end + 1;

  return path;
}
#endif

bool nesc_attributep(gcc_attribute a)
/* Returns: TRUE if a is a nesc-specific attribute
 */
{
  const char *name = a->word1->cstring.data;

  return !strcmp(name, "C") || 
    !strcmp(name, "spontaneous") ||
    !strcmp(name, "combine") ||
    !strcmp(name, "nx_base") ||
    !strcmp(name, "hwevent") ||
    !strcmp(name, "atomic_hwevent");
}

type get_actual_function_type(type t)
/* Returns: The actual function type for a (possibly generic) type t
     representing the type of a function/command/event
 */
{
  if (type_generic(t))
    return type_function_return_type(t);
  else
    return t;
}

function_declarator ddecl_get_fdeclarator(data_declaration fndecl)
/* Effects: Returns fndecl's function_declarator
   Requires: fndecl represent a function or function pointer
*/
{
  declarator fd;

  if (is_variable_decl(fndecl->ast))
    fd = CAST(variable_decl, fndecl->ast)->declarator;
  else
    fd = CAST(function_decl, fndecl->ast)->declarator;

  return get_fdeclarator(fd);
}

declaration ddecl_get_gparms(data_declaration fndecl)
/* Effects: Returns the declaration list for fndecl's generic parameters 
   Requires: fndecl represent a function or function pointer
*/
{
  /* For functions in interfaces we get them from the interface ref */
  if (fndecl->kind == decl_function && fndecl->interface)
    return CAST(interface_ref, fndecl->interface->ast)->gparms;

  /* Otherwise we get them from the function's declarator */
  return ddecl_get_fdeclarator(fndecl)->gparms;
}

bool ddecl_is_command_or_event(data_declaration decl)
{
  return decl->kind == decl_function &&
    (decl->ftype == function_event || decl->ftype == function_command);
}

bool nesc_filename(const char *name)
{
  char *dot = strrchr(basename((char *)name), '.');

  if (dot)
    {
      if (!strcmp(dot, ".nc"))
	return TRUE;
    }
  return FALSE; /* C by default */
}

const char *element_name(region r, const char *path)
/* Returns: Return the "identifier part"
     of path, i.e., remove any directory and extension
     The returned string is allocated in region r.
*/
{
  const char *base, *dot;

  base = basename((char *)path);
  dot = strrchr(base, '.');

  if (dot)
    {
      /* Extract id */
      char *copy = rarrayalloc(r, dot - base + 1, char);

      memcpy(copy, base, dot - base);
      copy[dot - base] = '\0';

      return copy;
    }
  else
    return rstrdup(r, base);
}

const char *language_name(source_language l)
{
  switch (l)
    {
    case l_interface: return "interface";
    case l_component: case l_implementation: return "component";
    case l_c: return "C file";
    default: assert(0); return "BUG";
    }
}

node compile(location loc, source_language l,
	     const char *name, bool name_is_path)
{
  const char *path =
    name_is_path ? name : find_nesc_file(parse_region, l, name);
  FILE *f = NULL;
  node parse_tree = NULL;
  struct semantic_state old_semantic_state = current;

  if (!path)
    error_with_location(loc, "%s %s not found", language_name(l), name);
  else
    {
      if (flag_verbose)
	fprintf(stderr, "preprocessing %s\n", path);
      f = preprocess(path, l);

      if (!f)
	error_with_location(loc, "failed to preprocess %s", path);
      else
	{
	  set_input(f, path);

	  start_lex(l);
	  start_semantics(l_c, NULL, NULL);
	  current.fileregion = newregion();
	  parse_tree = parse();
	  deleteregion_ptr(&current.fileregion);
	  end_input();

	  preprocess_file_end();
	}
    }

  current = old_semantic_state;

  return parse_tree;
}

nesc_decl dummy_nesc_decl(source_language sl, location loc, const char *name,
			  bool declareit)
{
  word wname = build_word(parse_region, name);
  nesc_decl nd;
  nesc_declaration d = NULL;

  if (declareit)
    {
      d = nesc_lookup(name);
      /* If we managed to declare name, then we preserve its kind */
      if (d)
	sl = d->kind;
    }
  if (!d)
    d = new_nesc_declaration(parse_region, sl, name);

  switch (sl)
    {
    case l_component: {
      environment env = new_environment(parse_region, global_env, TRUE, FALSE);
      implementation impl = CAST(implementation,
	new_module(parse_region, loc, env, NULL));
      nd = CAST(nesc_decl,
		new_component(parse_region, dummy_location, wname, NULL, FALSE, NULL, NULL, impl));
      break;
    }
    case l_interface:
      nd = CAST(nesc_decl,
		new_interface(parse_region, loc, wname, NULL, NULL));
      break;
    default:
      assert(0);
      nd = NULL;
      break;
    }
  d->env = new_environment(parse_region, global_env, TRUE, FALSE);
  d->ast = nd;
  nd->cdecl = d;

  return nd;
}

void build(nesc_decl ast)
{
  struct semantic_state old_semantic_state = current;
  nesc_declaration decl = ast->cdecl;

  current.container = decl;

  switch (decl->kind)
    {
    case l_interface:
      build_interface(parse_region, decl);
      break;
    case l_component:
      build_component(parse_region, decl);
      break;
    default:
      assert(0);
    }

  current = old_semantic_state;
}

nesc_declaration load(source_language sl, location l,
		      const char *name, bool name_is_path)
{
  const char *element = name_is_path ? element_name(parse_region, name) : name;
  const char *actual_name;
  node ptree;
  nesc_decl ast;

  ptree = compile(l, sl, name, name_is_path);
  if (ptree)
    ast = CAST(nesc_decl, ptree);
  else
    ast = dummy_nesc_decl(sl, new_location(name, 0), element, TRUE);

  actual_name = ast->word1->cstring.data;
  if (strcmp(element, actual_name))
    error_with_location(ast->location,
			"expected %s `%s', but got %s '%s'",
			language_name(ast->cdecl->kind),
			element,
			language_name(ast->cdecl->kind),
			actual_name);

  build(ast);

  return ast->cdecl;
}

nesc_declaration start_nesc_entity(source_language sl, word name)
{
  nesc_declaration decl = new_nesc_declaration(parse_region, sl,
					       name->cstring.data);
  char *docstring = get_docstring();
    
  if (docstring)
    separate_short_docstring(docstring, &decl->short_docstring,
			     &decl->long_docstring);

  /* We don't get duplicates as we only load on demand */
  nesc_declare(decl);

  start_semantics(sl, decl, global_env);
  decl->env = current.env;

  return decl;
}

bool is_module_local_static(data_declaration ddecl)
{
  return ddecl->kind == decl_variable &&
    (ddecl->vtype == variable_static && ddecl->container_function &&
     ddecl->container_function->container); 
}

bool is_module_variable(data_declaration ddecl)
{
  return ddecl->kind == decl_variable &&
    ddecl->Cname == FALSE &&
    /* top-level module var or local static module var */
    (ddecl->container || is_module_local_static(ddecl));
}

const char *make_intf_printname(const char *iname, const char *fname)
/* Returns: string "iname.fname" allocated in current.fileregion
 */
{
  size_t ilen = strlen(iname);
  size_t dlen = strlen(fname);
  char *fullname = rstralloc(current.fileregion, ilen + dlen + 2);

  memcpy(fullname, iname, ilen);
  fullname[ilen] = '.';
  strcpy(fullname + ilen + 1, fname);

  return fullname;
}

const char *decl_printname(data_declaration ddecl)
{
  if (ddecl_is_command_or_event(ddecl) && ddecl->interface)
    return make_intf_printname(ddecl->interface->name, ddecl->name);
  else
    return ddecl->name;
}

data_declaration get_function_ddecl(expression e)
/* Returns: If e denotes a specific function, return its data_declaration
     Otherwise return NULL
*/
{
  if (is_identifier(e))
    {
      identifier id = CAST(identifier, e);

      if (id->ddecl->kind == decl_function)
	return id->ddecl;
    }
  else if (is_interface_deref(e))
    return CAST(interface_deref, e)->ddecl;

  return NULL;
}

data_declaration declare_function(location loc, const char *name, type signature)
/* Effects: If 'name' is already declared, check that it is a function with
     the specified signature.
     If it isn't declared, declare it as a function with the specified
     signature.
   Returns: data_declaration for the function, or NULL if an error was
     reported to the user.
*/
{
  data_declaration fdecl;
  bool ok = FALSE;

  /* If function already declared, declaration should match signature.
     If not, we declare it with given signature */
  fdecl = lookup_id(name, FALSE);
  if (fdecl)
    {
      if (fdecl->kind != decl_function ||
	  !(fdecl->ftype == function_normal || fdecl->ftype == function_static))
	error_with_location(loc, "function `%s' is not a C function",
			    name);
      else if (!type_compatible_unqualified(fdecl->type, signature))
	error_with_location(loc, "function `%s' does not have the right signature",
			    name);
      else
	ok = TRUE;
    }
  else
    {
      struct data_declaration tempdecl;
      declaration dummy = make_error_decl();

      /* Declare function  */
      dummy->location = loc;
      init_data_declaration(&tempdecl, dummy, name, signature);
      tempdecl.kind = decl_function;
      tempdecl.ftype = function_normal;
      tempdecl.isexternalscope = tempdecl.isfilescoperef = TRUE;
      fdecl = declare(current.env, &tempdecl, FALSE);

      ok = TRUE;
    }
  return ok ? fdecl : NULL;
}

void handle_combine_attribute(location loc, const char *combiner, type *t)
{
  data_declaration cdecl = 
    declare_function(loc, combiner,
		     build_function_type(parse_region, *t, *t, *t, NULL));

  if (cdecl)
    *t = make_combiner_type(*t, cdecl);
}

void handle_nxbase_attribute(location loc, const char *basename,
			     data_declaration ddecl)
{
  region r = parse_region;
  char *encoder_name, *decoder_name;
  type t = ddecl->type;

  encoder_name = rstralloc(r, strlen(basename) + 13);
  sprintf(encoder_name, "__nesc_hton_%s", basename);
  decoder_name = rstralloc(r, strlen(basename) + 13);
  sprintf(decoder_name, "__nesc_ntoh_%s", basename);

  ddecl->encoder = /* takes buffer and original value. returns original value */
    declare_function(loc, encoder_name,
		     build_function_type(r, t, ptr_void_type, t, NULL));

  ddecl->decoder = /* takes buffer and returns decoded value */
    declare_function(loc, decoder_name,
		     build_function_type(r, t, ptr_void_type, NULL));

  /* We do this even if we got an error, to ensure ddecl gets treated as
     a network type. */
  ddecl->basetype = t;
}

/* Create definition for template parameter 'elements d' with attributes
   attributes.
   Returns the declaration for the parameter.
*/
declaration declare_template_parameter(declarator d, type_element elements,
				       attribute attributes)
{
  /* There must be at least a declarator or some form of type specification */
  location l =
    d ? d->location : elements->location;
  variable_decl vd =
    new_variable_decl(parse_region, l, d, attributes, NULL, NULL, NULL);
  data_decl dd =
    new_data_decl(parse_region, l, elements, CAST(declaration, vd));
  data_declaration ddecl = NULL, old_decl = NULL;
  struct data_declaration tempdecl;
  dd_list extra_attr;
  int class;
  scflags scf;
  const char *name;
  bool defaulted_int;
  type parm_type;

  parse_declarator(elements, vd->declarator, FALSE, FALSE,
		   &class, &scf, NULL, &name, &parm_type,
		   &defaulted_int, NULL, &extra_attr);
  vd->declared_type = parm_type;

  /* Storage class checks */
  if (class)
    {
      /* Detect "typedef t", to declare a type parameter */
      if (class == RID_TYPEDEF && defaulted_int &&
	  is_identifier_declarator(d))
	return declare_type_parameter(d->location,
				      CAST(identifier_declarator, d)->cstring,
				      attributes, extra_attr);
      error("storage class specified for parameter `%s'", name);
      class = 0;
    }

  check_variable_scflags(scf, vd->location, "parameter", name);

/* Allow real, integral and string types. Not allowing complex for now,
   though it would be a trivial extension */
  if (!(type_real(parm_type) || type_chararray(parm_type, TRUE))) 
    error("only char [] and arithmetic types allowed as component arguments");

  if (type_array(parm_type))
    /* Transfer const-ness of array into that of type pointed to.  */
    parm_type =
      make_pointer_type(qualify_type1(type_array_of(parm_type), parm_type));

  init_data_declaration(&tempdecl, CAST(declaration, vd), name, parm_type);
  tempdecl.kind = decl_constant;
  tempdecl.substitute = TRUE;
  tempdecl.definition = tempdecl.ast;

  old_decl = lookup_id(tempdecl.name, TRUE);
  if (old_decl && duplicate_decls(&tempdecl, old_decl, FALSE, FALSE))
    ddecl = old_decl;
  else
    ddecl = declare(current.env, &tempdecl, FALSE);
  vd->ddecl = ddecl;

  ignored_dd_attributes(extra_attr);
  ignored_attributes(attributes);

  return CAST(declaration, dd);
}

declaration declare_type_parameter(location l, cstring id, attribute attribs,
				   dd_list extra_attr)
{
  type_parm_decl d = new_type_parm_decl(parse_region, l, id, NULL);
  data_declaration ddecl;

  if ((ddecl = lookup_id(id.data, TRUE)))
    error("duplicate parameter name `%s' in parameter list", id.data);
  else
    {
      struct data_declaration tempdecl;

      init_data_declaration(&tempdecl, CAST(declaration, d), id.data,
			    error_type);
      tempdecl.kind = decl_typedef;
      tempdecl.typevar_kind = typevar_normal;
      tempdecl.definition = tempdecl.ast;
      handle_decl_attributes(attribs, &tempdecl);
      handle_decl_dd_attributes(extra_attr, &tempdecl);
      ddecl = declare(current.env, &tempdecl, FALSE);
      ddecl->type = make_variable_type(ddecl);
    }
  d->ddecl = ddecl;

  return CAST(declaration, d);
}

expression make_type_argument(asttype t)
{
  type_argument e = new_type_argument(parse_region, t->location, t);

  e->type = t->type;

  return CAST(expression, e);
}

nesc_declaration original_component(nesc_declaration c)
{
  while (c->original)
    c = c->original;

  return c;
}

data_declaration original_declaration(data_declaration d)
{
  while (d->shadowed)
    d = d->shadowed;

  return d;
}

