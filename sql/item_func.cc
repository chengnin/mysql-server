/* Copyright (c) 2000, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file

  @brief
  This file defines all numerical functions
*/

#include "item_func.h"

#include <string.h>
#include <time.h>
#include <algorithm>
#include <cfloat>                // DBL_DIG
#include <cmath>                 // std::log2
#include <exception>             // std::exception subclasses
#include <iosfwd>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include "auth_acls.h"
#include "auth_common.h"         // check_password_strength
#include "binlog.h"              // mysql_bin_log
#include "check_stack.h"
#include "current_thd.h"         // current_thd
#include "dd/info_schema/stats.h" // dd::info_schema::Statistics_cache
#include "dd/object_id.h"
#include "dd/properties.h"       // dd::Properties
#include "dd_sql_view.h"         // push_view_warning_or_error
#include "dd_table_share.h"      // dd_get_old_field_type
#include "debug_sync.h"          // DEBUG_SYNC
#include "derror.h"              // ER_THD
#include "error_handler.h"       // Internal_error_handler
#include "hash.h"
#include "item_cmpfunc.h"        // get_datetime_value
#include "item_create.h"
#include "item_strfunc.h"        // Item_func_concat_ws
#include "json_dom.h"            // Json_wrapper
#include "key.h"
#include "log_event.h"
#include "m_string.h"
#include "mdl.h"
#include "my_bit.h"              // my_count_bits
#include "my_bitmap.h"
#include "my_psi_config.h"
#include "my_sqlcommand.h"
#include "my_thread.h"
#include "my_user.h"             // parse_user
#include "mysql/plugin.h"
#include "mysql/plugin_audit.h"
#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/psi/psi_base.h"
#include "mysql/psi/psi_mutex.h"
#include "mysql/service_mysql_password_policy.h"
#include "mysql/service_thd_wait.h"
#include "mysqld.h"              // log_10 stage_user_sleep
#include "parse_tree_helpers.h"  // PT_item_list
#include "prealloced_array.h"
#include "psi_memory_key.h"
#include "query_result.h"        // sql_exchange
#include "rpl_gtid.h"
#include "rpl_mi.h"              // Master_info
#include "rpl_msr.h"             // channel_map
#include "rpl_rli.h"             // Relay_log_info
#include "session_tracker.h"
#include "sp.h"                  // sp_setup_routine
#include "sp_head.h"             // sp_name
#include "sql_audit.h"           // audit_global_variable
#include "sql_base.h"            // Internal_error_handler_holder
#include "sql_bitmap.h"
#include "sql_class.h"           // THD
#include "sql_error.h"
#include "sql_lex.h"
#include "sql_list.h"
#include "sql_optimizer.h"       // JOIN
#include "sql_parse.h"           // check_stack_overrun
#include "sql_plugin.h"
#include "sql_plugin_ref.h"
#include "sql_security_ctx.h"
#include "sql_show.h"            // append_identifier
#include "sql_time.h"            // TIME_from_longlong_packed
#include "strfunc.h"             // find_type
#include "thr_mutex.h"
#include "val_int_compare.h"     // Integer_value

class Protocol;
class sp_rcontext;

using std::min;
using std::max;

bool check_reserved_words(LEX_STRING *name)
{
  if (!my_strcasecmp(system_charset_info, name->str, "GLOBAL") ||
      !my_strcasecmp(system_charset_info, name->str, "LOCAL") ||
      !my_strcasecmp(system_charset_info, name->str, "SESSION"))
    return TRUE;
  return FALSE;
}


/**
  Evaluate a constant condition, represented by an Item tree

  @param      thd   Thread handler
  @param      cond  The constant condition to evaluate
  @param[out] value Returned value, either true or false

  @returns false if evaluation is successful, true otherwise
*/

bool eval_const_cond(THD *thd, Item *cond, bool *value)
{
  DBUG_ASSERT(cond->const_item());
  *value= cond->val_int();
  return thd->is_error();
}


/**
   Test if the sum of arguments overflows the ulonglong range.
*/
static inline bool test_if_sum_overflows_ull(ulonglong arg1, ulonglong arg2)
{
  return ULLONG_MAX - arg1 < arg2;
}

void Item_func::set_arguments(List<Item> &list, bool context_free)
{
  allowed_arg_cols= 1;
  arg_count=list.elements;
  args= tmp_arg;                                // If 2 arguments
  if (arg_count <= 2 || (args=(Item**) sql_alloc(sizeof(Item*)*arg_count)))
  {
    List_iterator_fast<Item> li(list);
    Item *item;
    Item **save_args= args;

    while ((item=li++))
    {
      *(save_args++)= item;
      if (!context_free)
        with_sum_func|= item->with_sum_func;
    }
  }
  else
    arg_count= 0; // OOM
  list.empty();					// Fields are used
}

Item_func::Item_func(List<Item> &list)
  :allowed_arg_cols(1)
{
  set_arguments(list, false);
}


Item_func::Item_func(const POS &pos, PT_item_list *opt_list)
  : super(pos), allowed_arg_cols(1)
{
  if (opt_list == NULL)
  {
    args= tmp_arg;
    arg_count= 0;
  }
  else
    set_arguments(opt_list->value, true);
}

Item_func::Item_func(THD *thd, Item_func *item)
  :Item_result_field(thd, item),
   const_item_cache(0),
   allowed_arg_cols(item->allowed_arg_cols),
   used_tables_cache(item->used_tables_cache),
   not_null_tables_cache(item->not_null_tables_cache),
   arg_count(item->arg_count)
{
  if (arg_count)
  {
    if (arg_count <=2)
      args= tmp_arg;
    else
    {
      if (!(args=(Item**) thd->alloc(sizeof(Item*)*arg_count)))
	return;
    }
    memcpy((char*) args, (char*) item->args, sizeof(Item*)*arg_count);
  }
}


bool Item_func::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  with_sum_func= 0;
  const bool no_named_params= !may_have_named_parameters();
  for (size_t i= 0; i < arg_count; i++)
  {
    with_sum_func|= args[i]->with_sum_func;
    if (args[i]->itemize(pc, &args[i]))
      return true;
    if (no_named_params && !args[i]->item_name.is_autogenerated())
    {
      my_error(functype() == FUNC_SP ? ER_WRONG_PARAMETERS_TO_STORED_FCT
                                     : ER_WRONG_PARAMETERS_TO_NATIVE_FCT,
               MYF(0), func_name());
      return true;
    }
  }
  return false;
}


/*
  Resolve references to table column for a function and its argument

  SYNOPSIS:
  fix_fields()
  thd		Thread object
  ref		Pointer to where this object is used.  This reference
		is used if we want to replace this object with another
		one (for example in the summary functions).

  DESCRIPTION
    Call fix_fields() for all arguments to the function.  The main intention
    is to allow all Item_field() objects to setup pointers to the table fields.

    Sets as a side effect the following class variables:
      maybe_null	Set if any argument may return NULL
      with_sum_func	Set if any of the arguments contains a sum function
      used_tables_cache Set to union of the tables used by arguments

      str_value.charset If this is a string function, set this to the
			character set for the first argument.
			If any argument is binary, this is set to binary

   If for any item any of the defaults are wrong, then this can
   be fixed in the resolve_type() function that is called after this one or
   by writing a specialized fix_fields() for the item.

  RETURN VALUES
  FALSE	ok
  TRUE	Got error.  Stored with my_error().
*/

bool
Item_func::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0 || basic_const_item());

  Item **arg,**arg_end;
  uchar buff[STACK_BUFF_ALLOC];			// Max argument in function

  /*
    Semi-join flattening should only be performed for top-level
    predicates. Disable it for predicates that live under an
    Item_func.
  */
  Disable_semijoin_flattening DSF(thd->lex->current_select(), true);

  used_tables_cache= get_initial_pseudo_tables();
  not_null_tables_cache= 0;
  const_item_cache=1;

  /*
    Use stack limit of STACK_MIN_SIZE * 2 since
    on some platforms a recursive call to fix_fields
    requires more than STACK_MIN_SIZE bytes (e.g. for
    MIPS, it takes about 22kB to make one recursive
    call to Item_func::fix_fields())
  */
  if (check_stack_overrun(thd, STACK_MIN_SIZE * 2, buff))
    return TRUE;				// Fatal error if flag is set!
  if (arg_count)
  {						// Print purify happy
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      if (fix_func_arg(thd, arg))
        return true;
    }
  }
  if (resolve_type(thd) || thd->is_error()) // Some impls still not error-safe
    return true;
  fixed= true;
  return false;
}


bool Item_func::fix_func_arg(THD *thd, Item **arg)
{
  if ((!(*arg)->fixed && (*arg)->fix_fields(thd, arg)))
    return true;                                /* purecov: inspected */
  Item *item= *arg;

  if (allowed_arg_cols)
  {
    if (item->check_cols(allowed_arg_cols))
      return true;
  }
  else
  {
    /*  we have to fetch allowed_arg_cols from first argument */
    DBUG_ASSERT(arg == args); // it is first argument
    allowed_arg_cols= item->cols();
    DBUG_ASSERT(allowed_arg_cols); // Can't be 0 any more
  }

  maybe_null|=            item->maybe_null;
  with_sum_func|=         item->with_sum_func;
  used_tables_cache|=     item->used_tables();
  not_null_tables_cache|= item->not_null_tables();
  const_item_cache&=      item->const_item();
  with_subselect|=        item->has_subquery();
  with_stored_program|=   item->has_stored_program();
  return false;
}

void Item_func::fix_after_pullout(SELECT_LEX *parent_select,
                                  SELECT_LEX *removed_select)
{
  if (const_item())
  {
    /*
      Pulling out a const item changes nothing to it. Moreover, some items may
      have decided that they're const by some other logic than the generic
      one below, and we must preserve that decision.
    */
    return;
  }

  Item **arg,**arg_end;

  used_tables_cache= get_initial_pseudo_tables();
  not_null_tables_cache= 0;
  const_item_cache=1;

  if (arg_count)
  {
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      Item *const item= *arg;
      item->fix_after_pullout(parent_select, removed_select);

      used_tables_cache|=     item->used_tables();
      not_null_tables_cache|= item->not_null_tables();
      const_item_cache&=      item->const_item();
    }
  }
}


bool Item_func::walk(Item_processor processor, enum_walk walk, uchar *argument)
{
  if ((walk & WALK_PREFIX) && (this->*processor)(argument))
    return true;

  Item **arg, **arg_end;
  for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
  {
    if ((*arg)->walk(processor, walk, argument))
      return true;
  }
  return (walk & WALK_POSTFIX) && (this->*processor)(argument);
}

void Item_func::traverse_cond(Cond_traverser traverser,
                              void *argument, traverse_order order)
{
  if (arg_count)
  {
    Item **arg,**arg_end;

    switch (order) {
    case(PREFIX):
      (*traverser)(this, argument);
      for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
      {
	(*arg)->traverse_cond(traverser, argument, order);
      }
      break;
    case (POSTFIX):
      for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
      {
	(*arg)->traverse_cond(traverser, argument, order);
      }
      (*traverser)(this, argument);
    }
  }
  else
    (*traverser)(this, argument);
}


/**
  Transform an Item_func object with a transformer callback function.

    The function recursively applies the transform method to each
    argument of the Item_func node.
    If the call of the method for an argument item returns a new item
    the old item is substituted for a new one.
    After this the transformer is applied to the root node
    of the Item_func object. 
*/

Item *Item_func::transform(Item_transformer transformer, uchar *argument)
{
  if (arg_count)
  {
    Item **arg,**arg_end;
    for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
    {
      Item *new_item= (*arg)->transform(transformer, argument);
      if (new_item == NULL)
        return NULL;                 /* purecov: inspected */

      /*
        THD::change_item_tree() should be called only if the tree was
        really transformed, i.e. when a new item has been created.
        Otherwise we'll be allocating a lot of unnecessary memory for
        change records at each execution.
      */
      if (*arg != new_item)
        current_thd->change_item_tree(arg, new_item);
    }
  }
  return (this->*transformer)(argument);
}


/**
  Compile Item_func object with a processor and a transformer
  callback functions.

    First the function applies the analyzer to the root node of
    the Item_func object. Then if the analyzer succeeeds (returns TRUE)
    the function recursively applies the compile method to each argument
    of the Item_func node.
    If the call of the method for an argument item returns a new item
    the old item is substituted for a new one.
    After this the transformer is applied to the root node
    of the Item_func object. 
*/

Item *Item_func::compile(Item_analyzer analyzer, uchar **arg_p,
                         Item_transformer transformer, uchar *arg_t)
{
  if (!(this->*analyzer)(arg_p))
    return this;
  if (arg_count)
  {
    Item **arg,**arg_end;
    for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
    {
      /* 
        The same parameter value of arg_p must be passed
        to analyze any argument of the condition formula.
      */   
      uchar *arg_v= *arg_p;
      Item *new_item= (*arg)->compile(analyzer, &arg_v, transformer, arg_t);
      if (new_item == NULL)
        return NULL;
      if (*arg != new_item)
        current_thd->change_item_tree(arg, new_item);
    }
  }
  return (this->*transformer)(arg_t);
}

/**
  See comments in Item_cmp_func::split_sum_func()
*/

void Item_func::split_sum_func(THD *thd, Ref_item_array ref_item_array,
                               List<Item> &fields)
{
  Item **arg, **arg_end;
  for (arg= args, arg_end= args+arg_count; arg != arg_end ; arg++)
    (*arg)->split_sum_func2(thd, ref_item_array, fields, arg, TRUE);
}


void Item_func::update_used_tables()
{
  used_tables_cache= get_initial_pseudo_tables();
  const_item_cache=1;
  with_subselect= false;
  with_stored_program= false;
  for (uint i=0 ; i < arg_count ; i++)
  {
    args[i]->update_used_tables();
    used_tables_cache|=args[i]->used_tables();
    const_item_cache&=args[i]->const_item();
    with_subselect|= args[i]->has_subquery();
    with_stored_program|= args[i]->has_stored_program();
  }
}


table_map Item_func::used_tables() const
{
  return used_tables_cache;
}


table_map Item_func::not_null_tables() const
{
  return not_null_tables_cache;
}


void Item_func::print(String *str, enum_query_type query_type)
{
  str->append(func_name());
  str->append('(');
  print_args(str, 0, query_type);
  str->append(')');
}


void Item_func::print_args(String *str, uint from, enum_query_type query_type)
{
  for (uint i=from ; i < arg_count ; i++)
  {
    if (i != from)
      str->append(',');
    args[i]->print(str, query_type);
  }
}


void Item_func::print_op(String *str, enum_query_type query_type)
{
  str->append('(');
  for (uint i=0 ; i < arg_count-1 ; i++)
  {
    args[i]->print(str, query_type);
    str->append(' ');
    str->append(func_name());
    str->append(' ');
  }
  args[arg_count-1]->print(str, query_type);
  str->append(')');
}

/// @note Please keep in sync with Item_sum::eq().
bool Item_func::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;
  if (item->type() != FUNC_ITEM)
    return 0;
  Item_func *item_func=(Item_func*) item;
  Item_func::Functype func_type;
  if ((func_type= functype()) != item_func->functype() ||
      arg_count != item_func->arg_count ||
      (func_type != Item_func::FUNC_SP &&
       func_name() != item_func->func_name()) ||
      (func_type == Item_func::FUNC_SP &&
       my_strcasecmp(system_charset_info, func_name(), item_func->func_name())))
    return 0;
  for (uint i=0; i < arg_count ; i++)
    if (!args[i]->eq(item_func->args[i], binary_cmp))
      return 0;
  return 1;
}


Field *Item_func::tmp_table_field(TABLE *table)
{
  Field *field= NULL;

  switch (result_type()) {
  case INT_RESULT:
    if (max_char_length() > MY_INT32_NUM_DECIMAL_DIGITS)
      field= new Field_longlong(max_char_length(), maybe_null, item_name.ptr(),
                                unsigned_flag);
    else
      field= new Field_long(max_char_length(), maybe_null, item_name.ptr(),
                            unsigned_flag);
    break;
  case REAL_RESULT:
    field= new Field_double(max_char_length(), maybe_null, item_name.ptr(), decimals);
    break;
  case STRING_RESULT:
    return make_string_field(table);
    break;
  case DECIMAL_RESULT:
    field= Field_new_decimal::create_from_item(this);
    break;
  case ROW_RESULT:
  default:
    // This case should never be chosen
    DBUG_ASSERT(0);
    field= 0;
    break;
  }
  if (field)
    field->init(table);
  return field;
}


my_decimal *Item_func::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed);
  longlong nr= val_int();
  if (null_value)
    return 0; /* purecov: inspected */
  int2my_decimal(E_DEC_FATAL_ERROR, nr, unsigned_flag, decimal_value);
  return decimal_value;
}


type_conversion_status Item_func::save_possibly_as_json(Field *field,
                                                        bool no_conversions)
{
  if (field_type() == MYSQL_TYPE_JSON && field->type() == MYSQL_TYPE_JSON)
  {
    // Store the value in the JSON binary format.
    Field_json *f= down_cast<Field_json *>(field);
    Json_wrapper wr;
    val_json(&wr);

    if (null_value)
      return set_field_to_null(field);

    field->set_notnull();
    return f->store_json(&wr);
  }

  return Item_func::save_in_field_inner(field, no_conversions);
}

String *Item_real_func::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  double nr= val_real();
  if (null_value)
    return 0; /* purecov: inspected */
  str->set_real(nr, decimals, collation.collation);
  return str;
}


my_decimal *Item_real_func::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed);
  double nr= val_real();
  if (null_value)
    return 0; /* purecov: inspected */
  double2my_decimal(E_DEC_FATAL_ERROR, nr, decimal_value);
  return decimal_value;
}


void Item_func::fix_num_length_and_dec()
{
  uint fl_length= 0;
  decimals=0;
  for (uint i=0 ; i < arg_count ; i++)
  {
    set_if_bigger(decimals,args[i]->decimals);
    set_if_bigger(fl_length, args[i]->max_length);
  }
  max_length=float_length(decimals);
  if (fl_length > max_length)
  {
    decimals= NOT_FIXED_DEC;
    max_length= float_length(NOT_FIXED_DEC);
  }
}


void Item_func_numhybrid::fix_num_length_and_dec()
{}



/**
  Count max_length and decimals for temporal functions.

  @param item    Argument array
  @param nitems  Number of arguments in the array.
*/

void Item_func::count_datetime_length(Item **item, uint nitems)
{
  unsigned_flag= 0;
  decimals= 0;
  if (field_type() != MYSQL_TYPE_DATE)
  {
    for (uint i= 0; i < nitems; i++)
      set_if_bigger(decimals,
                    field_type() == MYSQL_TYPE_TIME ?
                    item[i]->time_precision() : item[i]->datetime_precision());
  }
  set_if_smaller(decimals, DATETIME_MAX_DECIMALS);
  uint len= decimals ? (decimals + 1) : 0;
  switch (field_type())
  {
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      len+= MAX_DATETIME_WIDTH;
      break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      len+= MAX_DATE_WIDTH;
      break;
    case MYSQL_TYPE_TIME:
      len+= MAX_TIME_WIDTH;
      break;
    default:
      DBUG_ASSERT(0);
  }
  fix_char_length(len);
}

/**
  Set max_length/decimals of function if function is fixed point and
  result length/precision depends on argument ones.

  @param item    Argument array.
  @param nitems  Number of arguments in the array.

  This function doesn't set unsigned_flag. Call agg_result_type()
  first to do that.
*/

void Item_func::count_decimal_length(Item **item, uint nitems)
{
  int max_int_part= 0;
  decimals= 0;
  for (uint i=0 ; i < nitems ; i++)
  {
    set_if_bigger(decimals, item[i]->decimals);
    set_if_bigger(max_int_part, item[i]->decimal_int_part());
  }
  int precision= min(max_int_part + decimals, DECIMAL_MAX_PRECISION);
  fix_char_length(my_decimal_precision_to_length_no_truncation(precision,
                                                               decimals,
                                                               unsigned_flag));
}

/**
  Set char_length to the maximum number of characters required by any
  of this function's arguments.

  This function doesn't set unsigned_flag. Call agg_result_type()
  first to do that.
*/

void Item_func::count_only_length(Item **item, uint nitems)
{
  uint32 char_length= 0;
  for (uint i= 0; i < nitems; i++)
    set_if_bigger(char_length, item[i]->max_char_length());
  fix_char_length(char_length);
}

/**
  Set max_length/decimals of function if function is floating point and
  result length/precision depends on argument ones.

  @param item    Argument array.
  @param nitems  Number of arguments in the array.
*/

void Item_func::count_real_length(Item **item, uint nitems)
{
  uint32 length= 0;
  decimals= 0;
  max_length= 0;
  for (uint i=0 ; i < nitems; i++)
  {
    if (decimals != NOT_FIXED_DEC)
    {
      set_if_bigger(decimals, item[i]->decimals);
      set_if_bigger(length, (item[i]->max_length - item[i]->decimals));
    }
    set_if_bigger(max_length, item[i]->max_length);
  }
  if (decimals != NOT_FIXED_DEC)
  {
    max_length= length;
    length+= decimals;
    if (length < max_length)  // If previous operation gave overflow
      max_length= UINT_MAX32;
    else
      max_length= length;
  }
}

/**
  Calculate max_length and decimals for STRING_RESULT functions.

  @param field_type  Field type.
  @param items       Argument array.
  @param nitems      Number of arguments.

  @retval            False on success, true on error.
*/
bool Item_func::count_string_result_length(enum_field_types field_type,
                                           Item **items, uint nitems)
{
  if (agg_arg_charsets_for_string_result(collation, items, nitems))
    return true;
  if (is_temporal_type(field_type))
    count_datetime_length(items, nitems);
  else
  {
    decimals= NOT_FIXED_DEC;
    count_only_length(items, nitems);
  }
  return false;
}


void Item_func::signal_divide_by_null()
{
  THD *thd= current_thd;
  if (thd->variables.sql_mode & MODE_ERROR_FOR_DIVISION_BY_ZERO)
    push_warning(thd, Sql_condition::SL_WARNING, ER_DIVISION_BY_ZERO,
                 ER_THD(thd, ER_DIVISION_BY_ZERO));
  null_value= 1;
}


void Item_func::signal_invalid_argument_for_log()
{
  THD *thd= current_thd;
  push_warning(thd, Sql_condition::SL_WARNING,
               ER_INVALID_ARGUMENT_FOR_LOGARITHM,
               ER_THD(thd, ER_INVALID_ARGUMENT_FOR_LOGARITHM));
  null_value= TRUE;
}


Item *Item_func::get_tmp_table_item(THD *thd)
{
  if (!with_sum_func && !const_item())
    return new Item_field(result_field);
  return copy_or_same(thd);
}

const Item_field* 
Item_func::contributes_to_filter(table_map read_tables,
                                 table_map filter_for_table,
                                 const MY_BITMAP *fields_to_ignore) const
{
  DBUG_ASSERT((read_tables & filter_for_table) == 0);
  /*
    Multiple equality (Item_equal) should not call this function
    because it would reject valid comparisons.
  */
  DBUG_ASSERT(functype() != MULT_EQUAL_FUNC);

   /*
     To contribute to filering effect, the condition must refer to
     exactly one unread table: the table filtering is currently
     calculated for.
   */
   if ((used_tables() & ~read_tables) != filter_for_table)
     return NULL;

  /*
    Whether or not this Item_func has an operand that is a field in
    'filter_for_table' that is not in 'fields_to_ignore'.
  */
  Item_field* usable_field= NULL;

  /*
    Whether or not this Item_func has an operand that can be used as
    available value. arg_count==1 for Items with implicit values like
    "field IS NULL".
  */
  bool found_comparable= (arg_count == 1);

  for (uint i= 0; i < arg_count; i++)
  {
    const Item::Type arg_type= args[i]->real_item()->type();

    if (arg_type == Item::SUBSELECT_ITEM)
    {
      if (args[i]->const_item())
      {
        // Constant subquery, i.e., not a dependent subquery. 
        found_comparable= true;
        continue;
      }

      /*
        This is either "fld OP <dependent_subquery>" or "fld BETWEEN X
        and Y" where either X or Y is a dependent subquery. Filtering
        effect should not be calculated for this item because the cost
        of evaluating the dependent subquery is currently not
        calculated and its accompanying filtering effect is too
        uncertain. See WL#7384.
      */
      return NULL;
    } // ... if subquery.

    const table_map used_tabs= args[i]->used_tables();

    if (arg_type == Item::FIELD_ITEM && (used_tabs == filter_for_table))
    {
      /*
        The qualifying table of args[i] is filter_for_table. args[i]
        may be a field or a reference to a field, e.g. through a
        view.
      */
      Item_field *fld= static_cast<Item_field*>(args[i]->real_item());

      /*
        Use args[i] as value if
        1) this field shall be ignored, or 
        2) a usable field has already been found (meaning that
        this is "filter_for_table.colX OP filter_for_table.colY").
      */
      if (bitmap_is_set(fields_to_ignore, fld->field->field_index) || // 1)
          usable_field)                                               // 2)
      {
        found_comparable= true;
        continue;
      }

      /*
        This field shall contribute to filtering effect if a
        value is found for it
      */
      usable_field= fld;
    } // if field.
    else
    {
      /*
        It's not a subquery. May be a function, a constant, an outer
        reference, a field of another table...

        Already checked that this predicate does not refer to tables
        later in the join sequence. Verify it:
      */
      DBUG_ASSERT(!(used_tabs & (~read_tables & ~filter_for_table)));
      found_comparable= true;
    }
  }
  return (found_comparable ? usable_field : NULL);
}

/**
  Return new Item_field if given expression matches GC

  @see substitute_gc()

  @param func           Expression to be replaced
  @param fld            GCs field
  @param type           Result type to match with Field

  @returns
    item new Item_field for matched GC
    NULL otherwise
*/

Item_field *get_gc_for_expr(Item_func **func, Field *fld, Item_result type)
{
  Item_func *expr= down_cast<Item_func*>(fld->gcol_info->expr_item);

  /*
    In the case where the generated column expression returns JSON and
    the predicate compares the values as strings, it is not safe to
    replace the expression with the generated column, since the
    indexed string values will be double-quoted. The generated column
    expression should use the JSON_UNQUOTE function to strip off the
    double-quotes in order to get a usable index for looking up
    strings. See also the comment below.
  */
  if (type == STRING_RESULT && expr->field_type() == MYSQL_TYPE_JSON)
    return NULL;

  /*
    Skip unquoting function. This is needed to address JSON string
    comparison issue. All JSON_* functions return quoted strings. In
    order to create usable index, GC column expression has to include
    JSON_UNQUOTE function, e.g JSON_UNQUOTE(JSON_EXTRACT(..)).
    Hence, the unquoting function in column expression have to be
    skipped in order to correctly match GC expr to expr in
    WHERE condition.  The exception is if user has explicitly used
    JSON_UNQUOTE in WHERE condition.
  */
  if (!strcmp(expr->func_name(),"json_unquote") &&
      strcmp((*func)->func_name(),"json_unquote"))
  {
    if (!expr->arguments()[0]->can_be_substituted_for_gc())
      return NULL;
    expr= down_cast<Item_func*>(expr->arguments()[0]);
  }
  DBUG_ASSERT(expr->can_be_substituted_for_gc());

  if (type == fld->result_type() && (*func)->eq(expr, false))
  {
    Item_field *field= new Item_field(fld);
    // Mark field for read
    fld->table->mark_column_used(fld->table->in_use, fld, MARK_COLUMNS_READ);
    return field;
  }
  return NULL;
}


/**
  Transformer function for GC substitution.

  @param arg  List of indexed GC field

  @return this item 

  @details This function transforms the WHERE condition. It doesn't change
  'this' item but rather changes its arguments. It takes list of GC fields
  and checks whether arguments of 'this' item matches them and index over
  the GC field isn't disabled with hints. If so, it replaces
  the argument with newly created Item_field which uses the matched GC
  field. Following functions' arguments could be transformed:
  - EQ_FUNC, LT_FUNC, LE_FUNC, GE_FUNC, GT_FUNC
    - Left _or_ right argument if the opposite argument is a constant.
  - IN_FUNC, BETWEEN
    - Left argument if all other arguments are constant and of the same type.

  After transformation comparators are updated to take into account the new
  field.
*/

Item *Item_func::gc_subst_transformer(uchar *arg)
{
  switch(functype()) {
  case EQ_FUNC:
  case LT_FUNC:
  case LE_FUNC:
  case GE_FUNC:
  case GT_FUNC:
  {
    Item_func **func= NULL;
    Item **val= NULL;
    List<Field> *gc_fields= (List<Field> *)arg;
    List_iterator<Field> li(*gc_fields);
    // Check if we can substitute a function with a GC
    if (args[0]->can_be_substituted_for_gc() && args[1]->const_item())
    {
      func= (Item_func**)args;
      val= args + 1;
    }
    else if (args[1]->can_be_substituted_for_gc() && args[0]->const_item())
    {
      func= (Item_func**)args + 1;
      val= args;
    }
    if (func)
    {
      Field *fld;
      while((fld= li++))
      {
        // Check whether field has usable keys
        Key_map tkm= fld->part_of_key;
        tkm.intersect(fld->table->keys_in_use_for_query);
        Item_field *field;

        if (!tkm.is_clear_all() &&
            (field= get_gc_for_expr(func, fld, (*val)->result_type())))
        {
          // Matching expression is found, substutite arg with the new
          // field
          fld->table->in_use->change_item_tree(pointer_cast<Item**>(func),
                                               field);
          // Adjust comparator
          if (down_cast<Item_bool_func2 *>(this)->set_cmp_func())
            return NULL;
          break;
        }
      }
    }
    break;
  }
  case BETWEEN:
  case IN_FUNC:
  {
    List<Field> *gc_fields= (List<Field> *)arg;
    List_iterator<Field> li(*gc_fields);
    if (!args[0]->can_be_substituted_for_gc())
      break;
    Item_result type= args[1]->result_type();
    bool can_do_subst= args[1]->const_item();
    for (uint i= 2; i < arg_count && can_do_subst; i++)
      if (!args[i]->const_item() || args[i]->result_type() != type)
      {
        can_do_subst= false;
        break;
      }
    if (can_do_subst)
    {
      Field *fld;
      while ((fld= li++))
      {
        // Check whether field has usable keys
        Key_map tkm= fld->part_of_key;
        tkm.intersect(fld->table->keys_in_use_for_query);
        Item_field *field;

        if (!tkm.is_clear_all() &&
            (field= get_gc_for_expr(pointer_cast<Item_func**>(args), fld,
                                    type)))
        {
          // Matching expression is found, substutite arg[0] with the new
          // field
          fld->table->in_use->change_item_tree(pointer_cast<Item**>(args),
                                               field);
          // Adjust comparators
          if (functype() == IN_FUNC)
            ((Item_func_in*)this)->cleanup_arrays();
          if (resolve_type(fld->table->in_use))
            return NULL;
          break;
        }
      }
    }
    break;
  }
  default:
    break;
  }
  return this;
}


void Item_func::replace_argument(THD *thd, Item **oldpp, Item *newp)
{
  thd->change_item_tree(oldpp, newp);
}


double Item_int_func::val_real()
{
  DBUG_ASSERT(fixed == 1);

  return unsigned_flag ? (double) ((ulonglong) val_int()) : (double) val_int();
}


String *Item_int_func::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  longlong nr=val_int();
  if (null_value)
    return 0;
  str->set_int(nr, unsigned_flag, collation.collation);
  return str;
}


bool Item_func_connection_id::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->safe_to_cache_query= false;
  return false;
}


bool Item_func_connection_id::resolve_type(THD *thd)
{
  if (Item_int_func::resolve_type(thd))
    return true;
  unsigned_flag= true;
  return false;
}


bool Item_func_connection_id::fix_fields(THD *thd, Item **ref)
{
  if (Item_int_func::fix_fields(thd, ref))
    return TRUE;
  thd->thread_specific_used= TRUE;
  value= thd->variables.pseudo_thread_id;
  return FALSE;
}


/**
  Check arguments here to determine result's type for a numeric
  function of two arguments.
*/

void Item_num_op::find_num_type(void)
{
  DBUG_ENTER("Item_num_op::find_num_type");
  DBUG_PRINT("info", ("name %s", func_name()));
  DBUG_ASSERT(arg_count == 2);
  Item_result r0= args[0]->numeric_context_result_type();
  Item_result r1= args[1]->numeric_context_result_type();
  
  DBUG_ASSERT(r0 != STRING_RESULT && r1 != STRING_RESULT);

  if (r0 == REAL_RESULT || r1 == REAL_RESULT)
  {
    /*
      Since DATE/TIME/DATETIME data types return INT_RESULT/DECIMAL_RESULT
      type codes, we should never get to here when both fields are temporal.
    */
    DBUG_ASSERT(!args[0]->is_temporal() || !args[1]->is_temporal());
    count_real_length(args, arg_count);
    max_length= float_length(decimals);
    hybrid_type= REAL_RESULT;
  }
  else if (r0 == DECIMAL_RESULT || r1 == DECIMAL_RESULT)
  {
    hybrid_type= DECIMAL_RESULT;
    result_precision();
  }
  else
  {
    DBUG_ASSERT(r0 == INT_RESULT && r1 == INT_RESULT);
    decimals= 0;
    hybrid_type=INT_RESULT;
    result_precision();
  }
  DBUG_PRINT("info", ("Type: %s",
             (hybrid_type == REAL_RESULT ? "REAL_RESULT" :
              hybrid_type == DECIMAL_RESULT ? "DECIMAL_RESULT" :
              hybrid_type == INT_RESULT ? "INT_RESULT" :
              "--ILLEGAL!!!--")));
  DBUG_VOID_RETURN;
}


/**
  Set result type for a numeric function of one argument
  (can be also used by a numeric function of many arguments, if the result
  type depends only on the first argument)
*/

void Item_func_num1::find_num_type()
{
  DBUG_ENTER("Item_func_num1::find_num_type");
  DBUG_PRINT("info", ("name %s", func_name()));
  switch (hybrid_type= args[0]->result_type()) {
  case INT_RESULT:
    unsigned_flag= args[0]->unsigned_flag;
    break;
  case STRING_RESULT:
  case REAL_RESULT:
    hybrid_type= REAL_RESULT;
    max_length= float_length(decimals);
    break;
  case DECIMAL_RESULT:
    unsigned_flag= args[0]->unsigned_flag;
    break;
  default:
    DBUG_ASSERT(0);
  }
  DBUG_PRINT("info", ("Type: %s",
                      (hybrid_type == REAL_RESULT ? "REAL_RESULT" :
                       hybrid_type == DECIMAL_RESULT ? "DECIMAL_RESULT" :
                       hybrid_type == INT_RESULT ? "INT_RESULT" :
                       "--ILLEGAL!!!--")));
  DBUG_VOID_RETURN;
}


void Item_func_num1::fix_num_length_and_dec()
{
  decimals= args[0]->decimals;
  max_length= args[0]->max_length;
}

/*
  Reject geometry arguments, should be called in resolve_type() for
  SQL functions/operators where geometries are not suitable as operands.
 */
bool reject_geometry_args(uint arg_count, Item **args, Item_result_field *me)
{
  /*
    We want to make sure the operands are not GEOMETRY strings because
    it's meaningless for them to participate in arithmetic and/or numerical
    calculations.

    When a variable holds a MySQL Geometry byte string, it is regarded as a
    string rather than a MYSQL_TYPE_GEOMETRY, so here we can't catch an illegal
    variable argument which was assigned with a geometry.

    Item::field_type() requires the item not be of ROW_RESULT, since a row
    isn't a field.
  */
  for (uint i= 0; i < arg_count; i++)
  {
    if (args[i]->result_type() != ROW_RESULT &&
        args[i]->field_type() == MYSQL_TYPE_GEOMETRY)
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), me->func_name());
      return true;
    }
  }

  return false;
}


/**
  Go through the arguments of a function and check if any of them are
  JSON. If a JSON argument is found, raise a warning saying that this
  operation is not supported yet. This function is used to notify
  users that they are comparing JSON values using a mechanism that has
  not yet been updated to use the JSON comparator. JSON values are
  typically handled as strings in that case.

  @param arg_count  the number of arguments
  @param args       the arguments to go through looking for JSON values
  @param msg        the message that explains what is not supported
*/
void unsupported_json_comparison(size_t arg_count, Item **args, const char *msg)
{
  for (size_t i= 0; i < arg_count; ++i)
  {
    if (args[i]->result_type() == STRING_RESULT &&
        args[i]->field_type() == MYSQL_TYPE_JSON)
    {
      push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                          ER_NOT_SUPPORTED_YET,
                          ER_THD(current_thd, ER_NOT_SUPPORTED_YET),
                          msg);
      break;
    }
  }
}


void handle_std_exception(const char *funcname)
{
  try
  {
    throw;
  }
  catch (const std::bad_alloc &e)
  {
    my_error(ER_STD_BAD_ALLOC_ERROR, MYF(0), e.what(), funcname);
  }
  catch (const std::domain_error &e)
  {
    my_error(ER_STD_DOMAIN_ERROR, MYF(0), e.what(), funcname);
  }
  catch (const std::length_error &e)
  {
    my_error(ER_STD_LENGTH_ERROR, MYF(0), e.what(), funcname);
  }
  catch (const std::invalid_argument &e)
  {
    my_error(ER_STD_INVALID_ARGUMENT, MYF(0), e.what(), funcname);
  }
  catch (const std::out_of_range &e)
  {
    my_error(ER_STD_OUT_OF_RANGE_ERROR, MYF(0), e.what(), funcname);
  }
  catch (const std::overflow_error &e)
  {
    my_error(ER_STD_OVERFLOW_ERROR, MYF(0), e.what(), funcname);
  }
  catch (const std::range_error &e)
  {
    my_error(ER_STD_RANGE_ERROR, MYF(0), e.what(), funcname);
  }
  catch (const std::underflow_error &e)
  {
    my_error(ER_STD_UNDERFLOW_ERROR, MYF(0), e.what(), funcname);
  }
  catch (const std::logic_error &e)
  {
    my_error(ER_STD_LOGIC_ERROR, MYF(0), e.what(), funcname);
  }
  catch (const std::runtime_error &e)
  {
    my_error(ER_STD_RUNTIME_ERROR, MYF(0), e.what(), funcname);
  }
  catch (const std::exception &e)
  {
    my_error(ER_STD_UNKNOWN_EXCEPTION, MYF(0), e.what(), funcname);
  }
  catch (...)
  {
    my_error(ER_UNKNOWN_ERROR, MYF(0));
  }
}


bool Item_func_numhybrid::resolve_type(THD *thd)
{
  fix_num_length_and_dec();
  find_num_type();
  return reject_geometry_args(arg_count, args, this);
}

String *Item_func_numhybrid::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  switch (hybrid_type) {
  case DECIMAL_RESULT:
  {
    my_decimal decimal_value, *val;
    if (!(val= decimal_op(&decimal_value)))
      return 0;                                 // null is set
    my_decimal_round(E_DEC_FATAL_ERROR, val, decimals, FALSE, val);
    str->set_charset(collation.collation);
    my_decimal2string(E_DEC_FATAL_ERROR, val, 0, 0, 0, str);
    break;
  }
  case INT_RESULT:
  {
    longlong nr= int_op();
    if (null_value)
      return 0; /* purecov: inspected */
    str->set_int(nr, unsigned_flag, collation.collation);
    break;
  }
  case REAL_RESULT:
  {
    double nr= real_op();
    if (null_value)
      return 0; /* purecov: inspected */
    str->set_real(nr, decimals, collation.collation);
    break;
  }
  case STRING_RESULT:
    switch (field_type()) {
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      return val_string_from_datetime(str);
    case MYSQL_TYPE_DATE:
      return val_string_from_date(str);
    case MYSQL_TYPE_TIME:
      return val_string_from_time(str);
    default:
      break;
    }
    return str_op(&str_value);
  default:
    DBUG_ASSERT(0);
  }
  return str;
}


double Item_func_numhybrid::val_real()
{
  DBUG_ASSERT(fixed == 1);
  switch (hybrid_type) {
  case DECIMAL_RESULT:
  {
    my_decimal decimal_value, *val;
    double result;
    if (!(val= decimal_op(&decimal_value)))
      return 0.0;                               // null is set
    my_decimal2double(E_DEC_FATAL_ERROR, val, &result);
    return result;
  }
  case INT_RESULT:
  {
    longlong result= int_op();
    return unsigned_flag ? (double) ((ulonglong) result) : (double) result;
  }
  case REAL_RESULT:
    return real_op();
  case STRING_RESULT:
  {
    switch (field_type())
    {
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      return val_real_from_decimal();
    default:
      break;
    }
    char *end_not_used;
    int err_not_used;
    String *res= str_op(&str_value);
    return (res ? my_strntod(res->charset(), (char*) res->ptr(), res->length(),
			     &end_not_used, &err_not_used) : 0.0);
  }
  default:
    DBUG_ASSERT(0);
  }
  return 0.0;
}


longlong Item_func_numhybrid::val_int()
{
  DBUG_ASSERT(fixed == 1);
  switch (hybrid_type) {
  case DECIMAL_RESULT:
  {
    my_decimal decimal_value, *val;
    if (!(val= decimal_op(&decimal_value)))
      return 0;                                 // null is set
    longlong result;
    my_decimal2int(E_DEC_FATAL_ERROR, val, unsigned_flag, &result);
    return result;
  }
  case INT_RESULT:
    return int_op();
  case REAL_RESULT:
    return (longlong) rint(real_op());
  case STRING_RESULT:
  {
    switch (field_type())
    {
    case MYSQL_TYPE_DATE:
      return val_int_from_date();
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      return val_int_from_datetime();
    case MYSQL_TYPE_TIME:
      return val_int_from_time();
    default:
      break;
    }
    int err_not_used;
    String *res;
    if (!(res= str_op(&str_value)))
      return 0;

    char *end= (char*) res->ptr() + res->length();
    const CHARSET_INFO *cs= res->charset();
    return (*(cs->cset->strtoll10))(cs, res->ptr(), &end, &err_not_used);
  }
  default:
    DBUG_ASSERT(0);
  }
  return 0;
}


my_decimal *Item_func_numhybrid::val_decimal(my_decimal *decimal_value)
{
  my_decimal *val= decimal_value;
  DBUG_ASSERT(fixed == 1);
  switch (hybrid_type) {
  case DECIMAL_RESULT:
    val= decimal_op(decimal_value);
    break;
  case INT_RESULT:
  {
    longlong result= int_op();
    int2my_decimal(E_DEC_FATAL_ERROR, result, unsigned_flag, decimal_value);
    break;
  }
  case REAL_RESULT:
  {
    double result= real_op();
    double2my_decimal(E_DEC_FATAL_ERROR, result, decimal_value);
    break;
  }
  case STRING_RESULT:
  {
    switch (field_type())
    {
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      return val_decimal_from_date(decimal_value);
    case MYSQL_TYPE_TIME:
      return val_decimal_from_time(decimal_value);
    default:
      break;
    }
    String *res;
    if (!(res= str_op(&str_value)))
      return NULL;

    str2my_decimal(E_DEC_FATAL_ERROR, (char*) res->ptr(),
                   res->length(), res->charset(), decimal_value);
    break;
  }  
  case ROW_RESULT:
  default:
    DBUG_ASSERT(0);
  }
  return val;
}


bool Item_func_numhybrid::get_date(MYSQL_TIME *ltime, my_time_flags_t fuzzydate)
{
  DBUG_ASSERT(fixed == 1);
  switch (field_type())
  {
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
    return date_op(ltime, fuzzydate);
  case MYSQL_TYPE_TIME:
    return get_date_from_time(ltime);
  default:
    return Item::get_date_from_non_temporal(ltime, fuzzydate);
  }
}


bool Item_func_numhybrid::get_time(MYSQL_TIME *ltime)
{
  DBUG_ASSERT(fixed == 1);
  switch (field_type())
  {
  case MYSQL_TYPE_TIME:
    return time_op(ltime);
  case MYSQL_TYPE_DATE:
    return get_time_from_date(ltime);
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
    return get_time_from_datetime(ltime);
  default:
    return Item::get_time_from_non_temporal(ltime);
  }
}


void Item_func_signed::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as signed)"));

}


bool Item_func_signed::resolve_type(THD *thd)
{
  fix_char_length(std::min<uint32>(args[0]->max_char_length(),
                                   MY_INT64_NUM_DECIMAL_DIGITS));
  return reject_geometry_args(arg_count, args, this);
}


longlong Item_func_signed::val_int_from_str(int *error)
{
  char buff[MAX_FIELD_WIDTH], *end, *start;
  size_t length;
  String tmp(buff,sizeof(buff), &my_charset_bin), *res;
  longlong value;
  const CHARSET_INFO *cs;

  /*
    For a string result, we must first get the string and then convert it
    to a longlong
  */

  if (!(res= args[0]->val_str(&tmp)))
  {
    null_value= 1;
    *error= 0;
    return 0;
  }
  null_value= 0;
  start= (char *)res->ptr();
  length= res->length();
  cs= res->charset();

  end= start + length;
  value= cs->cset->strtoll10(cs, start, &end, error);
  if (*error > 0 || end != start+ length)
  {
    ErrConvString err(res);
    push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                        ER_TRUNCATED_WRONG_VALUE,
                        ER_THD(current_thd, ER_TRUNCATED_WRONG_VALUE),
                        "INTEGER",
                        err.ptr());
  }
  return value;
}


longlong Item_func_signed::val_int()
{
  longlong value;
  int error;

  if (args[0]->cast_to_int_type() != STRING_RESULT ||
      args[0]->is_temporal())
  {
    value= args[0]->val_int();
    null_value= args[0]->null_value; 
    return value;
  }

  value= val_int_from_str(&error);
  if (value < 0 && error == 0)
  {
    push_warning(current_thd, Sql_condition::SL_WARNING, ER_UNKNOWN_ERROR,
                 "Cast to signed converted positive out-of-range integer to "
                 "it's negative complement");
  }
  return value;
}


void Item_func_unsigned::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as unsigned)"));

}


longlong Item_func_unsigned::val_int()
{
  longlong value;
  int error;

  if (args[0]->cast_to_int_type() == DECIMAL_RESULT)
  {
    my_decimal tmp, *dec= args[0]->val_decimal(&tmp);
    if (!(null_value= args[0]->null_value))
      my_decimal2int(E_DEC_FATAL_ERROR, dec, 1, &value);
    else
      value= 0;
    return value;
  }
  else if (args[0]->cast_to_int_type() != STRING_RESULT ||
           args[0]->is_temporal())
  {
    value= args[0]->val_int();
    null_value= args[0]->null_value; 
    return value;
  }

  value= val_int_from_str(&error);
  if (error < 0)
    push_warning(current_thd, Sql_condition::SL_WARNING, ER_UNKNOWN_ERROR,
                 "Cast to unsigned converted negative integer to it's "
                 "positive complement");
  return value;
}


String *Item_decimal_typecast::val_str(String *str)
{
  my_decimal tmp_buf, *tmp= val_decimal(&tmp_buf);
  if (null_value)
    return NULL;
  my_decimal2string(E_DEC_FATAL_ERROR, tmp, 0, 0, 0, str);
  return str;
}


double Item_decimal_typecast::val_real()
{
  my_decimal tmp_buf, *tmp= val_decimal(&tmp_buf);
  double res;
  if (null_value)
    return 0.0;
  my_decimal2double(E_DEC_FATAL_ERROR, tmp, &res);
  return res;
}


longlong Item_decimal_typecast::val_int()
{
  my_decimal tmp_buf, *tmp= val_decimal(&tmp_buf);
  longlong res;
  if (null_value)
    return 0;
  my_decimal2int(E_DEC_FATAL_ERROR, tmp, unsigned_flag, &res);
  return res;
}


my_decimal *Item_decimal_typecast::val_decimal(my_decimal *dec)
{
  my_decimal tmp_buf, *tmp= args[0]->val_decimal(&tmp_buf);
  bool sign;
  uint precision;

  if ((null_value= args[0]->null_value))
    return NULL;
  my_decimal_round(E_DEC_FATAL_ERROR, tmp, decimals, FALSE, dec);
  sign= dec->sign();
  if (unsigned_flag)
  {
    if (sign)
    {
      my_decimal_set_zero(dec);
      goto err;
    }
  }
  precision= my_decimal_length_to_precision(max_length,
                                            decimals, unsigned_flag);
  if (precision - decimals < (uint) my_decimal_intg(dec))
  {
    max_my_decimal(dec, precision, decimals);
    dec->sign(sign);
    goto err;
  }
  return dec;

err:
  push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                      ER_WARN_DATA_OUT_OF_RANGE,
                      ER_THD(current_thd, ER_WARN_DATA_OUT_OF_RANGE),
                      item_name.ptr(), 1L);
  return dec;
}


void Item_decimal_typecast::print(String *str, enum_query_type query_type)
{
  char len_buf[20*3 + 1];
  char *end;

  uint precision= my_decimal_length_to_precision(max_length, decimals,
                                                 unsigned_flag);
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as decimal("));

  end=int10_to_str(precision, len_buf,10);
  str->append(len_buf, (uint32) (end - len_buf));

  str->append(',');

  end=int10_to_str(decimals, len_buf,10);
  str->append(len_buf, (uint32) (end - len_buf));

  str->append(')');
  str->append(')');
}


double Item_func_plus::real_op()
{
  double value= args[0]->val_real() + args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return check_float_overflow(value);
}


longlong Item_func_plus::int_op()
{
  longlong val0= args[0]->val_int();
  longlong val1= args[1]->val_int();
  longlong res= val0 + val1;
  bool     res_unsigned= FALSE;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;

  /*
    First check whether the result can be represented as a
    (bool unsigned_flag, longlong value) pair, then check if it is compatible
    with this Item's unsigned_flag by calling check_integer_overflow().
  */
  if (args[0]->unsigned_flag)
  {
    if (args[1]->unsigned_flag || val1 >= 0)
    {
      if (test_if_sum_overflows_ull((ulonglong) val0, (ulonglong) val1))
        goto err;
      res_unsigned= TRUE;
    }
    else
    {
      /* val1 is negative */
      if ((ulonglong) val0 > (ulonglong) LLONG_MAX)
        res_unsigned= TRUE;
    }
  }
  else
  {
    if (args[1]->unsigned_flag)
    {
      if (val0 >= 0)
      {
        if (test_if_sum_overflows_ull((ulonglong) val0, (ulonglong) val1))
          goto err;
        res_unsigned= TRUE;
      }
      else
      {
        if ((ulonglong) val1 > (ulonglong) LLONG_MAX)
          res_unsigned= TRUE;
      }
    }
    else
    {
      if (val0 >=0 && val1 >= 0)
        res_unsigned= TRUE;
      else if (val0 < 0 && val1 < 0 && res >= 0)
        goto err;
    }
  }
  return check_integer_overflow(res, res_unsigned);

err:
  return raise_integer_overflow();
}


/**
  Calculate plus of two decimals.

  @param decimal_value	Buffer that can be used to store result

  @return Value of operation as a decimal
  @retval
    0  Value was NULL;  In this case null_value is set
*/

my_decimal *Item_func_plus::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;
  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if (!(null_value= (args[1]->null_value ||
                     check_decimal_overflow(my_decimal_add(E_DEC_FATAL_ERROR &
                                                           ~E_DEC_OVERFLOW,
                                                           decimal_value,
                                                           val1, val2)) > 3)))
    return decimal_value;
  return 0;
}

/**
  Set precision of results for additive operations (+ and -)
*/
void Item_func_additive_op::result_precision()
{
  decimals= max(args[0]->decimals, args[1]->decimals);
  int arg1_int= args[0]->decimal_precision() - args[0]->decimals;
  int arg2_int= args[1]->decimal_precision() - args[1]->decimals;
  int precision= max(arg1_int, arg2_int) + 1 + decimals;

  /* Integer operations keep unsigned_flag if one of arguments is unsigned */
  if (result_type() == INT_RESULT)
    unsigned_flag= args[0]->unsigned_flag | args[1]->unsigned_flag;
  else
    unsigned_flag= args[0]->unsigned_flag & args[1]->unsigned_flag;
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


/**
  The following function is here to allow the user to force
  subtraction of UNSIGNED BIGINT/DECIMAL to return negative values.
*/

bool Item_func_minus::resolve_type(THD *thd)
{
  if (Item_num_op::resolve_type(thd))
    return true;
  if (unsigned_flag && (thd->variables.sql_mode & MODE_NO_UNSIGNED_SUBTRACTION))
    unsigned_flag=0;
  return false;
}


double Item_func_minus::real_op()
{
  double value= args[0]->val_real() - args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return check_float_overflow(value);
}


longlong Item_func_minus::int_op()
{
  longlong val0= args[0]->val_int();
  longlong val1= args[1]->val_int();
  longlong res= val0 - val1;
  bool     res_unsigned= FALSE;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;

  /*
    First check whether the result can be represented as a
    (bool unsigned_flag, longlong value) pair, then check if it is compatible
    with this Item's unsigned_flag by calling check_integer_overflow().
  */
  if (args[0]->unsigned_flag)
  {
    if (args[1]->unsigned_flag)
    {
      if ((ulonglong) val0 < (ulonglong) val1)
      {
        if (res >= 0)
          goto err;
      }
      else
        res_unsigned= TRUE;
    }
    else
    {
      if (val1 >= 0)
      {
        if ((ulonglong) val0 > (ulonglong) val1)
          res_unsigned= TRUE;
      }
      else
      {
        if (test_if_sum_overflows_ull((ulonglong) val0, (ulonglong) -val1))
          goto err;
        res_unsigned= TRUE;
      }
    }
  }
  else
  {
    if (args[1]->unsigned_flag)
    {
      if ((ulonglong) (val0 - LLONG_MIN) < (ulonglong) val1)
        goto err;
    }
    else
    {
      if (val0 > 0 && val1 < 0)
        res_unsigned= TRUE;
      else if (val0 < 0 && val1 > 0 && res >= 0)
        goto err;
    }
  }
  return check_integer_overflow(res, res_unsigned);

err:
  return raise_integer_overflow();
}


/**
  See Item_func_plus::decimal_op for comments.
*/

my_decimal *Item_func_minus::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;

  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return NULL;

  val2= args[1]->val_decimal(&value2);
  if ((null_value= args[1]->null_value))
    return NULL;

  if ((null_value=
       check_decimal_overflow(my_decimal_sub(E_DEC_FATAL_ERROR &
                                             ~E_DEC_OVERFLOW,
                                             decimal_value, val1, val2)) > 3))
  {
    /*
      Do not return a NULL pointer, as the result may be used in subsequent
      arithmetic operations.
     */
    my_decimal_set_zero(decimal_value);
    return decimal_value;
  }
  /*
   Allow sign mismatch only if sql_mode includes MODE_NO_UNSIGNED_SUBTRACTION
   See Item_func_minus::resolve_type().
  */
  if (unsigned_flag && decimal_value->sign())
  {
    /*
      Do not return a NULL pointer, as the result may be used in subsequent
      arithmetic operations.
     */
    my_decimal_set_zero(decimal_value);
    null_value= maybe_null;
    raise_decimal_overflow();
    return decimal_value;
  }
  return decimal_value;
}


double Item_func_mul::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real() * args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return check_float_overflow(value);
}


longlong Item_func_mul::int_op()
{
  DBUG_ASSERT(fixed == 1);
  longlong a= args[0]->val_int();
  longlong b= args[1]->val_int();
  longlong res;
  ulonglong res0, res1;
  ulong a0, a1, b0, b1;
  bool     res_unsigned= FALSE;
  bool     a_negative= FALSE, b_negative= FALSE;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;

  /*
    First check whether the result can be represented as a
    (bool unsigned_flag, longlong value) pair, then check if it is compatible
    with this Item's unsigned_flag by calling check_integer_overflow().

    Let a = a1 * 2^32 + a0 and b = b1 * 2^32 + b0. Then
    a * b = (a1 * 2^32 + a0) * (b1 * 2^32 + b0) = a1 * b1 * 2^64 +
            + (a1 * b0 + a0 * b1) * 2^32 + a0 * b0;
    We can determine if the above sum overflows the ulonglong range by
    sequentially checking the following conditions:
    1. If both a1 and b1 are non-zero.
    2. Otherwise, if (a1 * b0 + a0 * b1) is greater than ULONG_MAX.
    3. Otherwise, if (a1 * b0 + a0 * b1) * 2^32 + a0 * b0 is greater than
    ULLONG_MAX.

    Since we also have to take the unsigned_flag for a and b into account,
    it is easier to first work with absolute values and set the
    correct sign later.
  */
  if (!args[0]->unsigned_flag && a < 0)
  {
    a_negative= TRUE;
    a= -a;
  }
  if (!args[1]->unsigned_flag && b < 0)
  {
    b_negative= TRUE;
    b= -b;
  }

  a0= 0xFFFFFFFFUL & a;
  a1= ((ulonglong) a) >> 32;
  b0= 0xFFFFFFFFUL & b;
  b1= ((ulonglong) b) >> 32;

  if (a1 && b1)
    goto err;

  res1= (ulonglong) a1 * b0 + (ulonglong) a0 * b1;
  if (res1 > 0xFFFFFFFFUL)
    goto err;

  res1= res1 << 32;
  res0= (ulonglong) a0 * b0;

  if (test_if_sum_overflows_ull(res1, res0))
    goto err;
  res= res1 + res0;

  if (a_negative != b_negative)
  {
    if ((ulonglong) res > (ulonglong) LLONG_MIN + 1)
      goto err;
    res= -res;
  }
  else
    res_unsigned= TRUE;

  return check_integer_overflow(res, res_unsigned);

err:
  return raise_integer_overflow();
}


/** See Item_func_plus::decimal_op for comments. */

my_decimal *Item_func_mul::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;
  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if (!(null_value= (args[1]->null_value ||
                     (check_decimal_overflow(my_decimal_mul(E_DEC_FATAL_ERROR &
                                                            ~E_DEC_OVERFLOW,
                                                            decimal_value, val1,
                                                            val2)) > 3))))
    return decimal_value;
  return 0;
}


void Item_func_mul::result_precision()
{
  /* Integer operations keep unsigned_flag if one of arguments is unsigned */
  if (result_type() == INT_RESULT)
    unsigned_flag= args[0]->unsigned_flag | args[1]->unsigned_flag;
  else
    unsigned_flag= args[0]->unsigned_flag & args[1]->unsigned_flag;
  decimals= min(args[0]->decimals + args[1]->decimals, DECIMAL_MAX_SCALE);
  uint est_prec = args[0]->decimal_precision() + args[1]->decimal_precision();
  uint precision= min<uint>(est_prec, DECIMAL_MAX_PRECISION);
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


double Item_func_div::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  double val2= args[1]->val_real();
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0.0;
  if (val2 == 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return check_float_overflow(value/val2);
}


my_decimal *Item_func_div::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;
  int err;

  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if ((null_value= args[1]->null_value))
    return 0;
  if ((err= check_decimal_overflow(my_decimal_div(E_DEC_FATAL_ERROR &
                                                  ~E_DEC_OVERFLOW &
                                                  ~E_DEC_DIV_ZERO,
                                                  decimal_value,
                                                  val1, val2,
                                                  prec_increment))) > 3)
  {
    if (err == E_DEC_DIV_ZERO)
      signal_divide_by_null();
    null_value= 1;
    return 0;
  }
  return decimal_value;
}


void Item_func_div::result_precision()
{
  uint precision= min<uint>(args[0]->decimal_precision() +
                            args[1]->decimals + prec_increment,
                            DECIMAL_MAX_PRECISION);

  if (result_type() == DECIMAL_RESULT)
    DBUG_ASSERT(precision > 0);

  /* Integer operations keep unsigned_flag if one of arguments is unsigned */
  if (result_type() == INT_RESULT)
    unsigned_flag= args[0]->unsigned_flag | args[1]->unsigned_flag;
  else
    unsigned_flag= args[0]->unsigned_flag & args[1]->unsigned_flag;
  decimals= min<uint>(args[0]->decimals + prec_increment, DECIMAL_MAX_SCALE);
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


bool Item_func_div::resolve_type(THD *thd)
{
  DBUG_ENTER("Item_func_div::resolve_type");
  prec_increment= thd->variables.div_precincrement;
  if (Item_num_op::resolve_type(thd))
    DBUG_RETURN(true);

  switch(hybrid_type) {
  case REAL_RESULT:
  {
    decimals=max(args[0]->decimals,args[1]->decimals)+prec_increment;
    set_if_smaller(decimals, NOT_FIXED_DEC);
    uint tmp=float_length(decimals);
    if (decimals == NOT_FIXED_DEC)
      max_length= tmp;
    else
    {
      max_length=args[0]->max_length - args[0]->decimals + decimals;
      set_if_smaller(max_length,tmp);
    }
    break;
  }
  case INT_RESULT:
    hybrid_type= DECIMAL_RESULT;
    DBUG_PRINT("info", ("Type changed: DECIMAL_RESULT"));
    result_precision();
    break;
  case DECIMAL_RESULT:
    result_precision();
    break;
  default:
    DBUG_ASSERT(0);
  }
  maybe_null= true; // division by zero
  DBUG_RETURN(false);
}


/* Integer division */
longlong Item_func_int_div::val_int()
{
  DBUG_ASSERT(fixed == 1);

  /*
    Perform division using DECIMAL math if either of the operands has a
    non-integer type
  */
  if (args[0]->result_type() != INT_RESULT ||
      args[1]->result_type() != INT_RESULT)
  {
    my_decimal tmp;
    my_decimal *val0p= args[0]->val_decimal(&tmp);
    if ((null_value= args[0]->null_value))
      return 0;
    my_decimal val0= *val0p;

    my_decimal *val1p= args[1]->val_decimal(&tmp);
    if ((null_value= args[1]->null_value))
      return 0;
    my_decimal val1= *val1p;

    int err;
    if ((err= my_decimal_div(E_DEC_FATAL_ERROR & ~E_DEC_DIV_ZERO, &tmp,
                             &val0, &val1, 0)) > 3)
    {
      if (err == E_DEC_DIV_ZERO)
        signal_divide_by_null();
      return 0;
    }

    my_decimal truncated;
    const bool do_truncate= true;
    if (my_decimal_round(E_DEC_FATAL_ERROR, &tmp, 0, do_truncate, &truncated))
      DBUG_ASSERT(false);

    longlong res;
    if (my_decimal2int(E_DEC_FATAL_ERROR, &truncated, unsigned_flag, &res) &
        E_DEC_OVERFLOW)
      raise_integer_overflow();
    return res;
  }
  
  longlong val0=args[0]->val_int();
  longlong val1=args[1]->val_int();
  bool val0_negative, val1_negative, res_negative;
  ulonglong uval0, uval1, res;
  if ((null_value= (args[0]->null_value || args[1]->null_value)))
    return 0;
  if (val1 == 0)
  {
    signal_divide_by_null();
    return 0;
  }

  val0_negative= !args[0]->unsigned_flag && val0 < 0;
  val1_negative= !args[1]->unsigned_flag && val1 < 0;
  res_negative= val0_negative != val1_negative;
  uval0= (ulonglong) (val0_negative && val0 != LLONG_MIN ? -val0 : val0);
  uval1= (ulonglong) (val1_negative && val1 != LLONG_MIN ? -val1 : val1);
  res= uval0 / uval1;
  if (res_negative)
  {
    if (res > (ulonglong) LLONG_MAX)
      return raise_integer_overflow();
    res= (ulonglong) (-(longlong) res);
  }
  return check_integer_overflow(res, !res_negative);
}


bool Item_func_int_div::resolve_type(THD *thd)
{
  Item_result argtype= args[0]->result_type();
  /* use precision ony for the data type it is applicable for and valid */
  uint32 char_length= args[0]->max_char_length() -
                      (argtype == DECIMAL_RESULT || argtype == INT_RESULT ?
                       args[0]->decimals : 0);
  fix_char_length(char_length > MY_INT64_NUM_DECIMAL_DIGITS ?
                  MY_INT64_NUM_DECIMAL_DIGITS : char_length);
  maybe_null= true;
  unsigned_flag=args[0]->unsigned_flag | args[1]->unsigned_flag;
  return reject_geometry_args(arg_count, args, this);
}


longlong Item_func_mod::int_op()
{
  DBUG_ASSERT(fixed == 1);
  longlong val0= args[0]->val_int();
  longlong val1= args[1]->val_int();
  bool val0_negative, val1_negative;
  ulonglong uval0, uval1;
  ulonglong res;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0; /* purecov: inspected */
  if (val1 == 0)
  {
    signal_divide_by_null();
    return 0;
  }

  /*
    '%' is calculated by integer division internally. Since dividing
    LLONG_MIN by -1 generates SIGFPE, we calculate using unsigned values and
    then adjust the sign appropriately.
  */
  val0_negative= !args[0]->unsigned_flag && val0 < 0;
  val1_negative= !args[1]->unsigned_flag && val1 < 0;
  uval0= (ulonglong) (val0_negative && val0 != LLONG_MIN ? -val0 : val0);
  uval1= (ulonglong) (val1_negative && val1 != LLONG_MIN ? -val1 : val1);
  res= uval0 % uval1;
  return check_integer_overflow(val0_negative ? -(longlong) res : res,
                                !val0_negative);
}

double Item_func_mod::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  double val2=  args[1]->val_real();
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0.0; /* purecov: inspected */
  if (val2 == 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return fmod(value,val2);
}


my_decimal *Item_func_mod::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;

  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if ((null_value= args[1]->null_value))
    return 0;
  switch (my_decimal_mod(E_DEC_FATAL_ERROR & ~E_DEC_DIV_ZERO, decimal_value,
                         val1, val2)) {
  case E_DEC_TRUNCATED:
  case E_DEC_OK:
    return decimal_value;
  case E_DEC_DIV_ZERO:
    signal_divide_by_null();
  default:
    null_value= 1;
    return 0;
  }
}


void Item_func_mod::result_precision()
{
  decimals= max(args[0]->decimals, args[1]->decimals);
  max_length= max(args[0]->max_length, args[1]->max_length);
  // Increase max_length if we have: signed % unsigned(precision == scale)
  if (!args[0]->unsigned_flag && args[1]->unsigned_flag &&
      args[0]->max_length <= args[1]->max_length &&
      args[1]->decimals == args[1]->decimal_precision())
  {
    max_length+= 1;
  }
}


bool Item_func_mod::resolve_type(THD *thd)
{
  if (Item_num_op::resolve_type(thd))
    return true;
  maybe_null= true;
  unsigned_flag= args[0]->unsigned_flag;
  return false;
}


double Item_func_neg::real_op()
{
  double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return -value;
}


longlong Item_func_neg::int_op()
{
  longlong value= args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0;
  if (args[0]->unsigned_flag &&
      (ulonglong) value > (ulonglong) LLONG_MAX + 1ULL)
    return raise_integer_overflow();
  // For some platforms we need special handling of LLONG_MIN to
  // guarantee overflow.
  if (value == LLONG_MIN &&
      !args[0]->unsigned_flag &&
      !unsigned_flag)
    return raise_integer_overflow();
  // Avoid doing '-value' below, it is undefined.
  if (value == LLONG_MIN &&
      args[0]->unsigned_flag &&
      !unsigned_flag)
    return LLONG_MIN;
  return check_integer_overflow(-value, !args[0]->unsigned_flag && value < 0);
}


my_decimal *Item_func_neg::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= args[0]->null_value))
  {
    my_decimal2decimal(value, decimal_value);
    my_decimal_neg(decimal_value);
    return decimal_value;
  }
  return 0;
}


void Item_func_neg::fix_num_length_and_dec()
{
  decimals= args[0]->decimals;
  /* 1 add because sign can appear */
  max_length= args[0]->max_length + 1;
}


bool Item_func_neg::resolve_type(THD *thd)
{
  DBUG_ENTER("Item_func_neg::resolve_type");
  if (Item_func_num1::resolve_type(thd))
    DBUG_RETURN(true);
  /*
    If this is in integer context keep the context as integer if possible
    (This is how multiplication and other integer functions works)
    Use val() to get value as arg_type doesn't mean that item is
    Item_int or Item_real due to existence of Item_param.
  */
  if (hybrid_type == INT_RESULT && args[0]->const_item())
  {
    longlong val= args[0]->val_int();
    if ((ulonglong) val >= (ulonglong) LLONG_MIN &&
        ((ulonglong) val != (ulonglong) LLONG_MIN ||
          args[0]->type() != INT_ITEM))        
    {
      /*
        Ensure that result is converted to DECIMAL, as longlong can't hold
        the negated number
      */
      hybrid_type= DECIMAL_RESULT;
      DBUG_PRINT("info", ("Type changed: DECIMAL_RESULT"));
    }
  }
  unsigned_flag= false;
  DBUG_RETURN(false);
}


double Item_func_abs::real_op()
{
  double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return fabs(value);
}


longlong Item_func_abs::int_op()
{
  longlong value= args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0;
  if (unsigned_flag)
    return value;
  /* -LLONG_MIN = LLONG_MAX + 1 => outside of signed longlong range */
  if (value == LLONG_MIN)
    return raise_integer_overflow();
  return (value >= 0) ? value : -value;
}


my_decimal *Item_func_abs::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= args[0]->null_value))
  {
    my_decimal2decimal(value, decimal_value);
    if (decimal_value->sign())
      my_decimal_neg(decimal_value);
    return decimal_value;
  }
  return 0;
}


bool Item_func_abs::resolve_type(THD *thd)
{
  if (Item_func_num1::resolve_type(thd))
    return true;
  unsigned_flag= args[0]->unsigned_flag;
    return false;
}


bool Item_dec_func::resolve_type(THD *thd)
{
  decimals= NOT_FIXED_DEC;
  max_length= float_length(decimals);
  maybe_null= true;
  return reject_geometry_args(arg_count, args, this);
}


/** Gateway to natural LOG function. */
double Item_func_ln::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_invalid_argument_for_log();
    return 0.0;
  }
  return log(value);
}


/** 
  Extended but so slower LOG function.

  We have to check if all values are > zero and first one is not one
  as these are the cases then result is not a number.
*/ 
double Item_func_log::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_invalid_argument_for_log();
    return 0.0;
  }
  if (arg_count == 2)
  {
    double value2= args[1]->val_real();
    if ((null_value= args[1]->null_value))
      return 0.0;
    if (value2 <= 0.0 || value == 1.0)
    {
      signal_invalid_argument_for_log();
      return 0.0;
    }
    return log(value2) / log(value);
  }
  return log(value);
}

double Item_func_log2::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();

  if ((null_value=args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_invalid_argument_for_log();
    return 0.0;
  }
  return std::log2(value);
}

double Item_func_log10::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_invalid_argument_for_log();
    return 0.0;
  }
  return log10(value);
}

double Item_func_exp::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0; /* purecov: inspected */
  return check_float_overflow(exp(value));
}

double Item_func_sqrt::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || value < 0)))
    return 0.0; /* purecov: inspected */
  return sqrt(value);
}

double Item_func_pow::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  double val2= args[1]->val_real();
  if ((null_value=(args[0]->null_value || args[1]->null_value)))
    return 0.0; /* purecov: inspected */
  return check_float_overflow(pow(value,val2));
}

// Trigonometric functions

double Item_func_acos::val_real()
{
  DBUG_ASSERT(fixed == 1);
  /* One can use this to defer SELECT processing. */
  DEBUG_SYNC(current_thd, "before_acos_function");
  // the volatile's for BUG #2338 to calm optimizer down (because of gcc's bug)
  volatile double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || (value < -1.0 || value > 1.0))))
    return 0.0;
  return acos(value);
}

double Item_func_asin::val_real()
{
  DBUG_ASSERT(fixed == 1);
  // the volatile's for BUG #2338 to calm optimizer down (because of gcc's bug)
  volatile double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || (value < -1.0 || value > 1.0))))
    return 0.0;
  return asin(value);
}

double Item_func_atan::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  if (arg_count == 2)
  {
    double val2= args[1]->val_real();
    if ((null_value=args[1]->null_value))
      return 0.0;
    return check_float_overflow(atan2(value,val2));
  }
  return atan(value);
}

double Item_func_cos::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return cos(value);
}

double Item_func_sin::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return sin(value);
}

double Item_func_tan::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return check_float_overflow(tan(value));
}


double Item_func_cot::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return check_float_overflow(1.0 / tan(value));
}


// Bitwise functions

bool Item_func_bit::resolve_type(THD *thd)
{
  if (bit_func_returns_binary(args[0],
                              binary_result_requires_binary_second_arg() ?
                              args[1] : nullptr))
  {
    hybrid_type= STRING_RESULT;
    collation.set(&my_charset_bin);
    fix_char_length_ulonglong(
      max<ulonglong>(args[0]->max_length,
                     binary_result_requires_binary_second_arg() ?
                     args[1]->max_length : 0));
  }
  else
  {
    hybrid_type= INT_RESULT;
    decimals= 0;
    unsigned_flag= true;
    collation.set_numeric();
    fix_char_length(MAX_BIGINT_WIDTH + 1);
  }
  return reject_geometry_args(arg_count, args, this);
}


longlong Item_func_bit::val_int()
{
  DBUG_ASSERT(fixed);
  if (hybrid_type == INT_RESULT)
    return int_op();
  else
  {
    String *res;
    if (!(res= str_op(&str_value)))
      return 0;

    int ovf_error;
    char *from= const_cast<char *>(res->ptr());
    size_t len= res->length();
    char *end= from + len;
    return my_strtoll10(from, &end, &ovf_error);
  }
}


double Item_func_bit::val_real()
{
  DBUG_ASSERT(fixed);
  if (hybrid_type == INT_RESULT)
    return static_cast<ulonglong>(int_op());
  else
  {
    String *res;
    if (!(res= str_op(&str_value)))
      return 0.0;

    int ovf_error;
    char *from= const_cast<char *>(res->ptr());
    size_t len= res->length();
    char *end= from + len;
    return my_strtod(from, &end, &ovf_error);
  }
}


my_decimal *Item_func_bit::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed);
  if (hybrid_type == INT_RESULT)
    return val_decimal_from_int(decimal_value);
  else
    return val_decimal_from_string(decimal_value);
}


String *Item_func_bit::val_str(String *str)
{
  DBUG_ASSERT(fixed);
  if (hybrid_type == INT_RESULT)
  {
    longlong nr= int_op();
    if (null_value)
      return nullptr;
    str->set_int(nr, unsigned_flag, collation.collation);
    return str;
  }
  else
    return str_op(str);
}


// Shift-functions, same as << and >> in C/C++

/**
  Template function that evaluates the bitwise shift operation over integer
  arguments.
  @tparam to_left True if left-shift, false if right-shift
*/
template<bool to_left> longlong Item_func_shift::eval_int_op()
{
  DBUG_ASSERT(fixed);
  null_value= maybe_null;
  ulonglong res= args[0]->val_int();
  if (args[0]->null_value)
    return 0;

  uint shift= args[1]->val_int();
  if (args[1]->null_value)
    return 0;

  null_value= false;
  if (shift < sizeof(longlong) * 8)
    return to_left ? (res << shift) : (res >> shift);
  return 0;
}

/// Instantiations of the above
template longlong Item_func_shift::eval_int_op<true>();
template longlong Item_func_shift::eval_int_op<false>();


/**
  Template function that evaluates the bitwise shift operation over binary
  string arguments.
  @tparam to_left True if left-shift, false if right-shift
  @param str      String usable as scratch buffer
*/
template<bool to_left> String *Item_func_shift::eval_str_op(String *str)
{
  DBUG_ASSERT(fixed);
  null_value= maybe_null;

  String tmp_str;
  String *arg= args[0]->val_str(&tmp_str);
  if (!arg || tmp_value.alloc(arg->length()) || args[0]->null_value)
    return nullptr;

  ssize_t arg_length= arg->length();
  size_t shift= min(args[1]->val_uint(),
                    static_cast<ulonglong>(arg_length) * 8);
  if (args[1]->null_value)
    return nullptr;
  null_value= false;
  tmp_value.length(arg_length);
  tmp_value.set_charset(&my_charset_bin);
  /*
    Example with left-shift-by-21-bits:
    |........|........|........|........|
      byte i  byte i+1 byte i+2 byte i+3
    First (leftmost) bit has number 1.
    21 = 2*8 + 5.
    Bits of number 1-3 of byte 'i' receive bits 22-24 i.e. the last 3 bits of
    byte 'i+2'. So, take byte 'i+2', shift it left by 5 bits, that puts the
    last 3 bits of byte 'i+2' in bits 1-3, and 0s elsewhere.
    Bits of number 4-8 of byte 'i' receive bits 25-39 i.e. the first 5 bits of
    byte 'i+3'. So, take byte 'i+3', shift it right by 3 bits, that puts the
    first 5 bits of byte 'i+3' in bits 4-8, and 0s elsewhere.
    In total, do OR of both results.
  */
  size_t mod= shift % 8;
  size_t mod_complement= 8 - mod;
  ssize_t entire_bytes= shift / 8;

  const unsigned char *from_c= pointer_cast<const unsigned char *>(arg->ptr());
  unsigned char *to_c= pointer_cast<unsigned char *>(tmp_value.c_ptr_quick());

  if (to_left)
  {
    // Bytes of lower index are overwritten by bytes of higher index
    for (ssize_t i= 0; i < arg_length; i++)
      if (i + entire_bytes + 1 < arg_length)
        to_c[i]= (from_c[i + entire_bytes] << mod) |
          (from_c[i + entire_bytes + 1] >> mod_complement);
      else if (i + entire_bytes + 1 == arg_length)
        to_c[i]= from_c[i + entire_bytes] << mod;
      else
        to_c[i]= 0;
  }
  else
  {
    // Bytes of higher index are overwritten by bytes of lower index
    for (ssize_t i= arg_length - 1; i >= 0; i--)
      if (i > entire_bytes)
        to_c[i]= (from_c[i - entire_bytes] >> mod) |
          (from_c[i - entire_bytes - 1] << mod_complement);
      else if (i == entire_bytes)
        to_c[i]= from_c[i - entire_bytes] >> mod;
      else
        to_c[i]= 0;
  }
  return &tmp_value;
}


/// Instantiations of the above
template String *Item_func_shift::eval_str_op<true>(String *);
template String *Item_func_shift::eval_str_op<false>(String *);


// Bit negation ('~')

longlong Item_func_bit_neg::int_op()
{
  DBUG_ASSERT(fixed == 1);
  ulonglong res= (ulonglong) args[0]->val_int();
  if ((null_value=args[0]->null_value))
    return 0;
  return ~res;
}


String *Item_func_bit_neg::str_op(String *str)
{
  DBUG_ASSERT(fixed);
  null_value= maybe_null;
  String *res= args[0]->val_str(str);
  if (!res || args[0]->null_value || tmp_value.alloc(res->length()))
    return nullptr;

  size_t arg_length= res->length();
  tmp_value.length(arg_length);
  tmp_value.set_charset(&my_charset_bin);
  const unsigned char *from_c= pointer_cast<const unsigned char *>(res->ptr());
  unsigned char *to_c= pointer_cast<unsigned char *>(tmp_value.c_ptr_quick());
  size_t i= 0;
  while (i + sizeof(longlong) <= arg_length)
  {
    int8store(&to_c[i], ~(uint8korr(&from_c[i])));
    i+= sizeof(longlong);
  }
  while (i < arg_length)
  {
    to_c[i]= ~from_c[i];
    i++;
  }

  null_value= false;
  return &tmp_value;
}


/**
  Template function used to evaluate the bitwise operation over int arguments.

  @param int_func  The bitwise function.
*/
template<class Int_func> longlong
Item_func_bit_two_param::eval_int_op(Int_func int_func)
{
  DBUG_ASSERT(fixed);
  null_value= maybe_null;
  ulonglong arg0= args[0]->val_uint();
  if (args[0]->null_value)
    return 0;
  ulonglong arg1= args[1]->val_uint();
  if (args[1]->null_value)
    return 0;
  null_value= false;
  return (longlong) int_func(arg0, arg1);
}

/// Instantiations of the above
template longlong Item_func_bit_two_param::eval_int_op
<std::bit_or<ulonglong> >(std::bit_or<ulonglong>);
template longlong Item_func_bit_two_param::eval_int_op
<std::bit_and<ulonglong> >(std::bit_and<ulonglong>);
template longlong Item_func_bit_two_param::eval_int_op
<std::bit_xor<ulonglong> >(std::bit_xor<ulonglong>);


/**
  Template function that evaluates the bitwise operation over binary arguments.
  Checks that both arguments have same length and applies the bitwise operation

   @param str        Buffer
   @param char_func  The Bitwise function used to evaluate unsigned chars.
   @param int_func   The Bitwise function used to evaluate unsigned long longs.
*/
template<class Char_func, class Int_func> String *
Item_func_bit_two_param::eval_str_op(String *str, Char_func char_func,
                                     Int_func int_func)
{
  DBUG_ASSERT(fixed);
  null_value= maybe_null;

  String arg0_buff;
  String *s1= args[0]->val_str(&arg0_buff);

  if (!s1)
    return nullptr;

  String arg1_buff;
  String *s2= args[1]->val_str(&arg1_buff);

  if (!s2)
    return nullptr;

  size_t arg_length= s1->length();
  if (arg_length != s2->length())
  {
    my_error(ER_INVALID_BITWISE_OPERANDS_SIZE, MYF(0), func_name());
    return nullptr;
  }

  if(tmp_value.alloc(arg_length))
    return nullptr;

  null_value= false;
  tmp_value.length(arg_length);
  tmp_value.set_charset(&my_charset_bin);

  const uchar *s1_c_p= pointer_cast<const uchar *>(s1->ptr());
  const uchar *s2_c_p= pointer_cast<const uchar *>(s2->ptr());
  char *res= const_cast<char *>(tmp_value.ptr());
  size_t i= 0;
  while (i + sizeof(longlong) <= arg_length)
  {
    int8store(&res[i], int_func(uint8korr(&s1_c_p[i]), uint8korr(&s2_c_p[i])));
    i+= sizeof(longlong);
  }
  while (i < arg_length)
  {
    res[i]= char_func(s1_c_p[i], s2_c_p[i]);
    i++;
  }

  return &tmp_value;
}


/// Instantiations of the above
template String* Item_func_bit_two_param::eval_str_op
<std::bit_or<char>, std::bit_or<ulonglong> >
(String*, std::bit_or<char>, std::bit_or<ulonglong>);
template String* Item_func_bit_two_param::eval_str_op
<std::bit_and<char>, std::bit_and<ulonglong> >
(String*, std::bit_and<char>, std::bit_and<ulonglong>);
template String* Item_func_bit_two_param::eval_str_op
<std::bit_xor<char>, std::bit_xor<ulonglong> >
(String*, std::bit_xor<char>, std::bit_xor<ulonglong>);


bool Item::bit_func_returns_binary(const Item *a, const Item *b)
{
  /*
    Checks if the bitwise function should return binary data.
    The conditions to return true are the following:

    1. If there's only one argument(so b is nullptr),
    then a must be a [VAR]BINARY Item, different from the hex/bit/NULL literal.

    2. If there are two arguments, both should be [VAR]BINARY
    and at least one of them should be different from the hex/bit/NULL literal
  */
  // Check if a is [VAR]BINARY Item
  bool a_is_binary= a->result_type() == STRING_RESULT &&
                    a->collation.collation == &my_charset_bin;
  // Check if b is not null and is [VAR]BINARY Item
  bool b_is_binary= b && b->result_type() == STRING_RESULT &&
                    b->collation.collation == &my_charset_bin;

  return a_is_binary && (!b || b_is_binary) &&
    ((a->type() != Item::VARBIN_ITEM && a->type() != Item::NULL_ITEM) ||
     (b && b->type() != Item::VARBIN_ITEM && b->type() != Item::NULL_ITEM));
}

// Conversion functions

bool Item_func_integer::resolve_type(THD *thd)
{
  max_length=args[0]->max_length - args[0]->decimals+1;
  uint tmp=float_length(decimals);
  set_if_smaller(max_length,tmp);
  decimals=0;
  return reject_geometry_args(arg_count, args, this);
}

void Item_func_int_val::fix_num_length_and_dec()
{
  ulonglong tmp_max_length= (ulonglong ) args[0]->max_length - 
    (args[0]->decimals ? args[0]->decimals + 1 : 0) + 2;
  max_length= static_cast<uint32>(std::min(4294967295ULL, tmp_max_length));
  /*
    Avoid setting hybrid_type to INT_RESULT when we are in DECIMAL context.
    See Item_func_int_val::find_num_type()
  */
  if (args[0]->result_type() != DECIMAL_RESULT)
  {
    uint tmp= float_length(decimals);
    set_if_smaller(max_length,tmp);
  }
  decimals= 0;
}


void Item_func_int_val::find_num_type()
{
  DBUG_ENTER("Item_func_int_val::find_num_type");
  DBUG_PRINT("info", ("name %s", func_name()));
  switch(hybrid_type= args[0]->result_type())
  {
  case STRING_RESULT:
  case REAL_RESULT:
    hybrid_type= REAL_RESULT;
    max_length= float_length(decimals);
    break;
  case INT_RESULT:
  case DECIMAL_RESULT:
    /*
      -2 because in most high position can't be used any digit for longlong
      and one position for increasing value during operation
    */
    if ((args[0]->max_length - args[0]->decimals) >=
        (DECIMAL_LONGLONG_DIGITS - 2))
    {
      unsigned_flag= args[0]->unsigned_flag;
      hybrid_type= DECIMAL_RESULT;
    }
    else
    {
      unsigned_flag= args[0]->unsigned_flag;
      hybrid_type= INT_RESULT;
    }
    break;
  default:
    DBUG_ASSERT(0);
  }
  DBUG_PRINT("info", ("Type: %s",
                      (hybrid_type == REAL_RESULT ? "REAL_RESULT" :
                       hybrid_type == DECIMAL_RESULT ? "DECIMAL_RESULT" :
                       hybrid_type == INT_RESULT ? "INT_RESULT" :
                       "--ILLEGAL!!!--")));

  DBUG_VOID_RETURN;
}


longlong Item_func_ceiling::int_op()
{
  longlong result;
  switch (args[0]->result_type()) {
  case INT_RESULT:
    result= args[0]->val_int();
    null_value= args[0]->null_value;
    break;
  case DECIMAL_RESULT:
  {
    my_decimal dec_buf, *dec;
    if ((dec= Item_func_ceiling::decimal_op(&dec_buf)))
      my_decimal2int(E_DEC_FATAL_ERROR, dec, unsigned_flag, &result);
    else
      result= 0;
    break;
  }
  default:
    result= (longlong)Item_func_ceiling::real_op();
  };
  return result;
}


double Item_func_ceiling::real_op()
{
  /*
    the volatile's for BUG #3051 to calm optimizer down (because of gcc's
    bug)
  */
  volatile double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return ceil(value);
}


my_decimal *Item_func_ceiling::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= (args[0]->null_value ||
                     my_decimal_ceiling(E_DEC_FATAL_ERROR, value,
                                        decimal_value) > 1)))
    return decimal_value;
  return 0;
}


longlong Item_func_floor::int_op()
{
  longlong result;
  switch (args[0]->result_type()) {
  case INT_RESULT:
    result= args[0]->val_int();
    null_value= args[0]->null_value;
    break;
  case DECIMAL_RESULT:
  {
    my_decimal dec_buf, *dec;
    if ((dec= Item_func_floor::decimal_op(&dec_buf)))
      my_decimal2int(E_DEC_FATAL_ERROR, dec, unsigned_flag, &result);
    else
      result= 0;
    break;
  }
  default:
    result= (longlong)Item_func_floor::real_op();
  };
  return result;
}


double Item_func_floor::real_op()
{
  /*
    the volatile's for BUG #3051 to calm optimizer down (because of gcc's
    bug)
  */
  volatile double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return floor(value);
}


my_decimal *Item_func_floor::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= (args[0]->null_value ||
                     my_decimal_floor(E_DEC_FATAL_ERROR, value,
                                      decimal_value) > 1)))
    return decimal_value;
  return 0;
}


bool Item_func_round::resolve_type(THD *thd)
{
  int      decimals_to_set;
  longlong val1;
  bool     val1_unsigned;
  
  unsigned_flag= args[0]->unsigned_flag;
  if (reject_geometry_args(arg_count, args, this))
    return true;

  if (!args[1]->const_item())
  {
    decimals= args[0]->decimals;
    max_length= float_length(decimals);
    if (args[0]->result_type() == DECIMAL_RESULT)
    {
      max_length++;
      hybrid_type= DECIMAL_RESULT;
    }
    else
      hybrid_type= REAL_RESULT;
    return false;
  }

  val1= args[1]->val_int();
  if ((null_value= args[1]->is_null()))
    return false;

  val1_unsigned= args[1]->unsigned_flag;
  if (val1 < 0)
    decimals_to_set= val1_unsigned ? INT_MAX : 0;
  else
    decimals_to_set= (val1 > INT_MAX) ? INT_MAX : (int) val1;

  if (args[0]->decimals == NOT_FIXED_DEC)
  {
    decimals= min(decimals_to_set, NOT_FIXED_DEC);
    max_length= float_length(decimals);
    hybrid_type= REAL_RESULT;
    return false;
  }
  
  switch (args[0]->result_type()) {
  case REAL_RESULT:
  case STRING_RESULT:
    hybrid_type= REAL_RESULT;
    decimals= min(decimals_to_set, NOT_FIXED_DEC);
    max_length= float_length(decimals);
    break;
  case INT_RESULT:
    if ((!decimals_to_set && truncate) || (args[0]->decimal_precision() < DECIMAL_LONGLONG_DIGITS))
    {
      int length_can_increase= MY_TEST(!truncate && (val1 < 0) && !val1_unsigned);
      max_length= args[0]->max_length + length_can_increase;
      /* Here we can keep INT_RESULT */
      hybrid_type= INT_RESULT;
      decimals= 0;
      break;
    }
    /* fall through */
  case DECIMAL_RESULT:
  {
    hybrid_type= DECIMAL_RESULT;
    decimals_to_set= min(DECIMAL_MAX_SCALE, decimals_to_set);
    int decimals_delta= args[0]->decimals - decimals_to_set;
    int precision= args[0]->decimal_precision();
    int length_increase= ((decimals_delta <= 0) || truncate) ? 0:1;

    precision-= decimals_delta - length_increase;
    decimals= min(decimals_to_set, DECIMAL_MAX_SCALE);
    max_length= my_decimal_precision_to_length_no_truncation(precision,
                                                             decimals,
                                                             unsigned_flag);
    break;
  }
  default:
    DBUG_ASSERT(0); /* This result type isn't handled */
  }
  return false;
}

double my_double_round(double value, longlong dec, bool dec_unsigned,
                       bool truncate)
{
  double tmp;
  bool dec_negative= (dec < 0) && !dec_unsigned;
  ulonglong abs_dec= dec_negative ? -dec : dec;
  /*
    tmp2 is here to avoid return the value with 80 bit precision
    This will fix that the test round(0.1,1) = round(0.1,1) is true
    Tagging with volatile is no guarantee, it may still be optimized away...
  */
  volatile double tmp2;

  tmp=(abs_dec < array_elements(log_10) ?
       log_10[abs_dec] : pow(10.0,(double) abs_dec));

  // Pre-compute these, to avoid optimizing away e.g. 'floor(v/tmp) * tmp'.
  volatile double value_div_tmp= value / tmp;
  volatile double value_mul_tmp= value * tmp;

  if (dec_negative && std::isinf(tmp))
    tmp2= 0.0;
  else if (!dec_negative &&
           (std::isinf(value_mul_tmp) || std::isnan(value_mul_tmp)))
    tmp2= value;
  else if (truncate)
  {
    if (value >= 0.0)
      tmp2= dec < 0 ? floor(value_div_tmp) * tmp : floor(value_mul_tmp) / tmp;
    else
      tmp2= dec < 0 ? ceil(value_div_tmp) * tmp : ceil(value_mul_tmp) / tmp;
  }
  else
    tmp2=dec < 0 ? rint(value_div_tmp) * tmp : rint(value_mul_tmp) / tmp;

  return tmp2;
}


double Item_func_round::real_op()
{
  const double value= args[0]->val_real();
  const longlong decimal_places= args[1]->val_int();

  if (!(null_value= args[0]->null_value || args[1]->null_value))
    return my_double_round(value, decimal_places, args[1]->unsigned_flag,
                           truncate);

  return 0.0;
}

/*
  Rounds a given value to a power of 10 specified as the 'to' argument,
  avoiding overflows when the value is close to the ulonglong range boundary.
*/

static inline ulonglong my_unsigned_round(ulonglong value, ulonglong to)
{
  ulonglong tmp= value / to * to;
  return (value - tmp < (to >> 1)) ? tmp : tmp + to;
}


longlong Item_func_round::int_op()
{
  longlong value= args[0]->val_int();
  longlong dec= args[1]->val_int();
  decimals= 0;
  ulonglong abs_dec;
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;
  if ((dec >= 0) || args[1]->unsigned_flag)
    return value; // integer have not digits after point

  abs_dec= -dec;
  longlong tmp;
  
  if(abs_dec >= array_elements(log_10_int))
    return 0;
  
  tmp= log_10_int[abs_dec];
  
  if (truncate)
    value= (unsigned_flag) ?
      ((ulonglong) value / tmp) * tmp : (value / tmp) * tmp;
  else
    value= (unsigned_flag || value >= 0) ?
      my_unsigned_round((ulonglong) value, tmp) :
      -(longlong) my_unsigned_round((ulonglong) -value, tmp);
  return value;
}


my_decimal *Item_func_round::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  longlong dec= args[1]->val_int();
  if (dec >= 0 || args[1]->unsigned_flag)
    dec= min<ulonglong>(dec, decimals);
  else if (dec < INT_MIN)
    dec= INT_MIN;
    
  if (!(null_value= (args[0]->null_value || args[1]->null_value ||
                     my_decimal_round(E_DEC_FATAL_ERROR, value, (int) dec,
                                      truncate, decimal_value) > 1))) 
    return decimal_value;
  return 0;
}


bool Item_func_rand::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  /*
    When RAND() is binlogged, the seed is binlogged too.  So the
    sequence of random numbers is the same on a replication slave as
    on the master.  However, if several RAND() values are inserted
    into a table, the order in which the rows are modified may differ
    between master and slave, because the order is undefined.  Hence,
    the statement is unsafe to log in statement format.
  */
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);

  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_RAND);
  return false;
}


void Item_func_rand::seed_random(Item *arg)
{
  /*
    TODO: do not do reinit 'rand' for every execute of PS/SP if
    args[0] is a constant.
  */
  uint32 tmp= (uint32) arg->val_int();
  randominit(rand, (uint32) (tmp*0x10001L+55555555L),
             (uint32) (tmp*0x10000001L));
}


bool Item_func_rand::resolve_type(THD *thd)
{
  if (Item_real_func::resolve_type(thd))
    return true;
  return reject_geometry_args(arg_count, args, this);
}


bool Item_func_rand::fix_fields(THD *thd,Item **ref)
{
  if (Item_real_func::fix_fields(thd, ref))
    return TRUE;

  if (arg_count)
  {					// Only use argument once in query
    /*
      Allocate rand structure once: we must use thd->stmt_arena
      to create rand in proper mem_root if it's a prepared statement or
      stored procedure.

      No need to send a Rand log event if seed was given eg: RAND(seed),
      as it will be replicated in the query as such.
    */
    if (!rand && !(rand= (struct rand_struct*)
                   thd->stmt_arena->alloc(sizeof(*rand))))
      return TRUE;
  }
  else
  {
    /*
      Save the seed only the first time RAND() is used in the query
      Once events are forwarded rather than recreated,
      the following can be skipped if inside the slave thread
    */
    if (!thd->rand_used)
    {
      thd->rand_used= 1;
      thd->rand_saved_seed1= thd->rand.seed1;
      thd->rand_saved_seed2= thd->rand.seed2;
    }
    rand= &thd->rand;
  }
  return FALSE;
}


double Item_func_rand::val_real()
{
  DBUG_ASSERT(fixed == 1);
  if (arg_count)
  {
    if (!args[0]->const_item())
      seed_random(args[0]);
    else if (first_eval)
    {
      /*
        Constantness of args[0] may be set during JOIN::optimize(), if arg[0]
        is a field item of "constant" table. Thus, we have to evaluate
        seed_random() for constant arg there but not at the fix_fields method.
      */
      first_eval= FALSE;
      seed_random(args[0]);
    }
  }
  return my_rnd(rand);
}


bool Item_func_sign::resolve_type(THD *thd)
{
  if (Item_int_func::resolve_type(thd))
    return true;
  return reject_geometry_args(arg_count, args, this);
}


longlong Item_func_sign::val_int()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  null_value=args[0]->null_value;
  return value < 0.0 ? -1 : (value > 0 ? 1 : 0);
}


bool Item_func_units::resolve_type(THD *thd)
{
  decimals= NOT_FIXED_DEC;
  max_length= float_length(decimals);
  return reject_geometry_args(arg_count, args, this);
}


double Item_func_units::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0;
  return check_float_overflow(value * mul + add);
}


bool Item_func_min_max::resolve_type(THD *thd)
{
  uint string_arg_count= 0;
  uint unsigned_arg_count= 0;
  int max_int_part=0;
  bool datetime_found= FALSE;
  decimals=0;
  max_length=0;
  maybe_null=0;
  cmp_type= args[0]->temporal_with_date_as_number_result_type();

  for (uint i=0 ; i < arg_count ; i++)
  {
    set_if_bigger(max_length, args[i]->max_length);
    set_if_bigger(decimals, args[i]->decimals);
    set_if_bigger(max_int_part, args[i]->decimal_int_part());
    if (args[i]->maybe_null)
      maybe_null=1;
    cmp_type= item_cmp_type(cmp_type,
                            args[i]->temporal_with_date_as_number_result_type());
    if (args[i]->result_type() == STRING_RESULT)
     string_arg_count++;
    if (args[i]->result_type() != ROW_RESULT &&
        args[i]->is_temporal_with_date())
    {
      datetime_found= TRUE;
      if (!datetime_item || args[i]->field_type() == MYSQL_TYPE_DATETIME)
        datetime_item= args[i];
    }
    if (args[i]->result_type() == INT_RESULT && args[i]->unsigned_flag)
      ++unsigned_arg_count;
  }
  
  if (string_arg_count == arg_count)
  {
    // We compare as strings only if all arguments were strings.
    if (agg_arg_charsets_for_string_result_with_comparison(collation,
                                                           args, arg_count))
      return true;

    if (datetime_found)
    {
      compare_as_dates= TRUE;
      /*
        We should not do this:
          cached_field_type= datetime_item->field_type();
          count_datetime_length(args, arg_count);
        because compare_as_dates can be TRUE but
        result type can still be VARCHAR.
      */
    }
  }
  else if ((cmp_type == DECIMAL_RESULT) || (cmp_type == INT_RESULT))
  {
    collation.set_numeric();
    if (cmp_type == INT_RESULT)
    {
      // For greatest: one unsigned input means result must be >= 0
      if (-1 == cmp_sign && unsigned_arg_count)
        unsigned_flag= true;
      // For least: all unsigned input means result must be >= 0
      if (1 == cmp_sign && unsigned_arg_count == arg_count)
        unsigned_flag= true;
    }
    fix_char_length(my_decimal_precision_to_length_no_truncation(max_int_part +
                                                                 decimals,
                                                                 decimals,
                                                                 unsigned_flag));
  }
  else if (cmp_type == REAL_RESULT)
  {
    fix_char_length(float_length(decimals));
  }
  cached_field_type= agg_field_type(args, arg_count);

  /*
    See comment above: We should not do this:
    However: we need to re-calculate max_length for this case,
    so we temporarily set cached_field_type, calculate lenghts, and set it back.
   */
  if (compare_as_dates && cached_field_type == MYSQL_TYPE_VARCHAR)
  {
    cached_field_type= datetime_item->field_type();
    count_datetime_length(args, arg_count);
    cached_field_type= MYSQL_TYPE_VARCHAR;
  }

  /*
    LEAST and GREATEST convert JSON values to strings before they are
    compared, so their JSON nature is lost. Raise a warning to
    indicate to the users that the values are not compared using the
    JSON comparator, as they might expect. Also update the field type
    of the result to reflect that the result is a string.
  */
  unsupported_json_comparison(arg_count, args,
                              "comparison of JSON in the "
                              "LEAST and GREATEST operators");
  if (cached_field_type == MYSQL_TYPE_JSON)
    cached_field_type= MYSQL_TYPE_VARCHAR;

  return reject_geometry_args(arg_count, args, this);
}


/*
  Compare item arguments in the DATETIME context.

  SYNOPSIS
    cmp_datetimes()
    value [out]   found least/greatest DATE/DATETIME value

  DESCRIPTION
    Compare item arguments as DATETIME values and return the index of the
    least/greatest argument in the arguments array.
    The correct integer DATE/DATETIME value of the found argument is
    stored to the value pointer, if latter is provided.

  RETURN
   0	If one of arguments is NULL or there was a execution error
   #	index of the least/greatest argument
*/

uint Item_func_min_max::cmp_datetimes(longlong *value)
{
  longlong min_max= 0;
  uint min_max_idx= 0;

  for (uint i=0; i < arg_count ; i++)
  {
    Item **arg= args + i;
    bool is_null;
    THD *thd= current_thd;
    longlong res= get_datetime_value(thd, &arg, 0, datetime_item, &is_null);

    /* Check if we need to stop (because of error or KILL)  and stop the loop */
    if (thd->is_error())
    {
      null_value= 1;
      return 0;
    }

    if ((null_value= args[i]->null_value))
      return 0;
    if (i == 0 || (res < min_max ? cmp_sign : -cmp_sign) > 0)
    {
      min_max= res;
      min_max_idx= i;
    }
  }
  if (value)
    *value= min_max;
  return min_max_idx;
}


uint Item_func_min_max::cmp_times(longlong *value)
{
  longlong min_max= 0;
  uint min_max_idx= 0;
  for (uint i=0; i < arg_count ; i++)
  {
    longlong res= args[i]->val_time_temporal();
    if ((null_value= args[i]->null_value))
      return 0;
    if (i == 0 || (res < min_max ? cmp_sign : -cmp_sign) > 0)
    {
      min_max= res;
      min_max_idx= i;
    }
  }
  if (value)
    *value= min_max;
  return min_max_idx;
}


String *Item_func_min_max::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  if (compare_as_dates)
  {
    if (is_temporal())
    {
      /*
        In case of temporal data types, we always return
        string value according the format of the data type.
        For example, in case of LEAST(time_column, datetime_column)
        the result date type is DATETIME,
        so we return a 'YYYY-MM-DD hh:mm:ss' string even if time_column wins
        (conversion from TIME to DATETIME happens in this case).
      */
      longlong result;
      cmp_datetimes(&result);
      if (null_value)
        return 0;
      MYSQL_TIME ltime;
      TIME_from_longlong_packed(&ltime, field_type(), result);
      return (null_value= my_TIME_to_str(&ltime, str, decimals)) ?
             (String *) 0 : str;
    }
    else
    {
      /*
        In case of VARCHAR result type we just return val_str()
        value of the winning item AS IS, without conversion.
      */
      String *str_res;
      uint min_max_idx= cmp_datetimes(NULL);
      if (null_value)
        return 0;
      str_res= args[min_max_idx]->val_str(str);
      if (args[min_max_idx]->null_value)
      {
        // check if the call to val_str() above returns a NULL value
        null_value= 1;
        return NULL;
      }
      str_res->set_charset(collation.collation);
      return str_res;
    }
  }

  switch (cmp_type) {
  case INT_RESULT:
  {
    longlong nr=val_int();
    if (null_value)
      return 0;
    str->set_int(nr, unsigned_flag, collation.collation);
    return str;
  }
  case DECIMAL_RESULT:
  {
    my_decimal dec_buf, *dec_val= val_decimal(&dec_buf);
    if (null_value)
      return 0;
    my_decimal2string(E_DEC_FATAL_ERROR, dec_val, 0, 0, 0, str);
    return str;
  }
  case REAL_RESULT:
  {
    double nr= val_real();
    if (null_value)
      return 0; /* purecov: inspected */
    str->set_real(nr, decimals, collation.collation);
    return str;
  }
  case STRING_RESULT:
  {
    String *res= NULL;
    for (uint i=0; i < arg_count ; i++)
    {
      if (i == 0)
	res=args[i]->val_str(str);
      else
      {
	String *res2;
	res2= args[i]->val_str(res == str ? &tmp_value : str);
	if (res2)
	{
	  int cmp= sortcmp(res,res2,collation.collation);
	  if ((cmp_sign < 0 ? cmp : -cmp) < 0)
	    res=res2;
	}
      }
      if ((null_value= args[i]->null_value))
        return 0;
    }
    res->set_charset(collation.collation);
    return res;
  }
  case ROW_RESULT:
  default:
    // This case should never be chosen
    DBUG_ASSERT(0);
    return 0;
  }
  return 0;					// Keep compiler happy
}


bool Item_func_min_max::get_date(MYSQL_TIME *ltime, my_time_flags_t fuzzydate)
{
  DBUG_ASSERT(fixed == 1);
  if (compare_as_dates)
  {
    longlong result;
    cmp_datetimes(&result);
    if (null_value)
      return true;
    TIME_from_longlong_packed(ltime, datetime_item->field_type(), result);
    int warnings;
    return check_date(ltime, non_zero_date(ltime), fuzzydate, &warnings);
  }

  switch (field_type())
  {
  case MYSQL_TYPE_TIME:
    return get_date_from_time(ltime);
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATE:
    DBUG_ASSERT(0); // Should have been processed in "compare_as_dates" block.
  default:
    return get_date_from_non_temporal(ltime, fuzzydate);
  }
}


bool Item_func_min_max::get_time(MYSQL_TIME *ltime)
{
  DBUG_ASSERT(fixed == 1);
  if (compare_as_dates)
  {
    longlong result;
    cmp_datetimes(&result);
    if (null_value)
      return true;
    TIME_from_longlong_packed(ltime, datetime_item->field_type(), result);
    datetime_to_time(ltime);
    return false;
  }

  switch (field_type())
  {
  case MYSQL_TYPE_TIME:
    {
      longlong result;
      cmp_times(&result);
      if (null_value)
        return true;
      TIME_from_longlong_time_packed(ltime, result);
      return false;
    }
    break;
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATETIME:
    DBUG_ASSERT(0); // Should have been processed in "compare_as_dates" block.
  default:
    return get_time_from_non_temporal(ltime);
    break;
  }
}


double Item_func_min_max::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value=0.0;
  if (compare_as_dates)
  {
    longlong result= 0;
    (void)cmp_datetimes(&result);
    return double_from_datetime_packed(datetime_item->field_type(), result);
  }
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      value= args[i]->val_real();
    else
    {
      double tmp= args[i]->val_real();
      if (!args[i]->null_value && (tmp < value ? cmp_sign : -cmp_sign) > 0)
	value=tmp;
    }
    if ((null_value= args[i]->null_value))
      break;
  }
  return value;
}


longlong Item_func_min_max::val_int()
{
  DBUG_ASSERT(fixed == 1);
  longlong value=0;
  if (compare_as_dates)
  {
    longlong result= 0;
    (void)cmp_datetimes(&result);
    return longlong_from_datetime_packed(datetime_item->field_type(), result);
  }
  /*
    TS-TODO: val_str decides which type to use using cmp_type.
    val_int, val_decimal, val_real do not check cmp_type and
    decide data type according to the method type.
    This is probably not good:

mysql> select least('11', '2'), least('11', '2')+0, concat(least(11,2));
+------------------+--------------------+---------------------+
| least('11', '2') | least('11', '2')+0 | concat(least(11,2)) |
+------------------+--------------------+---------------------+
| 11               |                  2 | 2                   |
+------------------+--------------------+---------------------+
1 row in set (0.00 sec)

    Should not the second column return 11?
    I.e. compare as strings and return '11', then convert to number.
  */
  value= args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return value;
  bool val_unsigned= args[0]->unsigned_flag;

  for (uint i= 1; i < arg_count; i++)
  {
    const longlong tmp= args[i]->val_int();
    if ((null_value= args[i]->null_value))
      break;

    const bool tmp_unsigned= args[i]->unsigned_flag;
    const bool tmp_is_smaller=
      Integer_value(tmp, tmp_unsigned) < Integer_value(value, val_unsigned);

    if ((tmp_is_smaller ? cmp_sign : -cmp_sign) > 0)
    {
      value= tmp;
      val_unsigned= tmp_unsigned;
    }
  }
  return value;
}


my_decimal *Item_func_min_max::val_decimal(my_decimal *dec)
{
  DBUG_ASSERT(fixed == 1);
  my_decimal tmp_buf, *tmp, *res= NULL;

  if (compare_as_dates)
  {
    longlong value= 0;
    (void)cmp_datetimes(&value);
    return my_decimal_from_datetime_packed(dec, datetime_item->field_type(),
                                           value);
  }
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      res= args[i]->val_decimal(dec);
    else
    {
      tmp= args[i]->val_decimal(&tmp_buf);      // Zero if NULL
      if (tmp && (my_decimal_cmp(tmp, res) * cmp_sign) < 0)
      {
        if (tmp == &tmp_buf)
        {
          /* Move value out of tmp_buf as this will be reused on next loop */
          my_decimal2decimal(tmp, dec);
          res= dec;
        }
        else
          res= tmp;
      }
    }
    if ((null_value= args[i]->null_value))
    {
      res= 0;
      break;
    }
  }
  
  if (res)
  {
    /*
      Need this to make val_str() always return fixed
      number of fractional digits, according to "decimals".
    */
    my_decimal_round(E_DEC_FATAL_ERROR, res, decimals, false, res);
  }
  return res;
}

double Item_func_rollup_const::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double res= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  return res;
}

longlong Item_func_rollup_const::val_int()
{
  DBUG_ASSERT(fixed == 1);
  longlong res= args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0;
  return res;
}

String *Item_func_rollup_const::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String *res= args[0]->val_str(str);
  if ((null_value= args[0]->null_value))
    return 0;
  return res;
}

my_decimal *Item_func_rollup_const::val_decimal(my_decimal *dec)
{
  DBUG_ASSERT(fixed == 1);
  my_decimal *res= args[0]->val_decimal(dec);
  if ((null_value= args[0]->null_value))
    return 0;
  return res;
}


bool Item_func_rollup_const::val_json(Json_wrapper *result)
{
  DBUG_ASSERT(fixed == 1);
  bool res= args[0]->val_json(result);
  null_value= args[0]->null_value;
  return res;
}


longlong Item_func_length::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  return (longlong) res->length();
}


longlong Item_func_char_length::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  return (longlong) res->numchars();
}


longlong Item_func_coercibility::val_int()
{
  DBUG_ASSERT(fixed == 1);
  null_value= 0;
  return (longlong) args[0]->collation.derivation;
}


bool Item_func_locate::resolve_type(THD *thd)
{
  max_length= MY_INT32_NUM_DECIMAL_DIGITS;
  return agg_arg_charsets_for_comparison(cmp_collation, args, 2);
}


longlong Item_func_locate::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *a=args[0]->val_str(&value1);
  String *b=args[1]->val_str(&value2);
  if (!a || !b)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  /* must be longlong to avoid truncation */
  longlong start=  0; 
  longlong start0= 0;
  my_match_t match;

  if (arg_count == 3)
  {
    const longlong tmp= args[2]->val_int();
    if (tmp <= 0)
      return 0;
    start0= start= tmp - 1;

    if (start > static_cast<longlong>(a->length()))
      return 0;

    /* start is now sufficiently valid to pass to charpos function */
    start= a->charpos((int) start);

    if (start + b->length() > a->length())
      return 0;
  }

  if (!b->length())				// Found empty string at start
    return start + 1;
  
  if (!cmp_collation.collation->coll->instr(cmp_collation.collation,
                                            a->ptr()+start,
                                            (uint) (a->length()-start),
                                            b->ptr(), b->length(),
                                            &match, 1))
    return 0;
  return (longlong) match.mb_len + start0 + 1;
}


void Item_func_locate::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("locate("));
  args[1]->print(str, query_type);
  str->append(',');
  args[0]->print(str, query_type);
  if (arg_count == 3)
  {
    str->append(',');
    args[2]->print(str, query_type);
  }
  str->append(')');
}


longlong Item_func_validate_password_strength::val_int()
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  String value(buff, sizeof(buff), system_charset_info);
  String *field= args[0]->val_str(&value);
  if ((null_value= args[0]->null_value) || field->length() == 0)
    return 0;
  return (my_calculate_password_strength(field->ptr(), field->length()));
}


longlong Item_func_field::val_int()
{
  DBUG_ASSERT(fixed == 1);

  if (cmp_type == STRING_RESULT)
  {
    String *field;
    if (!(field= args[0]->val_str(&value)))
      return 0;
    for (uint i=1 ; i < arg_count ; i++)
    {
      String *tmp_value=args[i]->val_str(&tmp);
      if (tmp_value && !sortcmp(field,tmp_value,cmp_collation.collation))
        return (longlong) (i);
    }
  }
  else if (cmp_type == INT_RESULT)
  {
    longlong val= args[0]->val_int();
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count ; i++)
    {
      if (val == args[i]->val_int() && !args[i]->null_value)
        return (longlong) (i);
    }
  }
  else if (cmp_type == DECIMAL_RESULT)
  {
    my_decimal dec_arg_buf, *dec_arg,
               dec_buf, *dec= args[0]->val_decimal(&dec_buf);
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count; i++)
    {
      dec_arg= args[i]->val_decimal(&dec_arg_buf);
      if (!args[i]->null_value && !my_decimal_cmp(dec_arg, dec))
        return (longlong) (i);
    }
  }
  else
  {
    double val= args[0]->val_real();
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count ; i++)
    {
      if (val == args[i]->val_real() && !args[i]->null_value)
        return (longlong) (i);
    }
  }
  return 0;
}


bool Item_func_field::resolve_type(THD *thd)
{
  maybe_null=0; max_length=3;
  cmp_type= args[0]->result_type();
  for (uint i=1; i < arg_count ; i++)
    cmp_type= item_cmp_type(cmp_type, args[i]->result_type());
  if (cmp_type == STRING_RESULT)
    return agg_arg_charsets_for_comparison(cmp_collation, args, arg_count);
  return false;
}


longlong Item_func_ascii::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  return (longlong) (res->length() ? (uchar) (*res)[0] : (uchar) 0);
}

longlong Item_func_ord::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  if (!res->length()) return 0;
  if (use_mb(res->charset()))
  {
    const char *str=res->ptr();
    uint32 n=0, l=my_ismbchar(res->charset(),str,str+res->length());
    if (!l)
      return (longlong)((uchar) *str);
    while (l--)
      n=(n<<8)|(uint32)((uchar) *str++);
    return (longlong) n;
  }
  return (longlong) ((uchar) (*res)[0]);
}

	/* Search after a string in a string of strings separated by ',' */
	/* Returns number of found type >= 1 or 0 if not found */
	/* This optimizes searching in enums to bit testing! */

bool Item_func_find_in_set::resolve_type(THD *thd)
{
  decimals=0;
  max_length=3;					// 1-999
  if (args[0]->const_item() && args[1]->type() == FIELD_ITEM)
  {
    Field *field= ((Item_field*) args[1])->field;
    if (field->real_type() == MYSQL_TYPE_SET)
    {
      String *find=args[0]->val_str(&value);
      if (find)
      {
        // find is not NULL pointer so args[0] is not a null-value
        DBUG_ASSERT(!args[0]->null_value);
	enum_value= find_type(((Field_enum*) field)->typelib,find->ptr(),
			      find->length(), 0);
	enum_bit=0;
	if (enum_value)
	  enum_bit= 1LL << (enum_value-1);
      }
    }
  }
  return agg_arg_charsets_for_comparison(cmp_collation, args, 2);
}

static const char separator=',';

longlong Item_func_find_in_set::val_int()
{
  DBUG_ASSERT(fixed == 1);
  if (enum_value)
  {
    // enum_value is set iff args[0]->const_item() in resolve_type().
    DBUG_ASSERT(args[0]->const_item());

    ulonglong tmp= (ulonglong) args[1]->val_int();
    null_value= args[1]->null_value;
    /* 
      No need to check args[0]->null_value since enum_value is set iff
      args[0] is a non-null const item. Note: no DBUG_ASSERT on
      args[0]->null_value here because args[0] may have been replaced
      by an Item_cache on which val_int() has not been called. See
      BUG#11766317
    */
    if (!null_value)
    {
      if (tmp & enum_bit)
        return enum_value;
    }
    return 0L;
  }

  String *find=args[0]->val_str(&value);
  String *buffer=args[1]->val_str(&value2);
  if (!find || !buffer)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;

  if (buffer->length() >= find->length())
  {
    my_wc_t wc= 0;
    const CHARSET_INFO *cs= cmp_collation.collation;
    const char *str_begin= buffer->ptr();
    const char *str_end= buffer->ptr();
    const char *real_end= str_end+buffer->length();
    const uchar *find_str= (const uchar *) find->ptr();
    size_t find_str_len= find->length();
    int position= 0;
    while (1)
    {
      int symbol_len;
      if ((symbol_len= cs->cset->mb_wc(cs, &wc, (uchar*) str_end, 
                                       (uchar*) real_end)) > 0)
      {
        const char *substr_end= str_end + symbol_len;
        bool is_last_item= (substr_end == real_end);
        bool is_separator= (wc == (my_wc_t) separator);
        if (is_separator || is_last_item)
        {
          position++;
          if (is_last_item && !is_separator)
            str_end= substr_end;
          if (!my_strnncoll(cs, (const uchar *) str_begin,
                            (uint) (str_end - str_begin),
                            find_str, find_str_len))
            return (longlong) position;
          else
            str_begin= substr_end;
        }
        str_end= substr_end;
      }
      else if (str_end - str_begin == 0 &&
               find_str_len == 0 &&
               wc == (my_wc_t) separator)
        return (longlong) ++position;
      else
        return 0LL;
    }
  }
  return 0;
}

longlong Item_func_bit_count::val_int()
{
  DBUG_ASSERT(fixed);
  if (bit_func_returns_binary(args[0], NULL))
  {
    String *s= args[0]->val_str(&str_value);
    if ((null_value= args[0]->null_value))
      return 0;

    char *val= const_cast<char *>(s->ptr());

    longlong len= 0;
    size_t i= 0;
    size_t arg_length= s->length();
    while (i + sizeof(longlong) <= arg_length)
    {
      len+= my_count_bits(uint8korr(&val[i]));
      i+= sizeof(longlong);
    }
    while (i < arg_length)
    {
      len+= _my_bits_nbits[(uchar) val[i]];
      i++;
    }

    return len;
  }
  ulonglong value= (ulonglong) args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0; /* purecov: inspected */
  return (longlong) my_count_bits(value);
}


/****************************************************************************
** Functions to handle dynamic loadable functions
** Original source by: Alexis Mikhailov <root@medinf.chuvashia.su>
** Rewritten by monty.
****************************************************************************/

void udf_handler::cleanup()
{
  if (!not_original)
  {
    if (initialized)
    {
      if (u_d->func_deinit != NULL)
      {
        Udf_func_deinit deinit= u_d->func_deinit;
        (*deinit)(&initid);
      }
      free_udf(u_d);
      initialized= FALSE;
    }
    if (buffers)				// Because of bug in ecc
      delete [] buffers;
    buffers= 0;
  }
}


bool
udf_handler::fix_fields(THD *thd, Item_result_field *func,
			uint arg_count, Item **arguments)
{
  uchar buff[STACK_BUFF_ALLOC];			// Max argument in function
  DBUG_ENTER("Item_udf_func::fix_fields");

  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    DBUG_RETURN(TRUE);				// Fatal error flag is set!

  udf_func *tmp_udf=find_udf(u_d->name.str,(uint) u_d->name.length,1);

  if (!tmp_udf)
  {
    my_error(ER_CANT_FIND_UDF, MYF(0), u_d->name.str);
    DBUG_RETURN(TRUE);
  }
  u_d=tmp_udf;
  args=arguments;

  /* Fix all arguments */
  func->maybe_null=0;
  used_tables_cache=0;
  const_item_cache=1;

  if ((f_args.arg_count=arg_count))
  {
    if (!(f_args.arg_type= (Item_result*)
	  sql_alloc(f_args.arg_count*sizeof(Item_result))))

    {
      free_udf(u_d);
      DBUG_RETURN(TRUE);
    }
    uint i;
    Item **arg,**arg_end;
    for (i=0, arg=arguments, arg_end=arguments+arg_count;
	 arg != arg_end ;
	 arg++,i++)
    {
      if (!(*arg)->fixed &&
          (*arg)->fix_fields(thd, arg))
	DBUG_RETURN(1);
      // we can't assign 'item' before, because fix_fields() can change arg
      Item *item= *arg;
      if (item->check_cols(1))
	DBUG_RETURN(TRUE);
      /*
	TODO: We should think about this. It is not always
	right way just to set an UDF result to return my_charset_bin
	if one argument has binary sorting order.
	The result collation should be calculated according to arguments
	derivations in some cases and should not in other cases.
	Moreover, some arguments can represent a numeric input
	which doesn't effect the result character set and collation.
	There is no a general rule for UDF. Everything depends on
        the particular user defined function.
      */
      if (item->collation.collation->state & MY_CS_BINSORT)
	func->collation.set(&my_charset_bin);
      if (item->maybe_null)
	func->maybe_null=1;
      func->with_sum_func= func->with_sum_func || item->with_sum_func;
      used_tables_cache|=item->used_tables();
      const_item_cache&=item->const_item();
      f_args.arg_type[i]=item->result_type();
    }
    //TODO: why all following memory is not allocated with 1 call of sql_alloc?
    if (!(buffers=new String[arg_count]) ||
	!(f_args.args= (char**) sql_alloc(arg_count * sizeof(char *))) ||
	!(f_args.lengths= (ulong*) sql_alloc(arg_count * sizeof(long))) ||
	!(f_args.maybe_null= (char*) sql_alloc(arg_count * sizeof(char))) ||
	!(num_buffer= (char*) sql_alloc(arg_count *
					ALIGN_SIZE(sizeof(double)))) ||
	!(f_args.attributes= (char**) sql_alloc(arg_count * sizeof(char *))) ||
	!(f_args.attribute_lengths= (ulong*) sql_alloc(arg_count *
						       sizeof(long))))
    {
      free_udf(u_d);
      DBUG_RETURN(TRUE);
    }
  }
  if (func->resolve_type(thd))
    DBUG_RETURN(true);
  initid.max_length=func->max_length;
  initid.maybe_null=func->maybe_null;
  initid.const_item=const_item_cache;
  initid.decimals=func->decimals;
  initid.ptr=0;

  if (u_d->func_init)
  {
    char init_msg_buff[MYSQL_ERRMSG_SIZE];
    char *to=num_buffer;
    for (uint i=0; i < arg_count; i++)
    {
      /*
       For a constant argument i, args->args[i] points to the argument value. 
       For non-constant, args->args[i] is NULL.
      */
      f_args.args[i]= NULL;         /* Non-const unless updated below. */

      f_args.lengths[i]= arguments[i]->max_length;
      f_args.maybe_null[i]= arguments[i]->maybe_null;
      f_args.attributes[i]= (char*) arguments[i]->item_name.ptr();
      f_args.attribute_lengths[i]= arguments[i]->item_name.length();

      if (arguments[i]->const_item())
      {
        switch (arguments[i]->result_type()) 
        {
        case STRING_RESULT:
        case DECIMAL_RESULT:
        {
          String *res= arguments[i]->val_str(&buffers[i]);
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= res->c_ptr_safe();
          f_args.lengths[i]= res->length();
          break;
        }
        case INT_RESULT:
          *((longlong*) to)= arguments[i]->val_int();
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= to;
          to+= ALIGN_SIZE(sizeof(longlong));
          break;
        case REAL_RESULT:
          *((double*) to)= arguments[i]->val_real();
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= to;
          to+= ALIGN_SIZE(sizeof(double));
          break;
        case ROW_RESULT:
        default:
          // This case should never be chosen
          DBUG_ASSERT(0);
          break;
        }
      }
    }
    Udf_func_init init= u_d->func_init;
    if ((error=(uchar) init(&initid, &f_args, init_msg_buff)))
    {
      my_error(ER_CANT_INITIALIZE_UDF, MYF(0),
               u_d->name.str, init_msg_buff);
      free_udf(u_d);
      DBUG_RETURN(TRUE);
    }
    func->max_length= min<uint32>(initid.max_length, MAX_BLOB_WIDTH);
    func->maybe_null=initid.maybe_null;
    const_item_cache=initid.const_item;
    /* 
      Keep used_tables_cache in sync with const_item_cache.
      See the comment in Item_udf_func::update_used tables.
    */  
    if (!const_item_cache && !used_tables_cache)
      used_tables_cache= RAND_TABLE_BIT;
    func->decimals= min<uint>(initid.decimals, NOT_FIXED_DEC);
  }
  initialized=1;
  if (error)
  {
    my_error(ER_CANT_INITIALIZE_UDF, MYF(0),
             u_d->name.str, ER_THD(thd, ER_UNKNOWN_ERROR));
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


bool udf_handler::get_arguments()
{
  if (error)
    return 1;					// Got an error earlier
  char *to= num_buffer;
  uint str_count=0;
  for (uint i=0; i < f_args.arg_count; i++)
  {
    f_args.args[i]=0;
    switch (f_args.arg_type[i]) {
    case STRING_RESULT:
    case DECIMAL_RESULT:
      {
	String *res=args[i]->val_str(&buffers[str_count++]);
	if (!(args[i]->null_value))
	{
	  f_args.args[i]=    res->c_ptr_safe();
	  f_args.lengths[i]= res->length();
	}
	else
	{
	  f_args.lengths[i]= 0;
	}
	break;
      }
    case INT_RESULT:
      *((longlong*) to) = args[i]->val_int();
      if (!args[i]->null_value)
      {
	f_args.args[i]=to;
	to+= ALIGN_SIZE(sizeof(longlong));
      }
      break;
    case REAL_RESULT:
      *((double*) to)= args[i]->val_real();
      if (!args[i]->null_value)
      {
	f_args.args[i]=to;
	to+= ALIGN_SIZE(sizeof(double));
      }
      break;
    case ROW_RESULT:
    default:
      // This case should never be chosen
      DBUG_ASSERT(0);
      break;
    }
  }
  return 0;
}

/**
  @return
    (String*)NULL in case of NULL values
*/
String *udf_handler::val_str(String *str,String *save_str)
{
  uchar is_null_tmp=0;
  ulong res_length;
  DBUG_ENTER("udf_handler::val_str");

  if (get_arguments())
    DBUG_RETURN(0);
  char * (*func)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *)=
    (char* (*)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *))
    u_d->func;

  if ((res_length=str->alloced_length()) < MAX_FIELD_WIDTH)
  {						// This happens VERY seldom
    if (str->alloc(MAX_FIELD_WIDTH))
    {
      error=1;
      DBUG_RETURN(0);
    }
  }
  char *res=func(&initid, &f_args, (char*) str->ptr(), &res_length,
		 &is_null_tmp, &error);
  DBUG_PRINT("info", ("udf func returned, res_length: %lu", res_length));
  if (is_null_tmp || !res || error)		// The !res is for safety
  {
    DBUG_PRINT("info", ("Null or error"));
    DBUG_RETURN(0);
  }
  if (res == str->ptr())
  {
    str->length(res_length);
    DBUG_PRINT("exit", ("str: %*.s", (int) str->length(), str->ptr()));
    DBUG_RETURN(str);
  }
  save_str->set(res, res_length, str->charset());
  DBUG_PRINT("exit", ("save_str: %s", save_str->ptr()));
  DBUG_RETURN(save_str);
}


/*
  For the moment, UDF functions are returning DECIMAL values as strings
*/

my_decimal *udf_handler::val_decimal(my_bool *null_value, my_decimal *dec_buf)
{
  char buf[DECIMAL_MAX_STR_LENGTH+1], *end;
  ulong res_length= DECIMAL_MAX_STR_LENGTH;

  if (get_arguments())
  {
    *null_value=1;
    return 0;
  }
  char *(*func)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *)=
    (char* (*)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *))
    u_d->func;

  char *res= func(&initid, &f_args, buf, &res_length, &is_null, &error);
  if (is_null || error)
  {
    *null_value= 1;
    return 0;
  }
  end= res+ res_length;
  str2my_decimal(E_DEC_FATAL_ERROR, res, dec_buf, &end);
  return dec_buf;
}


bool Item_udf_func::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_UDF);
  pc->thd->lex->safe_to_cache_query= false;
  return false;
}


void Item_udf_func::cleanup()
{
  udf.cleanup();
  Item_func::cleanup();
}


void Item_udf_func::print(String *str, enum_query_type query_type)
{
  str->append(func_name());
  str->append('(');
  for (uint i=0 ; i < arg_count ; i++)
  {
    if (i != 0)
      str->append(',');
    args[i]->print_item_w_name(str, query_type);
  }
  str->append(')');
}


double Item_func_udf_float::val_real()
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_udf_float::val");
  DBUG_PRINT("info",("result_type: %d  arg_count: %d",
		     args[0]->result_type(), arg_count));
  DBUG_RETURN(udf.val(&null_value));
}


String *Item_func_udf_float::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  double nr= val_real();
  if (null_value)
    return 0;					/* purecov: inspected */
  str->set_real(nr,decimals,&my_charset_bin);
  return str;
}


longlong Item_func_udf_int::val_int()
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_udf_int::val_int");
  DBUG_RETURN(udf.val_int(&null_value));
}


String *Item_func_udf_int::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  longlong nr=val_int();
  if (null_value)
    return 0;
  str->set_int(nr, unsigned_flag, &my_charset_bin);
  return str;
}


longlong Item_func_udf_decimal::val_int()
{
  my_decimal dec_buf, *dec= udf.val_decimal(&null_value, &dec_buf);
  longlong result;
  if (null_value)
    return 0;
  my_decimal2int(E_DEC_FATAL_ERROR, dec, unsigned_flag, &result);
  return result;
}


double Item_func_udf_decimal::val_real()
{
  my_decimal dec_buf, *dec= udf.val_decimal(&null_value, &dec_buf);
  double result;
  if (null_value)
    return 0.0;
  my_decimal2double(E_DEC_FATAL_ERROR, dec, &result);
  return result;
}


my_decimal *Item_func_udf_decimal::val_decimal(my_decimal *dec_buf)
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_udf_decimal::val_decimal");
  DBUG_PRINT("info",("result_type: %d  arg_count: %d",
                     args[0]->result_type(), arg_count));

  DBUG_RETURN(udf.val_decimal(&null_value, dec_buf));
}


String *Item_func_udf_decimal::val_str(String *str)
{
  my_decimal dec_buf, *dec= udf.val_decimal(&null_value, &dec_buf);
  if (null_value)
    return 0;
  if (str->length() < DECIMAL_MAX_STR_LENGTH)
    str->length(DECIMAL_MAX_STR_LENGTH);
  my_decimal_round(E_DEC_FATAL_ERROR, dec, decimals, FALSE, &dec_buf);
  my_decimal2string(E_DEC_FATAL_ERROR, &dec_buf, 0, 0, '0', str);
  return str;
}


bool Item_func_udf_decimal::resolve_type(THD *thd)
{
  fix_num_length_and_dec();
  return false;
}


/* Default max_length is max argument length */

bool Item_func_udf_str::resolve_type(THD *thd)
{
  max_length=0;
  for (uint i = 0; i < arg_count; i++)
    set_if_bigger(max_length,args[i]->max_length);
  return false;
}

String *Item_func_udf_str::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String *res=udf.val_str(str,&str_value);
  null_value = !res;
  return res;
}

udf_handler::~udf_handler()
{
  /* Everything should be properly cleaned up by this moment. */
  DBUG_ASSERT(not_original || !(initialized || buffers));
}


bool Item_master_pos_wait::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->safe_to_cache_query= false;
  return false;
}


/**
  Wait until we are at or past the given position in the master binlog
  on the slave.
*/

longlong Item_master_pos_wait::val_int()
{
  DBUG_ASSERT(fixed == 1);
  THD* thd = current_thd;
  String *log_name = args[0]->val_str(&value);
  int event_count= 0;

  null_value=0;
  if (thd->slave_thread || !log_name || !log_name->length())
  {
    null_value = 1;
    return 0;
  }
#ifdef HAVE_REPLICATION
  Master_info *mi;
  longlong pos = (ulong)args[1]->val_int();
  longlong timeout = (arg_count>=3) ? args[2]->val_int() : 0 ;

  channel_map.rdlock();

  if (arg_count == 4)
  {
    String *channel_str;
    if(!(channel_str= args[3]->val_str(&value)))
    {
      null_value= 1;
      return 0;
    }

    mi= channel_map.get_mi(channel_str->ptr());

  }
  else
  {
    if (channel_map.get_num_instances() > 1)
    {
      mi = NULL;
      my_error(ER_SLAVE_MULTIPLE_CHANNELS_CMD, MYF(0));
    }
    else
      mi= channel_map.get_default_channel_mi();
  }

  if (mi != NULL)
    mi->inc_reference();

  channel_map.unlock();

  if (mi == NULL ||
      (event_count = mi->rli->wait_for_pos(thd, log_name, pos, timeout)) == -2)
  {
    null_value = 1;
    event_count=0;
  }

  if (mi != NULL)
    mi->dec_reference();
#endif
  return event_count;
}

bool Item_wait_for_executed_gtid_set::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  /*
    It is unsafe because the return value depends on timing. If the timeout
    happens, the return value is different from the one in which the function
    returns with success.
  */
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->safe_to_cache_query= false;
  return false;
}

/**
  Wait until the given gtid_set is found in the executed gtid_set independent
  of the slave threads.
*/
longlong Item_wait_for_executed_gtid_set::val_int()
{
  DBUG_ENTER("Item_wait_for_executed_gtid_set::val_int");
  DBUG_ASSERT(fixed == 1);
  THD* thd= current_thd;
  String *gtid_text= args[0]->val_str(&value);

  null_value= 0;

  if (gtid_text == NULL)
  {
    my_error(ER_MALFORMED_GTID_SET_SPECIFICATION, MYF(0), "NULL");
    DBUG_RETURN(0);
  }

  // Waiting for a GTID in a slave thread could cause the slave to
  // hang/deadlock.
  if (thd->slave_thread)
  {
    null_value= 1;
    DBUG_RETURN(0);
  }

  Gtid_set wait_for_gtid_set(global_sid_map, NULL);

  global_sid_lock->rdlock();
  if (get_gtid_mode(GTID_MODE_LOCK_SID) == GTID_MODE_OFF)
  {
    global_sid_lock->unlock();
    my_error(ER_GTID_MODE_OFF, MYF(0), "use WAIT_FOR_EXECUTED_GTID_SET");
    null_value= 1;
    DBUG_RETURN(0);
  }

  if (wait_for_gtid_set.add_gtid_text(gtid_text->c_ptr_safe()) !=
      RETURN_STATUS_OK)
  {
    global_sid_lock->unlock();
    // Error has already been generated.
    DBUG_RETURN(1);
  }

  // Cannot wait for a GTID that the thread owns since that would
  // immediately deadlock.
  if (thd->owned_gtid.sidno > 0 &&
      wait_for_gtid_set.contains_gtid(thd->owned_gtid))
  {
    char buf[Gtid::MAX_TEXT_LENGTH + 1];
    thd->owned_gtid.to_string(global_sid_map, buf);
    global_sid_lock->unlock();
    my_error(ER_CANT_WAIT_FOR_EXECUTED_GTID_SET_WHILE_OWNING_A_GTID, MYF(0),
             buf);
    DBUG_RETURN(0);
  }

  gtid_state->begin_gtid_wait(GTID_MODE_LOCK_SID);

  longlong timeout= (arg_count== 2) ? args[1]->val_int() : 0;

  bool result= gtid_state->wait_for_gtid_set(thd, &wait_for_gtid_set, timeout);
  global_sid_lock->unlock();
  gtid_state->end_gtid_wait();

  DBUG_RETURN(result);
}

bool Item_master_gtid_set_wait::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->safe_to_cache_query= false;
  return false;
}


longlong Item_master_gtid_set_wait::val_int()
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_master_gtid_set_wait::val_int");
  int event_count= 0;

  null_value=0;

#if defined(HAVE_REPLICATION)
  String *gtid= args[0]->val_str(&value);
  THD* thd = current_thd;
  Master_info *mi= NULL;
  longlong timeout = (arg_count>= 2) ? args[1]->val_int() : 0;

  if (thd->slave_thread || !gtid)
  {
    null_value = 1;
    DBUG_RETURN(0);
  }

  channel_map.rdlock();

  /* If replication channel is mentioned */
  if (arg_count == 3)
  {
    String *channel_str;
    if (!(channel_str= args[2]->val_str(&value)))
    {
      channel_map.unlock();
      null_value= 1;
      DBUG_RETURN(0);
    }
    mi= channel_map.get_mi(channel_str->ptr());
  }
  else
  {
    if (channel_map.get_num_instances() > 1)
    {
      channel_map.unlock();
      mi = NULL;
      my_error(ER_SLAVE_MULTIPLE_CHANNELS_CMD, MYF(0));
      DBUG_RETURN(0);
    }
    else
      mi= channel_map.get_default_channel_mi();
  }

  if (get_gtid_mode(GTID_MODE_LOCK_CHANNEL_MAP) == GTID_MODE_OFF)
  {
    null_value= 1;
    channel_map.unlock();
    DBUG_RETURN(0);
  }
  gtid_state->begin_gtid_wait(GTID_MODE_LOCK_CHANNEL_MAP);

  if (mi)
    mi->inc_reference();

  channel_map.unlock();

  if (mi && mi->rli)
  {
    event_count = mi->rli->wait_for_gtid_set(thd, gtid, timeout);
    if (event_count == -2)
    {
      null_value = 1;
      event_count=0;
    }
  }
  else
    /*
      Replication has not been set up, we should return NULL;
     */
    null_value = 1;

  if (mi != NULL)
    mi->dec_reference();
#endif

  gtid_state->end_gtid_wait();

  DBUG_RETURN(event_count);
}

/**
  Return 1 if both arguments are Gtid_sets and the first is a subset
  of the second.  Generate an error if any of the arguments is not a
  Gtid_set.
*/
longlong Item_func_gtid_subset::val_int()
{
  DBUG_ENTER("Item_func_gtid_subset::val_int()");
  if (args[0]->null_value || args[1]->null_value)
  {
    null_value= true;
    DBUG_RETURN(0);
  }
  String *string1, *string2;
  const char *charp1, *charp2;
  int ret= 1;
  enum_return_status status;
  // get strings without lock
  if ((string1= args[0]->val_str(&buf1)) != NULL &&
      (charp1= string1->c_ptr_safe()) != NULL &&
      (string2= args[1]->val_str(&buf2)) != NULL &&
      (charp2= string2->c_ptr_safe()) != NULL)
  {
    Sid_map sid_map(NULL/*no rwlock*/);
    // compute sets while holding locks
    const Gtid_set sub_set(&sid_map, charp1, &status);
    if (status == RETURN_STATUS_OK)
    {
      const Gtid_set super_set(&sid_map, charp2, &status);
      if (status == RETURN_STATUS_OK)
        ret= sub_set.is_subset(&super_set) ? 1 : 0;
    }
  }
  DBUG_RETURN(ret);
}


/**
  Enables a session to wait on a condition until a timeout or a network
  disconnect occurs.

  @remark The connection is polled every m_interrupt_interval nanoseconds.
*/

class Interruptible_wait
{
  THD *m_thd;
  struct timespec m_abs_timeout;
  static const ulonglong m_interrupt_interval;

  public:
    Interruptible_wait(THD *thd)
    : m_thd(thd) {}

    ~Interruptible_wait() {}

  public:
    /**
      Set the absolute timeout.

      @param timeout The amount of time in nanoseconds to wait
    */
    void set_timeout(ulonglong timeout)
    {
      /*
        Calculate the absolute system time at the start so it can
        be controlled in slices. It relies on the fact that once
        the absolute time passes, the timed wait call will fail
        automatically with a timeout error.
      */
      set_timespec_nsec(&m_abs_timeout, timeout);
    }

    /** The timed wait. */
    int wait(mysql_cond_t *, mysql_mutex_t *);
};


/** Time to wait before polling the connection status. */
const ulonglong Interruptible_wait::m_interrupt_interval= 5 * 1000000000ULL;


/**
  Wait for a given condition to be signaled.

  @param cond   The condition variable to wait on.
  @param mutex  The associated mutex.

  @remark The absolute timeout is preserved across calls.

  @retval return value from mysql_cond_timedwait
*/

int Interruptible_wait::wait(mysql_cond_t *cond, mysql_mutex_t *mutex)
{
  int error;
  struct timespec timeout;

  while (1)
  {
    /* Wait for a fixed interval. */
    set_timespec_nsec(&timeout, m_interrupt_interval);

    /* But only if not past the absolute timeout. */
    if (cmp_timespec(&timeout, &m_abs_timeout) > 0)
      timeout= m_abs_timeout;

    error= mysql_cond_timedwait(cond, mutex, &timeout);
    if (is_timeout(error))
    {
      /* Return error if timed out or connection is broken. */
      if (!cmp_timespec(&timeout, &m_abs_timeout) || !m_thd->is_connected())
        break;
    }
    /* Otherwise, propagate status to the caller. */
    else
      break;
  }

  return error;
}


/*
  User-level locks implementation.
*/


/**
  For locks with EXPLICIT duration, MDL returns a new ticket
  every time a lock is granted. This allows to implement recursive
  locks without extra allocation or additional data structures, such
  as below. However, if there are too many tickets in the same
  MDL_context, MDL_context::find_ticket() is getting too slow,
  since it's using a linear search.
  This is why a separate structure is allocated for a user
  level lock held by connection, and before requesting a new lock from MDL,
  GET_LOCK() checks thd->ull_hash if such lock is already granted,
  and if so, simply increments a reference counter.
*/

struct User_level_lock
{
  MDL_ticket *ticket;
  uint refs;
};


/** Extract a hash key from User_level_lock. */

static const uchar *ull_get_key(const uchar *ptr, size_t *length)
{
  const User_level_lock *ull = reinterpret_cast<const User_level_lock*>(ptr);
  const MDL_key *key = ull->ticket->get_key();
  *length= key->length();
  return key->ptr();
}


/**
  Release all user level locks for this THD.
*/

void mysql_ull_cleanup(THD *thd)
{
  User_level_lock *ull;
  DBUG_ENTER("mysql_ull_cleanup");

  for (ulong i= 0; i < thd->ull_hash.records; i++)
  {
    ull= reinterpret_cast<User_level_lock*>(my_hash_element(&thd->ull_hash, i));
    thd->mdl_context.release_lock(ull->ticket);
    my_free(ull);
  }

  my_hash_free(&thd->ull_hash);

  DBUG_VOID_RETURN;
}


/**
  Set explicit duration for metadata locks corresponding to
  user level locks to protect them from being released at the end
  of transaction.
*/

void mysql_ull_set_explicit_lock_duration(THD *thd)
{
  User_level_lock *ull;
  DBUG_ENTER("mysql_ull_set_explicit_lock_duration");

  for (ulong i= 0; i < thd->ull_hash.records; i++)
  {
    ull= reinterpret_cast<User_level_lock*>(my_hash_element(&thd->ull_hash, i));
    thd->mdl_context.set_lock_duration(ull->ticket, MDL_EXPLICIT);
  }
  DBUG_VOID_RETURN;
}


/**
  When MDL detects a lock wait timeout, it pushes an error into the statement
  diagnostics area. For GET_LOCK(), lock wait timeout is not an error, but a
  special return value (0). NULL is returned in case of error. Capture and
  suppress lock wait timeout.
  We also convert ER_LOCK_DEADLOCK error to ER_USER_LOCK_DEADLOCK error.
  The former means that implicit rollback of transaction has occurred
  which doesn't (and should not) happen when we get deadlock while waiting
  for user-level lock.
*/

class User_level_lock_wait_error_handler: public Internal_error_handler
{
public:
  User_level_lock_wait_error_handler()
    : m_lock_wait_timeout(false)
  { }

  bool got_timeout() const { return m_lock_wait_timeout; }

  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char *sqlstate,
                                Sql_condition::enum_severity_level *level,
                                const char *msg)
  {
    if (sql_errno == ER_LOCK_WAIT_TIMEOUT)
    {
      m_lock_wait_timeout= true;
      return true;
    }
    else if (sql_errno == ER_LOCK_DEADLOCK)
    {
      my_error(ER_USER_LOCK_DEADLOCK, MYF(0));
      return true;
    }

    return false;
  }

private:
  bool m_lock_wait_timeout;
};


class MDL_lock_get_owner_thread_id_visitor : public MDL_context_visitor
{
public:
  MDL_lock_get_owner_thread_id_visitor()
    : m_owner_id(0)
  { }

  void visit_context(const MDL_context *ctx)
  {
    m_owner_id= ctx->get_owner()->get_thd()->thread_id();
  }

  my_thread_id get_owner_id() const { return m_owner_id; }

private:
  my_thread_id m_owner_id;
};


/**
  Helper function which checks if user-level lock name is acceptable
  and converts it to system charset (utf8). Error is emitted if name
  is not acceptable. Name is also lowercased to ensure that user-level
  lock names are treated in case-insensitive fashion even though MDL
  subsystem which used by implementation does binary comparison of keys.

  @param buff      Buffer for lowercased name in system charset of
                   NAME_LEN + 1 bytes length.
  @param org_name  Original string passed as name parameter to
                   user-level lock function.

  @return True in case of error, false on success.
*/

static bool check_and_convert_ull_name(char *buff, String *org_name)
{
  if (!org_name || !org_name->length())
  {
    my_error(ER_USER_LOCK_WRONG_NAME, MYF(0), (org_name ? "" : "NULL"));
    return true;
  }

  const char *well_formed_error_pos;
  const char *cannot_convert_error_pos;
  const char *from_end_pos;
  size_t bytes_copied;

  bytes_copied= well_formed_copy_nchars(system_charset_info,
                                        buff, NAME_LEN,
                                        org_name->charset(),
                                        org_name->ptr(), org_name->length(),
                                        NAME_CHAR_LEN,
                                        &well_formed_error_pos,
                                        &cannot_convert_error_pos,
                                        &from_end_pos);

  if (well_formed_error_pos || cannot_convert_error_pos ||
      from_end_pos < org_name->ptr() + org_name->length())
  {
    ErrConvString err(org_name);
    my_error(ER_USER_LOCK_WRONG_NAME, MYF(0), err.ptr());
    return true;
  }

  buff[bytes_copied]= '\0';

  my_casedn_str(system_charset_info, buff);

  return false;
}


bool Item_func_get_lock::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  return false;
}


/**
  Get a user level lock.

  @note Sets null_value to TRUE on error.

  @note This means that SQL-function GET_LOCK() returns:
        1    - if lock was acquired.
        0    - if lock was not acquired due to timeout.
        NULL - in case of error such as bad lock name, deadlock,
               thread being killed (also error is emitted).

  @retval
    1    : Got lock
  @retval
    0    : Timeout, error.
*/

longlong Item_func_get_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res= args[0]->val_str(&value);
  ulonglong timeout= args[1]->val_int();
  char name[NAME_LEN + 1];
  THD *thd= current_thd;
  User_level_lock *ull;
  DBUG_ENTER("Item_func_get_lock::val_int");

  null_value= TRUE;
  /*
    In slave thread no need to get locks, everything is serialized. Anyway
    there is no way to make GET_LOCK() work on slave like it did on master
    (i.e. make it return exactly the same value) because we don't have the
    same other concurrent threads environment. No matter what we return here,
    it's not guaranteed to be same as on master. So we always return 1.
  */
  if (thd->slave_thread)
  {
    null_value= FALSE;
    DBUG_RETURN(1);
  }

  if (check_and_convert_ull_name(name, res))
    DBUG_RETURN(0);

  DBUG_PRINT("info", ("lock %s, thd=%lu", name, (ulong) thd->real_id));

  /*
    Convert too big and negative timeout values to INT_MAX32.
    This gives robust, "infinite" wait on all platforms.
  */
  if (timeout > INT_MAX32)
    timeout= INT_MAX32;

  /* HASH entries are of type User_level_lock. */
  if (! my_hash_inited(&thd->ull_hash) &&
      my_hash_init(&thd->ull_hash, &my_charset_bin,
                   16 /* small hash */, 0, ull_get_key, nullptr, 0,
                   key_memory_User_level_lock))
  {
    DBUG_RETURN(0);
  }

  MDL_request ull_request;
  MDL_REQUEST_INIT(&ull_request, MDL_key::USER_LEVEL_LOCK, "",
                   name, MDL_EXCLUSIVE, MDL_EXPLICIT);
  MDL_key *ull_key= &ull_request.key;

  if ((ull= reinterpret_cast<User_level_lock*>
         (my_hash_search(&thd->ull_hash, ull_key->ptr(), ull_key->length()))))
  {
    /* Recursive lock. */
    ull->refs++;
    null_value= FALSE;
    DBUG_RETURN(1);
  }

  User_level_lock_wait_error_handler error_handler;

  thd->push_internal_handler(&error_handler);
  bool error= thd->mdl_context.acquire_lock(&ull_request,
                                            static_cast<ulong>(timeout));
  (void) thd->pop_internal_handler();

  if (error)
  {
    /*
      Return 0 in case of timeout and NULL in case of deadlock/other
      errors. In the latter case error (e.g. ER_USER_LOCK_DEADLOCK)
      will be reported as well.
    */
    if (error_handler.got_timeout())
      null_value= FALSE;
    DBUG_RETURN(0);
  }

  ull= reinterpret_cast<User_level_lock*>(my_malloc(key_memory_User_level_lock,
                                                    sizeof(User_level_lock),
                                                    MYF(0)));

  if (ull == NULL)
  {
    thd->mdl_context.release_lock(ull_request.ticket);
    DBUG_RETURN(0);
  }

  ull->ticket= ull_request.ticket;
  ull->refs= 1;

  if (my_hash_insert(&thd->ull_hash, reinterpret_cast<uchar*>(ull)))
  {
    thd->mdl_context.release_lock(ull_request.ticket);
    my_free(ull);
    DBUG_RETURN(0);
  }

  null_value= FALSE;

  DBUG_RETURN(1);
}


bool Item_func_release_lock::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  return false;
}


/**
  Release a user level lock.

  @note Sets null_value to TRUE on error/if no connection holds such lock.

  @note This means that SQL-function RELEASE_LOCK() returns:
        1    - if lock was held by this connection and was released.
        0    - if lock was held by some other connection (and was not released).
        NULL - if name of lock is bad or if it was not held by any connection
               (in the former case also error will be emitted),

  @return
    - 1 if lock released
    - 0 if lock wasn't held/error.
*/

longlong Item_func_release_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res= args[0]->val_str(&value);
  char name[NAME_LEN + 1];
  THD *thd= current_thd;
  DBUG_ENTER("Item_func_release_lock::val_int");

  null_value= TRUE;

  if (check_and_convert_ull_name(name, res))
    DBUG_RETURN(0);

  DBUG_PRINT("info", ("lock %s", name));

  MDL_key ull_key;
  ull_key.mdl_key_init(MDL_key::USER_LEVEL_LOCK, "", name);

  User_level_lock *ull;

  if (!(ull= reinterpret_cast<User_level_lock*>
          (my_hash_search(&thd->ull_hash, ull_key.ptr(), ull_key.length()))))
  {
    /*
      When RELEASE_LOCK() is called for lock which is not owned by the
      connection it should return 0 or NULL depending on whether lock
      is owned by any other connection or not.
    */
    MDL_lock_get_owner_thread_id_visitor get_owner_visitor;

    if (thd->mdl_context.find_lock_owner(&ull_key, &get_owner_visitor))
      DBUG_RETURN(0);

    null_value= get_owner_visitor.get_owner_id() == 0;

    DBUG_RETURN(0);
  }
  null_value= FALSE;
  if (--ull->refs == 0)
  {
    my_hash_delete(&thd->ull_hash, reinterpret_cast<uchar*>(ull));
    thd->mdl_context.release_lock(ull->ticket);
    my_free(ull);
  }
  DBUG_RETURN(1);
}


bool Item_func_release_all_locks::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  return false;
}


/**
  Release all user level lock held by connection.

  @return Number of locks released including recursive lock count.
*/

longlong Item_func_release_all_locks::val_int()
{
  DBUG_ASSERT(fixed == 1);
  THD *thd= current_thd;
  uint result= 0;
  User_level_lock *ull;
  DBUG_ENTER("Item_func_release_all_locks::val_int");

  if (my_hash_inited(&thd->ull_hash))
  {
    for (ulong i= 0; i < thd->ull_hash.records; i++)
    {
      ull= reinterpret_cast<User_level_lock*>(my_hash_element(&thd->ull_hash,
                                                              i));
      thd->mdl_context.release_lock(ull->ticket);
      result+= ull->refs;
      my_free(ull);
    }
    my_hash_reset(&thd->ull_hash);
  }

  DBUG_RETURN(result);
}


bool Item_func_is_free_lock::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  return false;
}


/**
  Check if user level lock is free.

  @note Sets null_value=TRUE on error.

  @note As result SQL-function IS_FREE_LOCK() returns:
        1    - if lock is free,
        0    - if lock is in use
        NULL - if lock name is bad or OOM (also error is emitted).

  @retval
    1		Available
  @retval
    0		Already taken, or error
*/

longlong Item_func_is_free_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res= args[0]->val_str(&value);
  char name[NAME_LEN + 1];
  THD *thd= current_thd;

  null_value= TRUE;

  if (check_and_convert_ull_name(name, res))
    return 0;

  MDL_key ull_key;
  ull_key.mdl_key_init(MDL_key::USER_LEVEL_LOCK, "", name);

  MDL_lock_get_owner_thread_id_visitor get_owner_visitor;

  if (thd->mdl_context.find_lock_owner(&ull_key, &get_owner_visitor))
    return 0;

  null_value= FALSE;
  return MY_TEST(get_owner_visitor.get_owner_id() == 0);
}


bool Item_func_is_used_lock::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  return false;
}


/**
  Check if user level lock is used and return connection id of owner.

  @note Sets null_value=TRUE if lock is free/on error.

  @note SQL-function IS_USED_LOCK() returns:
        #    - connection id of lock owner if lock is acquired.
        NULL - if lock is free or on error (in the latter case
               also error is emitted).

  @return Connection id of lock owner, 0 if lock is free/on error.
*/

longlong Item_func_is_used_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res= args[0]->val_str(&value);
  char name[NAME_LEN + 1];
  THD *thd= current_thd;

  null_value= TRUE;

  if (check_and_convert_ull_name(name, res))
    return 0;

  MDL_key ull_key;
  ull_key.mdl_key_init(MDL_key::USER_LEVEL_LOCK, "", name);

  MDL_lock_get_owner_thread_id_visitor get_owner_visitor;

  if (thd->mdl_context.find_lock_owner(&ull_key, &get_owner_visitor))
    return 0;

  my_thread_id thread_id= get_owner_visitor.get_owner_id();
  if (thread_id == 0)
    return 0;

  null_value= FALSE;
  return thread_id;
}


bool Item_func_last_insert_id::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->safe_to_cache_query= false;
  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  return false;
}


longlong Item_func_last_insert_id::val_int()
{
  THD *thd= current_thd;
  DBUG_ASSERT(fixed == 1);
  if (arg_count)
  {
    longlong value= args[0]->val_int();
    null_value= args[0]->null_value;
    /*
      LAST_INSERT_ID(X) must affect the client's mysql_insert_id() as
      documented in the manual. We don't want to touch
      first_successful_insert_id_in_cur_stmt because it would make
      LAST_INSERT_ID(X) take precedence over an generated auto_increment
      value for this row.
    */
    thd->arg_of_last_insert_id_function= TRUE;
    thd->first_successful_insert_id_in_prev_stmt= value;
    return value;
  }
  return
    static_cast<longlong>(thd->read_first_successful_insert_id_in_prev_stmt());
}


bool Item_func_benchmark::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  return false;
}


/* This function is just used to test speed of different functions */

longlong Item_func_benchmark::val_int()
{
  DBUG_ASSERT(fixed == 1);
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff,sizeof(buff), &my_charset_bin);
  my_decimal tmp_decimal;
  THD *thd=current_thd;
  ulonglong loop_count;

  loop_count= (ulonglong) args[0]->val_int();

  if (args[0]->null_value ||
      (!args[0]->unsigned_flag && (((longlong) loop_count) < 0)))
  {
    if (!args[0]->null_value)
    {
      char buff[22];
      llstr(((longlong) loop_count), buff);
      push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                          ER_WRONG_VALUE_FOR_TYPE,
                          ER_THD(current_thd, ER_WRONG_VALUE_FOR_TYPE),
                          "count", buff, "benchmark");
    }

    null_value= 1;
    return 0;
  }

  null_value=0;
  for (ulonglong loop=0 ; loop < loop_count && !thd->killed; loop++)
  {
    switch (args[1]->result_type()) {
    case REAL_RESULT:
      (void) args[1]->val_real();
      break;
    case INT_RESULT:
      (void) args[1]->val_int();
      break;
    case STRING_RESULT:
      (void) args[1]->val_str(&tmp);
      break;
    case DECIMAL_RESULT:
      (void) args[1]->val_decimal(&tmp_decimal);
      break;
    case ROW_RESULT:
    default:
      // This case should never be chosen
      DBUG_ASSERT(0);
      return 0;
    }
  }
  return 0;
}


void Item_func_benchmark::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("benchmark("));
  args[0]->print(str, query_type);
  str->append(',');
  args[1]->print(str, query_type);
  str->append(')');
}


/**
  Lock which is used to implement interruptible wait for SLEEP() function.
*/

mysql_mutex_t LOCK_item_func_sleep;


#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_item_func_sleep;


static PSI_mutex_info item_func_sleep_mutexes[]=
{
  { &key_LOCK_item_func_sleep, "LOCK_item_func_sleep", PSI_FLAG_GLOBAL, 0}
};


static void init_item_func_sleep_psi_keys()
{
  int count;

  count= array_elements(item_func_sleep_mutexes);
  mysql_mutex_register("sql", item_func_sleep_mutexes, count);
}
#endif


static bool item_func_sleep_inited= false;


void item_func_sleep_init()
{
#ifdef HAVE_PSI_INTERFACE
  init_item_func_sleep_psi_keys();
#endif

  mysql_mutex_init(key_LOCK_item_func_sleep, &LOCK_item_func_sleep, MY_MUTEX_INIT_SLOW);
  item_func_sleep_inited= true;
}


void item_func_sleep_free()
{
  if (item_func_sleep_inited)
  {
    item_func_sleep_inited= false;
    mysql_mutex_destroy(&LOCK_item_func_sleep);
  }
}


bool Item_func_sleep::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  return false;
}


/** This function is just used to create tests with time gaps. */

longlong Item_func_sleep::val_int()
{
  THD *thd= current_thd;
  Interruptible_wait timed_cond(thd);
  mysql_cond_t cond;
  double timeout;
  int error;

  DBUG_ASSERT(fixed == 1);

  timeout= args[0]->val_real();
 
  /*
    Report error or warning depending on the value of SQL_MODE.
    If SQL is STRICT then report error, else report warning and continue
    execution.
  */

  if (args[0]->null_value || timeout < 0)
  {
    if (!thd->lex->is_ignore() && thd->is_strict_mode())
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), "sleep.");
      return 0;
    }
    else
      push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                          ER_THD(thd, ER_WRONG_ARGUMENTS), "sleep.");
  }
  /*
    On 64-bit OSX mysql_cond_timedwait() waits forever
    if passed abstime time has already been exceeded by 
    the system time.
    When given a very short timeout (< 10 mcs) just return 
    immediately.
    We assume that the lines between this test and the call 
    to mysql_cond_timedwait() will be executed in less than 0.00001 sec.
  */
  if (timeout < 0.00001)
    return 0;

  timed_cond.set_timeout((ulonglong) (timeout * 1000000000.0));

  mysql_cond_init(key_item_func_sleep_cond, &cond);
  mysql_mutex_lock(&LOCK_item_func_sleep);

  thd->ENTER_COND(&cond, &LOCK_item_func_sleep, &stage_user_sleep, NULL);

  error= 0;
  thd_wait_begin(thd, THD_WAIT_SLEEP);
  while (!thd->killed)
  {
    error= timed_cond.wait(&cond, &LOCK_item_func_sleep);
    if (is_timeout(error))
      break;
    error= 0;
  }
  thd_wait_end(thd);
  mysql_mutex_unlock(&LOCK_item_func_sleep);
  thd->EXIT_COND(NULL);

  mysql_cond_destroy(&cond);

  return MY_TEST(!error); 		// Return 1 killed
}

/*
  @param cs  character set; IF we are creating the user_var_entry,
             we give it this character set.
*/
static user_var_entry *get_variable(THD *thd, const Name_string &name,
                                    const CHARSET_INFO *cs)
{
  user_var_entry *entry;
  HASH *hash= & thd->user_vars;

  /* Protects thd->user_vars. */
  mysql_mutex_assert_owner(&thd->LOCK_thd_data);

  if (!(entry= (user_var_entry*) my_hash_search(hash, (uchar*) name.ptr(),
                                                 name.length())) &&
        cs != NULL)
  {
    if (!my_hash_inited(hash))
      return 0;
    if (!(entry= user_var_entry::create(thd, name, cs)))
      return 0;
    if (my_hash_insert(hash,(uchar*) entry))
    {
      my_free(entry);
      return 0;
    }
  }
  return entry;
}


void Item_func_set_user_var::cleanup()
{
  Item_func::cleanup();
  entry= NULL;
}


bool Item_func_set_user_var::set_entry(THD *thd, bool create_if_not_exists)
{
  if (entry && thd->thread_id() == entry_thread_id)
  {} // update entry->update_query_id for PS
  else
  {
    const CHARSET_INFO *cs=  create_if_not_exists ?
          (args[0]->collation.derivation == DERIVATION_NUMERIC ?
          default_charset() : args[0]->collation.collation) : NULL;

    /* Protects thd->user_vars. */
    mysql_mutex_lock(&thd->LOCK_thd_data);
    entry= get_variable(thd, name, cs);
    mysql_mutex_unlock(&thd->LOCK_thd_data);

    if (entry == NULL)
    {
      entry_thread_id= 0;
      return TRUE;
    }
    entry_thread_id= thd->thread_id();
  }
  /* 
    Remember the last query which updated it, this way a query can later know
    if this variable is a constant item in the query (it is if update_query_id
    is different from query_id).

    If this object has delayed setting of non-constness, we delay this
    until Item_func_set-user_var::save_item_result().
  */
  if (!delayed_non_constness)
    entry->update_query_id= thd->query_id;
  return FALSE;
}


/*
  When a user variable is updated (in a SET command or a query like
  SELECT @a:= ).
*/

bool Item_func_set_user_var::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  // fix_fields will call Item_func_set_user_var::resolve_type()
  if (Item_func::fix_fields(thd, ref) || set_entry(thd, TRUE))
    return TRUE;

  null_item= (args[0]->type() == NULL_ITEM);

  cached_result_type= args[0]->result_type();
  return FALSE;
}


bool Item_func_set_user_var::resolve_type(THD *thd)
{
  maybe_null=args[0]->maybe_null;
  decimals=args[0]->decimals;
  collation.set(DERIVATION_IMPLICIT);
  /* 
     this sets the character set of the item immediately; rules for the
     character set of the variable ("entry" object) are different: if "entry"
     did not exist previously, set_entry () has created it and has set its 
     character set; but if it existed previously, it keeps its previous 
     character set, which may change only when we are sure that the assignment
     is to be executed, i.e. in user_var_entry::store ().
  */
  if (args[0]->collation.derivation == DERIVATION_NUMERIC)
    fix_length_and_charset(args[0]->max_char_length(), default_charset());
  else
  {
    fix_length_and_charset(args[0]->max_char_length(),
                           args[0]->collation.collation);
  }
  unsigned_flag= args[0]->unsigned_flag;
  return false;
}


// static
user_var_entry* user_var_entry::create(THD *thd,
                                       const Name_string &name,
                                       const CHARSET_INFO *cs)
{
  if (check_column_name(name.ptr()))
  {
    my_error(ER_ILLEGAL_USER_VAR, MYF(0), name.ptr());
    return NULL;
  }

  user_var_entry *entry;
  size_t size= ALIGN_SIZE(sizeof(user_var_entry)) +
    (name.length() + 1) + extra_size;
  if (!(entry= (user_var_entry*) my_malloc(key_memory_user_var_entry,
                                           size, MYF(MY_WME |
                                                     ME_FATALERROR))))
    return NULL;
  entry->init(thd, name, cs);
  return entry;
}


bool user_var_entry::mem_realloc(size_t length)
{
  if (length <= extra_size)
  {
    /* Enough space to store value in value struct */
    free_value();
    m_ptr= internal_buffer_ptr();
  }
  else
  {
    /* Allocate an external buffer */
    if (m_length != length)
    {
      if (m_ptr == internal_buffer_ptr())
        m_ptr= 0;
      if (!(m_ptr= (char*) my_realloc(key_memory_user_var_entry_value,
                                      m_ptr, length,
                                      MYF(MY_ALLOW_ZERO_PTR | MY_WME |
                                      ME_FATALERROR))))
        return true;
    }
  }
  return false;
}


void user_var_entry::init(THD *thd, const Simple_cstring &name,
                          const CHARSET_INFO *cs)
{
  DBUG_ASSERT(thd != NULL);
  m_owner= thd;
  copy_name(name);
  reset_value();
  update_query_id= 0;
  collation.set(cs, DERIVATION_IMPLICIT, 0);
  unsigned_flag= 0;
  /*
    If we are here, we were called from a SET or a query which sets a
    variable. Imagine it is this:
    INSERT INTO t SELECT @a:=10, @a:=@a+1.
    Then when we have a Item_func_get_user_var (because of the @a+1) so we
    think we have to write the value of @a to the binlog. But before that,
    we have a Item_func_set_user_var to create @a (@a:=10), in this we mark
    the variable as "already logged" (line below) so that it won't be logged
    by Item_func_get_user_var (because that's not necessary).
  */
  used_query_id= thd->query_id;
  m_type= STRING_RESULT;
}


/**
  Set value to user variable.
  @param from           pointer to buffer with new value
  @param length         length of new value
  @param type           type of new value

  @retval  false   on success
  @retval  true    on allocation error

*/
bool user_var_entry::store(const void *from, size_t length, Item_result type)
{
  assert_locked();

  // Store strings with end \0
  if (mem_realloc(length + MY_TEST(type == STRING_RESULT)))
    return true;
  if (type == STRING_RESULT)
    m_ptr[length]= 0;     // Store end \0

  // Avoid memcpy of a my_decimal object, use copy CTOR instead.
  if (type == DECIMAL_RESULT)
  {
    DBUG_ASSERT(length == sizeof(my_decimal));
    const my_decimal* dec= static_cast<const my_decimal*>(from);
    dec->sanity_check();
    new (m_ptr) my_decimal(*dec);
  }
  else
    memcpy(m_ptr, from, length);

  m_length= length;
  m_type= type;
  return false;
}


void user_var_entry::assert_locked() const
{
  mysql_mutex_assert_owner(&m_owner->LOCK_thd_data);
}


/**
  Set value to user variable.

  @param ptr            pointer to buffer with new value
  @param length         length of new value
  @param type           type of new value
  @param cs             charset info for new value
  @param dv             derivation for new value
  @param unsigned_arg   indiates if a value of type INT_RESULT is unsigned

  @note Sets error and fatal error if allocation fails.

  @retval
    false   success
  @retval
    true    failure
*/

bool user_var_entry::store(const void *ptr, size_t length, Item_result type,
                           const CHARSET_INFO *cs, Derivation dv,
                           bool unsigned_arg)
{
  assert_locked();

  if (store(ptr, length, type))
    return true;
  collation.set(cs, dv);
  unsigned_flag= unsigned_arg;
  return false;
}

void user_var_entry::lock()
{
  DBUG_ASSERT(m_owner != NULL);
  mysql_mutex_lock(&m_owner->LOCK_thd_data);
}

void user_var_entry::unlock()
{
  DBUG_ASSERT(m_owner != NULL);
  mysql_mutex_unlock(&m_owner->LOCK_thd_data);
}

bool
Item_func_set_user_var::update_hash(const void *ptr, uint length,
                                    Item_result res_type,
                                    const CHARSET_INFO *cs, Derivation dv,
                                    bool unsigned_arg)
{
  entry->lock();

  /*
    If we set a variable explicitely to NULL then keep the old
    result type of the variable
  */
  // args[0]->null_value could be outdated
  if (args[0]->type() == Item::FIELD_ITEM)
    null_value= ((Item_field*)args[0])->field->is_null();
  else
    null_value= args[0]->null_value;

  if (ptr == NULL)
  {
    DBUG_ASSERT(length == 0);
    null_value= true;
  }

  if (null_value && null_item)
    res_type= entry->type();                    // Don't change type of item

  if (null_value)
    entry->set_null_value(res_type);
  else if (entry->store(ptr, length, res_type, cs, dv, unsigned_arg))
  {
    entry->unlock();
    null_value= 1;
    return 1;
  }
  entry->unlock();
  return 0;
}


/** Get the value of a variable as a double. */

double user_var_entry::val_real(my_bool *null_value) const
{
  if ((*null_value= (m_ptr == 0)))
    return 0.0;

  switch (m_type) {
  case REAL_RESULT:
    return *(double*) m_ptr;
  case INT_RESULT:
    return (double) *(longlong*) m_ptr;
  case DECIMAL_RESULT:
  {
    double result;
    my_decimal2double(E_DEC_FATAL_ERROR, (my_decimal *) m_ptr, &result);
    return result;
  }
  case STRING_RESULT:
    return my_atof(m_ptr);                    // This is null terminated
  case ROW_RESULT:
  case INVALID_RESULT:
    DBUG_ASSERT(false);                         // Impossible
    break;
  }
  return 0.0;					// Impossible
}


/** Get the value of a variable as an integer. */

longlong user_var_entry::val_int(my_bool *null_value) const
{
  if ((*null_value= (m_ptr == 0)))
    return 0LL;

  switch (m_type) {
  case REAL_RESULT:
    return (longlong) *(double*) m_ptr;
  case INT_RESULT:
    return *(longlong*) m_ptr;
  case DECIMAL_RESULT:
  {
    longlong result;
    my_decimal2int(E_DEC_FATAL_ERROR, (my_decimal *) m_ptr, 0, &result);
    return result;
  }
  case STRING_RESULT:
  {
    int error;
    return my_strtoll10(m_ptr, (char**) 0, &error);// String is null terminated
  }
  case ROW_RESULT:
  case INVALID_RESULT:
    DBUG_ASSERT(false);                         // Impossible
    break;
  }
  return 0LL;					// Impossible
}


/** Get the value of a variable as a string. */

String *user_var_entry::val_str(my_bool *null_value, String *str,
				uint decimals) const
{
  if ((*null_value= (m_ptr == 0)))
    return (String*) 0;

  switch (m_type) {
  case REAL_RESULT:
    str->set_real(*(double*) m_ptr, decimals, collation.collation);
    break;
  case INT_RESULT:
    if (!unsigned_flag)
      str->set(*(longlong*) m_ptr, collation.collation);
    else
      str->set(*(ulonglong*) m_ptr, collation.collation);
    break;
  case DECIMAL_RESULT:
    str_set_decimal((my_decimal *) m_ptr, str, collation.collation);
    break;
  case STRING_RESULT:
    if (str->copy(m_ptr, m_length, collation.collation))
      str= 0;					// EOM error
    break;
  case ROW_RESULT:
  case INVALID_RESULT:
    DBUG_ASSERT(false);                         // Impossible
    break;
  }
  return(str);
}

/** Get the value of a variable as a decimal. */

my_decimal *user_var_entry::val_decimal(my_bool *null_value, my_decimal *val) const
{
  if ((*null_value= (m_ptr == 0)))
    return 0;

  switch (m_type) {
  case REAL_RESULT:
    double2my_decimal(E_DEC_FATAL_ERROR, *(double*) m_ptr, val);
    break;
  case INT_RESULT:
    int2my_decimal(E_DEC_FATAL_ERROR, *(longlong*) m_ptr, 0, val);
    break;
  case DECIMAL_RESULT:
    my_decimal2decimal((my_decimal *) m_ptr, val);
    break;
  case STRING_RESULT:
    str2my_decimal(E_DEC_FATAL_ERROR, m_ptr, m_length,
                   collation.collation, val);
    break;
  case ROW_RESULT:
  case INVALID_RESULT:
    DBUG_ASSERT(false);                         // Impossible
    break;
  }
  return(val);
}

/**
  This functions is invoked on SET \@variable or
  \@variable:= expression.

  Evaluate (and check expression), store results.

  @note
    For now it always return OK. All problem with value evaluating
    will be caught by thd->is_error() check in sql_set_variables().

  @retval
    FALSE OK.
*/

bool
Item_func_set_user_var::check(bool use_result_field)
{
  DBUG_ENTER("Item_func_set_user_var::check");
  if (use_result_field && !result_field)
    use_result_field= FALSE;

  switch (cached_result_type) {
  case REAL_RESULT:
  {
    save_result.vreal= use_result_field ? result_field->val_real() :
                        args[0]->val_real();
    break;
  }
  case INT_RESULT:
  {
    save_result.vint= use_result_field ? result_field->val_int() :
                       args[0]->val_int();
    unsigned_flag= use_result_field ? ((Field_num*)result_field)->unsigned_flag:
                    args[0]->unsigned_flag;
    break;
  }
  case STRING_RESULT:
  {
    save_result.vstr= use_result_field ? result_field->val_str(&value) :
                       args[0]->val_str(&value);
    break;
  }
  case DECIMAL_RESULT:
  {
    save_result.vdec= use_result_field ?
                       result_field->val_decimal(&decimal_buff) :
                       args[0]->val_decimal(&decimal_buff);
    break;
  }
  case ROW_RESULT:
  default:
    // This case should never be chosen
    DBUG_ASSERT(0);
    break;
  }
  DBUG_RETURN(FALSE);
}


/**
  @brief Evaluate and store item's result.
  This function is invoked on "SELECT ... INTO @var ...".
  
  @param    item    An item to get value from.
*/

void Item_func_set_user_var::save_item_result(Item *item)
{
  DBUG_ENTER("Item_func_set_user_var::save_item_result");

  switch (cached_result_type) {
  case REAL_RESULT:
    save_result.vreal= item->val_result();
    break;
  case INT_RESULT:
    save_result.vint= item->val_int_result();
    unsigned_flag= item->unsigned_flag;
    break;
  case STRING_RESULT:
    save_result.vstr= item->str_result(&value);
    break;
  case DECIMAL_RESULT:
    save_result.vdec= item->val_decimal_result(&decimal_buff);
    break;
  case ROW_RESULT:
  default:
    // Should never happen
    DBUG_ASSERT(0);
    break;
  }
  /*
    Set the ID of the query that last updated this variable. This is
    usually set by Item_func_set_user_var::set_entry(), but if this
    item has delayed setting of non-constness, we must do it now.
   */
  if (delayed_non_constness)
    entry->update_query_id= current_thd->query_id;
  DBUG_VOID_RETURN;
}


/**
  This functions is invoked on
  SET \@variable or \@variable:= expression.

  @note
    We have to store the expression as such in the variable, independent of
    the value method used by the user

  @retval
    0	OK
  @retval
    1	EOM Error

*/

bool
Item_func_set_user_var::update()
{
  bool res= 0;
  DBUG_ENTER("Item_func_set_user_var::update");

  switch (cached_result_type) {
  case REAL_RESULT:
  {
    res= update_hash(&save_result.vreal,sizeof(save_result.vreal),
		     REAL_RESULT, default_charset(), DERIVATION_IMPLICIT, 0);
    break;
  }
  case INT_RESULT:
  {
    res= update_hash(&save_result.vint, sizeof(save_result.vint),
                     INT_RESULT, default_charset(), DERIVATION_IMPLICIT,
                     unsigned_flag);
    break;
  }
  case STRING_RESULT:
  {
    if (!save_result.vstr)					// Null value
      res= update_hash(NULL, 0, STRING_RESULT, &my_charset_bin,
		       DERIVATION_IMPLICIT, 0);
    else
      res= update_hash(save_result.vstr->ptr(),
		       save_result.vstr->length(), STRING_RESULT,
		       save_result.vstr->charset(),
		       DERIVATION_IMPLICIT, 0);
    break;
  }
  case DECIMAL_RESULT:
  {
    if (!save_result.vdec)					// Null value
      res= update_hash(NULL, 0, DECIMAL_RESULT, &my_charset_bin,
                       DERIVATION_IMPLICIT, false);
    else
      res= update_hash(save_result.vdec,
                       sizeof(my_decimal), DECIMAL_RESULT,
                       default_charset(), DERIVATION_IMPLICIT, 0);
    break;
  }
  case ROW_RESULT:
  default:
    // This case should never be chosen
    DBUG_ASSERT(0);
    break;
  }
  DBUG_RETURN(res);
}


double Item_func_set_user_var::val_real()
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return entry->val_real(&null_value);
}

longlong Item_func_set_user_var::val_int()
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return entry->val_int(&null_value);
}

String *Item_func_set_user_var::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return entry->val_str(&null_value, str, decimals);
}


my_decimal *Item_func_set_user_var::val_decimal(my_decimal *val)
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return entry->val_decimal(&null_value, val);
}


double Item_func_set_user_var::val_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return entry->val_real(&null_value);
}

longlong Item_func_set_user_var::val_int_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return entry->val_int(&null_value);
}

bool Item_func_set_user_var::val_bool_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return entry->val_int(&null_value) != 0;
}

String *Item_func_set_user_var::str_result(String *str)
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return entry->val_str(&null_value, str, decimals);
}


my_decimal *Item_func_set_user_var::val_decimal_result(my_decimal *val)
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return entry->val_decimal(&null_value, val);
}


bool Item_func_set_user_var::is_null_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return is_null();
}

// just the assignment, for use in "SET @a:=5" type self-prints
void Item_func_set_user_var::print_assignment(String *str,
                                              enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("@"));
  str->append(name);
  str->append(STRING_WITH_LEN(":="));
  args[0]->print(str, query_type);
}

// parenthesize assignment for use in "EXPLAIN EXTENDED SELECT (@e:=80)+5"
void Item_func_set_user_var::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("("));
  print_assignment(str, query_type);
  str->append(STRING_WITH_LEN(")"));
}

bool Item_func_set_user_var::send(Protocol *protocol, String *str_arg)
{
  if (result_field)
  {
    check(1);
    update();
    /*
      Workaround for metadata check in Protocol_text. Legacy Protocol_text
      is so well designed that it sends fields in text format, and functions'
      results in binary format. When this func tries to send its data as a
      field it breaks metadata asserts in the P_text.
      TODO This func have to be changed to avoid sending data as a field.
    */
    return result_field->send_binary(protocol);
  }
  return Item::send(protocol, str_arg);
}

void Item_func_set_user_var::make_field(Send_field *tmp_field)
{
  if (result_field)
  {
    result_field->make_field(tmp_field);
    DBUG_ASSERT(tmp_field->table_name != 0);
    if (Item::item_name.is_set())
      tmp_field->col_name=Item::item_name.ptr();    // Use user supplied name
  }
  else
    Item::make_field(tmp_field);
}


/*
  Save the value of a user variable into a field

  SYNOPSIS
    save_in_field()
      field           target field to save the value to
      no_conversion   flag indicating whether conversions are allowed

  DESCRIPTION
    Save the function value into a field and update the user variable
    accordingly. If a result field is defined and the target field doesn't
    coincide with it then the value from the result field will be used as
    the new value of the user variable.

    The reason to have this method rather than simply using the result
    field in the val_xxx() methods is that the value from the result field
    not always can be used when the result field is defined.
    Let's consider the following cases:
    1) when filling a tmp table the result field is defined but the value of it
    is undefined because it has to be produced yet. Thus we can't use it.
    2) on execution of an INSERT ... SELECT statement the save_in_field()
    function will be called to fill the data in the new record. If the SELECT
    part uses a tmp table then the result field is defined and should be
    used in order to get the correct result.

    The difference between the SET_USER_VAR function and regular functions
    like CONCAT is that the Item_func objects for the regular functions are
    replaced by Item_field objects after the values of these functions have
    been stored in a tmp table. Yet an object of the Item_field class cannot
    be used to update a user variable.
    Due to this we have to handle the result field in a special way here and
    in the Item_func_set_user_var::send() function.

  RETURN VALUES
    FALSE       Ok
    TRUE        Error
*/

type_conversion_status
Item_func_set_user_var::save_in_field(Field *field, bool no_conversions,
                                      bool can_use_result_field)
{
  bool use_result_field= (!can_use_result_field ? 0 :
                          (result_field && result_field != field));
  type_conversion_status error;

  /* Update the value of the user variable */
  check(use_result_field);
  update();

  if (result_type() == STRING_RESULT ||
      (result_type() == REAL_RESULT &&
      field->result_type() == STRING_RESULT))
  {
    String *result;
    const CHARSET_INFO *cs= collation.collation;
    char buff[MAX_FIELD_WIDTH];		// Alloc buffer for small columns
    str_value.set_quick(buff, sizeof(buff), cs);
    result= entry->val_str(&null_value, &str_value, decimals);

    if (null_value)
    {
      str_value.set_quick(0, 0, cs);
      return set_field_to_null_with_conversions(field, no_conversions);
    }

    /* NOTE: If null_value == FALSE, "result" must be not NULL.  */

    field->set_notnull();
    error=field->store(result->ptr(),result->length(),cs);
    str_value.set_quick(0, 0, cs);
  }
  else if (result_type() == REAL_RESULT)
  {
    double nr= entry->val_real(&null_value);
    if (null_value)
      return set_field_to_null(field);
    field->set_notnull();
    error=field->store(nr);
  }
  else if (result_type() == DECIMAL_RESULT)
  {
    my_decimal decimal_value;
    my_decimal *val= entry->val_decimal(&null_value, &decimal_value);
    if (null_value)
      return set_field_to_null(field);
    field->set_notnull();
    error=field->store_decimal(val);
  }
  else
  {
    longlong nr= entry->val_int(&null_value);
    if (null_value)
      return set_field_to_null_with_conversions(field, no_conversions);
    field->set_notnull();
    error=field->store(nr, unsigned_flag);
  }
  return error;
}


String *
Item_func_get_user_var::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_get_user_var::val_str");
  if (!var_entry)
    DBUG_RETURN((String*) 0);			// No such variable
  String *res= var_entry->val_str(&null_value, str, decimals);
  DBUG_RETURN(res);
}


double Item_func_get_user_var::val_real()
{
  DBUG_ASSERT(fixed == 1);
  if (!var_entry)
    return 0.0;					// No such variable
  return (var_entry->val_real(&null_value));
}


my_decimal *Item_func_get_user_var::val_decimal(my_decimal *dec)
{
  DBUG_ASSERT(fixed == 1);
  if (!var_entry)
    return 0;
  return var_entry->val_decimal(&null_value, dec);
}


longlong Item_func_get_user_var::val_int()
{
  DBUG_ASSERT(fixed == 1);
  if (!var_entry)
    return 0LL;				// No such variable
  return (var_entry->val_int(&null_value));
}


/**
  Get variable by name and, if necessary, put the record of variable 
  use into the binary log.

  When a user variable is invoked from an update query (INSERT, UPDATE etc),
  stores this variable and its value in thd->user_var_events, so that it can be
  written to the binlog (will be written just before the query is written, see
  log.cc).

  @param      thd         Current session.
  @param      sql_command The command the variable participates in.
  @param      name        Variable name
  @param[out] out_entry  variable structure or NULL. The pointer is set
                         regardless of whether function succeeded or not.

  @retval
    0  OK
  @retval
    1  Failed to put appropriate record into binary log

*/

static int
get_var_with_binlog(THD *thd, enum_sql_command sql_command,
                    Name_string &name, user_var_entry **out_entry)
{
  Binlog_user_var_event *user_var_event;
  user_var_entry *var_entry;

  /* Protects thd->user_vars. */
  mysql_mutex_lock(&thd->LOCK_thd_data);
  var_entry= get_variable(thd, name, NULL);
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  /*
    Any reference to user-defined variable which is done from stored
    function or trigger affects their execution and the execution of the
    calling statement. We must log all such variables even if they are 
    not involved in table-updating statements.
  */
  if (!(opt_bin_log && 
       (is_update_query(sql_command) || thd->in_sub_stmt)))
  {
    *out_entry= var_entry;
    return 0;
  }

  if (!var_entry)
  {
    /*
      If the variable does not exist, it's NULL, but we want to create it so
      that it gets into the binlog (if it didn't, the slave could be
      influenced by a variable of the same name previously set by another
      thread).
      We create it like if it had been explicitly set with SET before.
      The 'new' mimics what sql_yacc.yy does when 'SET @a=10;'.
      sql_set_variables() is what is called from 'case SQLCOM_SET_OPTION'
      in dispatch_command()). Instead of building a one-element list to pass to
      sql_set_variables(), we could instead manually call check() and update();
      this would save memory and time; but calling sql_set_variables() makes
      one unique place to maintain (sql_set_variables()). 

      Manipulation with lex is necessary since free_underlaid_joins
      is going to release memory belonging to the main query.
    */

    List<set_var_base> tmp_var_list;
    LEX *sav_lex= thd->lex, lex_tmp;
    thd->lex= &lex_tmp;
    lex_start(thd);
    tmp_var_list.push_back(new set_var_user(new Item_func_set_user_var(name,
                                                                       new Item_null(),
                                                                       false)));
    /* Create the variable */
    if (sql_set_variables(thd, &tmp_var_list, false))
    {
      thd->lex= sav_lex;
      goto err;
    }
    thd->lex= sav_lex;
    mysql_mutex_lock(&thd->LOCK_thd_data);
    var_entry= get_variable(thd, name, NULL);
    mysql_mutex_unlock(&thd->LOCK_thd_data);

    if (var_entry == NULL)
      goto err;
  }
  else if (var_entry->used_query_id == thd->query_id ||
           mysql_bin_log.is_query_in_union(thd, var_entry->used_query_id))
  {
    /* 
       If this variable was already stored in user_var_events by this query
       (because it's used in more than one place in the query), don't store
       it.
    */
    *out_entry= var_entry;
    return 0;
  }

  size_t size;
  /*
    First we need to store value of var_entry, when the next situation
    appears:
    > set @a:=1;
    > insert into t1 values (@a), (@a:=@a+1), (@a:=@a+1);
    We have to write to binlog value @a= 1.

    We allocate the user_var_event on user_var_events_alloc pool, not on
    the this-statement-execution pool because in SPs user_var_event objects 
    may need to be valid after current [SP] statement execution pool is
    destroyed.
  */
  size= ALIGN_SIZE(sizeof(Binlog_user_var_event)) + var_entry->length();
  if (!(user_var_event= (Binlog_user_var_event *)
        alloc_root(thd->user_var_events_alloc, size)))
    goto err;

  user_var_event->value= (char*) user_var_event +
    ALIGN_SIZE(sizeof(Binlog_user_var_event));
  user_var_event->user_var_event= var_entry;
  user_var_event->type= var_entry->type();
  user_var_event->charset_number= var_entry->collation.collation->number;
  user_var_event->unsigned_flag= var_entry->unsigned_flag;
  if (!var_entry->ptr())
  {
    /* NULL value*/
    user_var_event->length= 0;
    user_var_event->value= 0;
  }
  else
  {
    // Avoid memcpy of a my_decimal object, use copy CTOR instead.
    user_var_event->length= var_entry->length();
    if (user_var_event->type == DECIMAL_RESULT)
    {
      DBUG_ASSERT(var_entry->length() == sizeof(my_decimal));
      const my_decimal* dec=
        static_cast<const my_decimal*>
        (static_cast<const void*>(var_entry->ptr()));
      dec->sanity_check();
      new (user_var_event->value) my_decimal(*dec);
    }
    else
      memcpy(user_var_event->value, var_entry->ptr(),
             var_entry->length());
  }
  /* Mark that this variable has been used by this query */
  var_entry->used_query_id= thd->query_id;
  if (thd->user_var_events.push_back(user_var_event))
    goto err;

  *out_entry= var_entry;
  return 0;

err:
  *out_entry= var_entry;
  return 1;
}

bool Item_func_get_user_var::resolve_type(THD *thd)
{
  maybe_null=1;
  decimals=NOT_FIXED_DEC;
  max_length=MAX_BLOB_WIDTH;

  if (get_var_with_binlog(thd, thd->lex->sql_command, name, &var_entry))
    return true;

  /*
    If the variable didn't exist it has been created as a STRING-type.
    'var_entry' is NULL only if there occurred an error during the call to
    get_var_with_binlog.
  */
  if (var_entry)
  {
    m_cached_result_type= var_entry->type();
    unsigned_flag= var_entry->unsigned_flag;
    max_length= var_entry->length();

    collation.set(var_entry->collation);
    switch(m_cached_result_type) {
    case REAL_RESULT:
      fix_char_length(DBL_DIG + 8);
      break;
    case INT_RESULT:
      fix_char_length(MAX_BIGINT_WIDTH);
      decimals=0;
      break;
    case STRING_RESULT:
      max_length= MAX_BLOB_WIDTH - 1;
      break;
    case DECIMAL_RESULT:
      fix_char_length(DECIMAL_MAX_STR_LENGTH);
      decimals= DECIMAL_MAX_SCALE;
      break;
    case ROW_RESULT:                            // Keep compiler happy
    default:
      DBUG_ASSERT(0);
      break;
    }
  }
  else
  {
    collation.set(&my_charset_bin, DERIVATION_IMPLICIT);
    null_value= 1;
    m_cached_result_type= STRING_RESULT;
    max_length= MAX_BLOB_WIDTH;
  }

  return false;
}


bool Item_func_get_user_var::const_item() const
{
  return (!var_entry || current_thd->query_id != var_entry->update_query_id);
}


enum Item_result Item_func_get_user_var::result_type() const
{
  return m_cached_result_type;
}


void Item_func_get_user_var::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("(@"));
  append_identifier(current_thd, str, name);
  str->append(')');
}


bool Item_func_get_user_var::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;					// Same item is same.
  /* Check if other type is also a get_user_var() object */
  if (item->type() != FUNC_ITEM ||
      ((Item_func*) item)->functype() != functype())
    return 0;
  Item_func_get_user_var *other=(Item_func_get_user_var*) item;
  return name.eq_bin(other->name);
}


bool Item_func_get_user_var::set_value(THD *thd,
                                       sp_rcontext * /*ctx*/, Item **it)
{
  Item_func_set_user_var *suv= new Item_func_set_user_var(name, *it, false);
  /*
    Item_func_set_user_var is not fixed after construction, call
    fix_fields().
  */
  return (!suv || suv->fix_fields(thd, it) || suv->check(0) || suv->update());
}


bool Item_user_var_as_out_param::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  DBUG_ASSERT(thd->lex->exchange);
  /*
    Let us set the same collation which is used for loading
    of fields in LOAD DATA INFILE.
    (Since Item_user_var_as_out_param is used only there).
  */
  const CHARSET_INFO *cs= thd->lex->exchange->cs ?
    thd->lex->exchange->cs : thd->variables.collation_database;

  if (Item::fix_fields(thd, ref))
    return true;

  /* Protects thd->user_vars. */
  mysql_mutex_lock(&thd->LOCK_thd_data);
  entry= get_variable(thd, name, cs);
  if (entry != NULL)
  {
    entry->set_type(STRING_RESULT);
    entry->update_query_id= thd->query_id;
  }
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  if (entry == NULL)
    return true;

  return false;
}


void Item_user_var_as_out_param::set_null_value(const CHARSET_INFO* cs)
{
  entry->lock();
  entry->set_null_value(STRING_RESULT);
  entry->unlock();
}


void Item_user_var_as_out_param::set_value(const char *str, size_t length,
                                           const CHARSET_INFO* cs)
{
  entry->lock();
  entry->store((void*) str, length, STRING_RESULT, cs,
               DERIVATION_IMPLICIT, 0 /* unsigned_arg */);
  entry->unlock();
}


double Item_user_var_as_out_param::val_real()
{
  DBUG_ASSERT(0);
  return 0.0;
}


longlong Item_user_var_as_out_param::val_int()
{
  DBUG_ASSERT(0);
  return 0;
}


String* Item_user_var_as_out_param::val_str(String *str)
{
  DBUG_ASSERT(0);
  return 0;
}


my_decimal* Item_user_var_as_out_param::val_decimal(my_decimal *decimal_buffer)
{
  DBUG_ASSERT(0);
  return 0;
}


void Item_user_var_as_out_param::print(String *str, enum_query_type query_type)
{
  str->append('@');
  append_identifier(current_thd, str, name);
}


Item_func_get_system_var::
Item_func_get_system_var(sys_var *var_arg, enum_var_type var_type_arg,
                       LEX_STRING *component_arg, const char *name_arg,
                       size_t name_len_arg)
  :var(var_arg), var_type(var_type_arg), orig_var_type(var_type_arg),
  component(*component_arg), cache_present(0)
{
  /* copy() will allocate the name */
  item_name.copy(name_arg, (uint) name_len_arg);
}


bool Item_func_get_system_var::is_written_to_binlog()
{
  return var->is_written_to_binlog(var_type);
}


bool Item_func_get_system_var::resolve_type(THD *thd)
{
  char *cptr;
  maybe_null= TRUE;
  max_length= 0;

  if (!var->check_scope(var_type))
  {
    if (var_type != OPT_DEFAULT)
    {
      my_error(ER_INCORRECT_GLOBAL_LOCAL_VAR, MYF(0),
               var->name.str, var_type == OPT_GLOBAL ? "SESSION" : "GLOBAL");
      return true;
    }
    /* As there was no local variable, return the global value */
    var_type= OPT_GLOBAL;
  }

  switch (var->show_type())
  {
    case SHOW_LONG:
    case SHOW_INT:
    case SHOW_HA_ROWS:
    case SHOW_LONGLONG:
      unsigned_flag= TRUE;
      collation.set_numeric();
      fix_char_length(MY_INT64_NUM_DECIMAL_DIGITS);
      decimals=0;
      break;
    case SHOW_SIGNED_LONG:
      unsigned_flag= FALSE;
      collation.set_numeric();
      fix_char_length(MY_INT64_NUM_DECIMAL_DIGITS);
      decimals=0;
      break;
    case SHOW_CHAR:
    case SHOW_CHAR_PTR:
      mysql_mutex_lock(&LOCK_global_system_variables);
      cptr= var->show_type() == SHOW_CHAR ? 
        (char*) var->value_ptr(current_thd, var_type, &component) :
        *(char**) var->value_ptr(current_thd, var_type, &component);
      if (cptr)
        max_length= system_charset_info->cset->numchars(system_charset_info,
                                                        cptr,
                                                        cptr + strlen(cptr));
      mysql_mutex_unlock(&LOCK_global_system_variables);
      collation.set(system_charset_info, DERIVATION_SYSCONST);
      max_length*= system_charset_info->mbmaxlen;
      decimals=NOT_FIXED_DEC;
      break;
    case SHOW_LEX_STRING:
      {
        mysql_mutex_lock(&LOCK_global_system_variables);
        LEX_STRING *ls= ((LEX_STRING*)var->value_ptr(current_thd, var_type, &component));
        max_length= system_charset_info->cset->numchars(system_charset_info,
                                                        ls->str,
                                                        ls->str + ls->length);
        mysql_mutex_unlock(&LOCK_global_system_variables);
        collation.set(system_charset_info, DERIVATION_SYSCONST);
        max_length*= system_charset_info->mbmaxlen;
        decimals=NOT_FIXED_DEC;
      }
      break;
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
      unsigned_flag= FALSE;
      collation.set_numeric();
      fix_char_length(1);
      decimals=0;
      break;
    case SHOW_DOUBLE:
      unsigned_flag= FALSE;
      decimals= 6;
      collation.set_numeric();
      fix_char_length(DBL_DIG + 6);
      break;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
      return true;
  }
  return false;
}


void Item_func_get_system_var::print(String *str, enum_query_type query_type)
{
  str->append(item_name);
}


enum Item_result Item_func_get_system_var::result_type() const
{
  switch (var->show_type())
  {
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
    case SHOW_INT:
    case SHOW_LONG:
    case SHOW_SIGNED_LONG:
    case SHOW_LONGLONG:
    case SHOW_HA_ROWS:
      return INT_RESULT;
    case SHOW_CHAR: 
    case SHOW_CHAR_PTR: 
    case SHOW_LEX_STRING:
      return STRING_RESULT;
    case SHOW_DOUBLE:
      return REAL_RESULT;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
      return STRING_RESULT;                   // keep the compiler happy
  }
}


enum_field_types Item_func_get_system_var::field_type() const
{
  switch (var->show_type())
  {
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
    case SHOW_INT:
    case SHOW_LONG:
    case SHOW_SIGNED_LONG:
    case SHOW_LONGLONG:
    case SHOW_HA_ROWS:
      return MYSQL_TYPE_LONGLONG;
    case SHOW_CHAR: 
    case SHOW_CHAR_PTR: 
    case SHOW_LEX_STRING:
      return MYSQL_TYPE_VARCHAR;
    case SHOW_DOUBLE:
      return MYSQL_TYPE_DOUBLE;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
      return MYSQL_TYPE_VARCHAR;              // keep the compiler happy
  }
}


/*
  Uses var, var_type, component, cache_present, used_query_id, thd,
  cached_llval, null_value, cached_null_value
*/
#define get_sys_var_safe(type) \
do { \
  type value; \
  mysql_mutex_lock(&LOCK_global_system_variables); \
  value= *(type*) var->value_ptr(thd, var_type, &component); \
  mysql_mutex_unlock(&LOCK_global_system_variables); \
  cache_present |= GET_SYS_VAR_CACHE_LONG; \
  used_query_id= thd->query_id; \
  cached_llval= null_value ? 0 : (longlong) value; \
  cached_null_value= null_value; \
  return cached_llval; \
} while (0)


longlong Item_func_get_system_var::val_int()
{
  THD *thd= current_thd;

  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      return cached_llval;
    } 
    else if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      cached_llval= (longlong) cached_dval;
      cache_present|= GET_SYS_VAR_CACHE_LONG;
      return cached_llval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_llval= longlong_from_string_with_check (cached_strval.charset(),
                                                       cached_strval.c_ptr(),
                                                       cached_strval.c_ptr() +
                                                       cached_strval.length());
      else
        cached_llval= 0;
      cache_present|= GET_SYS_VAR_CACHE_LONG;
      return cached_llval;
    }
  }

  switch (var->show_type())
  {
    case SHOW_INT:      get_sys_var_safe (uint);
    case SHOW_LONG:     get_sys_var_safe (ulong);
    case SHOW_SIGNED_LONG: get_sys_var_safe (long);
    case SHOW_LONGLONG: get_sys_var_safe (ulonglong);
    case SHOW_HA_ROWS:  get_sys_var_safe (ha_rows);
    case SHOW_BOOL:     get_sys_var_safe (bool);
    case SHOW_MY_BOOL:  get_sys_var_safe (my_bool);
    case SHOW_DOUBLE:
      {
        double dval= val_real();

        used_query_id= thd->query_id;
        cached_llval= (longlong) dval;
        cache_present|= GET_SYS_VAR_CACHE_LONG;
        return cached_llval;
      }
    case SHOW_CHAR:
    case SHOW_CHAR_PTR:
    case SHOW_LEX_STRING:
      {
        String *str_val= val_str(NULL);
        // Treat empty strings as NULL, like val_real() does.
        if (str_val && str_val->length())
          cached_llval= longlong_from_string_with_check (system_charset_info,
                                                          str_val->c_ptr(), 
                                                          str_val->c_ptr() + 
                                                          str_val->length());
        else
        {
          null_value= TRUE;
          cached_llval= 0;
        }

        cache_present|= GET_SYS_VAR_CACHE_LONG;
        return cached_llval;
      }

    default:            
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str); 
      return 0;                               // keep the compiler happy
  }
}


String* Item_func_get_system_var::val_str(String* str)
{
  THD *thd= current_thd;

  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      return null_value ? NULL : &cached_strval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_strval.set (cached_llval, collation.collation);
      cache_present|= GET_SYS_VAR_CACHE_STRING;
      return null_value ? NULL : &cached_strval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_strval.set_real (cached_dval, decimals, collation.collation);
      cache_present|= GET_SYS_VAR_CACHE_STRING;
      return null_value ? NULL : &cached_strval;
    }
  }

  str= &cached_strval;
  null_value= FALSE;
  switch (var->show_type())
  {
    case SHOW_CHAR:
    case SHOW_CHAR_PTR:
    case SHOW_LEX_STRING:
    {
      mysql_mutex_lock(&LOCK_global_system_variables);
      char *cptr= var->show_type() == SHOW_CHAR ? 
        (char*) var->value_ptr(thd, var_type, &component) :
        *(char**) var->value_ptr(thd, var_type, &component);
      if (cptr)
      {
        size_t len= var->show_type() == SHOW_LEX_STRING ?
          ((LEX_STRING*)(var->value_ptr(thd, var_type, &component)))->length :
          strlen(cptr);
        if (str->copy(cptr, len, collation.collation))
        {
          null_value= TRUE;
          str= NULL;
        }
      }
      else
      {
        null_value= TRUE;
        str= NULL;
      }
      mysql_mutex_unlock(&LOCK_global_system_variables);
      break;
    }

    case SHOW_INT:
    case SHOW_LONG:
    case SHOW_SIGNED_LONG:
    case SHOW_LONGLONG:
    case SHOW_HA_ROWS:
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
      str->set (val_int(), collation.collation);
      break;
    case SHOW_DOUBLE:
      str->set_real (val_real(), decimals, collation.collation);
      break;

    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
      str= error_str();
      break;
  }

  cache_present|= GET_SYS_VAR_CACHE_STRING;
  used_query_id= thd->query_id;
  cached_null_value= null_value;
  return str;
}


double Item_func_get_system_var::val_real()
{
  THD *thd= current_thd;

  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      return cached_dval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      cached_dval= (double)cached_llval;
      cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
      return cached_dval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_dval= double_from_string_with_check (cached_strval.charset(),
                                                    cached_strval.c_ptr(),
                                                    cached_strval.c_ptr() +
                                                    cached_strval.length());
      else
        cached_dval= 0;
      cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
      return cached_dval;
    }
  }

  switch (var->show_type())
  {
    case SHOW_DOUBLE:
      mysql_mutex_lock(&LOCK_global_system_variables);
      cached_dval= *(double*) var->value_ptr(thd, var_type, &component);
      mysql_mutex_unlock(&LOCK_global_system_variables);
      used_query_id= thd->query_id;
      cached_null_value= null_value;
      if (null_value)
        cached_dval= 0;
      cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
      return cached_dval;
    case SHOW_CHAR:
    case SHOW_LEX_STRING:
    case SHOW_CHAR_PTR:
      {
        mysql_mutex_lock(&LOCK_global_system_variables);
        char *cptr= var->show_type() == SHOW_CHAR ? 
          (char*) var->value_ptr(thd, var_type, &component) :
          *(char**) var->value_ptr(thd, var_type, &component);
        // Treat empty strings as NULL, like val_int() does.
        if (cptr && *cptr)
          cached_dval= double_from_string_with_check (system_charset_info, 
                                                cptr, cptr + strlen (cptr));
        else
        {
          null_value= TRUE;
          cached_dval= 0;
        }
        mysql_mutex_unlock(&LOCK_global_system_variables);
        used_query_id= thd->query_id;
        cached_null_value= null_value;
        cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
        return cached_dval;
      }
    case SHOW_INT:
    case SHOW_LONG:
    case SHOW_SIGNED_LONG:
    case SHOW_LONGLONG:
    case SHOW_HA_ROWS:
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
        cached_dval= (double) val_int();
        cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
        used_query_id= thd->query_id;
        cached_null_value= null_value;
        return cached_dval;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
      return 0;
  }
}


bool Item_func_get_system_var::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;					// Same item is same.
  /* Check if other type is also a get_user_var() object */
  if (item->type() != FUNC_ITEM ||
      ((Item_func*) item)->functype() != functype())
    return 0;
  Item_func_get_system_var *other=(Item_func_get_system_var*) item;
  return (var == other->var && var_type == other->var_type);
}


void Item_func_get_system_var::cleanup()
{
  Item_func::cleanup();
  cache_present= 0;
  var_type= orig_var_type;
  cached_strval.mem_free();
}


bool Item_func_match::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res) || against->itemize(pc, &against))
    return true;
  with_sum_func|= against->with_sum_func;

  pc->select->add_ftfunc_to_list(this);
  pc->thd->lex->set_using_match();

  switch (pc->select->parsing_place)
  {
    case CTX_WHERE:
    case CTX_ON:
      used_in_where_only= true;
      break;
    default:
      used_in_where_only= false;
  }

  return false;
}


/**
  Initialize searching within full-text index.

  @param thd    Thread handler

  @returns false if success, true if error
*/

bool Item_func_match::init_search(THD *thd)
{
  DBUG_ENTER("Item_func_match::init_search");

  /*
    We will skip execution if the item is not fixed
    with fix_field
  */
  if (!fixed)
    DBUG_RETURN(false);

  TABLE *const table= table_ref->table;
  /* Check if init_search() has been called before */
  if (ft_handler && !master)
  {
    /*
      We should reset ft_handler as it is cleaned up
      on destruction of FT_SELECT object
      (necessary in case of re-execution of subquery).
      TODO: FT_SELECT should not clean up ft_handler.
    */
    if (join_key)
      table->file->ft_handler= ft_handler;
    DBUG_RETURN(false);
  }

  if (key == NO_SUCH_KEY)
  {
    List<Item> fields;
    if (fields.push_back(new Item_string(" ",1, cmp_collation.collation)))
      DBUG_RETURN(true);
    for (uint i= 0; i < arg_count; i++)
      fields.push_back(args[i]);
    concat_ws=new Item_func_concat_ws(fields);
    if (concat_ws == NULL)
      DBUG_RETURN(true);
    /*
      Above function used only to get value and do not need fix_fields for it:
      Item_string - basic constant
      fields - fix_fields() was already called for this arguments
      Item_func_concat_ws - do not need fix_fields() to produce value
    */
    concat_ws->quick_fix_field();
  }

  if (master)
  {
    if (master->init_search(thd))
      DBUG_RETURN(true);

    ft_handler=master->ft_handler;
    DBUG_RETURN(false);
  }

  String *ft_tmp= 0;

  // MATCH ... AGAINST (NULL) is meaningless, but possible
  if (!(ft_tmp=key_item()->val_str(&value)))
  {
    ft_tmp= &value;
    value.set("",0,cmp_collation.collation);
  }

  if (ft_tmp->charset() != cmp_collation.collation)
  {
    uint dummy_errors;
    search_value.copy(ft_tmp->ptr(), ft_tmp->length(), ft_tmp->charset(),
                      cmp_collation.collation, &dummy_errors);
    ft_tmp= &search_value;
  }

  if (!table->is_created())
  {
     my_error(ER_NO_FT_MATERIALIZED_SUBQUERY, MYF(0));
     DBUG_RETURN(true);
  }

  DBUG_ASSERT(master == NULL);
  ft_handler= table->file->ft_init_ext_with_hints(key, ft_tmp, get_hints());
  if (thd->is_error())
    DBUG_RETURN(true);

  if (join_key)
    table->file->ft_handler=ft_handler;

  DBUG_RETURN(false);
}


float Item_func_match::get_filtering_effect(table_map filter_for_table,
                                            table_map read_tables,
                                            const MY_BITMAP *fields_to_ignore,
                                            double rows_in_table)
{
  const Item_field* fld= 
    contributes_to_filter(read_tables, filter_for_table, fields_to_ignore);
  if (!fld)
    return COND_FILTER_ALLPASS;

  /*
    MATCH () ... AGAINST" is similar to "LIKE '...'" which has the
    same selectivity as "col BETWEEN ...".
  */
  return fld->get_cond_filter_default_probability(rows_in_table,
                                                  COND_FILTER_BETWEEN);
}


/**
   Add field into table read set.

   @param field field to be added to the table read set.
*/
static void update_table_read_set(Field *field)
{
  TABLE *table= field->table;

  if (!bitmap_fast_test_and_set(table->read_set, field->field_index))
    table->covering_keys.intersect(field->part_of_key);
}


bool Item_func_match::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  DBUG_ASSERT(arg_count > 0);
  Item *item= NULL;                        // Safe as arg_count is > 1

  maybe_null=1;
  join_key=0;

  /*
    const_item is assumed in quite a bit of places, so it would be difficult
    to remove;  If it would ever to be removed, this should include
    modifications to find_best and auto_close as complement to auto_init code
    above.
  */
  enum_mark_columns save_mark_used_columns= thd->mark_used_columns;
  /*
    Since different engines require different columns for FTS index lookup
    we prevent updating of table read_set in argument's ::fix_fields().
  */
  thd->mark_used_columns= MARK_COLUMNS_NONE;
  if (Item_func::fix_fields(thd, ref) ||
      fix_func_arg(thd, &against) || !against->const_during_execution())
  {
    thd->mark_used_columns= save_mark_used_columns;
    my_error(ER_WRONG_ARGUMENTS,MYF(0),"AGAINST");
    return TRUE;
  }
  thd->mark_used_columns= save_mark_used_columns;

  bool allows_multi_table_search= true;
  const_item_cache=0;
  for (uint i= 0 ; i < arg_count ; i++)
  {
    item= args[i]= args[i]->real_item(); 
    if (item->type() != Item::FIELD_ITEM ||
        /* Cannot use FTS index with outer table field */
        (item->used_tables() & OUTER_REF_TABLE_BIT))
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), "MATCH");
      return TRUE;
    }
    allows_multi_table_search &= 
      allows_search_on_non_indexed_columns(((Item_field *)item)->field->table);
  }

  /*
    Check that all columns come from the same table.
    We've already checked that columns in MATCH are fields so
    PARAM_TABLE_BIT can only appear from AGAINST argument.
  */
  if ((used_tables_cache & ~PARAM_TABLE_BIT) != item->used_tables())
    key=NO_SUCH_KEY;

  if (key == NO_SUCH_KEY && !allows_multi_table_search)
  {
    my_error(ER_WRONG_ARGUMENTS,MYF(0),"MATCH");
    return TRUE;
  }
  table_ref= ((Item_field *)item)->table_ref;

  /*
    Here we make an assumption that if the engine supports
    fulltext extension(HA_CAN_FULLTEXT_EXT flag) then table
    can have FTS_DOC_ID column. Atm this is the only way
    to distinguish MyISAM and InnoDB engines.
    Generally table_ref should be available, but in case of
    a generated column's generation expression it's not. Thus
    we use field's table, at this moment it's already available.
  */
  TABLE *const table= table_ref ?
    table_ref->table :
    ((Item_field *)item)->field->table;

  if (!(table->file->ha_table_flags() & HA_CAN_FULLTEXT))
  {
    my_error(ER_TABLE_CANT_HANDLE_FT, MYF(0));
    return 1;
  }

  if ((table->file->ha_table_flags() & HA_CAN_FULLTEXT_EXT))
  {
    Field *doc_id_field= table->fts_doc_id_field;
    /*
      Update read set with FTS_DOC_ID column so that indexes that have
      FTS_DOC_ID part can be considered as a covering index.
    */
    if (doc_id_field)
      update_table_read_set(doc_id_field);
    else
    {
      /* read_set needs to be updated for MATCH arguments */
      for (uint i= 0; i < arg_count; i++)
        update_table_read_set(((Item_field*)args[i])->field);
      /*
        Prevent index only accces by non-FTS index if table does not have
        FTS_DOC_ID column, find_relevance does not work properly without
        FTS_DOC_ID value. Decision for FTS index about index only access
        is made later by JOIN::fts_index_access() function.
      */
      table->covering_keys.clear_all();
    }

  }
  else
  {
    /*
      Since read_set is not updated for MATCH arguments
      it's necessary to update it here for MyISAM.
    */
    for (uint i= 0; i < arg_count; i++)
      update_table_read_set(((Item_field*)args[i])->field);
  }

  table->fulltext_searched=1;

  if (!master)
  {
    Prepared_stmt_arena_holder ps_arena_holder(thd);
    hints= new Ft_hints(flags);
    if (!hints)
    {
      my_error(ER_TABLE_CANT_HANDLE_FT, MYF(0));
      return true;
    }
  }
  return agg_item_collations_for_comparison(cmp_collation, func_name(),
                                            args, arg_count, 0);
}

bool Item_func_match::fix_index()
{
  Item_field *item;
  TABLE *table;
  uint ft_to_key[MAX_KEY], ft_cnt[MAX_KEY], fts=0, keynr;
  uint max_cnt=0, mkeys=0, i;

  if (!table_ref)
    goto err;

  /*
    We will skip execution if the item is not fixed
    with fix_field
  */
  if (!fixed)
  {
    if (allows_search_on_non_indexed_columns(table_ref->table))
      key= NO_SUCH_KEY;

    return false;
  }
  if (key == NO_SUCH_KEY)
    return 0;
  
  table= table_ref->table;
  for (keynr=0 ; keynr < table->s->keys ; keynr++)
  {
    if ((table->key_info[keynr].flags & HA_FULLTEXT) &&
        (flags & FT_BOOL ? table->keys_in_use_for_query.is_set(keynr) :
         table->s->usable_indexes().is_set(keynr)))

    {
      ft_to_key[fts]=keynr;
      ft_cnt[fts]=0;
      fts++;
    }
  }

  if (!fts)
    goto err;

  for (i= 0; i < arg_count; i++)
  {
    item=(Item_field*)(args[i]->real_item());
    for (keynr=0 ; keynr < fts ; keynr++)
    {
      KEY *ft_key=&table->key_info[ft_to_key[keynr]];
      uint key_parts=ft_key->user_defined_key_parts;

      for (uint part=0 ; part < key_parts ; part++)
      {
	if (item->field->eq(ft_key->key_part[part].field))
	  ft_cnt[keynr]++;
      }
    }
  }

  for (keynr=0 ; keynr < fts ; keynr++)
  {
    if (ft_cnt[keynr] > max_cnt)
    {
      mkeys=0;
      max_cnt=ft_cnt[mkeys]=ft_cnt[keynr];
      ft_to_key[mkeys]=ft_to_key[keynr];
      continue;
    }
    if (max_cnt && ft_cnt[keynr] == max_cnt)
    {
      mkeys++;
      ft_cnt[mkeys]=ft_cnt[keynr];
      ft_to_key[mkeys]=ft_to_key[keynr];
      continue;
    }
  }

  for (keynr=0 ; keynr <= mkeys ; keynr++)
  {
    // partial keys doesn't work
    if (max_cnt < arg_count ||
        max_cnt < table->key_info[ft_to_key[keynr]].user_defined_key_parts)
      continue;

    key=ft_to_key[keynr];

    return 0;
  }

err:
  if (table_ref != 0 && allows_search_on_non_indexed_columns(table_ref->table))
  {
    key=NO_SUCH_KEY;
    return 0;
  }
  my_error(ER_FT_MATCHING_KEY_NOT_FOUND, MYF(0));
  return 1;
}


bool Item_func_match::eq(const Item *item, bool binary_cmp) const
{
  /* We ignore FT_SORTED flag when checking for equality since result is
     equvialent regardless of sorting */
  if (item->type() != FUNC_ITEM ||
      ((Item_func*)item)->functype() != FT_FUNC ||
      (flags | FT_SORTED) != (((Item_func_match*)item)->flags | FT_SORTED))
    return 0;

  Item_func_match *ifm=(Item_func_match*) item;

  if (key == ifm->key && table_ref == ifm->table_ref &&
      key_item()->eq(ifm->key_item(), binary_cmp))
    return 1;

  return 0;
}


double Item_func_match::val_real()
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_match::val");
  if (ft_handler == NULL)
    DBUG_RETURN(-1.0);

  TABLE *const table= table_ref->table;
  if (key != NO_SUCH_KEY && table->has_null_row()) // NULL row from outer join
    DBUG_RETURN(0.0);

  if (get_master()->join_key)
  {
    if (table->file->ft_handler)
      DBUG_RETURN(ft_handler->please->get_relevance(ft_handler));
    get_master()->join_key= 0;
  }

  if (key == NO_SUCH_KEY)
  {
    String *a= concat_ws->val_str(&value);
    if ((null_value= (a == 0)) || !a->length())
      DBUG_RETURN(0);
    DBUG_RETURN(ft_handler->please->find_relevance(ft_handler,
				      (uchar *)a->ptr(), a->length()));
  }
  DBUG_RETURN(ft_handler->please->find_relevance(ft_handler,
                                                 table->record[0], 0));
}

void Item_func_match::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("(match "));
  print_args(str, 0, query_type);
  str->append(STRING_WITH_LEN(" against ("));
  against->print(str, query_type);
  if (flags & FT_BOOL)
    str->append(STRING_WITH_LEN(" in boolean mode"));
  else if (flags & FT_EXPAND)
    str->append(STRING_WITH_LEN(" with query expansion"));
  str->append(STRING_WITH_LEN("))"));
}


/**
  Function sets FT hints(LIMIT, flags) depending on
  various join conditions.

  @param join     Pointer to JOIN object.
  @param ft_flag  FT flag value.
  @param ft_limit Limit value.
  @param no_cond  true if MATCH is not used in WHERE condition.
*/

void Item_func_match::set_hints(JOIN *join, uint ft_flag,
                                ha_rows ft_limit, bool no_cond)
{
  DBUG_ASSERT(!master);

  if (!join)  // used for count() optimization
  {
    hints->set_hint_flag(ft_flag);
    return;
  }

  /* skip hints setting if there are aggregates(except of FT_NO_RANKING) */
  if (join->implicit_grouping || join->group_list || join->select_distinct)
  {
    /* 'No ranking' is possibe even if aggregates are present */
    if ((ft_flag & FT_NO_RANKING))
      hints->set_hint_flag(FT_NO_RANKING);
    return;
  }

  hints->set_hint_flag(ft_flag);

  /**
    Only one table is used, there is no aggregates,
    WHERE condition is a single MATCH expression
    (WHERE MATCH(..) or WHERE MATCH(..) [>=,>] value) or
    there is no WHERE condition.
  */
  if (join->primary_tables == 1 && (no_cond || is_simple_expression()))
    hints->set_hint_limit(ft_limit);
}


/***************************************************************************
  System variables
****************************************************************************/

/**
  @class Silence_deprecation_warnings

  @brief Disable deprecation warnings handler class
*/
class Silence_deprecation_warnings : public Internal_error_handler
{
public:
  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sqlstate,
                                Sql_condition::enum_severity_level *level,
                                const char* msg)
  {
    return sql_errno == ER_WARN_DEPRECATED_SYNTAX;
  }
};

/**
  Return value of an system variable base[.name] as a constant item.

  @param pc                     Current parse context
  @param var_type               global / session
  @param name                   Name of base or system variable
  @param component              Component.

  @note
    If component.str = 0 then the variable name is in 'name'

  @return
    - 0  : error
    - #  : constant item
*/


Item *get_system_var(Parse_context *pc,
                     enum_var_type var_type, LEX_STRING name,
                     LEX_STRING component)
{
  THD *thd= pc->thd;
  sys_var *var;
  LEX_STRING *base_name, *component_name;

  if (component.str)
  {
    base_name= &component;
    component_name= &name;
  }
  else
  {
    base_name= &name;
    component_name= &component;			// Empty string
  }

  if (!(var= find_sys_var(thd, base_name->str, base_name->length)))
    return 0;
  if (component.str)
  {
    if (!var->is_struct())
    {
      my_error(ER_VARIABLE_IS_NOT_STRUCT, MYF(0), base_name->str);
      return 0;
    }
  }
  thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);

  set_if_smaller(component_name->length, MAX_SYS_VAR_LENGTH);
  
  var->do_deprecated_warning(thd);

  Item_func_get_system_var *item= new Item_func_get_system_var(var, var_type,
                                                               component_name,
                                                               NULL, 0);
#ifndef EMBEDDED_LIBRARY
  if (var_type == OPT_GLOBAL && var->check_scope(OPT_GLOBAL))
  {
    String str;
    String *outStr;
    /* This object is just created for variable to string conversion.
       item object cannot be used after the conversion of the variable
       to string. It caches the data. */
    Item_func_get_system_var *si= new Item_func_get_system_var(var, var_type,
                                                               component_name,
                                                               NULL, 0);

    /* Disable deprecation warning during var to string conversion. */
    Silence_deprecation_warnings silencer;
    thd->push_internal_handler(&silencer);

    outStr= si ? si->val_str(&str) : &str;

    thd->pop_internal_handler();

    if (mysql_audit_notify(thd, AUDIT_EVENT(MYSQL_AUDIT_GLOBAL_VARIABLE_GET),
                           var->name.str,
                           outStr ? outStr->ptr() : NULL,
                           outStr ? outStr->length() : 0))
      {
        return 0;
      }
  }
#endif

  return item;
}


bool Item_func_row_count::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;

  LEX *lex= pc->thd->lex;
  lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  lex->safe_to_cache_query= 0;
  return false;
}

longlong Item_func_row_count::val_int()
{
  DBUG_ASSERT(fixed == 1);
  THD *thd= current_thd;

  return thd->get_row_count_func();
}


Item_func_sp::Item_func_sp(const POS &pos,
                           const LEX_STRING &db_name,
                           const LEX_STRING &fn_name,
                           bool use_explicit_name,
                           PT_item_list *opt_list)
: Item_func(pos, opt_list), m_sp(NULL), dummy_table(NULL), sp_result_field(NULL)
{
  maybe_null= 1;
  with_stored_program= true;
  THD *thd= current_thd;
  m_name= new (thd->mem_root) sp_name(to_lex_cstring(db_name), fn_name,
                                      use_explicit_name);
}


bool Item_func_sp::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  if (m_name == NULL)
    return true; // OOM

  THD *thd= pc->thd;
  LEX *lex= thd->lex;

  context= lex->current_context();
  lex->safe_to_cache_query= false;

  if (m_name->m_db.str == NULL) // use the default database name
  {
    /* Cannot match the function since no database is selected */
    if (thd->db().str == NULL)
    {
      my_error(ER_NO_DB_ERROR, MYF(0));
      return true;
    }
    m_name->m_db= thd->db();
    m_name->m_db.str= thd->strmake(m_name->m_db.str, m_name->m_db.length);
  }

  m_name->init_qname(thd);
  sp_add_used_routine(lex, thd, m_name, enum_sp_type::FUNCTION);

  dummy_table= (TABLE*) sql_calloc(sizeof(TABLE)+ sizeof(TABLE_SHARE));
  if (dummy_table == NULL)
    return true;
  dummy_table->s= (TABLE_SHARE*) (dummy_table+1);

  return false;
}


void
Item_func_sp::cleanup()
{
  if (sp_result_field)
  {
    delete sp_result_field;
    sp_result_field= NULL;
  }
  m_sp= NULL;
  if (dummy_table != NULL)
    dummy_table->alias= NULL;
  Item_func::cleanup();
  tables_locked_cache= false;
  with_stored_program= true;
}

const char *
Item_func_sp::func_name() const
{
  THD *thd= current_thd;
  /* Calculate length to avoid reallocation of string for sure */
  size_t len= (((m_name->m_explicit_name ? m_name->m_db.length : 0) +
                m_name->m_name.length)*2 + //characters*quoting
               2 +                         // ` and `
               (m_name->m_explicit_name ?
                3 : 0) +                   // '`', '`' and '.' for the db
               1 +                         // end of string
               ALIGN_SIZE(1));             // to avoid String reallocation
  String qname((char *)alloc_root(thd->mem_root, len), len,
               system_charset_info);

  qname.length(0);
  if (m_name->m_explicit_name)
  {
    append_identifier(thd, &qname, m_name->m_db.str, m_name->m_db.length);
    qname.append('.');
  }
  append_identifier(thd, &qname, m_name->m_name.str, m_name->m_name.length);
  return qname.ptr();
}


table_map Item_func_sp::get_initial_pseudo_tables() const
{
  return m_sp->m_chistics->detistic ? 0 : RAND_TABLE_BIT;
}


static void my_missing_function_error(const LEX_STRING &token,
                                      const char *func_name)
{
  if (token.length && is_lex_native_function (&token))
    my_error(ER_FUNC_INEXISTENT_NAME_COLLISION, MYF(0), func_name);
  else
    my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "FUNCTION", func_name);
}


/**
  @brief Initialize the result field by creating a temporary dummy table
    and assign it to a newly created field object. Meta data used to
    create the field is fetched from the sp_head belonging to the stored
    proceedure found in the stored procedure functon cache.
  
  @note This function should be called from fix_fields to init the result
    field. It is some what related to Item_field.

  @see Item_field

  @param thd A pointer to the session and thread context.

  @return Function return error status.
  @retval TRUE is returned on an error
  @retval FALSE is returned on success.
*/

bool
Item_func_sp::init_result_field(THD *thd)
{
  LEX_STRING empty_name= { C_STRING_WITH_LEN("") };
  TABLE_SHARE *share;
  DBUG_ENTER("Item_func_sp::init_result_field");

  DBUG_ASSERT(m_sp == NULL);
  DBUG_ASSERT(sp_result_field == NULL);

  Internal_error_handler_holder<View_error_handler, TABLE_LIST>
    view_handler(thd, context->view_error_handler,
                 context->view_error_handler_arg);
  if (!(m_sp= sp_setup_routine(thd, enum_sp_type::FUNCTION, m_name,
                               &thd->sp_func_cache)))
  {
    my_missing_function_error (m_name->m_name, m_name->m_qname.str);
    DBUG_RETURN(TRUE);
  }

  /*
     A Field need to be attached to a Table.
     Below we "create" a dummy table by initializing 
     the needed pointers.
   */
  
  share= dummy_table->s;
  dummy_table->alias = "";
  if (maybe_null)
    dummy_table->set_nullable();
  dummy_table->in_use= thd;
  dummy_table->copy_blobs= TRUE;
  share->table_cache_key = empty_name;
  share->table_name = empty_name;

  if (!(sp_result_field= m_sp->create_result_field(max_length, item_name.ptr(),
                                                   dummy_table)))
  {
   DBUG_RETURN(TRUE);
  }
  
  if (sp_result_field->pack_length() > sizeof(result_buf))
  {
    void *tmp;
    if (!(tmp= sql_alloc(sp_result_field->pack_length())))
      DBUG_RETURN(TRUE);
    sp_result_field->move_field((uchar*) tmp);
  }
  else
    sp_result_field->move_field(result_buf);
  
  sp_result_field->set_null_ptr((uchar *) &null_value, 1);
  DBUG_RETURN(FALSE);
}


/**
  @brief Initialize local members with values from the Field interface.

  @note called from Item::fix_fields.
*/

bool Item_func_sp::resolve_type(THD *thd)
{
  DBUG_ENTER("Item_func_sp::resolve_type");

  DBUG_ASSERT(sp_result_field);
  decimals= sp_result_field->decimals();
  max_length= sp_result_field->field_length;
  collation.set(sp_result_field->charset());
  maybe_null= 1;
  unsigned_flag= MY_TEST(sp_result_field->flags & UNSIGNED_FLAG);

  DBUG_RETURN(false);
}


bool Item_func_sp::val_json(Json_wrapper *result)
{
  if (sp_result_field->type() == MYSQL_TYPE_JSON)
  {
    if (execute())
      return true;

    if (null_value)
      return false;

    Field_json *json_value= down_cast<Field_json *>(sp_result_field);
    return json_value->val_json(result);
  }

  /* purecov: begin deadcode */
  DBUG_ABORT();
  my_error(ER_INVALID_CAST_TO_JSON, MYF(0));
  return error_json();
  /* purecov: end */
}


type_conversion_status
Item_func_sp::save_in_field_inner(Field *field, bool no_conversions)
{
  return save_possibly_as_json(field, no_conversions);
}


/**
  @brief Execute function & store value in field.

  @return Function returns error status.
  @retval FALSE on success.
  @retval TRUE if an error occurred.
*/

bool
Item_func_sp::execute()
{
  THD *thd= current_thd;

  Internal_error_handler_holder<View_error_handler, TABLE_LIST>
    view_handler(thd, context->view_error_handler,
                 context->view_error_handler_arg);
  /* Execute function and store the return value in the field. */

  if (execute_impl(thd))
  {
    null_value= 1;
    if (thd->killed)
      thd->send_kill_message();
    return TRUE;
  }

  /* Check that the field (the value) is not NULL. */
  null_value= sp_result_field->is_null();

  return false;
}


/**
   @brief Execute function and store the return value in the field.

   @note This function was intended to be the concrete implementation of
    the interface function execute. This was never realized.

   @return The error state.
   @retval FALSE on success
   @retval TRUE if an error occurred.
*/
bool
Item_func_sp::execute_impl(THD *thd)
{
  bool err_status= TRUE;
  Sub_statement_state statement_state;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *save_security_ctx= thd->security_context();
#endif
  enum enum_sp_data_access access=
    (m_sp->m_chistics->daccess == SP_DEFAULT_ACCESS) ?
     SP_DEFAULT_ACCESS_MAPPING : m_sp->m_chistics->daccess;

  DBUG_ENTER("Item_func_sp::execute_impl");

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (context->security_ctx)
  {
    /* Set view definer security context */
    thd->set_security_context(context->security_ctx);
  }
#endif
  if (sp_check_access(thd))
    goto error;

  /*
    Throw an error if a non-deterministic function is called while
    statement-based replication (SBR) is active.
  */

  if (!m_sp->m_chistics->detistic && !trust_function_creators &&
      (access == SP_CONTAINS_SQL || access == SP_MODIFIES_SQL_DATA) &&
      (mysql_bin_log.is_open() &&
       thd->variables.binlog_format == BINLOG_FORMAT_STMT))
  {
    my_error(ER_BINLOG_UNSAFE_ROUTINE, MYF(0));
    goto error;
  }
  /*
    Disable the binlogging if this is not a SELECT statement. If this is a
    SELECT, leave binlogging on, so execute_function() code writes the
    function call into binlog.
  */
  thd->reset_sub_statement_state(&statement_state, SUB_STMT_FUNCTION);
  err_status= m_sp->execute_function(thd, args, arg_count, sp_result_field);
  thd->restore_sub_statement_state(&statement_state);

error:
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  thd->set_security_context(save_security_ctx);
#endif

  DBUG_RETURN(err_status);
}


void
Item_func_sp::make_field(Send_field *tmp_field)
{
  DBUG_ENTER("Item_func_sp::make_field");
  DBUG_ASSERT(sp_result_field);
  sp_result_field->make_field(tmp_field);
  if (item_name.is_set())
    tmp_field->col_name= item_name.ptr();
  DBUG_VOID_RETURN;
}


enum enum_field_types
Item_func_sp::field_type() const
{
  DBUG_ENTER("Item_func_sp::field_type");
  DBUG_ASSERT(sp_result_field);
  DBUG_RETURN(sp_result_field->type());
}

Item_result
Item_func_sp::result_type() const
{
  DBUG_ENTER("Item_func_sp::result_type");
  DBUG_PRINT("info", ("m_sp = %p", (void *) m_sp));
  DBUG_ASSERT(sp_result_field);
  DBUG_RETURN(sp_result_field->result_type());
}


bool Item_func_found_rows::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->safe_to_cache_query= false;
  return false;
}


longlong Item_func_found_rows::val_int()
{
  DBUG_ASSERT(fixed == 1);
  return current_thd->found_rows();
}


Field *
Item_func_sp::tmp_table_field(TABLE *t_arg)
{
  DBUG_ENTER("Item_func_sp::tmp_table_field");

  DBUG_ASSERT(sp_result_field);
  DBUG_RETURN(sp_result_field);
}


/**
  @brief Checks if requested access to function can be granted to user.
    If function isn't found yet, it searches function first.
    If function can't be found or user don't have requested access
    error is raised.

  @param thd thread handler

  @return Indication if the access was granted or not.
  @retval FALSE Access is granted.
  @retval TRUE Requested access can't be granted or function doesn't exists.
    
*/

bool
Item_func_sp::sp_check_access(THD *thd)
{
  DBUG_ENTER("Item_func_sp::sp_check_access");
  DBUG_ASSERT(m_sp);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (check_routine_access(thd, EXECUTE_ACL,
			   m_sp->m_db.str, m_sp->m_name.str, 0, FALSE))
    DBUG_RETURN(TRUE);
#endif

  DBUG_RETURN(FALSE);
}


bool
Item_func_sp::fix_fields(THD *thd, Item **ref)
{
  bool res;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *save_security_ctx= thd->security_context();
#endif

  DBUG_ENTER("Item_func_sp::fix_fields");
  DBUG_ASSERT(fixed == 0);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  /*
    Checking privileges to execute the function while creating view and
    executing the function of select.
   */
  if (!(thd->lex->context_analysis_only & CONTEXT_ANALYSIS_ONLY_VIEW) ||
      (thd->lex->sql_command == SQLCOM_CREATE_VIEW))
  {
    if (context->security_ctx)
    {
      /* Set view definer security context */
      thd->set_security_context(context->security_ctx);
    }

    /*
      Check whether user has execute privilege or not
     */

    Internal_error_handler_holder<View_error_handler, TABLE_LIST>
      view_handler(thd, context->view_error_handler,
                   context->view_error_handler_arg);

    res= check_routine_access(thd, EXECUTE_ACL, m_name->m_db.str,
                              m_name->m_name.str, 0, FALSE);
    thd->set_security_context(save_security_ctx);

    if (res)
    {
      DBUG_RETURN(res);
    }
  }
#endif

  /*
    We must call init_result_field before Item_func::fix_fields() 
    to make m_sp and result_field members available to resolve_type(),
    which is called from Item_func::fix_fields().
  */
  res= init_result_field(thd);

  if (res)
    DBUG_RETURN(res);

  res= Item_func::fix_fields(thd, ref);

  /* These is reset/set by Item_func::fix_fields. */
  with_stored_program= true;
  if (!m_sp->m_chistics->detistic || !tables_locked_cache)
    const_item_cache= false;

  if (res)
    DBUG_RETURN(res);

  if (thd->lex->context_analysis_only & CONTEXT_ANALYSIS_ONLY_VIEW)
  {
    /*
      Here we check privileges of the stored routine only during view
      creation, in order to validate the view.  A runtime check is
      perfomed in Item_func_sp::execute(), and this method is not
      called during context analysis.  Notice, that during view
      creation we do not infer into stored routine bodies and do not
      check privileges of its statements, which would probably be a
      good idea especially if the view has SQL SECURITY DEFINER and
      the used stored procedure has SQL SECURITY DEFINER.
    */
    res= sp_check_access(thd);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    /*
      Try to set and restore the security context to see whether it's valid
    */
    Security_context *save_secutiry_ctx;
    res= m_sp->set_security_ctx(thd, &save_secutiry_ctx);
    if (!res)
      m_sp->m_security_ctx.restore_security_context(thd, save_secutiry_ctx);
    
#endif /* ! NO_EMBEDDED_ACCESS_CHECKS */
  }

  DBUG_RETURN(res);
}


void Item_func_sp::update_used_tables()
{
  Item_func::update_used_tables();

  if (!m_sp->m_chistics->detistic)
    const_item_cache= false;

  /* This is reset by Item_func::update_used_tables(). */
  with_stored_program= true;
}


/*
  uuid_short handling.

  The short uuid is defined as a longlong that contains the following bytes:

  Bytes  Comment
  1      Server_id & 255
  4      Startup time of server in seconds
  3      Incrementor

  This means that an uuid is guaranteed to be unique
  even in a replication environment if the following holds:

  - The last byte of the server id is unique
  - If you between two shutdown of the server don't get more than
    an average of 2^24 = 16M calls to uuid_short() per second.
*/

ulonglong uuid_value;

void uuid_short_init()
{
  uuid_value= ((((ulonglong) server_id) << 56) + 
               (((ulonglong) server_start_time) << 24));
}


bool Item_func_uuid_short::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->safe_to_cache_query= false;
  return false;
}


longlong Item_func_uuid_short::val_int()
{
  ulonglong val;
  mysql_mutex_lock(&LOCK_uuid_generator);
  val= uuid_value++;
  mysql_mutex_unlock(&LOCK_uuid_generator);
  return (longlong) val;
}


bool Item_func_version::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  return false;
}


/**
  Check if schema and table are hidden by NDB engine.

  @param    thd           Thread handle.
  @param    schema_name   Schema name.
  @param    table_name    Table name.

  @retval   true          If schema and table are hidden by NDB.
  @retval   false         If schema and table are not hidden by NDB.
*/

static inline bool is_hidden_by_ndb(THD *thd, const String *schema_name,
                                    const String *table_name)
{
  if (!strncmp(schema_name->ptr(), "ndb", 3))
  {
    List<LEX_STRING> list;

    // Check if schema is of ndb and if it is hidden by it.
    LEX_STRING sch_name= schema_name->lex_string();
    list.push_back(&sch_name);
    ha_find_files(thd, nullptr, nullptr, nullptr,
                  true, &list);
    if (list.elements == 0)
    {
      // Schema is hidden by ndb engine.
      return true;
    }

    // Check if table is hidden by ndb.
    if (table_name != nullptr)
    {
      list.empty();
      LEX_STRING tbl_name= table_name->lex_string();
      list.push_back(&tbl_name);
      ha_find_files(thd, schema_name->ptr(), nullptr, nullptr,
                    false, &list);
      if (list.elements == 0)
      {
        // Table is hidden by ndb engine.
        return true;
      }
    }
  }

  return false;
}


/**
  @brief
    INFORMATION_SCHEMA picks metadata from DD using system views.
    In order for INFORMATION_SCHEMA to skip listing database for which
    the user does not have rights, the following internal functions are used.

  Syntax:
    int CAN_ACCCESS_DATABASE(schema_name);

  @returns,
    1 - If current user has access.
    0 - If not.
*/

longlong Item_func_can_access_database::val_int()
{
  DBUG_ENTER("Item_func_can_access_database::val_int");

  // Read schema_name
  String schema_name;
  String *schema_name_ptr= args[0]->val_str(&schema_name);
  if (schema_name_ptr == nullptr)
  {
    null_value= TRUE;
    DBUG_RETURN(FALSE);
  }

  // Make sure we have safe string to access.
  schema_name_ptr->c_ptr_safe();

  // Check if schema is hidden.
  THD *thd= current_thd;
  if (is_hidden_by_ndb(thd, schema_name_ptr, nullptr))
    DBUG_RETURN(FALSE);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  // Skip INFORMATION_SCHEMA database
  if (is_infoschema_db(schema_name_ptr->ptr()))
    DBUG_RETURN(TRUE);

  // Check access
  Security_context *sctx= thd->security_context();
  if (!(sctx->master_access() & (DB_ACLS | SHOW_DB_ACL) ||
        acl_get(thd, sctx->host().str, sctx->ip().str,
                sctx->priv_user().str, schema_name_ptr->ptr(), 0) ||
        !check_grant_db(thd, schema_name_ptr->ptr()))
     )
  {
    DBUG_RETURN(FALSE);
  }
#endif

  DBUG_RETURN(TRUE);
}

/**
  @brief
    INFORMATION_SCHEMA picks metadata from DD using system views.
    In order for INFORMATION_SCHEMA to skip listing table for which
    the user does not have rights, the following UDF's is used.

  Syntax:
    int CAN_ACCCESS_TABLE(schema_name, table_name, skip_table);

  @returns,
    1 - If current user has access.
    0 - If not.
*/
longlong Item_func_can_access_table::val_int()
{
  DBUG_ENTER("Item_func_can_access_table::val_int");

  /*
    If CAN_ACCCESS_TABLE is called for the hidden database objects then skip
    listing those .
    For example,CAN_ACCESS_TABLE is called from the I_S query STATISTICS_BASE.
    In this case if index or index column is hidden then skip listing of it.

    New keyword EXTENDED is introduced to the SHOW INDEX command to list the
    hidden Indexes and Indexes columns.
  */
  THD *thd= current_thd;
  if (args[2]->val_bool() && !thd->lex->extended_show)
    DBUG_RETURN(false);

  // Read schema_name, table_name
  String schema_name;
  String *schema_name_ptr= args[0]->val_str(&schema_name);
  String table_name;
  String *table_name_ptr= args[1]->val_str(&table_name);
  if (schema_name_ptr == nullptr || table_name_ptr == nullptr)
  {
    null_value= TRUE;
    DBUG_RETURN(FALSE);
  }

  // Make sure we have safe string to access.
  schema_name_ptr->c_ptr_safe();
  table_name_ptr->c_ptr_safe();

  // Check if table is hidden.
  if (is_hidden_by_ndb(thd, schema_name_ptr, table_name_ptr))
    DBUG_RETURN(FALSE);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  // Skip INFORMATION_SCHEMA database
  if (is_infoschema_db(schema_name_ptr->ptr()))
    DBUG_RETURN(TRUE);

  // Check access
  ulong db_access= 0;
  if (check_access(thd, SELECT_ACL, schema_name_ptr->ptr(),
                   &db_access, nullptr, false, true))
    DBUG_RETURN(FALSE);

  if (!(db_access & TABLE_ACLS))
  {
    TABLE_LIST table_list;
    memset(&table_list, 0, sizeof (table_list));
    table_list.db= schema_name_ptr->ptr();
    table_list.db_length= schema_name_ptr->length();
    table_list.table_name= table_name_ptr->ptr();
    table_list.table_name_length= table_name_ptr->length();
    table_list.grant.privilege= db_access;

    // Check access
    if (check_grant(thd, TABLE_ACLS, &table_list, true, 1, true))
    {
      DBUG_RETURN(FALSE);
    }
  }
#endif

  DBUG_RETURN(TRUE);
}

/**
  @brief
    INFORMATION_SCHEMA picks metadata from DD using system views.
    In order for INFORMATION_SCHEMA to skip listing column for which
    the user does not have rights, the following UDF's is used.

  Syntax:
    int CAN_ACCCESS_COLUMN(schema_name,
                           table_name,
                           field_name,
                           skip_column);

  @returns,
    1 - If current user has access.
    0 - If not.
*/
longlong Item_func_can_access_column::val_int()
{
  DBUG_ENTER("Item_func_can_access_column::val_int");

  THD *thd= current_thd;
  /*
    If CAN_ACCCESS_COLUMN is called for the hidden database objects then skip
    listing those .
    For example,CAN_ACCESS_COLUMN is called from the I_S query COLUMNS.
    In this case if column is hidden then skip listing of it.

    New keyword EXTENDED is introduced to the SHOW COLUMNS command to list the
    hidden columns.
  */
  if (args[3]->val_bool() && !thd->lex->extended_show)
    DBUG_RETURN(false);

  // Read schema_name, table_name
  String schema_name;
  String *schema_name_ptr= args[0]->val_str(&schema_name);
  String table_name;
  String *table_name_ptr= args[1]->val_str(&table_name);
  if (schema_name_ptr == nullptr || table_name_ptr == nullptr)
  {
    null_value= TRUE;
    DBUG_RETURN(FALSE);
  }

  // Make sure we have safe string to access.
  schema_name_ptr->c_ptr_safe();
  table_name_ptr->c_ptr_safe();

  // Check if table is hidden.
  if (is_hidden_by_ndb(thd, schema_name_ptr, table_name_ptr))
    DBUG_RETURN(FALSE);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  // Read column_name.
  String column_name;
  String *column_name_ptr= args[2]->val_str(&column_name);
  if (column_name_ptr == nullptr)
  {
    null_value= TRUE;
    DBUG_RETURN(FALSE);
  }

  // Make sure we have safe string to access.
  column_name_ptr->c_ptr_safe();

  // Skip INFORMATION_SCHEMA database
  if (is_infoschema_db(schema_name_ptr->ptr()))
    DBUG_RETURN(TRUE);

  // Check access
  GRANT_INFO grant_info;
  memset(&grant_info, 0, sizeof (grant_info));

  if (check_access(thd, SELECT_ACL, schema_name_ptr->ptr(),
                   &grant_info.privilege, nullptr, false, true))
    DBUG_RETURN(FALSE);

  uint col_access= get_column_grant(thd, &grant_info,
                                    schema_name_ptr->ptr(),
                                    table_name_ptr->ptr(),
                                    column_name_ptr->ptr()
                                   ) & COL_ACLS;
  if (!col_access)
  {
    DBUG_RETURN(FALSE);
  }
#endif

  DBUG_RETURN(TRUE);
}

/**
  @brief
    INFORMATION_SCHEMA picks metadata from DD using system views.
    In order for INFORMATION_SCHEMA to skip listing view definition
    for the user without rights, the following UDF's is used.

  Syntax:
    int CAN_ACCESS_VIEW(schema_name, view_name, definer, options);

  @returns,
    1 - If current user has access.
    0 - If not.
*/
longlong Item_func_can_access_view::val_int()
{
  DBUG_ENTER("item_func_can_access_view::val_int");

  // Read schema_name, table_name
  String schema_name;
  String table_name;
  String definer;
  String options;
  String *schema_name_ptr= args[0]->val_str(&schema_name);
  String *table_name_ptr= args[1]->val_str(&table_name);
  String *definer_ptr= args[2]->val_str(&definer);
  String *options_ptr= args[3]->val_str(&options);
  if (schema_name_ptr == nullptr || table_name_ptr == nullptr ||
      definer_ptr == nullptr || options_ptr == nullptr)
  {
    null_value= TRUE;
    DBUG_RETURN(FALSE);
  }

  // Make strings safe.
  schema_name_ptr->c_ptr_safe();
  table_name_ptr->c_ptr_safe();
  definer_ptr->c_ptr_safe();
  options_ptr->c_ptr_safe();

  // Skip INFORMATION_SCHEMA database
  if (is_infoschema_db(schema_name_ptr->ptr()) ||
      !my_strcasecmp(system_charset_info, schema_name_ptr->ptr(), "sys"))
    DBUG_RETURN(TRUE);

  // Check if view is valid. If view is invalid then push invalid view
  // warning.
  bool is_view_valid= true;
  std::unique_ptr<dd::Properties>
    view_options(dd::Properties::parse_properties(options_ptr->c_ptr_safe()));
  if (view_options->get_bool("view_valid", &is_view_valid))
    DBUG_RETURN(FALSE);

  THD *thd= current_thd;
  if (!is_view_valid)
    push_view_warning_or_error(thd,
                               schema_name_ptr->c_ptr_safe(),
                               table_name_ptr->c_ptr_safe());

  //
  // Check if definer user/host has access.
  //

  Security_context *sctx= thd->security_context();

  // NOTE: this is a copy/paste from sp_head::set_definer().

  char user_name_holder[USERNAME_LENGTH + 1];
  LEX_STRING user_name= { user_name_holder, USERNAME_LENGTH };

  char host_name_holder[HOSTNAME_LENGTH + 1];
  LEX_STRING host_name= { host_name_holder, HOSTNAME_LENGTH };

  parse_user(definer_ptr->ptr(), definer_ptr->length(),
             user_name.str, &user_name.length,
             host_name.str, &host_name.length);

  std::string definer_user(user_name.str, user_name.length);
  std::string definer_host(host_name.str, host_name.length);

  if (!my_strcasecmp(system_charset_info, definer_user.c_str(),
                     sctx->priv_user().str) &&
      !my_strcasecmp(system_charset_info, definer_host.c_str(),
                     sctx->priv_host().str))
    DBUG_RETURN(TRUE);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  //
  // Check for ACL's
  //

  if ((thd->col_access & (SHOW_VIEW_ACL|SELECT_ACL)) ==
      (SHOW_VIEW_ACL|SELECT_ACL))
    DBUG_RETURN(TRUE);

  TABLE_LIST table_list;
  uint view_access;
  memset(&table_list, 0, sizeof(table_list));
  table_list.db= schema_name_ptr->ptr();
  table_list.table_name= table_name_ptr->ptr();
  table_list.grant.privilege= thd->col_access;
  view_access= get_table_grant(thd, &table_list);
  if ((view_access & (SHOW_VIEW_ACL|SELECT_ACL)) ==
      (SHOW_VIEW_ACL|SELECT_ACL))
    DBUG_RETURN(TRUE);
#endif

  DBUG_RETURN(FALSE);
}

static ulonglong get_statistics_from_cache(
                   Item** args,
                   dd::info_schema::enum_statistics_type stype,
                   my_bool *null_value)
{
  DBUG_ENTER("get_statistics_from_cache");
  *null_value= FALSE;

  // Reads arguments
  String schema_name;
  String table_name;
  String engine_name;
  String *schema_name_ptr=args[0]->val_str(&schema_name);
  String *table_name_ptr=args[1]->val_str(&table_name);
  String *engine_name_ptr=args[2]->val_str(&engine_name);
  if (schema_name_ptr == nullptr || table_name_ptr == nullptr ||
      engine_name_ptr == nullptr)
  {
    *null_value= TRUE;
    DBUG_RETURN(0);
  }

  // Make sure we have safe string to access.
  schema_name_ptr->c_ptr_safe();
  table_name_ptr->c_ptr_safe();
  engine_name_ptr->c_ptr_safe();

  // Do not read dynamic stats for I_S tables.
  if (is_infoschema_db(schema_name_ptr->ptr()))
    DBUG_RETURN(0);

  // Read the statistic value from cache.
  THD *thd= current_thd;
  dd::Object_id se_private_id= (dd::Object_id) args[3]->val_uint();
  ulonglong result= thd->lex->m_IS_dyn_stat_cache.read_stat(thd,
                                                      *schema_name_ptr,
                                                      *table_name_ptr,
                                                      *engine_name_ptr,
                                                      se_private_id,
                                                      stype);
  DBUG_RETURN(result);
}

longlong Item_func_internal_table_rows::val_int()
{
  DBUG_ENTER("Item_func_internal_table_rows::val_int");

  ulonglong result= get_statistics_from_cache(
                      args,
                      dd::info_schema::enum_statistics_type::TABLE_ROWS,
                      &null_value);

  if (null_value == FALSE && result == (ulonglong) -1)
    null_value= TRUE;

  DBUG_RETURN(result);
}

longlong Item_func_internal_avg_row_length::val_int()
{
  DBUG_ENTER("Item_func_internal_avg_row_length::val_int");

  ulonglong result=
    get_statistics_from_cache(
      args,
      dd::info_schema::enum_statistics_type::TABLE_AVG_ROW_LENGTH,
      &null_value);
  DBUG_RETURN(result);
}

longlong Item_func_internal_data_length::val_int()
{
  DBUG_ENTER("Item_func_internal_data_length::val_int");

  ulonglong result= get_statistics_from_cache(
                      args,
                      dd::info_schema::enum_statistics_type::DATA_LENGTH,
                      &null_value);
  DBUG_RETURN(result);
}

longlong Item_func_internal_max_data_length::val_int()
{
  DBUG_ENTER("Item_func_internal_max_data_length::val_int");

  ulonglong result= get_statistics_from_cache(
                      args,
                      dd::info_schema::enum_statistics_type::MAX_DATA_LENGTH,
                      &null_value);
  DBUG_RETURN(result);
}

longlong Item_func_internal_index_length::val_int()
{
  DBUG_ENTER("Item_func_internal_index_length::val_int");

  ulonglong result= get_statistics_from_cache(
                      args,
                      dd::info_schema::enum_statistics_type::INDEX_LENGTH,
                      &null_value);
  DBUG_RETURN(result);
}

longlong Item_func_internal_data_free::val_int()
{
  DBUG_ENTER("Item_func_internal_data_free::val_int");

  ulonglong result= get_statistics_from_cache(
                      args,
                      dd::info_schema::enum_statistics_type::DATA_FREE,
                      &null_value);

  if (null_value == FALSE && result == (ulonglong) -1)
    null_value= TRUE;

  DBUG_RETURN(result);
}

longlong Item_func_internal_auto_increment::val_int()
{
  DBUG_ENTER("Item_func_internal_auto_increment::val_int");

  ulonglong result= get_statistics_from_cache(
                      args,
                      dd::info_schema::enum_statistics_type::AUTO_INCREMENT,
                      &null_value);

  if (null_value == FALSE && result < (ulonglong) 1)
    null_value= TRUE;

  DBUG_RETURN(result);
}

longlong Item_func_internal_checksum::val_int()
{
  DBUG_ENTER("Item_func_internal_checksum::val_int");

  ulonglong result= get_statistics_from_cache(
                      args,
                      dd::info_schema::enum_statistics_type::CHECKSUM,
                      &null_value);

  if (null_value == FALSE && result == 0)
    null_value= TRUE;

  DBUG_RETURN(result);
}

/**
  @brief
    INFORMATION_SCHEMA picks metadata from DD using system views.
    INFORMATION_SCHEMA.STATISTICS.COMMENT is used to indicate if the indexes are
    disabled by ALTER TABLE ... DISABLE KEYS. This property of table is stored
    in mysql.tables.options as 'keys_disabled=0/1/'. This internal function
    returns value of option 'keys_disabled' for a given table.

  Syntax:
    int INTERNAL_KEYS_DISABLED(table_options);

  @returns,
    1 - If keys are disabled.
    0 - If not.
*/
longlong Item_func_internal_keys_disabled::val_int()
{
  DBUG_ENTER("Item_func_internal_keys_disabled::val_int");

  // Read options.
  String options;
  String *options_ptr=args[0]->val_str(&options);
  if (options_ptr == nullptr)
    DBUG_RETURN(FALSE);

  // Read table option from properties
  std::unique_ptr<dd::Properties> p
    (dd::Properties::parse_properties(options_ptr->c_ptr_safe()));

    // Read keys_disabled sub type.
  uint keys_disabled= 0;
  p->get_uint32("keys_disabled", &keys_disabled);

  DBUG_RETURN(keys_disabled);
}

/**
  @brief
    INFORMATION_SCHEMA picks metadata from DD using system views.
    INFORMATION_SCHEMA.STATISTICS.CARDINALITY is can be read from SE when
    information_schema_stats is set to 'latest'.

  Syntax:
    int INTERNAL_INDEX_COLUMN_CARDINALITY(
          schema_name,
          table_name,
          index_name,
          column_ordinal_position);

  @returns Cardinatily. Or sets null_value to true if cardinality is -1.
*/
longlong Item_func_internal_index_column_cardinality::val_int()
{
  DBUG_ENTER("Item_func_internal_index_column_cardinality::val_int");
  null_value= FALSE;

  // Read arguments
  String schema_name;
  String table_name;
  String index_name;
  String engine_name;
  String *schema_name_ptr= args[0]->val_str(&schema_name);
  String *table_name_ptr= args[1]->val_str(&table_name);
  String *index_name_ptr= args[2]->val_str(&index_name);
  String *engine_name_ptr= args[5]->val_str(&engine_name);
  uint index_ordinal_position= args[3]->val_uint();
  uint column_ordinal_position= args[4]->val_uint();
  dd::Object_id se_private_id= (dd::Object_id) args[6]->val_uint();
  if (schema_name_ptr == nullptr || table_name_ptr == nullptr ||
      index_name_ptr == nullptr || engine_name_ptr == nullptr ||
      args[3]->null_value || args[4]->null_value)
  {
    null_value= TRUE;
    DBUG_RETURN(0);
  }

  // Make sure we have safe string to access.
  schema_name_ptr->c_ptr_safe();
  table_name_ptr->c_ptr_safe();
  index_name_ptr->c_ptr_safe();
  engine_name_ptr->c_ptr_safe();

  ulonglong result= 0;
  THD *thd= current_thd;
  result= thd->lex->m_IS_dyn_stat_cache.read_stat(thd,
            *schema_name_ptr,
            *table_name_ptr,
            *index_name_ptr,
            index_ordinal_position - 1,
            column_ordinal_position - 1,
            *engine_name_ptr,
            se_private_id,
            dd::info_schema::enum_statistics_type::INDEX_COLUMN_CARDINALITY);

  if (result == (ulonglong) -1)
    null_value= TRUE;

  DBUG_RETURN(result);
}


Item_func_version::Item_func_version(const POS &pos)
  : Item_static_string_func(pos, NAME_STRING("version()"),
                            server_version,
                            strlen(server_version),
                            system_charset_info,
                            DERIVATION_SYSCONST)
{}

/**
  @brief

    Syntax:
      string get_dd_char_length()

*/
longlong Item_func_internal_dd_char_length::val_int()
{
  DBUG_ENTER("Item_func_get_dd_char_length::val_real");
  null_value= FALSE;

  dd::enum_column_types col_type= (dd::enum_column_types) args[0]->val_int();
  uint field_length= args[1]->val_int();
  String cs_name;
  String *cs_name_ptr= args[2]->val_str(&cs_name);
  uint flag= args[3]->val_int();

  // Stop if we found a NULL argument.
  if (args[0]->null_value ||
      args[1]->null_value ||
      cs_name_ptr == nullptr ||
      args[3]->null_value)
  {
    null_value= TRUE;
    DBUG_RETURN(0);
  }

  // Read character set.
  CHARSET_INFO *cs= get_charset_by_name(cs_name_ptr->c_ptr_safe(), MYF(0));
  if (!cs)
  {
    null_value= TRUE;
    DBUG_RETURN(0);
  }

  // Check data types for getting info
  enum_field_types field_type= dd_get_old_field_type(col_type);
  bool blob_flag= is_blob(field_type);
  if (!blob_flag &&
      field_type != MYSQL_TYPE_ENUM &&
      field_type != MYSQL_TYPE_SET &&
      field_type != MYSQL_TYPE_VARCHAR &&  // For varbinary type
      field_type != MYSQL_TYPE_STRING)     // For binary type
  {
    null_value= TRUE;
    DBUG_RETURN(0);
  }

  std::ostringstream oss("");
  switch (field_type)
  {
    case MYSQL_TYPE_BLOB:
      field_length= 65535;
      break;
    case MYSQL_TYPE_TINY_BLOB:
      field_length= 255;
      break;
    case MYSQL_TYPE_MEDIUM_BLOB:
      field_length= 16777215;
      break;
    case MYSQL_TYPE_LONG_BLOB:
      field_length= 4294967295;
      break;
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
      break;
    default:
      break;
  }

  if (!flag && field_length)
  {
    if (blob_flag)
      DBUG_RETURN (field_length / cs->mbminlen);
    else
      DBUG_RETURN (field_length / cs->mbmaxlen);
  }
  else if (flag && field_length)
  {
    DBUG_RETURN (field_length);
  }

  DBUG_RETURN(0);
}


longlong Item_func_internal_get_view_warning_or_error::val_int()
{
  DBUG_ENTER("Item_func_internal_get_view_warning_or_error::val_int");

  String schema_name;
  String table_name;
  String table_type;
  String *schema_name_ptr= args[0]->val_str(&schema_name);
  String *table_name_ptr= args[1]->val_str(&table_name);
  String *table_type_ptr= args[2]->val_str(&table_type);


  if (table_type_ptr == nullptr || schema_name_ptr == nullptr ||
      table_name_ptr == nullptr)
  {
    DBUG_RETURN(FALSE);
  }

  String options;
  String *options_ptr= args[3]->val_str(&options);
  if (strcmp(table_type_ptr->c_ptr_safe(), "VIEW") == 0 &&
      options_ptr != nullptr)
  {
    bool is_view_valid= true;
    std::unique_ptr<dd::Properties>
      view_options(dd::Properties::parse_properties(options_ptr->c_ptr_safe()));

    // Return 0 if get_bool() or push_view_warning_or_error() fails
    if (view_options->get_bool("view_valid", &is_view_valid))
      DBUG_RETURN(FALSE);

    if (is_view_valid == false)
    {
      push_view_warning_or_error(current_thd,
                                 schema_name_ptr->c_ptr_safe(),
                                 table_name_ptr->c_ptr_safe());
      DBUG_RETURN(FALSE);
    }
  }

  DBUG_RETURN(TRUE);
}
