/**
 * @file
 * @brief Manages blobs, what else is there to say
 */

/// A lent-out copy-on-write pointer to shared
/// memory
class BlobInstance {
  /// Free this allocation
  free() {
    BlobManager.instance.free(this);
  }

  /// Get the allocation data
  get(): Uint8Array|undefined {
    return BlobManager.instance.get(this);
  }

  /// Get the allocation data as a string
  getString(): string|undefined {
    const out = this.get();
    if (out == undefined) {
      return undefined;
    }
    return BlobManager.decoder.decode(out);
  }

  /// Set the allocation data
  set(newData: Uint8Array) {
    BlobManager.instance.set(this, newData);
  }
}

/// Lends out blob instances which point to internal memory.
/// Blobs are copy-on-write only
class BlobManager {
  /// Singleton instance
  static instance = new BlobManager();

  /// Used for encoding text
  static encoder = new TextEncoder();

  /// Used for decoding text
  static decoder = new TextDecoder();

  /// Maps allocation IDs to allocations
  private allocations = new Map<Number, Uint8Array>();

  /// Maps allocation IDs to the set of instances which own them
  private allocationViewIDs =
      new Map<Number, Set<BlobInstance>>();

  /// Maps lent shared pointers to their allocation IDs
  private pointers = new Map<BlobInstance, Number>();

  /// A queue of free-d IDs to use before creating a new one
  private nextAllocationIDs = [];

  /// The next ID to add
  private nextToAdd = 0;

  /// The current number of used bytes
  static bytesUsed: number = 0;

  /// The max number of bytes
  static maxBytes?: number = 1024 * 64;

  /// Remove the allocation associated with `who`
  free(who: BlobInstance) {
    if (!this.pointers.has(who)) {
      return;
    }
    const ID = this.pointers.get(who);
    this.pointers.delete(who);
    this.allocationViewIDs.get(ID).delete(who);

    if (this.allocationViewIDs.get(ID).size == 0) {
      // No more pointers to this allocation! Delete it
      this.allocationViewIDs.delete(ID);
      BlobManager.bytesUsed -= this.allocations.get(ID).length;
      this.allocations.delete(ID);
      this.nextAllocationIDs.push(ID);
    }
  }

  /// Get a read-only view of the data
  get(who: BlobInstance): Readonly<Uint8Array>|undefined {
    if (this.pointers.has(who)) {
      return this.allocations.get(this.pointers.get(who));
    }
    return undefined;
  }

  /// Write modified data, creating a new allocation
  set(who: BlobInstance, what: Readonly<Uint8Array>) {
    this.free(who);

    if (BlobManager.maxBytes != undefined &&
        BlobManager.bytesUsed + what.length >
            BlobManager.maxBytes) {
      throw Error(`BlobManager allocation of ${
          what.length} bytes would overrun max of ${
          BlobManager.maxBytes} bytes`);
    }

    // Create new allocation
    if (this.nextAllocationIDs.length == 0) {
      this.nextAllocationIDs.push(this.pointers.size);
      ++this.nextToAdd;
    }
    const ID = this.nextAllocationIDs.pop();

    BlobManager.bytesUsed += what.length;
    this.allocations.set(ID, new Uint8Array(what));
    this.allocationViewIDs.set(ID,
                               new Set<BlobInstance>([ who ]));
    this.pointers.set(who, ID);
  }
}

export {BlobManager, BlobInstance};
