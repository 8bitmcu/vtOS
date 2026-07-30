/*
 * MicroPython XML Interface Library
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 */

#include "py/misc.h"
#include "py/mperrno.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include <stdbool.h>
#include <string.h>

#include "mxml.h"

// XHTML content can nest fairly deeply (nested lists, blockquotes, etc.).
// mxml_node_to_py() recurses per element depth, on the same mp_task stack
// that overflowed silently (heap corruption, no fault at the overflow
// site -- see boards/LILYGO_T_DECK/mpconfigboard.h) for both codec2 and
// the BLE mesh transport. 64 levels is far past any realistic document
// structure; past that, remaining descendant elements are just dropped
// from `children` rather than recursed into further.
#define XML_MAX_DEPTH 64

// ====================================================================
// PARSE: modxml.parse(xml_string) -> nested dict tree, or None
//
// {"tag": str, "attrs": {name: value, ...}, "text": str, "children": [...]}
//
// `text` collects this element's own OPAQUE/TEXT/CDATA child content
// (attribute-less inline text); child *elements* are broken out into
// `children` instead of being flattened into `text`.
// ====================================================================

static mp_obj_t mxml_node_to_py(mxml_node_t *node, int depth) {
  const char *elem = mxmlGetElement(node);
  mp_obj_t tag = mp_obj_new_str(elem, strlen(elem));

  size_t attr_count = mxmlElementGetAttrCount(node);
  mp_obj_t attrs = mp_obj_new_dict(attr_count);
  for (size_t i = 0; i < attr_count; i++) {
    const char *name = NULL;
    const char *value = mxmlElementGetAttrByIndex(node, i, &name);
    if (name == NULL || value == NULL) {
      continue; // Defensive: shouldn't happen for i < attr_count
    }
    mp_obj_dict_store(attrs, mp_obj_new_str(name, strlen(name)),
                      mp_obj_new_str(value, strlen(value)));
  }

  mp_obj_t children = mp_obj_new_list(0, NULL);
  vstr_t text;
  vstr_init(&text, 16); // 16 guarantees buf is never NULL

  for (mxml_node_t *child = mxmlGetFirstChild(node); child != NULL;
       child = mxmlGetNextSibling(child)) {
    switch (mxmlGetType(child)) {
    case MXML_TYPE_ELEMENT:
      if (depth < XML_MAX_DEPTH) {
        mp_obj_list_append(children, mxml_node_to_py(child, depth + 1));
      }
      break;
    case MXML_TYPE_OPAQUE: {
      const char *s = mxmlGetOpaque(child);
      if (s != NULL) {
        vstr_add_str(&text, s);
      }
      break;
    }
    case MXML_TYPE_TEXT: {
      bool ws = false;
      const char *s = mxmlGetText(child, &ws);
      if (s != NULL) {
        if (ws) {
          vstr_add_char(&text, ' ');
        }
        vstr_add_str(&text, s);
      }
      break;
    }
    case MXML_TYPE_CDATA: {
      const char *s = mxmlGetCDATA(child);
      if (s != NULL) {
        vstr_add_str(&text, s);
      }
      break;
    }
    default:
      // COMMENT/DECLARATION/DIRECTIVE/INTEGER/REAL/CUSTOM/IGNORE: not
      // needed for OPF/NCX/nav/XHTML content, skipped.
      break;
    }
  }

  mp_obj_t dict = mp_obj_new_dict(4);
  mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_tag), tag);
  mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_attrs), attrs);
  mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_text), mp_obj_new_str(text.buf, text.len));
  mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_children), children);
  vstr_clear(&text);
  return dict;
}

static mp_obj_t xml_parse(mp_obj_t xml_str_in) {
  const char *s = mp_obj_str_get_str(xml_str_in);

  mxml_options_t *options = mxmlOptionsNew();
  if (options == NULL) {
    mp_raise_OSError(MP_ENOMEM);
  }
  // Whole-string OPAQUE text nodes instead of the MXML_TYPE_TEXT default
  // (which word-splits content into separate sibling nodes) -- far
  // simpler to reassemble for our purposes.
  mxmlOptionsSetTypeValue(options, MXML_TYPE_OPAQUE);

  // Passing an explicit `top` (rather than NULL) means the source string
  // doesn't have to start with its own "<?xml ...?>" declaration to
  // parse -- some real-world EPUB XHTML omits it. This is the specific
  // strictness yxml had (hard-halt on any deviation from well-formed
  // XML) that we're trading away by moving to mxml.
  mxml_node_t *top = mxmlNewXML("1.0");
  if (top == NULL) {
    mxmlOptionsDelete(options);
    mp_raise_OSError(MP_ENOMEM);
  }

  mxml_node_t *result = mxmlLoadString(top, options, s);
  mxmlOptionsDelete(options);

  if (result == NULL) {
    mxmlDelete(top);
    return mp_const_none; // Malformed XML -- caller sees None, doesn't raise
  }

  mxml_node_t *root = NULL;
  for (mxml_node_t *n = mxmlGetFirstChild(top); n != NULL; n = mxmlGetNextSibling(n)) {
    if (mxmlGetType(n) == MXML_TYPE_ELEMENT) {
      root = n;
      break;
    }
  }

  mp_obj_t py_tree = root != NULL ? mxml_node_to_py(root, 0) : mp_const_none;

  // mxml's tree is plain malloc()'d memory, invisible to MicroPython's
  // GC -- must be freed explicitly here rather than left for a Python
  // finalizer (which this port doesn't reliably run). Safe to do before
  // returning since mxml_node_to_py() already copied everything needed
  // into GC-tracked mp_obj_t structures.
  mxmlDelete(top);

  return py_tree;
}
static MP_DEFINE_CONST_FUN_OBJ_1(xml_parse_obj, xml_parse);

// ====================================================================
// MODULE REGISTRATION
// ====================================================================

static const mp_rom_map_elem_t modxml_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modxml)},
    {MP_ROM_QSTR(MP_QSTR_parse), MP_ROM_PTR(&xml_parse_obj)},
};
static MP_DEFINE_CONST_DICT(modxml_module_globals, modxml_module_globals_table);

const mp_obj_module_t modxml_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modxml_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modxml, modxml_user_cmodule);
