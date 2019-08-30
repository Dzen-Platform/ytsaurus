#pragma once

#include "public.h"

#include "small_vector.h"

namespace NYT {

///////////////////////////////////////////////////////////////////////////////

//! A flat map implementation over SmallVector that tries to keep data inline.
/*!
 *  Similarly to SmallSet, this is implemented via binary search over a sorted
 *  vector. Unlike SmallSet, however, this one never falls back to std::map (or
 *  set) for larger sizes. This means that the flat map is only useful
 *    - at small sizes, when there's absolutely no chance of it getting big, or
 *    - when it's filled once and is then only read from.
 *
 *  In return, the flat map provides
 *    - a smaller size overhead and
 *    - a guarantee that if data fits into inline storage, it goes there.
 */
template <class K, class V, unsigned N>
class TSmallFlatMap
{
public:
    // NB: can't make this pair<const K, V> as SmallVector requires its type
    // parameter to be copy-assignable.
    using value_type = std::pair<K, V>;

private:
    using TStorage = SmallVector<value_type, N>;

    struct TKeyComparer
    {
        bool operator()(const K& lhs, const value_type& rhs)
        {
            return lhs < rhs.first;
        }

        bool operator()(const value_type& lhs, const K& rhs)
        {
            return lhs.first < rhs;
        }
    };

public:
    using iterator = typename TStorage::iterator;
    using const_iterator = typename TStorage::const_iterator;
    using size_type = size_t;

    TSmallFlatMap() = default;

    template <class TInputIterator>
    TSmallFlatMap(TInputIterator begin, TInputIterator end);

    iterator begin();
    const_iterator begin() const;

    iterator end();
    const_iterator end() const;

    void reserve(size_type n);

    size_type size() const;
    int ssize() const;

    bool empty() const;
    void clear();

    iterator find(const K& k);
    const_iterator find(const K& k) const;

    std::pair<iterator, bool> insert(const value_type& value);

    template <class TInputIterator>
    void insert(TInputIterator begin, TInputIterator end);

    V& operator[](const K& k);

    void erase(const K& k);
    void erase(iterator b, iterator e);

private:
    std::pair<iterator, iterator> EqualRange(const K& k);
    std::pair<const_iterator, const_iterator> EqualRange(const K& k) const;

    TStorage Storage_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define SMALL_FLAT_MAP_INL_H_
#include "small_flat_map-inl.h"
#undef SMALL_FLAT_MAP_INL_H_
