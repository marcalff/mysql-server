/* Copyright (c) 2016, 2026, Oracle and/or its affiliates.

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

#include <mysql/components/component_implementation.h>
#include <mysql/components/service_implementation.h>
#include <mysql/components/services/log_builtins.h>

#include <mysql/components/services/mysql_user_defined_type.h>

#include "udt_complex.h"
#include "udt_log.h"

namespace udt_example {

REQUIRES_SERVICE_PLACEHOLDER_AS(log_builtins, log_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(log_builtins_string, log_string_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(udt_registration, udt_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(udt_value_null, val_null_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(udt_value_string, val_string_srv);
REQUIRES_SERVICE_PLACEHOLDER_AS(udt_value_blob, val_blob_srv);

const char *component_name = "udt_example";

// NATIVE TYPE VARCHAR

struct mysql_type_descriptor_t VARCHAR_TYPE_DESCRIPTOR = {
    MYSQL_FIELD_TYPE_VARCHAR,  // mysql_type
    0,                         // type_flags
    0,                         // length
    0,                         // decimals
    nullptr,                   // charset
    false,                     // has_explicit_collation
    nullptr                    // type_ident
};

// TYPE math.complex_number

struct mysql_type_ident_t COMPLEX_NUMBER_TYPE_NAME = {
    "math",           // schema
    "complex_number"  // object
};

struct mysql_type_descriptor_t COMPLEX_NUMBER_TYPE_DESCRIPTOR = {
    MYSQL_FIELD_TYPE_BLOB,     // mysql_type
    0,                         // type_flags
    16,                        // length
    0,                         // decimals
    nullptr,                   // charset
    false,                     // has_explicit_collation
    &COMPLEX_NUMBER_TYPE_NAME  // type_ident
};

// FUNCTION complex_number_from_string

struct mysql_type_descriptor_t *FROM_STRING_ARGS[] = {&VARCHAR_TYPE_DESCRIPTOR};

struct mysql_function_descriptor_t FROM_STRING = {
    "complex_number_from_string",     // name
    &COMPLEX_NUMBER_TYPE_DESCRIPTOR,  // return_type
    1,                                // arguments
    &FROM_STRING_ARGS[0]              // argument_type_array
};

static int complex_number_from_string(UDT_value *result, size_t argument_count,
                                      UDT_value **argument_value_array) {
  fprintf(stderr, "complex_number_from_string()\n");

  assert(argument_count == 1);

  UDT_value *p1 = argument_value_array[0];
  bool p1_is_null{false};
  val_null_srv->get_null(p1, &p1_is_null);

  if (p1_is_null) {
    // complex_number_from_string(NULL) -> NULL
    val_null_srv->set_null(result, true);
    return 0;
  }

  const char *str{nullptr};
  unsigned int len{0};

  val_string_srv->get_utf8mb4(p1, &str, &len);

  if (len == 0) {
    // complex_number_from_string("") -> NULL
    val_null_srv->set_null(result, true);
    return 0;  // FIXME: error ?
  }

  double r;
  double i;
  int n;

  n = sscanf(str, "%lf%lfi", &r, &i);
  if (n != 2) {
    // complex_number_from_string("unparsable") -> NULL
    val_null_srv->set_null(result, true);
    return 0;  // FIXME: error ?
  }

  fprintf(stderr, "complex_number_from_string() found r = %lf, i = %lf\n", r,
          i);

  // Build a binary image with (r, i)
  Complex c(r, i);
  serialized_complex serialized;
  c.serialize_to(serialized);

  // complex_number_from_string("valid string")
  // -> TYPE complex AS BINARY(16)
  val_null_srv->set_null(result, false);
  val_blob_srv->set(result, serialized.ptr(), serialized.length());

  return 0;
}

// FUNCTION complex_number_to_string

struct mysql_type_descriptor_t *TO_STRING_ARGS[] = {
    &COMPLEX_NUMBER_TYPE_DESCRIPTOR};

struct mysql_function_descriptor_t TO_STRING = {
    "complex_number_to_string",  // name
    &VARCHAR_TYPE_DESCRIPTOR,    // return_type
    1,                           // arguments
    &TO_STRING_ARGS[0]           // argument_type_array
};

static int complex_number_to_string(UDT_value *result, size_t argument_count,
                                    UDT_value **argument_value_array) {
  return 0;
}

// FUNCTION complex_number_add

struct mysql_type_descriptor_t *ADD_ARGS[] = {
    &COMPLEX_NUMBER_TYPE_DESCRIPTOR,  // p1
    &COMPLEX_NUMBER_TYPE_DESCRIPTOR   // p2
};

struct mysql_function_descriptor_t ADD = {
    "complex_number_add",             // name
    &COMPLEX_NUMBER_TYPE_DESCRIPTOR,  // return_type
    2,                                // arguments
    &ADD_ARGS[0]                      // argument_type_array
};

static int complex_number_add(UDT_value *result, size_t argument_count,
                              UDT_value **argument_value_array) {
  return 0;
}

static mysql_service_status_t udt_example_init() {
  Log::init(log_srv, log_string_srv);
  log_info("%s: Starting ...", component_name);

  udt_srv->register_type(&COMPLEX_NUMBER_TYPE_DESCRIPTOR, nullptr);
  udt_srv->register_function(&ADD, complex_number_add);
  udt_srv->register_function(&FROM_STRING, complex_number_from_string);
  udt_srv->register_function(&TO_STRING, complex_number_to_string);

  log_info("%s: Started.", component_name);
  return 0;
}

static mysql_service_status_t udt_example_deinit() {
  log_info("%s: Stopping ...", component_name);

  udt_srv->unregister_function(&ADD);
  udt_srv->unregister_function(&FROM_STRING);
  udt_srv->unregister_function(&TO_STRING);
  udt_srv->unregister_type(&COMPLEX_NUMBER_TYPE_DESCRIPTOR);

  log_info("%s: Stopped.", component_name);
  return 0;
}

// clang-format off
BEGIN_COMPONENT_PROVIDES(udt_example)
END_COMPONENT_PROVIDES();
// clang-format on

// clang-format off
BEGIN_COMPONENT_REQUIRES(udt_example)
  REQUIRES_SERVICE_AS(log_builtins, log_srv),
  REQUIRES_SERVICE_AS(log_builtins_string, log_string_srv),
  REQUIRES_SERVICE_AS(udt_registration, udt_srv),
  REQUIRES_SERVICE_AS(udt_value_null, val_null_srv),
  REQUIRES_SERVICE_AS(udt_value_string, val_string_srv),
  REQUIRES_SERVICE_AS(udt_value_blob, val_blob_srv),
END_COMPONENT_REQUIRES();
// clang-format on

// clang-format off
BEGIN_COMPONENT_METADATA(udt_example)
  METADATA("mysql.author", "Oracle Corporation"),
  METADATA("mysql.license", "GPL"),
END_COMPONENT_METADATA();
// clang-format on

// clang-format off
DECLARE_COMPONENT(udt_example, "mysql:udt_example")
  udt_example_init,
  udt_example_deinit
END_DECLARE_COMPONENT();
// clang-format on

// clang-format off
DECLARE_LIBRARY_COMPONENTS
  &COMPONENT_REF(udt_example)
END_DECLARE_LIBRARY_COMPONENTS
// clang-format on

}  // namespace udt_example
