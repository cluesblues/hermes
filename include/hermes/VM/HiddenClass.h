/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#ifndef HERMES_VM_HIDDENCLASS_H
#define HERMES_VM_HIDDENCLASS_H

#include "hermes/Support/OptValue.h"
#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/DictPropertyMap.h"
#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/PropertyDescriptor.h"
#include "hermes/VM/SegmentedArray.h"
#include "hermes/VM/WeakValueMap.h"

#include <functional>
#include "llvm/ADT/ArrayRef.h"

namespace hermes {
namespace vm {

/// The storage type used for properties. Its size may be restricted depending
/// on the current configuration, for example because it must fit in a single
/// GenGCNC segment.
using PropStorage = ArrayStorage;

/// The storage type used for large arrays that don't necessarily fit in a
/// single GenGCNC segment.
using BigStorage = SegmentedArray;

/// Flags associated with a hidden class.
struct ClassFlags {
  /// This class is in dictionary mode, meaning that adding and removing fields
  /// doesn't cause transitions but simply updates the property map.
  uint32_t dictionaryMode : 1;

  /// Set when we have index-like named properties (e.g. "0", "1", etc) defined
  /// using defineOwnProperty. Array accesses will have to check the named
  /// properties first. The absence of this flag is important as it indicates
  /// that named properties whose name is an integer index don't need to be
  /// searched for - they don't exist.
  uint32_t hasIndexLikeProperties : 1;

  /// All properties in this class are non-configurable. This flag can sometimes
  /// be set lazily, after we have checked whether all properties are non-
  /// configurable.
  uint32_t allNonConfigurable : 1;

  /// All properties in this class are both non-configurable and non-writable.
  /// It imples that \c allNonConfigurable is also set.
  /// This flag can sometimes be set lazily, after we have checked whether all
  /// properties are "read-only".
  uint32_t allReadOnly : 1;

  ClassFlags() {
    ::memset(this, 0, sizeof(*this));
  }
};

/// A "hidden class" describes a fixed set of properties, their property flags
/// and the order that they were created in. It is logically immutable (unless
/// it is in "dictionary mode", which is described below).
///
/// Overview
/// ========
/// Adding, deleting or updating a property of a "hidden class" is represented
/// as a transition to a new "hidden class", which encodes the new state of the
/// property set. We call the old class a "parent" and the new class a
/// "child". Starting from a given parent class, its children and their
/// children (etc...) form a tree.
///
/// Each class contains a transition table from itself to its children, keyed on
/// the new/updated property name (SymbolID) and the new/updated property flags
/// that caused each child to be created.
///
/// When a new empty JavaScript object is created, it is assigned an empty
/// "root" hidden class. Adding a new property causes a transition from the
/// root class to a new child class and the transition is recorded in the root
/// class transition table. Adding a second property causes another class to
/// be allocated and a transition to be recorded in its parent, and so on.
/// When a second empty JavaScript object is created and the same properties
/// are added in the same order, the existing classes will be found by looking
/// up in each transition table.
///
/// In this way, JavaScript objects which have the same properties added in the
/// same order will end up having the same hidden class identifying their set
/// of properties. That can decreases the memory dramatically (because we have
/// only one set description per class instead of one per object) and can be
/// used for caching property offsets and other attributes.
///
/// Dictionary Mode
/// ===============
/// When more than a predefined number of properties are added (\c
/// kDictionaryThreshold) or if a property is deleted, a new class is created
/// without a parent and placed in "dictionary mode". In that mode the class
/// is not shared - it belongs to exactly one object - and updates are done "in
/// place" instead of creating new child classes.
///
/// Property Maps
/// =============
/// Conceptually every hidden class has a property map - a table mapping from
/// a property name (SymbolID) to a property descriptor (slot + flags).
///
/// In order to conserve memory, we create the property map associated with a
/// class the first time it is needed. To delay creation further, if we are
/// looking for a property for a "put-by-name" operation, we can avoid needing
/// the map by looking for the property in the transition table first. Lastly,
/// when we transition from a parent class to a child class, we "steal" the
/// parent's property map and assign it to the child.
///
/// The desired effect is that only "leaf" classes have property maps and normal
/// property assignment doesn't create a map at all in the intermediate states
/// (except the first time).
class HiddenClass final : public GCCell {
  friend void HiddenClassBuildMeta(const GCCell *cell, Metadata::Builder &mb);

 public:
  /// Adding more than this number of properties will switch to "dictionary
  /// mode".
  static constexpr unsigned kDictionaryThreshold = 64;

  static VTable vt;

  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::HiddenClassKind;
  }

  /// Create a "root" hidden class - one that doesn't define any properties, but
  /// is a starting point for a hierarchy.
  static CallResult<HermesValue> createRoot(Runtime *runtime);

  /// \return true if this hidden class is guaranteed to be a leaf.
  /// It can return false negatives, so it should only be used for stats
  /// reporting and such.
  bool isKnownLeaf() const {
    return transitionMap_.isKnownEmpty();
  }

  /// \return the number of own properties described by this hidden class.
  /// This corresponds to the size of the property map, if it is initialized.
  unsigned getNumProperties() const {
    return numProperties_;
  }

  /// \return true if this class is in "dictionary mode" - i.e. changes to it
  /// don't result in creation of new classes.
  bool isDictionary() const {
    return flags_.dictionaryMode;
  }

  bool getHasIndexLikeProperties() const {
    return flags_.hasIndexLikeProperties;
  }

  /// \return a hidden class that we originated from entirely by using "flag
  /// transitions", in other words, one that has exactly the same fields in the
  /// same order as this class, but possibly different property flags.
  const HiddenClass *getFamily() const {
    return family_;
  }

  /// \return The for-in cache if one has been set, otherwise nullptr.
  BigStorage *getForInCache() const {
    return forInCache_;
  }

  void setForInCache(BigStorage *arr, Runtime *runtime) {
    forInCache_.set(arr, &runtime->getHeap());
  }

  void clearForInCache() {
    forInCache_ = nullptr;
  }

  /// An opaque class representing a reference to a valid property in the
  /// property map.
  using PropertyPos = DictPropertyMap::PropertyPos;

  /// Call the supplied callback pass each property's \c SymbolID and \c
  /// NamedPropertyDescriptor as parameters.
  /// Obviously the callback shouldn't be doing naughty things like modifying
  /// the property map or creating new hidden classes (even implicitly).
  /// A marker for the current gcScope is obtained in the beginning and the
  /// scope is flushed after every callback.
  template <typename CallbackFunction>
  static void forEachProperty(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime,
      const CallbackFunction &callback);

  /// Same as forEachProperty() but the callback returns true to continue or
  /// false to stop immediately.
  /// A marker for the current gcScope is obtained in the beginning and the
  /// scope is flushed after every callback.
  /// \return false if the callback returned false, true otherwise.
  template <typename CallbackFunction>
  static bool forEachPropertyWhile(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime,
      const CallbackFunction &callback);

  /// Look for a property in the property map. If the property is found, return
  /// a \c PropertyPos identifying it and store its descriptor in \p desc.
  /// \param expectedFlags if valid, we can search the transition table for this
  ///   property with these precise flags. If found in the transition table,
  ///   we don't need to create a property map.
  /// \return the "position" of the property, if found.
  static OptValue<PropertyPos> findProperty(
      PseudoHandle<HiddenClass> self,
      Runtime *runtime,
      SymbolID name,
      PropertyFlags expectedFlags,
      NamedPropertyDescriptor &desc);

  /// An optimistic fast path for \c findProperty(). It only succeeds if there
  /// is an allocated property map. If it fails, the "slow path",
  /// \c findProperty() itself, must be used.
  static bool tryFindPropertyFast(
      const HiddenClass *self,
      SymbolID name,
      NamedPropertyDescriptor &desc);

  /// Performs a very slow linear search for the specified property. This should
  /// only be used for debug tests where we don't want to allocate a property
  /// map because doing so would change the behavior.
  /// \return true if the property is defined, false otherwise.
  static bool debugIsPropertyDefined(HiddenClass *self, SymbolID name);

  /// Delete a property which we found earlier using \c findProperty.
  /// \return the resulting new class.
  static Handle<HiddenClass> deleteProperty(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime,
      PropertyPos pos);

  /// Add a new property. It must not already exist.
  /// \return the resulting new class and the index of the new property.
  static CallResult<std::pair<Handle<HiddenClass>, SlotIndex>> addProperty(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime,
      SymbolID name,
      PropertyFlags propertyFlags);

  /// Update an existing property's flags and return the resulting class.
  /// \param pos is the position of the property into the property map.
  static Handle<HiddenClass> updateProperty(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime,
      PropertyPos pos,
      PropertyFlags newFlags);

  /// Mark all properties as non-configurable.
  /// \return the resulting class
  static Handle<HiddenClass> makeAllNonConfigurable(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime);

  /// Mark all properties as non-writable and non-configurable.
  /// \return the resulting class
  static Handle<HiddenClass> makeAllReadOnly(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime);

  /// Update the flags for the properties in the list \p props with \p
  /// flagsToClear and \p flagsToSet. If in dictionary mode, the properties are
  /// updated on the hidden class directly; otherwise, create only one new
  /// hidden class as result. Updating the properties mutates the property map
  /// directly without creating transitions.
  /// \p flagsToClear and \p flagsToSet are masks for updating the property
  /// flags.
  /// \p props is a list of SymbolIDs for properties that need to be updated
  /// made read-only. It should contain a subset of properties in the hidden
  /// class, so the SymbolIDs won't get freed by gc. It can be llvm::None; if it
  /// is llvm::None, update every property.
  /// \return the resulting hidden class.
  static Handle<HiddenClass> updatePropertyFlagsWithoutTransitions(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime,
      PropertyFlags flagsToClear,
      PropertyFlags flagsToSet,
      OptValue<llvm::ArrayRef<SymbolID>> props);

  /// \return true if all properties are non-configurable
  static bool areAllNonConfigurable(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime);

  /// \return true if all properties are non-writable and non-configurable
  static bool areAllReadOnly(Handle<HiddenClass> selfHandle, Runtime *runtime);

  /// Encode a transition from this hidden class to a child, keyed on the
  /// name of the property and its property flags.
  /// This is an internal type but has to be made public so we can define
  /// a llvm::DenseMapInfo<> trait for it.
  class Transition {
   public:
    SymbolID symbolID;
    PropertyFlags propertyFlags;

    /// An explicit constructor for creating DenseMap sentinel values.
    explicit Transition(SymbolID symbolID)
        : symbolID(symbolID), propertyFlags() {}
    Transition(SymbolID symbolID, PropertyFlags flags)
        : symbolID(symbolID), propertyFlags(flags) {}

    bool operator==(const Transition &a) const {
      return symbolID == a.symbolID && propertyFlags == a.propertyFlags;
    }
  };

 private:
  HiddenClass(
      Runtime *runtime,
      ClassFlags flags,
      Handle<HiddenClass> parent,
      SymbolID symbolID,
      PropertyFlags propertyFlags,
      unsigned numProperties)
      : GCCell(&runtime->getHeap(), &vt),
        flags_(flags),
        parent_(*parent, &runtime->getHeap()),
        family_(this, &runtime->getHeap()),
        symbolID_(symbolID),
        propertyFlags_(propertyFlags),
        numProperties_(numProperties) {
    assert(propertyFlags.isValid() && "propertyFlags must be valid");
  }

  /// Allocate a new hidden class instance with the supplied parameters.
  static CallResult<HermesValue> create(
      Runtime *runtime,
      ClassFlags flags,
      Handle<HiddenClass> parent,
      SymbolID symbolID,
      PropertyFlags propertyFlags,
      unsigned numProperties);

  /// Create a copy of this \c HiddenClass and switch the copy to dictionary
  /// mode. If the current class has a property map, it will be moved to the
  /// new class. Otherwise a new property map will be created for the new class.
  /// In either case, the current class will have no property map and the new
  /// class will have one.
  /// \return the new class.
  static Handle<HiddenClass> convertToDictionary(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime);

  /// Add a new property pair (\p name and \p desc) to the property map (which
  /// must have been initialized).
  static ExecutionStatus addToPropertyMap(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime,
      SymbolID name,
      NamedPropertyDescriptor desc);

  /// Construct a property map by walking back the chain of hidden classes and
  /// store it in \c propertyMap_.
  static void initializeMissingPropertyMap(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime);

  /// Initialize the property map by transferring the parent's map to ourselves
  /// and adding a our property to it. It must only be called if we don't have a
  /// property map of our own but have a valid parent with a property map.
  static void stealPropertyMapFromParent(
      Handle<HiddenClass> selfHandle,
      Runtime *runtime);

  /// Free all non-GC managed resources associated with the object.
  static void _finalizeImpl(GCCell *cell, GC *gc);

  /// Mark all the weak references for an object.
  static void _markWeakImpl(GCCell *cell, GC *gc);

  /// \return the amount of non-GC memory being used by the given \p cell, which
  /// is assumed to be a HiddenClass.
  static size_t _mallocSizeImpl(GCCell *cell);

 private:
  /// Flags associated with this hidden class.
  ClassFlags flags_{};

  /// The parent hidden class which contains a transition from itself to this
  /// one keyed on \c symbolID_+propertyFlags_. It can be null if there is no
  /// parent.
  GCPointer<HiddenClass> parent_;

  /// A hidden class that we originated from entirely by using "flag
  /// transitions", in other words, one that has exactly the same fields in the
  /// same order as this class, but possibly different property flags.
  /// By default it points to its own class.
  /// It is supposed to be used when caching property reads.
  GCPointer<HiddenClass> family_;

  /// The symbol that was added when transitioning to this hidden class.
  const SymbolID symbolID_;
  /// The flags of the added symbol.
  const PropertyFlags propertyFlags_;

  /// Total number of properties encoded in the entire chain from this class
  /// to the root. Note that some transitions do not introduce a new property,
  /// so this is not the same as the length of the transition chain.
  /// Before we enter "dictionary mode", this determines the offset of a new
  /// property.
  unsigned numProperties_;

  /// Optional property map of all properties defined by this hidden class.
  /// This includes \c symbolID_, \c parent_->symbolID_, \c
  /// parent_->parent_->symbolID_ and so on (in reverse order).
  /// It is constructed lazily when needed, or is "stolen" from the parent class
  /// when a transition is performed from the parent class to this one.
  GCPointer<DictPropertyMap> propertyMap_{};

  /// This hash table encodes the transitions from this class to child classes
  /// keyed on the property being added (or updated) and its flags.
  WeakValueMap<Transition, HiddenClass> transitionMap_;

  /// Cache that contains for-in property names for objects of this class.
  /// Never used in dictionary mode.
  GCPointer<BigStorage> forInCache_{};
};

//===----------------------------------------------------------------------===//
// HiddenClass inline methods.

template <typename CallbackFunction>
void HiddenClass::forEachProperty(
    Handle<HiddenClass> selfHandle,
    Runtime *runtime,
    const CallbackFunction &callback) {
  if (LLVM_UNLIKELY(!selfHandle->propertyMap_))
    initializeMissingPropertyMap(selfHandle, runtime);

  return DictPropertyMap::forEachProperty(
      runtime->makeHandle(selfHandle->propertyMap_), runtime, callback);
}

template <typename CallbackFunction>
bool HiddenClass::forEachPropertyWhile(
    Handle<HiddenClass> selfHandle,
    Runtime *runtime,
    const CallbackFunction &callback) {
  if (LLVM_UNLIKELY(!selfHandle->propertyMap_))
    initializeMissingPropertyMap(selfHandle, runtime);

  return DictPropertyMap::forEachPropertyWhile(
      runtime->makeHandle(selfHandle->propertyMap_), runtime, callback);
}

inline bool HiddenClass::tryFindPropertyFast(
    const HiddenClass *self,
    SymbolID name,
    NamedPropertyDescriptor &desc) {
  if (LLVM_LIKELY(self->propertyMap_)) {
    auto found = DictPropertyMap::find(self->propertyMap_, name);
    if (LLVM_LIKELY(found)) {
      desc = DictPropertyMap::getDescriptorPair(self->propertyMap_, *found)
                 ->second;
      return true;
    }
  }
  return false;
}

} // namespace vm
} // namespace hermes

// Enable using HiddenClass::Transition in DenseMap.
namespace llvm {

using namespace hermes::vm;

template <>
struct DenseMapInfo<HiddenClass::Transition> {
  static inline HiddenClass::Transition getEmptyKey() {
    return HiddenClass::Transition(SymbolID::empty());
  }

  static inline HiddenClass::Transition getTombstoneKey() {
    return HiddenClass::Transition(SymbolID::deleted());
  }

  static inline unsigned getHashValue(HiddenClass::Transition transition) {
    return transition.symbolID.unsafeGetRaw() ^ transition.propertyFlags._flags;
  }

  static inline bool isEqual(
      const HiddenClass::Transition &a,
      const HiddenClass::Transition &b) {
    return a == b;
  }
};

} // namespace llvm

#endif // HERMES_VM_HIDDENCLASS_H
