#include "memcached/btree/rget.hpp"

#include "errors.hpp"
#include <boost/bind.hpp>
#include <boost/make_shared.hpp>

#include "arch/runtime/runtime.hpp"
#include "btree/iteration.hpp"
#include "containers/iterators.hpp"
#include "memcached/btree/btree_data_provider.hpp"
#include "memcached/btree/node.hpp"
#include "memcached/btree/value.hpp"

/*
 * Possible rget designs:
 * 1. Depth-first search through the B-tree, then iterating through leaves (and maintaining a stack
 *    with some data to be able to backtrack).
 * 2. Breadth-first search, by maintaining a queue of blocks and releasing the lock on the block
 *    when we extracted the IDs of its children.
 * 3. Hybrid of 1 and 2: maintain a deque and use it as a queue, like in 2, thus releasing the locks
 *    for the top of the B-tree quickly, however when the deque reaches some size, start using it as
 *    a stack in depth-first search (but not quite in a usual way; see the note below).
 *
 * Problems of 1: we have to lock the whole path from the root down to the current node, which works
 * fine with small rgets (when max_results is low), but causes unnecessary amounts of locking (and
 * probably copy-on-writes, once we implement them).
 *
 * Problem of 2: while it doesn't hold unnecessary locks to the top (close to root) levels of the
 * B-tree, it may try to lock too much at once if the rget query effectively spans too many blocks
 * (e.g. when we try to rget the whole database).
 *
 * Hybrid approach seems to be the best choice here, because we hold the locks as low (far from the
 * root) in the tree as possible, while minimizing their number by doing a depth-first search from
 * some level.
 *
 * Note (on hybrid implementation):
 * If the deque approach is used, it is important to note that all the nodes in the current level
 * are in a reversed order when we decide to switch to popping from the stack:
 *
 *      P       Lets assume that we have node P in our deque, P is locked: [P]
 *    /   \     We remove P from the deque, lock its children, and push them back to the deque: [A, B]
 *   A     B    Now we can release the P lock.
 *  /|\   /.\   Next, we remove A, lock its children, and push them back to the deque: [B, c, d, e]
 * c d e .....  We release the A lock.
 * ..... ......
 *              At this point we decide that we need to do a depth-first search (to limit the number
 * of locked nodes), and start to use deque as a stack. However since we want
 * an inorder traversal, not the reversed inorder, we can't pop from the end of
 * the deque, we need to pop node 'c' instead of 'e', then (once we're done
 * with its depth-first search) do 'd', and then do 'e'.
 *
 * There are several possible approaches, one of them is putting markers in the deque in
 * between the nodes of different B-tree levels, another (probably a better one) is maintaining a
 * deque of deques, where the inner deques contain the nodes from the current B-tree level.
 *
 *
 * Currently the DFS design is implemented, since it's the simplest solution, also it is a good
 * fit for small rgets (the most popular use-case).
 *
 *
 * Most of the implementation now resides in btree/iteration.{hpp,cc}.
 * Actual merging of the slice iterators is done in server/key_value_store.cc.
 */

bool is_not_expired(key_value_pair_t<memcached_value_t>& pair, exptime_t effective_time) {
    const memcached_value_t *value = reinterpret_cast<const memcached_value_t *>(pair.value.get());
    return !value->expired(effective_time);
}

key_with_data_buffer_t pair_to_key_with_data_buffer(transaction_t *txn, key_value_pair_t<memcached_value_t>& pair) {
    on_thread_t th(txn->home_thread());
    boost::intrusive_ptr<data_buffer_t> data_provider(value_to_data_buffer(reinterpret_cast<memcached_value_t *>(pair.value.get()), txn));
    return key_with_data_buffer_t(pair.key, reinterpret_cast<memcached_value_t *>(pair.value.get())->mcflags(), data_provider);
}

template <class T>
struct transaction_holding_iterator_t : public one_way_iterator_t<T> {
    transaction_holding_iterator_t(boost::scoped_ptr<transaction_t>& txn, one_way_iterator_t<T> *ownee)
        : txn_(), ownee_(ownee) {
        txn_.swap(txn);
    }

    ~transaction_holding_iterator_t() {
        delete ownee_;

        on_thread_t th(txn_->home_thread());
        txn_.reset();
    }

    typename boost::optional<T> next() {
        return ownee_->next();
    }

    void prefetch() {
        ownee_->prefetch();
    }

private:
    boost::scoped_ptr<transaction_t> txn_;
    one_way_iterator_t<T> *ownee_;

    DISABLE_COPYING(transaction_holding_iterator_t);
};

btree_bound_mode_t convert_bound_mode(rget_bound_mode_t m) {
    switch (m) {
        case rget_bound_open: return btree_bound_open;
        case rget_bound_closed: return btree_bound_closed;
        case rget_bound_none: return btree_bound_none;
        default: unreachable();
    }
}

rget_result_t memcached_rget_slice(btree_slice_t *slice, rget_bound_mode_t left_mode, const store_key_t &left_key, rget_bound_mode_t right_mode, const store_key_t &right_key,
    exptime_t effective_time, boost::scoped_ptr<transaction_t>& txn, got_superblock_t& superblock) {

    boost::shared_ptr<value_sizer_t<memcached_value_t> > sizer = boost::make_shared<memcached_value_sizer_t>(txn->get_cache()->get_block_size());

    return boost::shared_ptr<one_way_iterator_t<key_with_data_buffer_t> >(
       new transaction_holding_iterator_t<key_with_data_buffer_t>(txn,
            new transform_iterator_t<key_value_pair_t<memcached_value_t>, key_with_data_buffer_t>(
                boost::bind(pair_to_key_with_data_buffer, txn.get(), _1),
                new filter_iterator_t<key_value_pair_t<memcached_value_t> >(
                    boost::bind(&is_not_expired, _1, effective_time),
                    new slice_keys_iterator_t<memcached_value_t>(sizer, txn.get(), superblock.sb, slice->home_thread(), convert_bound_mode(left_mode), left_key, convert_bound_mode(right_mode), right_key)))));
}