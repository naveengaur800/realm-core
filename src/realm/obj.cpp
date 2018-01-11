/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include "realm/obj.hpp"
#include "realm/cluster_tree.hpp"
#include "realm/array_basic.hpp"
#include "realm/array_integer.hpp"
#include "realm/array_bool.hpp"
#include "realm/array_string.hpp"
#include "realm/array_binary.hpp"
#include "realm/array_timestamp.hpp"
#include "realm/array_key.hpp"
#include "realm/array_backlink.hpp"
#include "realm/column_type_traits.hpp"
#include "realm/cluster_tree.hpp"
#include "realm/spec.hpp"
#include "realm/replication.hpp"

using namespace realm;


/********************************* ConstObj **********************************/

ConstObj::ConstObj(const ClusterTree* tree_top, ref_type ref, Key key, size_t row_ndx)
    : m_tree_top(tree_top)
    , m_key(key)
    , m_mem(ref, tree_top->get_alloc())
    , m_row_ndx(row_ndx)
{
    m_instance_version = m_tree_top->get_instance_version();
    m_storage_version = m_tree_top->get_storage_version(m_instance_version);
}

Allocator& ConstObj::get_alloc() const
{
    return m_tree_top->get_alloc();
}

template <class T>
inline int ConstObj::cmp(const ConstObj& other, size_t col_ndx) const
{
    T val1 = get<T>(col_ndx);
    T val2 = other.get<T>(col_ndx);
    if (val1 < val2) {
        return 1;
    }
    else if (val1 > val2) {
        return -1;
    }
    return 0;
}

int ConstObj::cmp(const ConstObj& other, size_t col_ndx) const
{
    const Spec& spec = m_tree_top->get_spec();
    ColumnAttrMask attr = spec.get_column_attr(col_ndx);
    REALM_ASSERT(!attr.test(col_attr_List)); // TODO: implement comparison of lists

    switch (spec.get_public_column_type(col_ndx)) {
        case type_Int:
            return cmp<Int>(other, col_ndx);
        case type_Bool:
            return cmp<Bool>(other, col_ndx);
        case type_Float:
            return cmp<Float>(other, col_ndx);
        case type_Double:
            return cmp<Double>(other, col_ndx);
        case type_String:
            return cmp<String>(other, col_ndx);
        case type_Binary:
            return cmp<Binary>(other, col_ndx);
        case type_Timestamp:
            return cmp<Timestamp>(other, col_ndx);
        case type_Link:
            return cmp<Key>(other, col_ndx);
        case type_OldDateTime:
        case type_OldTable:
        case type_OldMixed:
        case type_LinkList:
            REALM_ASSERT(false);
            break;
    }
    return 0;
}

bool ConstObj::operator==(const ConstObj& other) const
{
    size_t col_cnt = m_tree_top->get_spec().get_public_column_count();
    while (col_cnt--) {
        if (cmp(other, col_cnt) != 0) {
            return false;
        }
    }
    return true;
}

const Table* ConstObj::get_table() const
{
    return m_tree_top->get_owner();
}

bool ConstObj::is_valid() const
{
    return m_key && get_table()->is_valid(m_key);
}

void ConstObj::remove()
{
    const_cast<Table*>(get_table())->remove_object(m_key);
}

size_t ConstObj::get_column_index(StringData col_name) const
{
    return m_tree_top->get_spec().get_column_index(col_name);
}

TableKey ConstObj::get_table_key() const
{
    return m_tree_top->get_owner()->get_key();
}

TableRef ConstObj::get_target_table(size_t col_ndx) const
{
    return _impl::TableFriend::get_opposite_link_table(*m_tree_top->get_owner(), col_ndx);
}

Obj::Obj(ClusterTree* tree_top, ref_type ref, Key key, size_t row_ndx)
    : ConstObj(tree_top, ref, key, row_ndx)
    , m_writeable(!tree_top->get_alloc().is_read_only(ref))
{
}

// FIXME: Optimization - all the work needed to bump version counters
// and to check if it has changed must be optimized to avoid indirections
// and to allow inline compilation of the whole code path.
bool ConstObj::update_if_needed() const
{
    auto current_version = m_tree_top->get_storage_version(m_instance_version);
    if (current_version != m_storage_version) {
        // Get a new object from key
        ConstObj new_obj = m_tree_top->get(m_key);
        update(new_obj);

        return true;
    }
    return false;
}

template <class T>
T ConstObj::get(size_t col_ndx) const
{
    if (REALM_UNLIKELY(col_ndx > m_tree_top->get_spec().get_public_column_count()))
        throw LogicError(LogicError::column_index_out_of_range);
    const Spec& spec = m_tree_top->get_spec();
    ColumnAttrMask attr = spec.get_column_attr(col_ndx);
    REALM_ASSERT(attr.test(col_attr_List) || (spec.get_column_type(col_ndx) == ColumnTypeTraits<T>::column_id));

    update_if_needed();

    typename ColumnTypeTraits<T>::cluster_leaf_type values(m_tree_top->get_alloc());
    ref_type ref = to_ref(Array::get(m_mem.get_addr(), col_ndx + 1));
    values.init_from_ref(ref);

    return values.get(m_row_ndx);
}

template <class T>
inline bool ConstObj::do_is_null(size_t col_ndx) const
{
    T values(m_tree_top->get_alloc());
    ref_type ref = to_ref(Array::get(m_mem.get_addr(), col_ndx + 1));
    values.init_from_ref(ref);
    return values.is_null(m_row_ndx);
}

size_t ConstObj::get_link_count(size_t col_ndx) const
{
    return get_list<Key>(col_ndx).size();
}

bool ConstObj::is_null(size_t col_ndx) const
{
    if (REALM_UNLIKELY(col_ndx > m_tree_top->get_spec().get_public_column_count()))
        throw LogicError(LogicError::column_index_out_of_range);

    update_if_needed();
    ColumnAttrMask attr = m_tree_top->get_spec().get_column_attr(col_ndx);

    if (attr.test(col_attr_List)) {
        ArrayInteger values(m_tree_top->get_alloc());
        ref_type ref = to_ref(Array::get(m_mem.get_addr(), col_ndx + 1));
        values.init_from_ref(ref);
        return values.get(m_row_ndx) == 0;
    }

    if (attr.test(col_attr_Nullable)) {
        switch (m_tree_top->get_spec().get_column_type(col_ndx)) {
            case col_type_Int:
                return do_is_null<ArrayIntNull>(col_ndx);
            case col_type_Bool:
                return do_is_null<ArrayBoolNull>(col_ndx);
            case col_type_Float:
                return do_is_null<ArrayFloat>(col_ndx);
            case col_type_Double:
                return do_is_null<ArrayDouble>(col_ndx);
            case col_type_String:
                return do_is_null<ArrayString>(col_ndx);
            case col_type_Binary:
                return do_is_null<ArrayBinary>(col_ndx);
            case col_type_Timestamp:
                return do_is_null<ArrayTimestamp>(col_ndx);
            case col_type_Link:
                return do_is_null<ArrayKey>(col_ndx);
            default:
                break;
        }
    }
    return false;
}

size_t ConstObj::get_backlink_count(const Table& origin, size_t origin_col_ndx) const
{
    size_t cnt = 0;
    TableKey origin_table_key = origin.get_key();
    if (origin_table_key != TableKey()) {
        size_t backlink_col_ndx = m_tree_top->get_spec().find_backlink_column(origin_table_key, origin_col_ndx);
        cnt = get_backlink_count(backlink_col_ndx);
    }
    return cnt;
}

Key ConstObj::get_backlink(const Table& origin, size_t origin_col_ndx, size_t backlink_ndx) const
{
    TableKey origin_key = origin.get_key();
    size_t backlink_col_ndx = m_tree_top->get_spec().find_backlink_column(origin_key, origin_col_ndx);

    return get_backlink(backlink_col_ndx, backlink_ndx);
}

size_t ConstObj::get_backlink_count(size_t backlink_col_ndx) const
{
    Allocator& alloc = m_tree_top->get_alloc();
    Array fields(alloc);
    fields.init_from_mem(m_mem);

    ArrayBacklink backlinks(alloc);
    backlinks.set_parent(&fields, backlink_col_ndx + 1);
    backlinks.init_from_parent();

    return backlinks.get_backlink_count(m_row_ndx);
}

Key ConstObj::get_backlink(size_t backlink_col_ndx, size_t backlink_ndx) const
{
    Allocator& alloc = m_tree_top->get_alloc();
    Array fields(alloc);
    fields.init_from_mem(m_mem);

    ArrayBacklink backlinks(alloc);
    backlinks.set_parent(&fields, backlink_col_ndx + 1);
    backlinks.init_from_parent();
    return backlinks.get_backlink(m_row_ndx, backlink_ndx);
}

/*********************************** Obj *************************************/

bool Obj::update_if_needed() const
{
    bool updated = ConstObj::update_if_needed();
    if (updated) {
        m_writeable = !m_tree_top->get_alloc().is_read_only(m_mem.get_ref());
    }
    return updated;
}

void Obj::ensure_writeable()
{
    if (!m_writeable) {
        m_mem = const_cast<ClusterTree*>(m_tree_top)->ensure_writeable(m_key);
        m_writeable = true;
    }
}

void Obj::bump_content_version()
{
    Allocator& alloc = m_tree_top->get_alloc();
    alloc.bump_content_version();
}

template <>
Obj& Obj::set<int64_t>(size_t col_ndx, int64_t value, bool is_default)
{
    if (REALM_UNLIKELY(col_ndx > m_tree_top->get_spec().get_public_column_count()))
        throw LogicError(LogicError::column_index_out_of_range);

    update_if_needed();
    ensure_writeable();

    if (StringIndex* index = m_tree_top->get_owner()->get_search_index(col_ndx)) {
        index->set<int64_t>(m_key, value);
    }

    Allocator& alloc = m_tree_top->get_alloc();
    alloc.bump_content_version();
    Array fallback(alloc);
    Array& fields = m_tree_top->get_fields_accessor(fallback, m_mem);
    REALM_ASSERT(col_ndx + 1 < fields.size());
    ColumnAttrMask attr = m_tree_top->get_spec().get_column_attr(col_ndx);
    if (attr.test(col_attr_Nullable)) {
        ArrayIntNull values(alloc);
        values.set_parent(&fields, col_ndx + 1);
        values.init_from_parent();
        values.set(m_row_ndx, value);
    }
    else {
        ArrayInteger values(alloc);
        values.set_parent(&fields, col_ndx + 1);
        values.init_from_parent();
        values.set(m_row_ndx, value);
    }

    if (Replication* repl = alloc.get_replication()) {
        repl->set_int(m_tree_top->get_owner(), col_ndx, m_key, value,
                      is_default ? _impl::instr_SetDefault : _impl::instr_Set); // Throws
    }

    return *this;
}

Obj& Obj::add_int(size_t col_ndx, int64_t value)
{
    if (REALM_UNLIKELY(col_ndx > m_tree_top->get_spec().get_public_column_count()))
        throw LogicError(LogicError::column_index_out_of_range);

    update_if_needed();
    ensure_writeable();

    auto add_wrap = [](int64_t a, int64_t b) -> int64_t {
        uint64_t ua = uint64_t(a);
        uint64_t ub = uint64_t(b);
        return int64_t(ua + ub);
    };

    Allocator& alloc = m_tree_top->get_alloc();
    alloc.bump_content_version();
    Array fallback(alloc);
    Array& fields = m_tree_top->get_fields_accessor(fallback, m_mem);
    REALM_ASSERT(col_ndx + 1 < fields.size());
    ColumnAttrMask attr = m_tree_top->get_spec().get_column_attr(col_ndx);
    if (attr.test(col_attr_Nullable)) {
        ArrayIntNull values(alloc);
        values.set_parent(&fields, col_ndx + 1);
        values.init_from_parent();
        Optional<int64_t> old = values.get(m_row_ndx);
        if (old) {
            values.set(m_row_ndx, add_wrap(*old, value));
        }
        else {
            throw LogicError{LogicError::illegal_combination};
        }
    }
    else {
        ArrayInteger values(alloc);
        values.set_parent(&fields, col_ndx + 1);
        values.init_from_parent();
        int64_t old = values.get(m_row_ndx);
        values.set(m_row_ndx, add_wrap(old, value));
    }

    if (Replication* repl = alloc.get_replication()) {
        repl->add_int(m_tree_top->get_owner(), col_ndx, m_key, value); // Throws
    }

    return *this;
}

template <>
Obj& Obj::set<Key>(size_t col_ndx, Key target_key, bool is_default)
{
    if (REALM_UNLIKELY(col_ndx >= m_tree_top->get_spec().get_public_column_count()))
        throw LogicError(LogicError::column_index_out_of_range);
    TableRef target_table = get_target_table(col_ndx);
    if (target_key != null_key && !target_table->is_valid(target_key)) {
        throw LogicError(LogicError::target_row_index_out_of_range);
    }

    update_if_needed();
    ensure_writeable();

    Allocator& alloc = m_tree_top->get_alloc();
    alloc.bump_content_version();
    Array fallback(alloc);
    Array& fields = m_tree_top->get_fields_accessor(fallback, m_mem);
    REALM_ASSERT(col_ndx + 1 < fields.size());
    ArrayKey values(alloc);
    values.set_parent(&fields, col_ndx + 1);
    values.init_from_parent();

    Key old_key = values.get(m_row_ndx);

    if (target_key != old_key) {
        CascadeState state;
        bool recurse = update_backlinks(col_ndx, old_key, target_key, state);

        values.set(m_row_ndx, target_key);

        if (Replication* repl = alloc.get_replication()) {
            repl->set(m_tree_top->get_owner(), col_ndx, m_key, target_key,
                      is_default ? _impl::instr_SetDefault : _impl::instr_Set); // Throws
        }

        if (recurse)
            _impl::TableFriend::remove_recursive(*target_table, state);
    }

    return *this;
}

namespace {
template <class T>
inline bool value_is_null(const T& val)
{
    return val.is_null();
}
template <>
inline bool value_is_null(const bool&)
{
    return false;
}
template <>
inline bool value_is_null(const float& val)
{
    return null::is_null_float(val);
}
template <>
inline bool value_is_null(const double& val)
{
    return null::is_null_float(val);
}

template <class T>
inline void check_range(const T&)
{
}
template <>
inline void check_range(const StringData& val)
{
    if (REALM_UNLIKELY(val.size() > Table::max_string_size))
        throw LogicError(LogicError::string_too_big);
}
template <>
inline void check_range(const BinaryData& val)
{
    if (REALM_UNLIKELY(val.size() > ArrayBlob::max_binary_size))
        throw LogicError(LogicError::binary_too_big);
}
}

template <class T>
Obj& Obj::set(size_t col_ndx, T value, bool is_default)
{
    const Spec& spec = m_tree_top->get_spec();
    REALM_ASSERT(spec.get_column_type(col_ndx) == ColumnTypeTraits<T>::column_id);
    if (REALM_UNLIKELY(col_ndx > spec.get_public_column_count()))
        throw LogicError(LogicError::column_index_out_of_range);
    if (value_is_null(value) && !spec.get_column_attr(col_ndx).test(col_attr_Nullable))
        throw LogicError(LogicError::column_not_nullable);
    check_range(value);

    update_if_needed();
    ensure_writeable();

    if (StringIndex* index = m_tree_top->get_owner()->get_search_index(col_ndx)) {
        index->set<T>(m_key, value);
    }

    Allocator& alloc = m_tree_top->get_alloc();
    alloc.bump_content_version();
    Array fallback(alloc);
    Array& fields = m_tree_top->get_fields_accessor(fallback, m_mem);
    REALM_ASSERT(col_ndx + 1 < fields.size());
    typename ColumnTypeTraits<T>::cluster_leaf_type values(alloc);
    values.set_parent(&fields, col_ndx + 1);
    values.init_from_parent();
    values.set(m_row_ndx, value);

    if (Replication* repl = alloc.get_replication())
        repl->set<T>(m_tree_top->get_owner(), col_ndx, m_key, value,
                     is_default ? _impl::instr_SetDefault : _impl::instr_Set); // Throws

    return *this;
}

void Obj::set_int(size_t col_ndx, int64_t value)
{
    update_if_needed();
    ensure_writeable();

    Allocator& alloc = m_tree_top->get_alloc();
    alloc.bump_content_version();
    Array fallback(alloc);
    Array& fields = m_tree_top->get_fields_accessor(fallback, m_mem);
    REALM_ASSERT(col_ndx + 1 < fields.size());
    Array values(alloc);
    values.set_parent(&fields, col_ndx + 1);
    values.init_from_parent();
    values.set(m_row_ndx, value);
}

void Obj::add_backlink(size_t backlink_col, Key origin_key)
{
    ensure_writeable();

    Allocator& alloc = m_tree_top->get_alloc();
    alloc.bump_content_version();
    Array fallback(alloc);
    Array& fields = m_tree_top->get_fields_accessor(fallback, m_mem);

    ArrayBacklink backlinks(alloc);
    backlinks.set_parent(&fields, backlink_col + 1);
    backlinks.init_from_parent();

    backlinks.add(m_row_ndx, origin_key);
}

void Obj::remove_one_backlink(size_t backlink_col, Key origin_key)
{
    ensure_writeable();

    Allocator& alloc = m_tree_top->get_alloc();
    alloc.bump_content_version();
    Array fallback(alloc);
    Array& fields = m_tree_top->get_fields_accessor(fallback, m_mem);

    ArrayBacklink backlinks(alloc);
    backlinks.set_parent(&fields, backlink_col + 1);
    backlinks.init_from_parent();

    backlinks.remove(m_row_ndx, origin_key);
}

void Obj::nullify_link(size_t origin_col, Key target_key)
{
    ensure_writeable();

    Allocator& alloc = m_tree_top->get_alloc();
    alloc.bump_content_version();
    Array fallback(alloc);
    Array& fields = m_tree_top->get_fields_accessor(fallback, m_mem);

    const Spec& spec = m_tree_top->get_spec();
    ColumnAttrMask attr = spec.get_column_attr(origin_col);
    if (attr.test(col_attr_List)) {
        Array linklists(alloc);
        linklists.set_parent(&fields, origin_col + 1);
        linklists.init_from_parent();

        ArrayKey links(alloc);
        links.set_parent(&linklists, m_row_ndx);
        links.init_from_parent();
        links.nullify(target_key);
    }
    else {
        ArrayKey links(alloc);
        links.set_parent(&fields, origin_col + 1);
        links.init_from_parent();
        Key key = links.get(m_row_ndx);
        REALM_ASSERT(key == target_key);
        links.set(m_row_ndx, Key{});
        if (Replication* repl = alloc.get_replication())
            repl->set<Key>(m_tree_top->get_owner(), origin_col, m_key, Key{}, _impl::instr_Set); // Throws
    }
}

bool Obj::update_backlinks(size_t col_ndx, Key old_key, Key new_key, CascadeState& state)
{
    bool recurse = false;

    TableRef target_table = get_target_table(col_ndx);
    const Spec& target_table_spec = _impl::TableFriend::get_spec(*target_table);
    size_t backlink_col = target_table_spec.find_backlink_column(get_table_key(), col_ndx);

    if (old_key != realm::null_key) {
        const Table* origin_table = get_table();
        const Spec& origin_table_spec = _impl::TableFriend::get_spec(*origin_table);

        Obj target_obj = target_table->get_object(old_key);
        target_obj.remove_one_backlink(backlink_col, m_key); // Throws

        if (origin_table_spec.get_column_attr(col_ndx).test(col_attr_StrongLinks)) {
            size_t num_remaining = target_obj.get_backlink_count(*origin_table, col_ndx);
            if (num_remaining == 0) {
                state.rows.emplace_back(target_table->get_key(), old_key);
                recurse = true;
            }
        }
    }

    if (new_key != realm::null_key) {
        Obj target_obj = target_table->get_object(new_key);
        target_obj.add_backlink(backlink_col, m_key); // Throws
    }

    return recurse;
}

namespace realm {

template int64_t ConstObj::get<int64_t>(size_t col_ndx) const;
template util::Optional<int64_t> ConstObj::get<util::Optional<int64_t>>(size_t col_ndx) const;
template bool ConstObj::get<Bool>(size_t col_ndx) const;
template util::Optional<Bool> ConstObj::get<util::Optional<Bool>>(size_t col_ndx) const;
template float ConstObj::get<float>(size_t col_ndx) const;
template double ConstObj::get<double>(size_t col_ndx) const;
template StringData ConstObj::get<StringData>(size_t col_ndx) const;
template BinaryData ConstObj::get<BinaryData>(size_t col_ndx) const;
template Timestamp ConstObj::get<Timestamp>(size_t col_ndx) const;
template Key ConstObj::get<Key>(size_t col_ndx) const;

template Obj& Obj::set<bool>(size_t, bool, bool);
template Obj& Obj::set<float>(size_t, float, bool);
template Obj& Obj::set<double>(size_t, double, bool);
template Obj& Obj::set<StringData>(size_t, StringData, bool);
template Obj& Obj::set<BinaryData>(size_t, BinaryData, bool);
template Obj& Obj::set<Timestamp>(size_t, Timestamp, bool);
}

template <class T>
inline void Obj::do_set_null(size_t col_ndx)
{
    Allocator& alloc = m_tree_top->get_alloc();
    alloc.bump_content_version();
    Array fallback(alloc);
    Array& fields = m_tree_top->get_fields_accessor(fallback, m_mem);

    T values(alloc);
    values.set_parent(&fields, col_ndx + 1);
    values.init_from_parent();
    values.set_null(m_row_ndx);
}

Obj& Obj::set_null(size_t col_ndx, bool is_default)
{
    if (REALM_UNLIKELY(col_ndx > m_tree_top->get_spec().get_public_column_count()))
        throw LogicError(LogicError::column_index_out_of_range);
    if (REALM_UNLIKELY(!m_tree_top->get_spec().get_column_attr(col_ndx).test(col_attr_Nullable))) {
        throw LogicError(LogicError::column_not_nullable);
    }

    update_if_needed();
    ensure_writeable();

    if (StringIndex* index = m_tree_top->get_owner()->get_search_index(col_ndx)) {
        index->set(m_key, null{});
    }

    switch (m_tree_top->get_spec().get_column_type(col_ndx)) {
        case col_type_Int:
            do_set_null<ArrayIntNull>(col_ndx);
            break;
        case col_type_Bool:
            do_set_null<ArrayBoolNull>(col_ndx);
            break;
        case col_type_Float:
            do_set_null<ArrayFloat>(col_ndx);
            break;
        case col_type_Double:
            do_set_null<ArrayDouble>(col_ndx);
            break;
        case col_type_String:
            do_set_null<ArrayString>(col_ndx);
            break;
        case col_type_Binary:
            do_set_null<ArrayBinary>(col_ndx);
            break;
        case col_type_Timestamp:
            do_set_null<ArrayTimestamp>(col_ndx);
            break;
        case col_type_Link:
            do_set_null<ArrayKey>(col_ndx);
            break;
        default:
            break;
    }

    if (Replication* repl = m_tree_top->get_alloc().get_replication())
        repl->set_null(m_tree_top->get_owner(), col_ndx, m_key,
                       is_default ? _impl::instr_SetDefault : _impl::instr_Set); // Throws

    return *this;
}
