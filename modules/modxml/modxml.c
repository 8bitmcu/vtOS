/*
 * MicroPython XML Interface Library
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 */

#include "py/misc.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include <stdbool.h>
#include <string.h>

#include "yxml.h"

// 512 bytes on the C stack is plenty for deep XML nesting
// and perfectly safe for the ESP32's default 8KB task stack.
#ifndef XML_YXML_STACK_SIZE
#define XML_YXML_STACK_SIZE 512
#endif

// ====================================================================
// GENERIC DICTIONARY EXTRACTOR
// modxml.extract(xml_string, target_tag, ("field1", "field2"))
// ====================================================================

static mp_obj_t xml_extract(size_t n_args, const mp_obj_t *args) {
  size_t xml_len;
  const char *xml = mp_obj_str_get_data(args[0], &xml_len);
  const char *target_tag = mp_obj_str_get_str(args[1]);

  size_t num_fields;
  mp_obj_t *fields_arr;
  mp_obj_get_array(args[2], &num_fields, &fields_arr);

  // Pre-allocate field names and keys on the C stack
  const char *field_names[16];
  mp_obj_t field_keys[16];
  for (size_t i = 0; i < num_fields; i++) {
    field_names[i] = mp_obj_str_get_str(fields_arr[i]);
    field_keys[i] = mp_obj_new_str(field_names[i], strlen(field_names[i]));
  }

  yxml_t parser;
  char stack_buf[XML_YXML_STACK_SIZE]; // Stack allocated, zero GC overhead
  yxml_init(&parser, stack_buf, XML_YXML_STACK_SIZE);

  mp_obj_t list = mp_obj_new_list(0, NULL);
  mp_obj_t current_dict = mp_const_none;

  vstr_t vstr;
  vstr_init(&vstr, 16); // Init with 16 bytes guarantees buf is never NULL

  bool in_target = false;
  int target_depth = -1;
  int depth = 0;
  int current_field_idx = -1;
  int current_field_depth = -1;

  for (size_t i = 0; i < xml_len; i++) {
    yxml_ret_t r = yxml_parse(&parser, (unsigned char)xml[i]);
    if (r < 0)
      break; // Malformed XML gracefully halts

    switch (r) {
    case YXML_ELEMSTART:
      depth++;
      if (parser.elem == NULL)
        break; // Safety check

      if (!in_target && strcmp(parser.elem, target_tag) == 0) {
        in_target = true;
        target_depth = depth;
        current_dict = mp_obj_new_dict(num_fields);
      } else if (in_target && current_field_idx == -1) {
        for (size_t j = 0; j < num_fields; j++) {
          if (strcmp(parser.elem, field_names[j]) == 0) {
            current_field_idx = j;
            current_field_depth = depth;
            vstr_reset(&vstr);
            break;
          }
        }
      }
      break;

    case YXML_CONTENT:
      // We only care about data if we are tracking a field
      if (current_field_idx != -1) {
        vstr_add_str(&vstr, parser.data);
      }
      break;
    case YXML_ATTRVAL:
      if (current_field_idx != -1) {
        // Check for link/href specifically
        if (strcmp(parser.elem, "link") == 0 &&
            strcmp(parser.attr, "href") == 0) {
          vstr_add_str(&vstr, parser.data);
        }
      }
      break;

    case YXML_ELEMEND:
      // Close the field using our own depth bookkeeping, not parser.elem --
      // relying on parser.elem still naming the closing element *at the
      // exact moment* YXML_ELEMEND fires is an assumption about yxml's
      // internal timing we can't fully verify. depth is entirely ours, so
      // comparing against the depth we recorded when the field opened is
      // unambiguous regardless of that timing.
      if (current_field_idx != -1 && depth == current_field_depth) {
        mp_obj_dict_store(current_dict, field_keys[current_field_idx],
                          mp_obj_new_str(vstr.buf, vstr.len));
        current_field_idx = -1;
        current_field_depth = -1;
        vstr_reset(&vstr);
      }

      if (in_target && depth == target_depth) {
        mp_obj_list_append(list, current_dict);
        in_target = false;
        current_dict = mp_const_none;
      }
      depth--;
      break;

    default:
      break;
    }
  }

  vstr_clear(&vstr);
  return list;
}
// Signature requires exactly 3 arguments: (xml_str, target_tag, fields_tuple)
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(xml_extract_obj, 3, 3, xml_extract);

// ====================================================================
// ORIGINAL FIND / FINDALL (Raw Markup Slicing)
// ====================================================================

typedef void (*xml_match_cb_t)(void *ctx, const char *content, size_t len);

static void yxml_collect_matches(const char *xml, size_t xml_len,
                                 const char *tag, xml_match_cb_t cb, void *ctx,
                                 bool stop_after_first, bool *found) {
  yxml_t parser;
  char stack_buf[XML_YXML_STACK_SIZE]; // Stack allocated
  yxml_init(&parser, stack_buf, XML_YXML_STACK_SIZE);

  bool in_target = false;
  bool awaiting_content_start = false;
  bool have_content_end = false;
  int target_depth = -1;
  int depth = 0;
  size_t content_start = 0;
  size_t content_end = 0;

  *found = false;

  for (size_t i = 0; i < xml_len; i++) {
    yxml_ret_t r = yxml_parse(&parser, (unsigned char)xml[i]);
    if (r < 0)
      break;

    if (in_target && awaiting_content_start &&
        (r == YXML_CONTENT || r == YXML_ELEMSTART || r == YXML_ELEMEND)) {
      content_start = i;
      awaiting_content_start = false;
    }

    switch (r) {
    case YXML_ELEMSTART:
      depth++;
      if (!in_target && parser.elem != NULL && strcmp(parser.elem, tag) == 0) {
        in_target = true;
        target_depth = depth;
        awaiting_content_start = true;
        have_content_end = false;
      }
      break;
    case YXML_CONTENT:
      if (in_target && depth == target_depth) {
        content_end = i + 1;
        have_content_end = true;
      }
      break;
    case YXML_ELEMEND:
      if (in_target && depth > target_depth) {
        content_end = i + 1;
        have_content_end = true;
      }
      if (in_target && depth == target_depth) {
        size_t end = have_content_end ? content_end : content_start;
        if (end < content_start)
          end = content_start;
        cb(ctx, xml + content_start, end - content_start);
        *found = true;
        in_target = false;
        if (stop_after_first) {
          return; // Safe to return, stack_buf cleans itself up
        }
      }
      depth--;
      break;
    default:
      break;
    }
  }
}

typedef struct {
  mp_obj_t result;
} find_ctx_t;
static void find_cb(void *ctx_in, const char *content, size_t len) {
  ((find_ctx_t *)ctx_in)->result = mp_obj_new_str(content, len);
}

// modxml.find takes 2 args: (xml_str, tag)
static mp_obj_t xml_find(mp_obj_t xml_str_in, mp_obj_t tag_in) {
  size_t xml_len;
  const char *xml = mp_obj_str_get_data(xml_str_in, &xml_len);
  const char *tag = mp_obj_str_get_str(tag_in);
  find_ctx_t ctx = {.result = mp_const_none};
  bool found = false;
  yxml_collect_matches(xml, xml_len, tag, find_cb, &ctx, true, &found);
  return found ? ctx.result : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(xml_find_obj, xml_find);

typedef struct {
  mp_obj_t list;
} findall_ctx_t;
static void findall_cb(void *ctx_in, const char *content, size_t len) {
  mp_obj_list_append(((findall_ctx_t *)ctx_in)->list,
                     mp_obj_new_str(content, len));
}

// modxml.findall takes 2 args: (xml_str, tag)
static mp_obj_t xml_findall(mp_obj_t xml_str_in, mp_obj_t tag_in) {
  size_t xml_len;
  const char *xml = mp_obj_str_get_data(xml_str_in, &xml_len);
  const char *tag = mp_obj_str_get_str(tag_in);
  mp_obj_t list = mp_obj_new_list(0, NULL);
  findall_ctx_t ctx = {.list = list};
  bool found = false;
  yxml_collect_matches(xml, xml_len, tag, findall_cb, &ctx, false, &found);
  return list;
}
static MP_DEFINE_CONST_FUN_OBJ_2(xml_findall_obj, xml_findall);

// ====================================================================
// MODULE REGISTRATION
// ====================================================================

// Define the module's contents directly, no class wrapper needed
static const mp_rom_map_elem_t modxml_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modxml)},
    {MP_ROM_QSTR(MP_QSTR_find), MP_ROM_PTR(&xml_find_obj)},
    {MP_ROM_QSTR(MP_QSTR_findall), MP_ROM_PTR(&xml_findall_obj)},
    {MP_ROM_QSTR(MP_QSTR_extract), MP_ROM_PTR(&xml_extract_obj)},
};
static MP_DEFINE_CONST_DICT(modxml_module_globals, modxml_module_globals_table);

const mp_obj_module_t modxml_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modxml_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modxml, modxml_user_cmodule);
