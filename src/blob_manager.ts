"use strict";

/**
 * @file
 * @brief Manages blobs, what else is there to say
 */

/// A lent-out copy-on-write pointer to shared
/// memory
class BlobInstance {
  /// The allocation manager owning this instance
  owner: BlobManager;

  /// Create a new allocation
  constructor(owner: BlobManager) {
    this.owner = owner;
  }

  /// Creates a new pointer to the same data
  duplicate(): BlobInstance {
    return this.owner.duplicate(this);
  }

  /// Free this allocation
  free() {
    this.owner.free(this);
  }

  /// True iff we own data
  has(): boolean {
    return this.owner.has(this);
  }

  /// Get the allocation data
  get(): Uint8Array|undefined {
    return this.owner.get(this);
  }

  /// Set the allocation data
  set(newData: Uint8Array) {
    this.owner.set(this, newData);
  }
}

/// Lends out blob instances which point to internal memory.
/// Blobs are copy-on-write only
class BlobManager {
  /// Maps allocation IDs to allocations
  private allocations = new Map<Number, Uint8Array>();

  /// Maps allocation IDs to the set of instances which own them
  private allocationViewIDs =
      new Map<Number, Set<BlobInstance>>();

  /// Maps lent shared pointers to their allocation IDs
  private pointers = new Map<BlobInstance, Number>();

  /// A queue of freed IDs to use before creating a new one
  private nextAllocationIDs = [];

  /// The next ID to add
  private nextToAdd = 0;

  /// The current number of used bytes
  private bytesUsed: number = 0;

  /// The max number of bytes
  maxBytes?: number = 1024 * 64;

  /// Initialize w/ some max number of bytes
  constructor(maxBytes?: number) {
    this.maxBytes = maxBytes;
  }

  /// True iff we own data
  has(who: BlobInstance): boolean {
    return this.pointers.has(who);
  }

  /// Create a copy
  duplicate(who: BlobInstance): BlobInstance {
    let out = new BlobInstance(this);
    if (this.has(who)) {
      // Duplicate pointer to same allocation
      const allocationID = this.pointers.get(who);
      this.pointers.set(out, allocationID);
      this.allocationViewIDs.get(allocationID).add(out);
    }
    return out;
  }

  /// Remove the allocation associated with `who`
  free(who: BlobInstance) {
    if (!this.has(who)) {
      return;
    }
    const ID = this.pointers.get(who);
    this.pointers.delete(who);
    this.allocationViewIDs.get(ID).delete(who);

    if (this.allocationViewIDs.get(ID).size == 0) {
      // No more pointers to this allocation! Delete it
      this.allocationViewIDs.delete(ID);
      this.bytesUsed -= this.allocations.get(ID).length;
      this.allocations.delete(ID);
      this.nextAllocationIDs.push(ID);
    }
  }

  /// Return the number of bytes used
  usage(): number {
    return this.bytesUsed;
  }

  /// Get a read-only view of the data
  get(who: BlobInstance): Readonly<Uint8Array>|undefined {
    if (this.has(who)) {
      return this.allocations.get(this.pointers.get(who));
    }
    return undefined;
  }

  /// Write modified data, creating a new allocation
  set(who: BlobInstance, what: Readonly<Uint8Array>) {
    this.free(who);

    if (this.maxBytes != undefined &&
        this.bytesUsed + what.length > this.maxBytes) {
      throw Error(`BlobManager allocation of ${
          what.length} bytes would overrun max of ${
          this.maxBytes} bytes`);
    }

    // Create new allocation
    if (this.nextAllocationIDs.length == 0) {
      this.nextAllocationIDs.push(this.nextToAdd);
      ++this.nextToAdd;
    }
    const ID = this.nextAllocationIDs.pop();

    this.bytesUsed += what.length;
    this.allocations.set(ID, new Uint8Array(what));
    this.allocationViewIDs.set(ID,
                               new Set<BlobInstance>([ who ]));
    this.pointers.set(who, ID);
  }
}

export {BlobManager, BlobInstance};
