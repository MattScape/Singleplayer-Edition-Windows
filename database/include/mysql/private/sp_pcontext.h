/* -*- C++ -*- */
/* Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef _SP_PCONTEXT_H_
#define _SP_PCONTEXT_H_

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "sql_string.h"                         // LEX_STRING
#include "mysql_com.h"                          // enum_field_types
#include "field.h"                              // Create_field

class sp_pcontext;

typedef enum
{
  sp_param_in,
  sp_param_out,
  sp_param_inout
} sp_param_mode_t;

typedef struct sp_variable
{
  LEX_STRING name;
  enum enum_field_types type;
  sp_param_mode_t mode;
  
  /*
    offset -- this the index to the variable's value in the runtime frame.
    This is calculated during parsing and used when creating sp_instr_set
    instructions and Item_splocal items.
    I.e. values are set/referred by array indexing in runtime.
  */
  uint offset;

  Item *dflt;
  Create_field field_def;
} sp_variable_t;


#define SP_LAB_IMPL  0		// Implicit label generated by parser
#define SP_LAB_BEGIN 1		// Label at BEGIN
#define SP_LAB_ITER  2		// Label at iteration control

/*
  An SQL/PSM label. Can refer to the identifier used with the
  "label_name:" construct which may precede some SQL/PSM statements, or
  to an implicit implementation-dependent identifier which the parser
  inserts before a high-level flow control statement such as
  IF/WHILE/REPEAT/LOOP, when such statement is rewritten into
  a combination of low-level jump/jump_if instructions and labels.
*/

typedef struct sp_label
{
  char *name;
  uint ip;			// Instruction index
  int type;			// begin/iter or ref/free 
  sp_pcontext *ctx;             // The label's context
} sp_label_t;

typedef struct sp_cond_type
{
  enum { number, state, warning, notfound, exception } type;
  char sqlstate[SQLSTATE_LENGTH+1];
  uint mysqlerr;
} sp_cond_type_t;

/*
  Sanity check for SQLSTATEs. Will not check if it's really an existing
  state (there are just too many), but will check length bad characters.
*/
extern bool
sp_cond_check(LEX_STRING *sqlstate);

typedef struct sp_cond
{
  LEX_STRING name;
  sp_cond_type_t *val;
} sp_cond_t;

/**
  The scope of a label in Stored Procedures,
  for name resolution of labels in a parsing context.
*/
enum label_scope_type
{
  /**
    The labels declared in a parent context are in scope.
  */
  LABEL_DEFAULT_SCOPE,
  /**
    The labels declared in a parent context are not in scope.
  */
  LABEL_HANDLER_SCOPE
};

/**
  The parse-time context, used to keep track of declared variables/parameters,
  conditions, handlers, cursors and labels, during parsing.
  sp_contexts are organized as a tree, with one object for each begin-end
  block, one object for each exception handler,
  plus a root-context for the parameters.
  This is used during parsing for looking up defined names (e.g. declared
  variables and visible labels), for error checking, and to calculate offsets
  to be used at runtime. (During execution variable values, active handlers
  and cursors, etc, are referred to by an index in a stack.)
  Parsing contexts for exception handlers limit the visibility of labels.
  The pcontext tree is also kept during execution and is used for error
  checking (e.g. correct number of parameters), and in the future, used by
  the debugger.
*/

class sp_pcontext : public Sql_alloc
{
public:

  /**
    Constructor.
    Builds a parsing context root node.
  */
  sp_pcontext();

  // Free memory
  void
  destroy();

  /**
    Create and push a new context in the tree.
    @param label_scope label scope for the new parsing context
    @return the node created
  */
  sp_pcontext *
  push_context(label_scope_type label_scope);

  /**
    Pop a node from the parsing context tree.
    @return the parent node
  */
  sp_pcontext *
  pop_context();

  sp_pcontext *
  parent_context()
  {
    return m_parent;
  }

  /*
    Number of handlers/cursors to pop between this context and 'ctx'.
    If 'exclusive' is true, don't count the last block we are leaving;
    this is used for LEAVE where we will jump to the cpop/hpop instructions.
  */
  uint
  diff_handlers(sp_pcontext *ctx, bool exclusive);
  uint
  diff_cursors(sp_pcontext *ctx, bool exclusive);


  //
  // Parameters and variables
  //

  /*
    The maximum number of variables used in this and all child contexts
    In the root, this gives us the number of slots needed for variables
    during execution.
  */
  inline uint
  max_var_index()
  {
    return m_max_var_index;
  }

  /*
    The current number of variables used in the parents (from the root),
    including this context.
  */
  inline uint
  current_var_count()
  {
    return m_var_offset + m_vars.elements;
  }

  /* The number of variables in this context alone */
  inline uint
  context_var_count()
  {
    return m_vars.elements;
  }

  /* Map index in this pcontext to runtime offset */
  inline uint
  var_context2runtime(uint i)
  {
    return m_var_offset + i;
  }

  /* Set type of variable. 'i' is the offset from the top */
  inline void
  set_type(uint i, enum enum_field_types type)
  {
    sp_variable_t *p= find_variable(i);

    if (p)
      p->type= type;
  }

  /* Set default value of variable. 'i' is the offset from the top */
  inline void
  set_default(uint i, Item *it)
  {
    sp_variable_t *p= find_variable(i);

    if (p)
      p->dflt= it;
  }

  sp_variable_t *
  push_variable(LEX_STRING *name, enum enum_field_types type,
                sp_param_mode_t mode);

  /*
    Retrieve definitions of fields from the current context and its
    children.
  */
  void
  retrieve_field_definitions(List<Create_field> *field_def_lst);

  // Find by name
  sp_variable_t *
  find_variable(LEX_STRING *name, my_bool scoped=0);

  // Find by offset (from the top)
  sp_variable_t *
  find_variable(uint offset);

  /*
    Set the current scope boundary (for default values).
    The argument is the number of variables to skip.   
  */
  inline void
  declare_var_boundary(uint n)
  {
    m_pboundary= n;
  }

  /*
    CASE expressions support.
  */

  inline int
  register_case_expr()
  {
    return m_num_case_exprs++;
  }

  inline int
  get_num_case_exprs() const
  {
    return m_num_case_exprs;
  }

  inline bool
  push_case_expr_id(int case_expr_id)
  {
    return insert_dynamic(&m_case_expr_id_lst, (uchar*) &case_expr_id);
  }

  inline void
  pop_case_expr_id()
  {
    pop_dynamic(&m_case_expr_id_lst);
  }

  inline int
  get_current_case_expr_id() const
  {
    int case_expr_id;

    get_dynamic((DYNAMIC_ARRAY*)&m_case_expr_id_lst, (uchar*) &case_expr_id,
                m_case_expr_id_lst.elements - 1);

    return case_expr_id;
  }

  //
  // Labels
  //

  sp_label_t *
  push_label(char *name, uint ip);

  sp_label_t *
  find_label(char *name);

  inline sp_label_t *
  last_label()
  {
    sp_label_t *lab= m_label.head();

    if (!lab && m_parent)
      lab= m_parent->last_label();
    return lab;
  }

  inline sp_label_t *
  pop_label()
  {
    return m_label.pop();
  }

  //
  // Conditions
  //

  int
  push_cond(LEX_STRING *name, sp_cond_type_t *val);

  sp_cond_type_t *
  find_cond(LEX_STRING *name, my_bool scoped=0);

  //
  // Handlers
  //

  inline void
  push_handler(sp_cond_type_t *cond)
  {
    insert_dynamic(&m_handlers, (uchar*)&cond);
  }

  bool
  find_handler(sp_cond_type *cond);

  inline uint
  max_handler_index()
  {
    return m_max_handler_index + m_context_handlers;
  }

  inline void
  add_handlers(uint n)
  {
    m_context_handlers+= n;
  }

  //
  // Cursors
  //

  int
  push_cursor(LEX_STRING *name);

  my_bool
  find_cursor(LEX_STRING *name, uint *poff, my_bool scoped=0);

  /* Find by offset (for debugging only) */
  my_bool
  find_cursor(uint offset, LEX_STRING *n);

  inline uint
  max_cursor_index()
  {
    return m_max_cursor_index + m_cursors.elements;
  }

  inline uint
  current_cursor_count()
  {
    return m_cursor_offset + m_cursors.elements;
  }

protected:

  /**
    Constructor for a tree node.
    @param prev the parent parsing context
    @param label_scope label_scope for this parsing context
  */
  sp_pcontext(sp_pcontext *prev, label_scope_type label_scope);

  /*
    m_max_var_index -- number of variables (including all types of arguments)
    in this context including all children contexts.
    
    m_max_var_index >= m_vars.elements.

    m_max_var_index of the root parsing context contains number of all
    variables (including arguments) in all enclosed contexts.
  */
  uint m_max_var_index;		

  // The maximum sub context's framesizes
  uint m_max_cursor_index;
  uint m_max_handler_index;
  uint m_context_handlers;      // No. of handlers in this context

private:

  sp_pcontext *m_parent;	// Parent context

  /*
    m_var_offset -- this is an index of the first variable in this
                    parsing context.
    
    m_var_offset is 0 for root context.

    Since now each variable is stored in separate place, no reuse is done,
    so m_var_offset is different for all enclosed contexts.
  */
  uint m_var_offset;

  uint m_cursor_offset;		// Cursor offset for this context

  /*
    Boundary for finding variables in this context. This is the number
    of variables currently "invisible" to default clauses.
    This is normally 0, but will be larger during parsing of
    DECLARE ... DEFAULT, to get the scope right for DEFAULT values.
  */
  uint m_pboundary;

  int m_num_case_exprs;

  DYNAMIC_ARRAY m_vars;		// Parameters/variables
  DYNAMIC_ARRAY m_case_expr_id_lst; /* Stack of CASE expression ids. */
  DYNAMIC_ARRAY m_conds;        // Conditions
  DYNAMIC_ARRAY m_cursors;	// Cursors
  DYNAMIC_ARRAY m_handlers;	// Handlers, for checking for duplicates

  List<sp_label_t> m_label;	// The label list

  List<sp_pcontext> m_children;	// Children contexts, used for destruction

  /**
    Scope of labels for this parsing context.
  */
  label_scope_type m_label_scope;

private:
  sp_pcontext(const sp_pcontext &); /* Prevent use of these */
  void operator=(sp_pcontext &);
}; // class sp_pcontext : public Sql_alloc


#endif /* _SP_PCONTEXT_H_ */
