/*
  ==============================================================================

   This file is part of the JUCE library - "Jules' Utility Class Extensions"
   Copyright 2004-11 by Raw Material Software Ltd.

  ------------------------------------------------------------------------------

   JUCE can be redistributed and/or modified under the terms of the GNU General
   Public License (Version 2), as published by the Free Software Foundation.
   A copy of the license is included in the JUCE distribution, or can be found
   online at www.gnu.org/licenses.

   JUCE is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
   A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  ------------------------------------------------------------------------------

   To release a closed-source product which uses JUCE, commercial licenses are
   available: visit www.rawmaterialsoftware.com/juce for more information.

  ==============================================================================
*/

#ifndef __JUCE_HASHMAP_JUCEHEADER__
#define __JUCE_HASHMAP_JUCEHEADER__

#include "juce_OwnedArray.h"
#include "juce_LinkedListPointer.h"
#include "../memory/juce_ScopedPointer.h"


//==============================================================================
/**
    A simple class to generate hash functions for some primitive types, intended for
    use with the HashMap class.
    @see HashMap
*/
class DefaultHashFunctions
{
public:
    /** Generates a simple hash from an integer. */
    static int generateHash (const int key, const int upperLimit) noexcept        { return std::abs (key) % upperLimit; }
    /** Generates a simple hash from a string. */
    static int generateHash (const String& key, const int upperLimit) noexcept    { return (int) (((uint32) key.hashCode()) % upperLimit); }
    /** Generates a simple hash from a variant. */
    static int generateHash (const var& key, const int upperLimit) noexcept       { return generateHash (key.toString(), upperLimit); }
};


//==============================================================================
/**
    Holds a set of mappings between some key/value pairs.

    The types of the key and value objects are set as template parameters.
    You can also specify a class to supply a hash function that converts a key value
    into an hashed integer. This class must have the form:

    @code
    struct MyHashGenerator
    {
        static int generateHash (MyKeyType key, int upperLimit)
        {
            // The function must return a value 0 <= x < upperLimit
            return someFunctionOfMyKeyType (key) % upperLimit;
        }
    };
    @endcode

    Like the Array class, the key and value types are expected to be copy-by-value types, so
    if you define them to be pointer types, this class won't delete the objects that they
    point to.

    If you don't supply a class for the HashFunctionToUse template parameter, the
    default one provides some simple mappings for strings and ints.

    @code
    HashMap<int, String> hash;
    hash.set (1, "item1");
    hash.set (2, "item2");

    DBG (hash [1]); // prints "item1"
    DBG (hash [2]); // prints "item2"

    // This iterates the map, printing all of its key -> value pairs..
    for (HashMap<int, String>::Iterator i (hash); i.next();)
        DBG (i.getKey() << " -> " << i.getValue());
    @endcode

    @see CriticalSection, DefaultHashFunctions, NamedValueSet, SortedSet
*/
template <typename KeyType,
          typename ValueType,
          class HashFunctionToUse = DefaultHashFunctions,
          class TypeOfCriticalSectionToUse = DummyCriticalSection>
class HashMap
{
private:
    typedef PARAMETER_TYPE (KeyType)   KeyTypeParameter;
    typedef PARAMETER_TYPE (ValueType) ValueTypeParameter;

public:
    //==============================================================================
    /** Creates an empty hash-map.

        The numberOfSlots parameter specifies the number of hash entries the map will use. This
        will be the "upperLimit" parameter that is passed to your generateHash() function. The number
        of hash slots will grow automatically if necessary, or it can be remapped manually using remapTable().
    */
    explicit HashMap (const int numberOfSlots = defaultHashTableSize)
       : totalNumItems (0)
    {
        slots.insertMultiple (0, nullptr, numberOfSlots);
    }

    /** Destructor. */
    ~HashMap()
    {
        clear();
    }

    //==============================================================================
    /** Removes all values from the map.
        Note that this will clear the content, but won't affect the number of slots (see
        remapTable and getNumSlots).
    */
    void clear()
    {
        const ScopedLockType sl (getLock());

        for (int i = slots.size(); --i >= 0;)
        {
            HashEntry* h = slots.getUnchecked(i);

            while (h != nullptr)
            {
                const ScopedPointer<HashEntry> deleter (h);
                h = h->nextEntry;
            }

            slots.set (i, nullptr);
        }

        totalNumItems = 0;
    }

    //==============================================================================
    /** Returns the current number of items in the map. */
    inline int size() const noexcept
    {
        return totalNumItems;
    }

    /** Returns the value corresponding to a given key.
        If the map doesn't contain the key, a default instance of the value type is returned.
        @param keyToLookFor    the key of the item being requested
    */
    inline const ValueType operator[] (KeyTypeParameter keyToLookFor) const
    {
        const ScopedLockType sl (getLock());

        for (const HashEntry* entry = slots.getUnchecked (generateHashFor (keyToLookFor)); entry != nullptr; entry = entry->nextEntry)
            if (entry->key == keyToLookFor)
                return entry->value;

        return ValueType();
    }

    //==============================================================================
    /** Returns true if the map contains an item with the specied key. */
    bool contains (KeyTypeParameter keyToLookFor) const
    {
        const ScopedLockType sl (getLock());

        for (const HashEntry* entry = slots.getUnchecked (generateHashFor (keyToLookFor)); entry != nullptr; entry = entry->nextEntry)
            if (entry->key == keyToLookFor)
                return true;

        return false;
    }

    /** Returns true if the hash contains at least one occurrence of a given value. */
    bool containsValue (ValueTypeParameter valueToLookFor) const
    {
        const ScopedLockType sl (getLock());

        for (int i = getNumSlots(); --i >= 0;)
            for (const HashEntry* entry = slots.getUnchecked(i); entry != nullptr; entry = entry->nextEntry)
                if (entry->value == valueToLookFor)
                    return true;

        return false;
    }

    //==============================================================================
    /** Adds or replaces an element in the hash-map.
        If there's already an item with the given key, this will replace its value. Otherwise, a new item
        will be added to the map.
    */
    void set (KeyTypeParameter newKey, ValueTypeParameter newValue)
    {
        const ScopedLockType sl (getLock());
        const int hashIndex = generateHashFor (newKey);

        HashEntry* const firstEntry = slots.getUnchecked (hashIndex);

        for (HashEntry* entry = firstEntry; entry != nullptr; entry = entry->nextEntry)
        {
            if (entry->key == newKey)
            {
                entry->value = newValue;
                return;
            }
        }

        slots.set (hashIndex, new HashEntry (newKey, newValue, firstEntry));
        ++totalNumItems;

        if (totalNumItems > (getNumSlots() * 3) / 2)
            remapTable (getNumSlots() * 2);
    }

    /** Removes an item with the given key. */
    void remove (KeyTypeParameter keyToRemove)
    {
        const ScopedLockType sl (getLock());
        const int hashIndex = generateHashFor (keyToRemove);
        HashEntry* entry = slots.getUnchecked (hashIndex);
        HashEntry* previous = nullptr;

        while (entry != nullptr)
        {
            if (entry->key == keyToRemove)
            {
                const ScopedPointer<HashEntry> deleter (entry);

                entry = entry->nextEntry;

                if (previous != nullptr)
                    previous->nextEntry = entry;
                else
                    slots.set (hashIndex, entry);

                --totalNumItems;
            }
            else
            {
                previous = entry;
                entry = entry->nextEntry;
            }
        }
    }

    /** Removes all items with the given value. */
    void removeValue (ValueTypeParameter valueToRemove)
    {
        const ScopedLockType sl (getLock());

        for (int i = getNumSlots(); --i >= 0;)
        {
            HashEntry* entry = slots.getUnchecked(i);
            HashEntry* previous = nullptr;

            while (entry != nullptr)
            {
                if (entry->value == valueToRemove)
                {
                    const ScopedPointer<HashEntry> deleter (entry);

                    entry = entry->nextEntry;

                    if (previous != nullptr)
                        previous->nextEntry = entry;
                    else
                        slots.set (i, entry);

                    --totalNumItems;
                }
                else
                {
                    previous = entry;
                    entry = entry->nextEntry;
                }
            }
        }
    }

    /** Remaps the hash-map to use a different number of slots for its hash function.
        Each slot corresponds to a single hash-code, and each one can contain multiple items.
        @see getNumSlots()
    */
    void remapTable (int newNumberOfSlots)
    {
        HashMap newTable (newNumberOfSlots);

        for (int i = getNumSlots(); --i >= 0;)
            for (const HashEntry* entry = slots.getUnchecked(i); entry != nullptr; entry = entry->nextEntry)
                newTable.set (entry->key, entry->value);

        swapWith (newTable);
    }

    /** Returns the number of slots which are available for hashing.
        Each slot corresponds to a single hash-code, and each one can contain multiple items.
        @see getNumSlots()
    */
    inline int getNumSlots() const noexcept
    {
        return slots.size();
    }

    //==============================================================================
    /** Efficiently swaps the contents of two hash-maps. */
    void swapWith (HashMap& otherHashMap) noexcept
    {
        const ScopedLockType lock1 (getLock());
        const ScopedLockType lock2 (otherHashMap.getLock());

        slots.swapWithArray (otherHashMap.slots);
        std::swap (totalNumItems, otherHashMap.totalNumItems);
    }

    //==============================================================================
    /** Returns the CriticalSection that locks this structure.
        To lock, you can call getLock().enter() and getLock().exit(), or preferably use
        an object of ScopedLockType as an RAII lock for it.
    */
    inline const TypeOfCriticalSectionToUse& getLock() const noexcept      { return lock; }

    /** Returns the type of scoped lock to use for locking this array */
    typedef typename TypeOfCriticalSectionToUse::ScopedLockType ScopedLockType;

private:
    //==============================================================================
    class HashEntry
    {
    public:
        HashEntry (KeyTypeParameter key_, ValueTypeParameter value_, HashEntry* const nextEntry_)
            : key (key_), value (value_), nextEntry (nextEntry_)
        {}

        const KeyType key;
        ValueType value;
        HashEntry* nextEntry;

        JUCE_DECLARE_NON_COPYABLE (HashEntry);
    };

public:
    //==============================================================================
    /** Iterates over the items in a HashMap.

        To use it, repeatedly call next() until it returns false, e.g.
        @code
        HashMap <String, String> myMap;

        HashMap<String, String>::Iterator i (myMap);

        while (i.next())
        {
            DBG (i.getKey() << " -> " << i.getValue());
        }
        @endcode

        The order in which items are iterated bears no resemblence to the order in which
        they were originally added!

        Obviously as soon as you call any non-const methods on the original hash-map, any
        iterators that were created beforehand will cease to be valid, and should not be used.

        @see HashMap
    */
    class Iterator
    {
    public:
        //==============================================================================
        Iterator (const HashMap& hashMapToIterate)
            : hashMap (hashMapToIterate), entry (0), index (0)
        {}

        /** Moves to the next item, if one is available.
            When this returns true, you can get the item's key and value using getKey() and
            getValue(). If it returns false, the iteration has finished and you should stop.
        */
        bool next()
        {
            if (entry != nullptr)
                entry = entry->nextEntry;

            while (entry == nullptr)
            {
                if (index >= hashMap.getNumSlots())
                    return false;

                entry = hashMap.slots.getUnchecked (index++);
            }

            return true;
        }

        /** Returns the current item's key.
            This should only be called when a call to next() has just returned true.
        */
        const KeyType getKey() const
        {
            return entry != nullptr ? entry->key : KeyType();
        }

        /** Returns the current item's value.
            This should only be called when a call to next() has just returned true.
        */
        const ValueType getValue() const
        {
            return entry != nullptr ? entry->value : ValueType();
        }

    private:
        //==============================================================================
        const HashMap& hashMap;
        HashEntry* entry;
        int index;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Iterator);
    };

private:
    //==============================================================================
    enum { defaultHashTableSize = 101 };
    friend class Iterator;

    Array <HashEntry*> slots;
    int totalNumItems;
    TypeOfCriticalSectionToUse lock;

    int generateHashFor (KeyTypeParameter key) const
    {
        const int hash = HashFunctionToUse::generateHash (key, getNumSlots());
        jassert (isPositiveAndBelow (hash, getNumSlots())); // your hash function is generating out-of-range numbers!
        return hash;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HashMap);
};


#endif   // __JUCE_HASHMAP_JUCEHEADER__
