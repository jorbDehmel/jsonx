/**
 * @file
 * @brief Tests the JSONX interpreter
 */

import {
  JSONX,
  JSONXBlob,
} from "../src/parser";

function assert(condition: boolean,
                msg: string = 'Assertion failed'): void {
  if (!condition) {
    throw new Error(msg);
  }
}

function assertEq(a: any, b: any) {
  if (a != b) {
    throw new Error(`Expected ${a} , ${b}`);
  }
}

function getblob(what: any): JSONXBlob {
  assert(what != undefined, 'Potential blob is undefined');
  assert(what instanceof JSONXBlob,
         'Potential blob is not a blob');
  return what as JSONXBlob;
}

function get(on: any, path: (string|number)[]): any {
  let out: any = on;
  for (const tok of path) {
    out = out.get(tok);
  }
  return out;
}

/// Runs test cases
function main(): void {
  console.log('Running test_jsonx test cases...');

  console.log(JSONX.env.stringify());

  let obj = JSONX.loads("{value: 321}");
  console.log(obj.stringify());
  if (obj instanceof JSONX) {
    let a = obj.get("value");
    assertEq(getblob(a).getString(), "321");
  } else {
    throw new Error("Failed instance assertion");
  }

  obj = JSONX.loads("{a: {b: true}}");
  console.log(obj.stringify());
  if (obj instanceof JSONX) {
    let a = obj.get("a");
    if (a instanceof JSONX) {
      let b = a.get("b");
      assertEq(getblob(b).getString(), "true");
    } else {
      throw new Error("Failed instance assertion");
    }
  } else {
    throw new Error("Failed instance assertion");
  }

  obj = JSONX.loads('{a?: 123, a: true, a!: "Hi there!"}');
  console.log(obj.stringify());

  assertEq(getblob(obj.get("a")).getString(), "\"Hi there!\"");

  obj = JSONX.loads('[123, 321, 123]');
  console.log(obj.stringify());
  assertEq(getblob(obj.get(1)).getString(), "321");

  obj = JSONX.loads("{a: false, b: this.a}");
  console.log(obj.stringify());
  assertEq(getblob(obj.get('b')).getString(), "false");

  obj = JSONX.loads('{"a": false, "b": this."a"}');
  console.log(obj.stringify());
  assertEq(getblob(obj.get('"b"')).getString(), "false");

  obj = JSONX.loads('{a: "no", b: {a: "yes"}}');
  console.log(obj.stringify());
  assertEq(getblob(get(obj, [ "b", "a" ])).getString(),
           '"yes"');

  obj = JSONX.loads('{a: "no", b: {}}');
  console.log(obj.stringify());
  assertEq(get(obj, [ "b", "a" ]), undefined);
  assertEq(obj.get("this"), obj);
  assertEq(obj.get("b").get("parent"), obj);

  // Circular dependency
  obj = JSONX.loads('{a: this, b: this.a.b}');
  console.log(obj.stringify());
  let failed = false;
  try {
    console.log(obj.get("b").stringify());
  } catch {
    failed = true;
  }
  assert(failed);

  // Loadf: Note that we are running from ..
  let loaded = JSONX.loadf('./tests/files/test_1.jsonx');
  console.log(loaded.stringify());
  assertEq(getblob(loaded.get("\"a\"")).getString(), '123');
  assertEq(getblob(loaded.get("\"b\"")).getString(), '123');
  assertEq(getblob(get(loaded, [ "\"subscope\"", "\"a\"" ]))
               .getString(),
           '123');
  assertEq(getblob(get(loaded, [ "\"subscope\"", "\"b\"" ]))
               .getString(),
           '321');

  loaded = JSONX.loadf('./tests/files/test_2.jsonx');
  console.log(loaded.stringify());
  assertEq(getblob(get(loaded, [ "test_1", "a" ])).getString(),
           '123');
  assertEq(getblob(loaded.get("data")).getString(), '321');

  loaded = JSONX.loadf('./tests/files/test_3.jsonx');
  console.log(loaded.stringify());
  assert(Math.abs(+getblob(loaded.get('local_e')).getString() -
                  Math.E) < 0.01);
  assertEq(getblob(loaded.get('exponentiated')).getString(),
           '1024');

  loaded = JSONX.loadf('./tests/files/test_4.jsonx');
  console.log(loaded.stringify());
  assert(Math.abs(+getblob(loaded.get('val')).getString() -
                  Math.acos(1.0)) < 0.01);
  assert(Math.abs(+getblob(loaded.get('val')).getString() -
                  Math.acos(Math.acos(1.0))) < 0.01);

  console.log(`Blob usage: ${
      (100.0 * JSONXBlob.bytesUsed / JSONXBlob.maxBytes)
          .toPrecision(2)}%`);

  console.log('All test_jsonx test cases ran.');
}

main();
