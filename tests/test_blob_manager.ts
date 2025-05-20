"use strict";

/**
 * @file
 * @brief
 */

import {BlobManager, BlobInstance} from "../src/blob_manager";

function assert(condition: boolean, msg?: string): void {
  if (!condition) {
    throw new Error(msg == undefined ? 'Assertion failed'
                                     : msg);
  }
}

/// Run test cases
function main() {
  let encoder = new TextEncoder();
  let decoder = new TextDecoder();
  let manager = new BlobManager(8);
  assert(manager.usage() == 0);

  let a = new BlobInstance(manager);
  assert(manager.usage() == 0);

  // Allocate all the allowed memory
  a.set(encoder.encode("Hi there"));
  assert(manager.usage() == 8);

  // This points to the same data
  let b = a.duplicate();
  assert(manager.usage() == 8);

  // Should still be at 8 bytes usage after this
  a.free();
  assert(manager.usage() == 8);

  // Valid if the data wasn't deleted
  assert(decoder.decode(b.get()) == "Hi there",
         'Failed to get setted data');

  // Attempt to allocate beyond the allowed amount
  a = b.duplicate();
  assert(manager.usage() == 8);

  // Should throw an error
  let didFail = false;
  try {
    a.set(a.get());
  } catch {
    didFail = true;
  }
  assert(didFail, 'Expected out-of-mem, but saw none!');

  // This will have deallocated a upon error

  console.log("All BlobManager unit tests passed.");
}

main();
