/* Copyright (c) 2000, 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "my_macros.h"
#include "my_psi_config.h"

#include <unordered_map>

#include "map_helpers.h"
#include "my_alloc.h"
#include "my_dbug.h"
#include "sql/mysqld_cs.h"

#include "mysql/components/services/bits/mysql_rwlock_bits.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "mysql/components/services/bits/psi_memory_bits.h"
#include "mysql/components/services/bits/psi_rwlock_bits.h"
#include "mysql/psi/mysql_memory.h"
#include "mysql/psi/mysql_rwlock.h"
#include "mysql/strings/m_ctype.h"
#include "sql/item_create.h"
#include "sql/sql_class.h"
#include "sql/thr_malloc.h"

#include "mysql/components/services/mysql_user_defined_type.h"
#include "sql/current_thd.h"
#include "sql/server_component/mysql_user_defined_type_imp.h"
#include "sql/sql_udt.h"
#include "sql/warn_not_implemented.h"

//-------------------------------------------------------------------
// Parser, create func
//-------------------------------------------------------------------

Item *Create_udt_func::create(THD *thd, const POS &pos,
                              udt_function_record *udt_function,
                              PT_item_list *item_list) {
  fprintf(stderr, "Create_udt_func::create_func()\n");
  Item *item = new (thd->mem_root) Item_udt_func(pos, udt_function, item_list);
  return item;
}

//-------------------------------------------------------------------
// Internal hash
//-------------------------------------------------------------------

struct udt_type_record {
  mysql_type_descriptor_t *td;
  void *impl;
  ulonglong ref_count;
};

struct udt_function_record {
  mysql_function_descriptor_t *fd;
  eval_function_t impl;
  ulonglong ref_count;
};

static bool initialized = false;
static mysql_rwlock_t THR_LOCK_udt;
static MEM_ROOT MEM_ROOT_udt;
static constexpr const size_t UDT_ALLOC_BLOCK_SIZE{1024};
static collation_unordered_map<std::string, udt_type_record *> *udt_type_hash{
    nullptr};
static collation_unordered_map<std::string, udt_function_record *>
    *udt_function_hash{nullptr};

static PSI_rwlock_key key_rwlock_THR_LOCK_udt;

static PSI_memory_key key_memory_udt_mem;

#ifdef HAVE_PSI_INTERFACE
static PSI_rwlock_info all_udt_rwlocks[] = {{&key_rwlock_THR_LOCK_udt,
                                             "THR_LOCK_udt", PSI_FLAG_SINGLETON,
                                             0, PSI_DOCUMENT_ME}};

static PSI_memory_info all_udt_memory[] = {{&key_memory_udt_mem, "udt_mem",
                                            PSI_FLAG_ONLY_GLOBAL_STAT, 0,
                                            "Shared structure of UDTs."}};

static void init_udt_psi_keys(void) {
  const char *category = "sql";
  int count;

  count = static_cast<int>(array_elements(all_udt_rwlocks));
  mysql_rwlock_register(category, all_udt_rwlocks, count);

  count = static_cast<int>(array_elements(all_udt_memory));
  mysql_memory_register(category, all_udt_memory, count);
}
#endif

void udt_init_globals() {
  DBUG_TRACE;
  if (initialized) return;

#ifdef HAVE_PSI_INTERFACE
  init_udt_psi_keys();
#endif

  mysql_rwlock_init(key_rwlock_THR_LOCK_udt, &THR_LOCK_udt);
  init_sql_alloc(key_memory_udt_mem, &MEM_ROOT_udt, UDT_ALLOC_BLOCK_SIZE);

  udt_type_hash = new collation_unordered_map<std::string, udt_type_record *>(
      system_charset_info, key_memory_udt_mem);

  udt_function_hash =
      new collation_unordered_map<std::string, udt_function_record *>(
          system_charset_info, key_memory_udt_mem);
}

void udt_deinit_globals() {
  DBUG_TRACE;

  if (udt_function_hash != nullptr) {
    delete udt_function_hash;
    udt_function_hash = nullptr;
  }

  if (udt_type_hash != nullptr) {
    delete udt_type_hash;
    udt_type_hash = nullptr;
  }

  MEM_ROOT_udt.Clear();
  initialized = false;

  mysql_rwlock_destroy(&THR_LOCK_udt);
}

udt_function_record *acquire_udt_function(const char *name) {
  udt_function_record *record = nullptr;
  std::string key = name;

  mysql_rwlock_wrlock(&THR_LOCK_udt);

  const auto it = udt_function_hash->find(key);
  if (it != udt_function_hash->end()) {
    record = it->second;
    record->ref_count++;
  }

  mysql_rwlock_unlock(&THR_LOCK_udt);

  fprintf(stderr, "acquire_udt_function() name %s record %p\n", key.c_str(),
          record);

  return record;
}

void release_udt_function(udt_function_record *record) {
  std::string key = record->fd->name;

  mysql_rwlock_wrlock(&THR_LOCK_udt);

  const auto it = udt_function_hash->find(key);
  if (it != udt_function_hash->end()) {
    auto hash_record = it->second;
    hash_record->ref_count--;
    assert(hash_record == record);
  }

  mysql_rwlock_unlock(&THR_LOCK_udt);
}

//-------------------------------------------------------------------
// Service
//-------------------------------------------------------------------

class UDT_value {
 public:
  UDT_value(Item *item) : m_item(item) {}

  void set_null(bool is_null);
  void get_null(bool *is_null);

  void set_utf8mb4(const char *str, unsigned int length);
  void get_utf8mb4(const char **str, unsigned int *length);

 private:
  Item *m_item;
  String m_string_data;
};

void UDT_value::set_null(bool is_null) {}

void UDT_value::get_null(bool *is_null) {
  assert(m_item != nullptr);  // readable

  // Defensive, called from 3rd party components.
  if (m_item != nullptr) {
    *is_null = m_item->is_null();
  }
}

void UDT_value::get_utf8mb4(const char **str, unsigned int *length) {
  if (m_item != nullptr) {
    String *data = m_item->val_str(&m_string_data);
    *str = data->ptr();
    *length = data->length();
  }
}

DEFINE_METHOD(int, mysql_udt_registration_imp::register_type,
              (mysql_type_descriptor_t * td, void *impl)) {
  fprintf(stderr, "mysql_udt_registration_imp::register_type() %p %p\n", td,
          impl);

  return 0;
}

DEFINE_METHOD(int, mysql_udt_registration_imp::unregister_type,
              (mysql_type_descriptor_t * td)) {
  fprintf(stderr, "mysql_udt_registration_imp::unregister_type() %p\n", td);
  return 0;
}

DEFINE_METHOD(int, mysql_udt_registration_imp::register_function,
              (mysql_function_descriptor_t * fd, eval_function_t impl)) {
  fprintf(stderr, "mysql_udt_registration_imp::register_function() %p %p\n", fd,
          impl);

  int rc = 0;
  udt_function_record *record;

  record =
      (udt_function_record *)MEM_ROOT_udt.Alloc(sizeof(udt_function_record));
  record->fd = fd;
  record->impl = impl;
  record->ref_count = 0;

  std::string key = record->fd->name;

  mysql_rwlock_wrlock(&THR_LOCK_udt);

  auto res = udt_function_hash->emplace(key, record);

  if (!res.second) {
    rc = 1;  // Duplicate
  }

  mysql_rwlock_unlock(&THR_LOCK_udt);

  return rc;
}

DEFINE_METHOD(int, mysql_udt_registration_imp::unregister_function,
              (mysql_function_descriptor_t * fd)) {
  fprintf(stderr, "mysql_udt_registration_imp::unregister_function() %p\n", fd);
  return 0;
}

DEFINE_METHOD(void, mysql_udt_value_null_imp::set_null,
              (UDT_value * f, bool is_null)) {
  fprintf(stderr, "mysql_udt_value_null_imp::set_null()\n");
}

DEFINE_METHOD(void, mysql_udt_value_null_imp::get_null,
              (UDT_value * f, bool *is_null)) {
  fprintf(stderr, "mysql_udt_value_null_imp::get_null()\n");
  assert(f != nullptr);
  assert(is_null != nullptr);
  f->get_null(is_null);
}

DEFINE_METHOD(void, mysql_udt_value_string_imp::set_utf8mb4,
              (UDT_value * f, const char *value, unsigned int length)) {
  fprintf(stderr, "mysql_udt_value_string_imp::set_utf8mb4()\n");
}

DEFINE_METHOD(void, mysql_udt_value_string_imp::get_utf8mb4,
              (UDT_value * f, const char **str, unsigned int *length)) {
  fprintf(stderr, "mysql_udt_value_string_imp::get_utf8mb4()\n");
  assert(f != nullptr);
  assert(str != nullptr);
  assert(length != nullptr);
  f->get_utf8mb4(str, length);
}

DEFINE_METHOD(void, mysql_udt_value_blob_imp::set,
              (UDT_value * f, const unsigned char *val, unsigned int len)) {
  fprintf(stderr, "mysql_udt_value_blob_imp::set()\n");
}

DEFINE_METHOD(void, mysql_udt_value_blob_imp::get,
              (UDT_value * f, unsigned char *val, unsigned int *len)) {
  fprintf(stderr, "mysql_udt_value_blob_imp::get()\n");
}

//-------------------------------------------------------------------
// Runtime, item tree
//-------------------------------------------------------------------

Item_udt_func::Item_udt_func(const POS &pos, udt_function_record *udt_function,
                             PT_item_list *opt_list)
    : Item_func(pos, opt_list), m_udt_function(udt_function) {}

bool Item_udt_func::do_itemize(Parse_context *pc, Item **res) {
  fprintf(stderr, "Item_udt_func::do_itemize()\n");
  if (super::do_itemize(pc, res)) {
    return true;
  }

  return false;
}

bool Item_udt_func::resolve_type_inner(THD *thd) {
  fprintf(stderr, "Item_udt_func::resolve_type_inner()\n");

  const mysql_type_descriptor_t *td = m_udt_function->fd->return_type;
  auto td2 = static_cast<enum_field_types> (td->mysql_type);

  // FIXME: see Item_func_sp::resolve_type()
  set_data_type(td2);
  return false;
}

int build_argument_value_array(Item_udt_func *that,
                               mysql_function_descriptor_t *fd,
                               size_t *argument_count,
                               UDT_value ***argument_value_array) {
  size_t count = fd->argument_count;

  if (count == 0) {
    *argument_count = 0;
    *argument_value_array = nullptr;
    return 0;
  }

  UDT_value **array = new UDT_value *[count];
  Item *item;

  for (size_t i = 0; i < count; i++) {
    // FIXME: build proper value
    item = that->get_arg(i);
    array[i] = new UDT_value(item);
  }

  *argument_count = count;
  *argument_value_array = array;
  return 0;
}

type_conversion_status Item_udt_func::save_in_field_inner(Field *field,
                                                          bool no_conversions) {
  fprintf(stderr, "Item_udt_func::save_in_field_inner() field %s\n",
          field->field_name);
  int rc;

  // 1: Find the field actual type

  const mysql_type_descriptor_t *left_td = nullptr;

  // 2: Check the function return type

  const mysql_type_descriptor_t *right_td = m_udt_function->fd->return_type;
  eval_function_t eval = m_udt_function->impl;

  // 3: Build a field value

  UDT_value *result_value = nullptr;
  size_t param_count{0};
  UDT_value **param_array{nullptr};

  rc = build_argument_value_array(this, m_udt_function->fd, &param_count,
                                  &param_array);

  // 4: Evaluate the function into the value

  fprintf(stderr, "Item_udt_func::save_in_field_inner() field %s before eval\n",
          field->field_name);

  rc = (*eval)(result_value, param_count, param_array);

  fprintf(stderr, "Item_udt_func::save_in_field_inner() field %s after eval\n",
          field->field_name);

  // 5: Set the value to the field

  return TYPE_ERR_BAD_VALUE;
}

double Item_udt_func::val_real() {
  assert(false);
  return 0.0;
}

longlong Item_udt_func::val_int() {
  assert(false);
  return 0;
}

String *Item_udt_func::val_str(String *str) {
  assert(false);
  return nullptr;
}

bool Item_udt_func::val_date(Date_val *date, my_time_flags_t flags) {
  assert(false);
  return false;
}

bool Item_udt_func::val_time(Time_val *time) {
  assert(false);
  return false;
}

bool Item_udt_func::val_datetime(Datetime_val *dt, my_time_flags_t flags) {
  assert(false);
  return false;
}

const char *Item_udt_func::func_name() const {
  return m_udt_function->fd->name;
}
