"use strict";

/**
 * @file
 * @brief
 */

import {assert} from "console";
import {BlobManager, BlobInstance} from "../src/blob_manager";

/// Run test cases
function main() {
  let manager = new BlobManager(8);
  let a = new BlobInstance(manager);

  // Allocate all the allowed memory
  a.set(Uint8Array.from("Hi there"));

  // This points to the same data
  let b = a.duplicate();

  // Should still be at 8 bytes usage after this
  a.free();

  // Valid if the data wasn't deleted
  assert(b.get().toString() == "Hi there");

  // Attempt to allocate beyond the allowed amount
  a = b.duplicate();

  // Should throw an error
  let didFail = false;
  try {
    a.set(a.get());
  } catch {
    didFail = true;
  }
  assert(didFail);
}

main();
