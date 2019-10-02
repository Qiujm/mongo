/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <third_party/murmurhash3/MurmurHash3.h>

#include <bitset>
#include <boost/intrusive_ptr.hpp>

#include "mongo/base/static_assert.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
/** Helper class to make the position in a document abstract
 *  Warning: This is NOT guaranteed to be the ordered position.
 *           eg. the first field may not be at Position(0)
 */
class Position {
public:
    // This represents "not found" similar to std::string::npos
    Position() : index(static_cast<unsigned>(-1)) {}
    bool found() const {
        return index != Position().index;
    }

    bool operator==(Position rhs) const {
        return this->index == rhs.index;
    }
    bool operator!=(Position rhs) const {
        return !(*this == rhs);
    }

    // For debugging and ASSERT_EQUALS in tests.
    template <typename OStream>
    friend OStream& operator<<(OStream& stream, Position p) {
        stream << p.index;
        return stream;
    }

private:
    explicit Position(size_t i) : index(i) {}
    unsigned index;
    friend class DocumentStorage;
    friend class DocumentStorageIterator;
    friend class DocumentStorageCacheIterator;
};

#pragma pack(1)
/** This is how values are stored in the DocumentStorage buffer
 *  Internal class. Consumers shouldn't care about this.
 */
class ValueElement {
    ValueElement(const ValueElement&) = delete;
    ValueElement& operator=(const ValueElement&) = delete;

public:
    enum class Kind : char {
        // The value does not exist in the underlying BSON.
        kInserted,
        // The value has the image in the underlying BSON.
        kCached,
        // The value has been opportunistically inserted into the cache without checking the BSON.
        kMaybeInserted
    };

    Value val;
    Position nextCollision;  // Position of next field with same hashBucket
    const int nameLen;       // doesn't include '\0'
    Kind kind;               // See the possible kinds above for comments
    const char _name[1];     // pointer to start of name (use nameSD instead)

    ValueElement* next() {
        return align(plusBytes(sizeof(ValueElement) + nameLen));
    }

    const ValueElement* next() const {
        return align(plusBytes(sizeof(ValueElement) + nameLen));
    }

    StringData nameSD() const {
        return StringData(_name, nameLen);
    }


    // helpers for doing pointer arithmetic with this class
    char* ptr() {
        return reinterpret_cast<char*>(this);
    }
    const char* ptr() const {
        return reinterpret_cast<const char*>(this);
    }
    const ValueElement* plusBytes(size_t bytes) const {
        return reinterpret_cast<const ValueElement*>(ptr() + bytes);
    }
    ValueElement* plusBytes(size_t bytes) {
        return reinterpret_cast<ValueElement*>(ptr() + bytes);
    }

    // Round number or pointer up to N-byte boundary. No change if already aligned.
    template <typename T>
    static T align(T size) {
        const intmax_t ALIGNMENT = 8;  // must be power of 2 and <= 16 (malloc alignment)
        // Can't use c++ cast because of conversion between intmax_t and both ints and pointers
        return (T)(((intmax_t)(size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1));
    }

private:
    ValueElement();   // this class should never be constructed
    ~ValueElement();  // or destructed
};
// Real size is sizeof(ValueElement) + nameLen
#pragma pack()
MONGO_STATIC_ASSERT(sizeof(ValueElement) ==
                    (sizeof(Value) + sizeof(Position) + sizeof(int) + sizeof(char) + 1));

class DocumentStorage;

/**
 * This is an internal class for Document. See FieldIterator for the public version.
 *
 * We iterate the fields in 2 phases.
 * In the first phase we walk the underlying bson and consult the cache to see if the current
 * element has been modified. If the element has been deleted then we skip it, if it has been
 * updated then we return the updated value from the cache.
 * If it is not in the cache at all then the element has not been requested at all (i.e. nobody
 * called getField with the current field name). At this point we could construct the Value in cache
 * but we don't do it as not all iterator users actually inspect the values (e.g. size() just counts
 * # of elements, it does not care about the values at all).
 * This walk over the underlying bson makes the _it to 'jump around'.
 *
 * In the second phase (once we exhaust the bson) we walk the cache and return the inserted values
 * as they were not present in the original bson.
 *
 * We do this 'complicated' dance in order to guarantee the original ordering of fields.
 */
class DocumentStorageIterator {
public:
    // DocumentStorage::iterator() and iteratorCacheOnly() are easier to use
    DocumentStorageIterator(DocumentStorage* storage, BSONObjIterator bsonIt);

    bool atEnd() const {
        return !_bsonIt.more() && (_it == _end);
    }

    const ValueElement& get() {
        if (_it) {
            return *_it;
        } else {
            return constructInCache();
        }
    }

    Position position() const {
        return Position(_it->ptr() - _first->ptr());
    }

    void advance();

    const ValueElement* operator->() {
        return &get();
    }
    const ValueElement& operator*() {
        return get();
    }

    const ValueElement* cachedValue() const {
        return _it;
    }

    BSONObjIterator& bsonIter() {
        return _bsonIt;
    }

private:
    /** Construct a new ValueElement in the storage cache. The value is coming from the current
     *  BSONElement pointed to by _bsonIt.
     */
    const ValueElement& constructInCache();

    void advanceOne() {
        if (_bsonIt.more()) {
            ++_bsonIt;
            if (!_bsonIt.more()) {
                _it = _first;
            }
        } else {
            _it = _it->next();
        }
    }
    bool shouldSkipDeleted();

    BSONObjIterator _bsonIt;
    const ValueElement* _first;
    const ValueElement* _it;
    const ValueElement* _end;
    DocumentStorage* _storage;
};

class DocumentStorageCacheIterator {
public:
    DocumentStorageCacheIterator(const ValueElement* first, const ValueElement* end)
        : _first(first), _it(first), _end(end) {}

    bool atEnd() const {
        return _it == _end;
    }

    const ValueElement& get() const {
        return *_it;
    }

    Position position() const {
        return Position(_it->ptr() - _first->ptr());
    }

    void advance() {
        advanceOne();
    }

    const ValueElement* operator->() {
        return _it;
    }
    const ValueElement& operator*() {
        return *_it;
    }

private:
    void advanceOne() {
        _it = _it->next();
    }

    const ValueElement* _first;
    const ValueElement* _it;
    const ValueElement* _end;
};

/// Storage class used by both Document and MutableDocument
class DocumentStorage : public RefCountable {
public:
    DocumentStorage() : DocumentStorage(BSONObj(), false, false) {}

    /**
     * Construct a storage from the BSON. The BSON is lazily processed as fields are requested from
     * the document. If we know that the BSON does not contain any metadata fields we can set the
     * 'stripMetadata' flag to false that will speed up the field iteration.
     */
    DocumentStorage(const BSONObj& bson, bool stripMetadata, bool modified)
        : _cache(nullptr),
          _cacheEnd(nullptr),
          _usedBytes(0),
          _numFields(0),
          _hashTabMask(0),
          _bson(bson),
          _bsonIt(_bson),
          _stripMetadata(stripMetadata),
          _modified(modified) {}

    ~DocumentStorage();

    void reset(const BSONObj& bson, bool stripMetadata);

    static const DocumentStorage& emptyDoc() {
        return kEmptyDoc;
    }

    size_t size() const {
        // can't use _numFields because it includes removed Fields
        size_t count = 0;
        for (DocumentStorageIterator it = iterator(); !it.atEnd(); it.advance())
            count++;
        return count;
    }

    /// Returns the position of the next field to be inserted
    Position getNextPosition() const {
        return Position(_usedBytes);
    }

    enum class LookupPolicy {
        // When looking up a field check the cache only.
        kCacheOnly,
        // Look up in a cache and when not found search the unrelying BSON.
        kCacheAndBSON
    };

    /// Returns the position of the named field or Position()
    Position findField(StringData name, LookupPolicy policy) const;

    // Document uses these
    const ValueElement& getField(Position pos) const {
        verify(pos.found());
        return *(_firstElement->plusBytes(pos.index));
    }
    Value getField(StringData name) const {
        Position pos = findField(name, LookupPolicy::kCacheAndBSON);
        if (!pos.found())
            return Value();
        return getField(pos).val;
    }

    // MutableDocument uses these
    ValueElement& getField(Position pos) {
        _modified = true;
        verify(pos.found());
        return *(_firstElement->plusBytes(pos.index));
    }
    Value& getField(StringData name, LookupPolicy policy) {
        _modified = true;
        Position pos = findField(name, policy);
        if (!pos.found())
            return appendField(name, ValueElement::Kind::kMaybeInserted);
        return getField(pos).val;
    }

    /// Adds a new field with missing Value at the end of the document
    Value& appendField(StringData name, ValueElement::Kind kind);

    /** Preallocates space for fields. Use this to attempt to prevent buffer growth.
     *  This is only valid to call before anything is added to the document.
     */
    void reserveFields(size_t expectedFields);

    /// This returns values from the cache and underlying BSON.
    DocumentStorageIterator iterator() const {
        return DocumentStorageIterator(const_cast<DocumentStorage*>(this), BSONObjIterator(_bson));
    }

    /// This returns values from the cache only.
    auto iteratorCacheOnly() const {
        return DocumentStorageCacheIterator(_firstElement, end());
    }

    /// Shallow copy of this. Caller owns memory.
    boost::intrusive_ptr<DocumentStorage> clone() const;

    size_t allocatedBytes() const {
        return !_cache ? 0 : (_cacheEnd - _cache + hashTabBytes());
    }

    auto bsonObjSize() const {
        return _bson.objsize();
    }

    bool isOwned() const {
        // An empty BSON is a special case, it can be treated 'owned'. We save on memory allocation
        // when constructing an empty Document.
        return _bson.isEmpty() || _bson.isOwned();
    }

    void makeOwned() {
        _bson = _bson.getOwned();
        _bsonIt = BSONObjIterator(_bson);
    }

    /**
     * Compute the space allocated for the metadata fields. Will account for space allocated for
     * unused metadata fields as well.
     */
    size_t getMetadataApproximateSize() const;

    /**
     * Copies all metadata from source if it has any.
     * Note: does not clear metadata from this.
     */
    void copyMetaDataFrom(const DocumentStorage& source) {
        // If the underlying BSON object is shared and the source does not have metadata then
        // nothing needs to be copied. If the metadata is in the BSON then they are the same in
        // this and source.
        if (_bson.objdata() == source._bson.objdata() && !source._metadataFields) {
            return;
        }
        loadLazyMetadata();
        metadata().copyFrom(source.metadata());
    }

    /**
     * Returns a const reference to an object housing the metadata fields associated with this
     * WorkingSetMember.
     */
    const DocumentMetadataFields& metadata() const {
        if (_stripMetadata) {
            loadLazyMetadata();
        }
        return _metadataFields;
    }

    /**
     * Returns a non-const reference to an object housing the metadata fields associated with this
     * WorkingSetMember.
     */
    DocumentMetadataFields& metadata() {
        loadLazyMetadata();
        return _metadataFields;
    }

    DocumentMetadataFields releaseMetadata() {
        loadLazyMetadata();
        return std::move(_metadataFields);
    }

    void setMetadata(DocumentMetadataFields&& metadata) {
        _metadataFields = std::move(metadata);
    }

    static unsigned hashKey(StringData name) {
        // TODO consider FNV-1a once we have a better benchmark corpus
        unsigned out;
        MurmurHash3_x86_32(name.rawData(), name.size(), 0, &out);
        return out;
    }

    const ValueElement* begin() const {
        return _firstElement;
    }

    /// Same as lastElement->next() or firstElement() if empty.
    const ValueElement* end() const {
        return _firstElement ? _firstElement->plusBytes(_usedBytes) : nullptr;
    }

    auto stripMetadata() const {
        return _stripMetadata;
    }

    Position constructInCache(const BSONElement& elem);

    auto isModified() const {
        return _modified;
    }

    auto bsonObj() const {
        return _bson;
    }

private:
    /// Returns the position of the named field in the cache or Position()
    Position findFieldInCache(StringData name) const;

    /// Allocates space in _cache. Copies existing data if there is any.
    void alloc(unsigned newSize);

    /// Call after adding field to _cache and increasing _numFields
    void addFieldToHashTable(Position pos);

    // assumes _hashTabMask is (power of two) - 1
    unsigned hashTabBuckets() const {
        return _hashTabMask + 1;
    }
    unsigned hashTabBytes() const {
        return hashTabBuckets() * sizeof(Position);
    }

    /// rehash on buffer growth if load-factor > .5 (attempt to keep lf < 1 when full)
    bool needRehash() const {
        return _numFields * 2 > hashTabBuckets();
    }

    /// Initialize empty hash table
    void hashTabInit() {
        memset(static_cast<void*>(_hashTab), -1, hashTabBytes());
    }

    unsigned bucketForKey(StringData name) const {
        return hashKey(name) & _hashTabMask;
    }

    /// Adds all fields to the hash table
    void rehash() {
        hashTabInit();
        for (auto it = iteratorCacheOnly(); !it.atEnd(); it.advance())
            addFieldToHashTable(it.position());
    }

    void loadLazyMetadata() const;

    enum {
        HASH_TAB_INIT_SIZE = 8,  // must be power of 2
        HASH_TAB_MIN = 4,        // don't hash fields for docs smaller than this
                                 // set to 1 to always hash
    };

    // _cache layout:
    // -------------------------------------------------------------------------------
    // | ValueElement1 Name1 | ValueElement2 Name2 | ... FREE SPACE ... | Hash Table |
    // -------------------------------------------------------------------------------
    //  ^ _cache and _firstElement point here                           ^
    //                                _cacheEnd and _hashTab point here ^
    //
    //
    // When the buffer grows, the hash table moves to the new end.
    union {
        char* _cache;
        ValueElement* _firstElement;
    };

    union {
        // pointer to "end" of _cache element space and start of hash table (same position)
        char* _cacheEnd;
        Position* _hashTab;  // table lazily initialized once _numFields == HASH_TAB_MIN
    };

    unsigned _usedBytes;    // position where next field would start
    unsigned _numFields;    // this includes removed fields
    unsigned _hashTabMask;  // equal to hashTabBuckets()-1 but used more often

    BSONObj _bson;
    mutable BSONObjIterator _bsonIt;

    // If '_stripMetadata' is true, tracks whether or not the metadata has been lazy-loaded from the
    // backing '_bson' object. If so, then no attempt will be made to load the metadata again, even
    // if the metadata has been released by a call to 'releaseMetadata()'.
    mutable bool _haveLazyLoadedMetadata = false;

    mutable DocumentMetadataFields _metadataFields;

    // The storage constructed from a BSON value may contain metadata. When we process the BSON we
    // have to move the metadata to the MetadataFields object. If we know that the BSON does not
    // have any metadata we can set _stripMetadata to false that will speed up the iteration.
    bool _stripMetadata{false};

    // This flag is set to true anytime the storage returns a mutable field. It is used to optimize
    // a conversion to BSON; i.e. if there are not any modifications we can directly return _bson.
    bool _modified{false};

    // Defined in document.cpp
    static const DocumentStorage kEmptyDoc;

    friend class DocumentStorageIterator;
};
}  // namespace mongo
