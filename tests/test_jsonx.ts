/**
 * @file
 * @brief Tests the JSONX interpreter
 */

import {BlobInstance, BlobManager} from "../src/blob_manager";
import {JSONX, loadfJSONX, loadsJSONX} from "../src/jsonx";

function assert(condition: boolean): void {
  if (!condition) {
    throw new Error('Assertion failed');
  }
}

/// Runs test cases
function main(): void {
  let a = loadsJSONX('123');
  console.log(a);
  assert((a as BlobInstance).getString() == "123");

  let obj = loadsJSONX("{value: 321}") as JSONX;
  console.log(obj.stringify());
  if (obj instanceof JSONX) {
    a = obj.get("value");
    assert((a as BlobInstance).getString() == "321");
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
      assert((b as BlobInstance).getString() == "true");
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

  assert(
      ((obj as JSONX).get("a") as BlobInstance).getString() ==
      "\"Hi there!\"");

  obj = loadsJSONX('[123, 321, 123]') as JSONX;
  console.log(obj.stringify());
  assert(((obj as JSONX).get(1) as BlobInstance).getString() ==
         "321");

  obj = loadsJSONX("{a: false, b: this.a}") as JSONX;
  console.log(obj.stringify());
  assert(((obj as JSONX).get(1) as BlobInstance).getString() ==
         "false");

  obj = loadsJSONX('{"a": false, "b": this."a"}') as JSONX;
  console.log(obj.stringify());
  assert(
      ((obj as JSONX).get('b') as BlobInstance).getString() ==
      "false");

  obj = loadsJSONX('{a: "no", b: {a: "yes"}}') as JSONX;
  console.log(obj.stringify());
  assert(((obj as JSONX).get([ "b", "a" ]) as BlobInstance)
             .getString() == '"yes"');

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

  console.log(JSONX.env.stringify());

  loaded = loadfJSONX('./tests/files/test_3.jsonx') as JSONX;
  console.log(loaded.stringify());
  console.log(loaded.get("local_e"));

  loaded = loadfJSONX('./tests/files/test_4.jsonx') as JSONX;
  console.log(loaded.stringify());
  console.log(loaded.get("val"));

  console.log(
      `Blob usage: ${BlobManager.percentUsed.toPrecision(2)}%`);
}

main();
