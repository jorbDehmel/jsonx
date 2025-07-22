/**
 * @file
 * @brief Manages blobs, what else is there to say
 */

import {JSONXVar} from "./parser";

/// A lent-out copy-on-write pointer to shared
/// memory
export class JSONXBlob {
  /// The max number of bytes
  static maxBytes?: number = 1024 * 64;

  /// The current number of used bytes
  static bytesUsed: number = 0;

  /// Used for encoding text
  static encoder = new TextEncoder();

  /// Calls encoder.encode on its argument
  static encode = (x: string) => JSONXBlob.encoder.encode(x);

  /// Used for decoding text
  static decoder = new TextDecoder();

  /// Calls decoder.decode on its argument
  static decode = (x: Uint8Array) =>
      JSONXBlob.decoder.decode(x);

  /// Maps allocation IDs to allocations
  private static allocations = new Map<Number, Uint8Array>();

  /// Maps allocation IDs to the set of instances which own them
  private static allocationViewIDs =
      new Map<Number, Set<JSONXBlob>>();

  /// Maps lent shared pointers to their allocation IDs
  private static pointers = new Map<JSONXBlob, Number>();

  /// A queue of free-d IDs to use before creating a new one
  private static nextAllocationIDs = [];

  /// The next ID to add
  private static nextToAdd = 0;

  /// Turn into a string
  stringify(): string {
    return `"${this.getString()}"`;
  }

  /// Dummy fn to satisfy type requirements
  get(_: string): JSONXVar {
    throw new Error("Expected JSONX, but saw BlobInstance");
  }

  /// Get the allocation data as a string
  getString(): string|undefined {
    const out = this.getBytes();
    if (out == undefined) {
      return undefined;
    }
    return JSONXBlob.decode(out);
  }

  /// Free this allocation
  free() {
    if (!JSONXBlob.pointers.has(this)) {
      return;
    }
    const ID = JSONXBlob.pointers.get(this);
    JSONXBlob.pointers.delete(this);
    JSONXBlob.allocationViewIDs.get(ID).delete(this);

    if (JSONXBlob.allocationViewIDs.get(ID).size == 0) {
      // No more pointers to this allocation! Delete it
      JSONXBlob.allocationViewIDs.delete(ID);
      JSONXBlob.bytesUsed -=
          JSONXBlob.allocations.get(ID).length;
      JSONXBlob.allocations.delete(ID);
      JSONXBlob.nextAllocationIDs.push(ID);
    }
  }

  /// Get the allocation data
  getBytes(): Readonly<Uint8Array>|undefined {
    if (JSONXBlob.pointers.has(this)) {
      return JSONXBlob.allocations.get(
          JSONXBlob.pointers.get(this));
    }
    return undefined;
  }

  /// Set the allocation data
  set(what: Readonly<Uint8Array>) {
    this.free();

    if (JSONXBlob.maxBytes != undefined &&
        JSONXBlob.bytesUsed + what.length >
            JSONXBlob.maxBytes) {
      throw Error(`BlobInstance allocation of ${
          what.length} bytes would overrun max of ${
          JSONXBlob.maxBytes} bytes`);
    }

    if (JSONXBlob.nextAllocationIDs.length == 0) {
      JSONXBlob.nextAllocationIDs.push(JSONXBlob.pointers.size);
      ++JSONXBlob.nextToAdd;
    }
    const ID = JSONXBlob.nextAllocationIDs.pop();

    JSONXBlob.bytesUsed += what.length;
    JSONXBlob.allocations.set(ID, new Uint8Array(what));
    JSONXBlob.allocationViewIDs.set(
        ID, new Set<JSONXBlob>([ this ]));
    JSONXBlob.pointers.set(this, ID);
  }
}
