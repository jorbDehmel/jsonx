/**
 * @file
 * @brief Tests the JSONX interpreter
 */

import {
  BlobInstance,
  BlobManager,
  JSONX,
} from "../src/jsonx";

function assert(condition: boolean): void {
  if (!condition) {
    throw new Error('Assertion failed');
  }
}

/// Runs test cases
function main(): void {
  let a = JSONX.loads('123');
  console.log(a);
  assert((a as BlobInstance).getString() == "123");

  let obj = JSONX.loads("{value: 321}") as JSONX;
  console.log(obj.stringify());
  if (obj instanceof JSONX) {
    a = obj.get("value");
    assert((a as BlobInstance).getString() == "321");
  } else {
    throw new Error("Failed instance assertion");
  }

  obj = JSONX.loads("{a: {b: true}}") as JSONX;
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

  obj = JSONX.loads('{a?: 123, a: true, a!: "Hi there!"}') as
        JSONX;
  console.log(obj.stringify());
  console.log((obj as JSONX).get("a"));

  assert(
      ((obj as JSONX).get("a") as BlobInstance).getString() ==
      "\"Hi there!\"");

  obj = JSONX.loads('[123, 321, 123]') as JSONX;
  console.log(obj.stringify());
  assert(((obj as JSONX).get(1) as BlobInstance).getString() ==
         "321");

  obj = JSONX.loads("{a: false, b: this.a}") as JSONX;
  console.log(obj.stringify());
  assert(((obj as JSONX).get(1) as BlobInstance).getString() ==
         "false");

  obj = JSONX.loads('{"a": false, "b": this."a"}') as JSONX;
  console.log(obj.stringify());
  assert(
      ((obj as JSONX).get('b') as BlobInstance).getString() ==
      "false");

  obj = JSONX.loads('{a: "no", b: {a: "yes"}}') as JSONX;
  console.log(obj.stringify());
  assert(((obj as JSONX).get([ "b", "a" ]) as BlobInstance)
             .getString() == '"yes"');

  obj = JSONX.loads('{a: "no", b: {}}') as JSONX;
  console.log(obj.stringify());
  assert((obj as JSONX).get([ "b", "a" ]) == undefined);
  assert(((obj as JSONX).get([ "this" ]) as JSONX) == obj);
  assert(((obj as JSONX).get([ "b" ]) as JSONX).get("parent") ==
         obj);

  // Circular dependency
  obj = JSONX.loads('{a: this, b: this.a.b}') as JSONX;
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
      JSONX.loadf('./tests/files/test_1.jsonx') as JSONX;
  console.log(loaded.stringify());
  assert((loaded.get("a") as BlobInstance).getString() ==
         '123');
  assert(<BlobInstance>(loaded.get("b")).getString() == '123');
  assert(<BlobInstance>(loaded.get([ "subscope", "a" ]))
             .getString() == '123');
  assert(<BlobInstance>(loaded.get([ "subscope", "b" ]))
             .getString() == '321');

  loaded = JSONX.loadf('./tests/files/test_2.jsonx') as JSONX;
  console.log(loaded.stringify());
  assert(interpret<BlobInstance>(loaded.get([ "test_1", "a" ]))
             .getString() == '123');
  assert(
      interpret<BlobInstance>(loaded.get("data")).getString() ==
      '321');

  loaded = JSONX.loadf('./tests/files/test_3.jsonx') as JSONX;
  console.log(loaded.stringify());
  console.log(loaded.get("local_e"));
  assert(
      Math.abs(+interpret<BlobInstance>(loaded.get('local_e'))
                    .getString() -
               Math.E) < 0.01);
  assert(interpret<BlobInstance>(loaded.get('exponentiated'))
             .getString() == '1024');

  loaded = JSONX.loadf('./tests/files/test_4.jsonx') as JSONX;
  console.log(loaded.stringify());
  console.log((loaded.get("val") as BlobInstance).getString());
  console.log((loaded.get("val2") as BlobInstance).getString());
  assert(Math.abs(+interpret<BlobInstance>(loaded.get('val'))
                       .getString() -
                  Math.acos(1.0)) < 0.01);
  assert(Math.abs(+interpret<BlobInstance>(loaded.get('val'))
                       .getString() -
                  Math.acos(Math.acos(1.0))) < 0.01);

  console.log(
      `Blob usage: ${BlobManager.percentUsed.toPrecision(2)}%`);
}

main();
