/**
 * @file
 * @brief Tests the JSONX interpreter
 */

import {
  BlobInstance,
  BlobManager,
  JSONX,
  JSONXVarType,
} from "../src/parser";

/// Note that this FULLY RESOLVES everything recursively!
function printJSONX(what: JSONXVarType,
                    tabbing: string = ""): void {
  if (what instanceof JSONX) {
    console.log("{");
    for (const entry of what.variables) {
      process.stdout.write(`${tabbing}  ${entry.name}`);

      if (entry.weight < 0) {
        process.stdout.write('?'.repeat(-entry.weight));
      } else if (entry.weight > 0) {
        process.stdout.write('!'.repeat(entry.weight));
      }

      process.stdout.write(": ");
      printJSONX(entry.value, tabbing + "  ");
    }
    console.log(tabbing + "}");
  } else if (what instanceof BlobInstance) {
    console.log(`"${what.getString()}"`);
  } else { // Lambda
    process.stdout.write(`${what.argName} => `);
    if (what.body instanceof Function) {
      console.log("<EXTERNAL>");
    } else {
      printJSONX(what.body, tabbing);
    }
  }
}

function assert(condition: boolean): void {
  if (!condition) {
    throw new Error('Assertion failed');
  }
}

function getblob(what: any): BlobInstance {
  assert(what != undefined);
  assert(what instanceof BlobInstance);
  return what as BlobInstance;
}

function get(on: any, path: (string|number)[]): any {
  let out: any = on;
  for (const tok of path) {
    if (!(out instanceof JSONX)) {
      return undefined;
    }
    out = out.get(tok);
  }
  return out;
}

/// Runs test cases
function main(): void {
  let obj = JSONX.loads("{value: 321}") as JSONX;
  console.log(printJSONX(obj));
  if (obj instanceof JSONX) {
    let a = obj.get("value");
    assert(getblob(a).getString() == "321");
  } else {
    throw new Error("Failed instance assertion");
  }

  obj = JSONX.loads("{a: {b: true}}") as JSONX;
  console.log(printJSONX(obj));
  if (obj instanceof JSONX) {
    let a = obj.get("a") as JSONX;
    console.log(a);
    if (a instanceof JSONX) {
      let b = a.get("b");
      assert(getblob(b).getString() == "true");
    } else {
      throw new Error("Failed instance assertion");
    }
  } else {
    throw new Error("Failed instance assertion");
  }

  obj = JSONX.loads('{a?: 123, a: true, a!: "Hi there!"}') as
        JSONX;
  console.log(printJSONX(obj));
  console.log((obj as JSONX).get("a"));

  assert(getblob((obj as JSONX).get("a")).getString() ==
         "\"Hi there!\"");

  obj = JSONX.loads('[123, 321, 123]') as JSONX;
  console.log(printJSONX(obj));
  assert(getblob((obj as JSONX).get(1)).getString() == "321");

  obj = JSONX.loads("{a: false, b: this.a}") as JSONX;
  console.log(printJSONX(obj));
  assert(getblob((obj as JSONX).get(1)).getString() == "false");

  obj = JSONX.loads('{"a": false, "b": this."a"}') as JSONX;
  console.log(printJSONX(obj));
  assert(getblob((obj as JSONX).get('b')).getString() ==
         "false");

  obj = JSONX.loads('{a: "no", b: {a: "yes"}}') as JSONX;
  console.log(printJSONX(obj));
  assert(getblob(get(obj, [ "b", "a" ])).getString() ==
         '"yes"');

  obj = JSONX.loads('{a: "no", b: {}}') as JSONX;
  console.log(printJSONX(obj));
  assert(get(obj, [ "b", "a" ]) == undefined);
  assert(((obj as JSONX).get("this") as JSONX) == obj);
  assert(((obj as JSONX).get("b") as JSONX).get("parent") ==
         obj);

  // Circular dependency
  obj = JSONX.loads('{a: this, b: this.a.b}') as JSONX;
  console.log(printJSONX(obj));
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
  console.log(printJSONX(loaded));
  assert(getblob(loaded.get("a")).getString() == '123');
  assert(getblob(loaded.get("b")).getString() == '123');
  assert(
      getblob(get(loaded, [ "subscope", "a" ])).getString() ==
      '123');
  assert(
      getblob(get(loaded, [ "subscope", "b" ])).getString() ==
      '321');

  loaded = JSONX.loadf('./tests/files/test_2.jsonx') as JSONX;
  console.log(printJSONX(loaded));
  assert(getblob(get(loaded, [ "test_1", "a" ])).getString() ==
         '123');
  assert(getblob(loaded.get("data")).getString() == '321');

  loaded = JSONX.loadf('./tests/files/test_3.jsonx') as JSONX;
  console.log(printJSONX(loaded));
  assert(Math.abs(+getblob(loaded.get('local_e')).getString() -
                  Math.E) < 0.01);
  assert(getblob(loaded.get('exponentiated')).getString() ==
         '1024');

  loaded = JSONX.loadf('./tests/files/test_4.jsonx') as JSONX;
  console.log(printJSONX(loaded));
  console.log(getblob(loaded.get("val")).getString());
  console.log(getblob(loaded.get("val2")).getString());
  assert(Math.abs(+getblob(loaded.get('val')).getString() -
                  Math.acos(1.0)) < 0.01);
  assert(Math.abs(+getblob(loaded.get('val')).getString() -
                  Math.acos(Math.acos(1.0))) < 0.01);

  console.log(`Blob usage: ${
      (100.0 * BlobManager.bytesUsed / BlobManager.maxBytes)
          .toPrecision(2)}%`);
}

main();
