/**
 * @brief Tests keywords
 */

import {JSONX, JSONXVar} from "../src/parser";

function assertEq(a: JSONXVar, b: JSONXVar) {
  if (a != b) {
    console.log(`${a.stringify()} != ${b.stringify()}`);
    throw new Error("Failed assertion")
  }
}

function main() {
  console.log("Running keyword tests...");

  let g = JSONX.loads(`
    {
      pp: {
        p: {
          c: {}
        }
      }
    }`);

  let pp = g.get("pp");
  let p = pp.get("p");
  let c = p.get("c");

  console.log("Testing g");
  assertEq(g.get("this"), g);

  assertEq(pp.get("this"), pp);

  console.log("Testing pp");
  assertEq(pp.get("global"), g);

  assertEq(pp.get("parent"), g);

  console.log("Testing p");
  assertEq(p.get("this"), p);
  assertEq(p.get("global"), g);
  assertEq(p.get("parent"), pp);

  console.log("Testing c");
  assertEq(c.get("this"), c);
  assertEq(c.get("global"), g);
  assertEq(c.get("parent"), p);

  console.log("All keyword tests passed.");
}

main();
