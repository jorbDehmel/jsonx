/**
 * @file
 * @brief Tests the JSONX interpreter
 */

import {JSONX, loadfJSONX, loadsJSONX} from "../src/jsonx";
import {Token} from "../src/lexer";
import {JSONXVarType} from "../src/parser";

function assert(condition: boolean): void {
  if (!condition) {
    throw new Error('Assertion failed');
  }
}

/// Runs test cases
function main(): void {
  let a: JSONXVarType = loadsJSONX('123');
  console.log(a);
  assert(a instanceof Token && a.type == "NUM" &&
         a.text == "123");

  let obj = loadsJSONX("{value: 321}") as JSONX;
  console.log(obj.stringify());
  if (obj instanceof JSONX) {
    a = obj.get("value");
    assert(a instanceof Token && a.type == "NUM" &&
           a.text == "321");
  } else {
    throw new Error("Failed instance assertion");
  }

  obj = loadsJSONX("{a: {b: true}}") as JSONX;
  console.log(obj.stringify());
  if (obj instanceof JSONX) {
    a = obj.get("a");
    console.log(a);
    if (a instanceof JSONX) {
      let b = a.get("b");
      assert(b instanceof Token && b.type == "LIT" &&
             b.text == "true");
    } else {
      throw new Error("Failed instance assertion");
    }
  } else {
    throw new Error("Failed instance assertion");
  }

  obj = loadsJSONX('{a?: 123, a: true, a!: "Hi there!"}') as
        JSONX;
  console.log(obj.stringify());
  console.log((obj as JSONX).get("a"));

  assert(((obj as JSONX).get("a") as Token).text ==
         "\"Hi there!\"");

  obj = loadsJSONX('[123, 321, 123]') as JSONX;
  console.log(obj.stringify());
  assert(((obj as JSONX).get(1) as Token).text == "321");

  obj = loadsJSONX("{a: false, b: this.a}") as JSONX;
  console.log(obj.stringify());
  assert(((obj as JSONX).get(1) as Token).text == "false");

  obj = loadsJSONX('{"a": false, "b": this."a"}') as JSONX;
  console.log(obj.stringify());
  assert(((obj as JSONX).get('b') as Token).text == "false");

  obj = loadsJSONX('{a: "no", b: {a: "yes"}}') as JSONX;
  console.log(obj.stringify());
  assert(((obj as JSONX).get([ "b", "a" ]) as Token).text ==
         '"yes"');

  obj = loadsJSONX('{a: "no", b: {}}') as JSONX;
  console.log(obj.stringify());
  assert((obj as JSONX).get([ "b", "a" ]) == undefined);
  assert(((obj as JSONX).get([ "this" ]) as JSONX) == obj);
  assert(((obj as JSONX).get([ "b" ]) as JSONX).get("parent") ==
         obj);

  // Circular dependency
  obj = loadsJSONX('{a: this, b: this.a.b}') as JSONX;
  console.log(obj.stringify());
  let failed = false;
  try {
    console.log((obj as JSONX).get("b"));
  } catch {
    failed = true;
  }
  assert(failed);

  // Loadf: Note that we are running from ..
  let loaded =
      loadfJSONX('./tests/files/test_1.jsonx') as JSONX;
  console.log(loaded.stringify());

  loaded = loadfJSONX('./tests/files/test_2.jsonx') as JSONX;
  console.log(loaded.stringify());
}

main();
