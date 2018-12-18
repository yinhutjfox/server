/* This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "mariadb.h"
#include "sql_array.h"
#include "sql_string.h"
#include "sql_class.h"
#include "sql_show.h"
#include "field.h"
#include "table.h"
#include "opt_trace.h"
#include "sql_parse.h"
#include "set_var.h"
#include "my_json_writer.h"

const char I_S_table_name[] = "OPTIMIZER_TRACE";

/**
   Whether a list of tables contains information_schema.OPTIMIZER_TRACE.
   @param  tbl  list of tables

   Can we do better than this here??
   @note this does not catch that a stored routine or view accesses
   the OPTIMIZER_TRACE table. So using a stored routine or view to read
   OPTIMIZER_TRACE will overwrite OPTIMIZER_TRACE as it runs and provide
   uninteresting info.
*/
bool list_has_optimizer_trace_table(const TABLE_LIST *tbl)
{
  for (; tbl; tbl = tbl->next_global)
  {
    if (tbl->schema_table &&
        0 == strcmp(tbl->schema_table->table_name, I_S_table_name))
      return true;
  }
  return false;
}

/*
  Returns if a query has a set command with optimizer_trace being switched on/off.
  True: Don't trace the query(uninteresting)
*/

bool sets_var_optimizer_trace(enum enum_sql_command sql_command,
                              List<set_var_base> *set_vars) 
{
  if (sql_command == SQLCOM_SET_OPTION)
  {
    List_iterator_fast<set_var_base> it(*set_vars);
    const set_var_base *var;
    while ((var = it++))
      if (var->is_var_optimizer_trace()) return true;
  }
  return false;
}

const char brackets[] = {'[', '{', ']', '}'};
inline char opening_bracket(bool requires_key) 
{
  return brackets[requires_key];
}
inline char closing_bracket(bool requires_key)
{
  return brackets[requires_key + 2];
}

ST_FIELD_INFO optimizer_trace_info[] = {
    /* name, length, type, value, maybe_null, old_name, open_method */
    {"QUERY", 65535, MYSQL_TYPE_STRING, 0, false, NULL, SKIP_OPEN_TABLE},
    {"TRACE", 65535, MYSQL_TYPE_STRING, 0, false, NULL, SKIP_OPEN_TABLE},
    {"MISSING_BYTES_BEYOND_MAX_MEM_SIZE", 20, MYSQL_TYPE_LONG, 0, false, NULL,
     SKIP_OPEN_TABLE},
    {"INSUFFICIENT_PRIVILEGES", 1, MYSQL_TYPE_TINY, 0, false, NULL,
     SKIP_OPEN_TABLE},
    {NULL, 0, MYSQL_TYPE_STRING, 0, true, NULL, 0}};

/*
  TODO: one-line needs to be implemented seperately
*/
const char *Opt_trace_context::flag_names[] = {"enabled", "one_line", "default",
                                               NullS};

/*
  Returns if a particular command will be traced or not
*/

inline bool sql_command_can_be_traced(enum enum_sql_command sql_command)
{
  return (sql_command_flags[sql_command] & CF_OPTIMIZER_TRACE);
}

void opt_trace_print_expanded_query(THD *thd, SELECT_LEX *select_lex,
                                    Json_writer_object *writer)

{
  Opt_trace_context *const trace = &thd->opt_trace;

  if (!trace->get_current_trace()) 
    return;
  char buff[1024];
  String str(buff, sizeof(buff), system_charset_info);
  str.length(0);
  
  select_lex->print(thd, &str,
                    enum_query_type(QT_TO_SYSTEM_CHARSET |
                                    QT_SHOW_SELECT_NUMBER |
                                    QT_ITEM_IDENT_SKIP_DB_NAMES |
                                    QT_VIEW_INTERNAL
                                    ));
  /*
    The output is not very pretty lots of back-ticks, the output
    is as the one in explain extended , lets try to improved it here.
  */
  writer->add_member("expanded_query").add_str(str);
}

/**
  @class Opt_trace_stmt

  The trace of one statement.
*/

class Opt_trace_stmt {
 public:
  /**
     Constructor, starts a trace for information_schema and dbug.
     @param  ctx_arg          context
  */
  Opt_trace_stmt(Opt_trace_context *ctx_arg)
  {
    ctx= ctx_arg;
    current_json= new Json_writer();
  }
  ~Opt_trace_stmt()
  {
    delete current_json;
  }
  void set_query(const char *query_ptr, size_t length, const CHARSET_INFO *charset);
  void open_struct(const char *key, char opening_bracket);
  void close_struct(const char *saved_key, char closing_bracket);
  void fill_info(Opt_trace_info* info);
  void add(const char *key, char *opening_bracket, size_t val_length);
  Json_writer* get_current_json(){return current_json;}
private:
  Opt_trace_context *ctx;
  String query;  // store the query sent by the user
  Json_writer *current_json; // stores the trace
};

void Opt_trace_stmt::set_query(const char *query_ptr, size_t length, const CHARSET_INFO *charset)
{
  query.append(query_ptr, length, charset);
}

Json_writer* Opt_trace_context::get_current_json()
{
  if (!current_trace)
    return NULL;
  return current_trace->get_current_json();
}

Opt_trace_context::Opt_trace_context()
{
  current_trace= NULL;
  inited= FALSE;
  traces= NULL;
}
Opt_trace_context::~Opt_trace_context()
{
  inited= FALSE;
  /*
    would be nice to move this to a function
  */
  if (traces)
  {
    while (traces->elements())
    {
      Opt_trace_stmt *prev= traces->at(0);
      delete prev;
      traces->del(0);
    }
    delete traces;
    traces= NULL;
  }
}

void Opt_trace_context::set_query(const char *query, size_t length, const CHARSET_INFO *charset)
{
  current_trace->set_query(query, length, charset);
}

void Opt_trace_context::start(THD *thd, TABLE_LIST *tbl,
                  enum enum_sql_command sql_command,
                  const char *query,
                  size_t query_length,
                  const CHARSET_INFO *query_charset)
{
  current_trace= new Opt_trace_stmt(this);
  if (!inited)
  {
    traces= new Dynamic_array<Opt_trace_stmt*>();
    inited= TRUE;
  }
}

void Opt_trace_context::end()
{
  if (current_trace)
    traces->push(current_trace);

  if (!traces->elements())
    return;
  if (traces->elements() > 1)
  {
    Opt_trace_stmt *prev= traces->at(0);
    delete prev;
    traces->del(0);
  }
  current_trace= NULL;
}

Opt_trace_start::Opt_trace_start(THD *thd, TABLE_LIST *tbl,
                  enum enum_sql_command sql_command,
                  List<set_var_base> *set_vars,
                  const char *query,
                  size_t query_length,
                  const CHARSET_INFO *query_charset):ctx(&thd->opt_trace)
{
  /*
    if optimizer trace is enabled and the statment we have is traceable,
    then we start the context.
  */
  const ulonglong var = thd->variables.optimizer_trace;
  traceable= FALSE;
  if (unlikely(var & Opt_trace_context::FLAG_ENABLED) && 
      sql_command_can_be_traced(sql_command) && 
      !list_has_optimizer_trace_table(tbl) &&
      !sets_var_optimizer_trace(sql_command, set_vars))
  {
    ctx->start(thd, tbl, sql_command, query, query_length, query_charset);
    ctx->set_query(query, query_length, query_charset);
    traceable= TRUE;
  }
}

Opt_trace_start::~Opt_trace_start()
{
  if (traceable)
    ctx->end();
  traceable= FALSE;
}

void Opt_trace_stmt::fill_info(Opt_trace_info* info)
{
  info->trace_ptr = current_json->output.ptr();
  info->trace_length = current_json->output.length();
  info->query_ptr = query.ptr();
  info->query_length = query.length();
  info->query_charset = query.charset();
  info->missing_bytes = 0;
  info->missing_priv= 0;
}

/*void Opt_trace_stmt::add(const char *key, char *val, size_t val_length)
{
  if (key != NULL)
  {
    trace.append(key);
  }
  trace.append(val,val_length);
}*/

/*void Opt_trace_stmt::open_struct(const char *key,
                                char opening_bracket)
{
  add(key, &opening_bracket, 1);
}

void Opt_trace_stmt::close_struct(const char *saved_key, char closing_bracket)
{
  trace.append(&closing_bracket, 1);
}*/

void Json_writer::do_construct(Opt_trace_context *ctx_arg)
{
  if (!ctx_arg->get_current_trace())
    return;
  stmt= ctx_arg->get_current_trace();
}

/*
  Still needs to be thought about (mostly regarding utf8)
*/

void Json_writer::add_table_name(TABLE_LIST *tab)
{
  if (tab != NULL) 
  {
    THD *thd= current_thd;
    char buff[32];
    String str(buff, sizeof(buff), system_charset_info);
    str.length(0);
    ulonglong save_option_bits= thd->variables.option_bits;
    thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;
    tab->print(thd, table_map(0), &str,
              enum_query_type(QT_TO_SYSTEM_CHARSET | QT_SHOW_SELECT_NUMBER
                | QT_ITEM_IDENT_SKIP_DB_NAMES));
    thd->variables.option_bits= save_option_bits;
    add_str(str.c_ptr_safe());
  }
}

void add_table_scan_values_to_trace(Opt_trace_context* trace, JOIN_TAB *tab)
{
  Json_writer *writer= trace->get_current_json();
  Json_writer_object table_records(writer);
  table_records.add_member("table").add_table_name(tab->tab_list);
  Json_writer_object table_rec(writer, "table_scan");
  table_rec.add_member("rows").add_ll(tab->found_records);
  table_rec.add_member("cost").add_ll(tab->read_time);
}
/*
  Introduce enum_query_type flags parameter, maybe also allow
  EXPLAIN also use this function.
*/

void Json_writer::add_str(Item *item)
{
  if (item)
  {
    THD *thd= current_thd;
    char buff[256];
    String str(buff, sizeof(buff), system_charset_info);
    str.length(0);

    ulonglong save_option_bits= thd->variables.option_bits;
    thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;
    item->print(&str,
                enum_query_type(QT_TO_SYSTEM_CHARSET | QT_SHOW_SELECT_NUMBER
                               | QT_ITEM_IDENT_SKIP_DB_NAMES));
    thd->variables.option_bits= save_option_bits;
    add_str(str.c_ptr_safe());
  }
  else
    add_null();
}

void get_info(THD *thd, Opt_trace_info* info)
{
  Opt_trace_context* ctx= &thd->opt_trace;
  Opt_trace_stmt *stmt= ctx->get_top_trace();
  stmt->fill_info(info);
}


int fill_optimizer_trace_info(THD *thd, TABLE_LIST *tables, Item *)
{
  TABLE *table = tables->table;
  Opt_trace_info info;  

  /*  get_values of trace, query , missing bytes and missing_priv

      @todo: Need an iterator here to walk over all the traces
  */

  if (thd->opt_trace.empty())
  {
    get_info(thd, &info);

    table->field[0]->store(info.query_ptr, static_cast<uint>(info.query_length),
                             info.query_charset);
    table->field[1]->store(info.trace_ptr, static_cast<uint>(info.trace_length),
                             system_charset_info);
    table->field[2]->store(info.missing_bytes, true);
    table->field[3]->store(info.missing_priv, true);
    
    //  Store in IS
    if (schema_table_store_record(thd, table)) return 1;
  }
  return 0;
}