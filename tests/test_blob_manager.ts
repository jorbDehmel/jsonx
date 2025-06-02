/**
 * @file
 * @brief
 */

import {BlobInstance, BlobManager} from "../src/blob_manager";

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

  BlobManager.maxBytes = 8;
  assert(BlobManager.bytesUsed == 0);

  let a = new BlobInstance();
  assert(BlobManager.bytesUsed == 0);

  // Allocate all the allowed memory
  a.set(encoder.encode("Hi there"));
  assert(BlobManager.bytesUsed == 8);

  // Should still be at 8 bytes usage after this
  a.free();
  assert(BlobManager.bytesUsed == 0);

  // Should throw an error
  let didFail = false;
  try {
    a.set(BlobManager.encoder.encode(
        "Alabama banana charlie doughnut"));
  } catch {
    didFail = true;
  }
  assert(didFail, 'Expected out-of-mem, but saw none!');

  // This will have deallocated a upon error

  console.log("All BlobManager unit tests passed.");
}

main();
