/**
 *   BSD LICENSE
 *
 *   Copyright (c) 2018 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Samsung Electronics Co., Ltd. nor the names of
 *       its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string>
#include <algorithm>
#include <bitset>
#include <string>

#include <time.h>
#include "kv_emulator.hpp"

uint64_t _kv_emul_queue_latency;

namespace kvadi {

static kv_timer kv_emul_timer;

kv_emulator::kv_emulator(uint64_t capacity, std::vector<double> iops_model_coefficients, bool_t use_iops_model): stat(iops_model_coefficients), m_capacity(capacity),m_available(capacity), m_use_iops_model(use_iops_model) {}

// delete any remaining keys in memory
kv_emulator::~kv_emulator() {
    std::unique_lock<std::mutex> lock(m_map_mutex);
    emulator_map_t::iterator it_tmp;
    auto it = m_map.begin();
    while (it != m_map.end()) {
        kv_key *key = it->first;
        free(key->key);
        delete key;

        it_tmp = it;
        it++;
        m_map.erase(it_tmp);
    }
}

inline kv_key *new_kv_key(const kv_key *key) {
    kv_key *copied_key = new kv_key();
    copied_key->length = key->length;
    copied_key->key    = malloc(key->length);
    memcpy(copied_key->key, key->key, key->length);

    return copied_key;
}

uint64_t counter = 0;
// basic operations

kv_result kv_emulator::kv_store(const kv_key *key, const kv_value *value, uint8_t option, uint32_t *consumed_bytes, void *ioctx) {
    (void) ioctx;
    // track consumed spaced
    if (m_capacity != 0 && m_available < (value->length + key->length)) {
        // fprintf(stderr, "No more device space left\n");
        return KV_ERR_DEV_CAPACITY;
    }

    const std::string valstr = std::string((char *)value->value, value->length);
    struct timespec begin;
    if (m_use_iops_model) {
        kv_emul_timer.start2(&begin);
    }
    //const uint64_t start_tick = kv_emul_timer.start();
    {
        std::unique_lock<std::mutex> lock(m_map_mutex);


        auto it = m_map.find((kv_key *)key);
        if (it != m_map.end()) {
            if (option == KV_STORE_OPT_IDEMPOTENT) return KV_ERR_KEY_EXIST;

            // update space
            m_available -= value->length + it->second.length();

            // overwrite
            it->second = valstr;
            
            *consumed_bytes = value->length;
            if (m_use_iops_model) {
                stat.collect(STAT_UPDATE, value->length);
            }
        }
        else {
            kv_key *new_key = new_kv_key(key);
            m_map.emplace(std::make_pair(new_key, std::move(valstr)));

            m_available -= key->length + value->length;

            *consumed_bytes = key->length + value->length;

            if (m_use_iops_model) {
                stat.collect(STAT_INSERT, value->length);
            }
        }
        counter ++;
    }

    if (m_use_iops_model) {
        kv_emul_timer.wait_until2(&begin,stat.get_expected_latency_ns() - _kv_emul_queue_latency);
    }
//    kv_emul_timer.wait_until(start_tick, stat.get_expected_latency_ns(), _kv_emul_queue_latency);

    return KV_SUCCESS;
}

kv_result kv_emulator::kv_retrieve(const kv_key *key, uint8_t option, kv_value *value, void *ioctx) {
    (void) ioctx;

    kv_result ret = KV_ERR_KEY_NOT_EXIST;

    struct timespec begin;
    if (m_use_iops_model) {
        kv_emul_timer.start2(&begin);
    }
    //const uint64_t start_tick = kv_emul_timer.start();
    {

        std::unique_lock<std::mutex> lock(m_map_mutex);
        auto it = m_map.find((kv_key*)key);
        if (it != m_map.end()) {
            uint32_t dlen = it->second.length();
            if (value->offset >= dlen) {
                return KV_ERR_VALUE_OFFSET_INVALID;
            }
            uint32_t copylen = std::min(dlen - value->offset, value->length);

            memcpy(value->value, it->second.data() + value->offset, copylen);

            value->length = copylen;
            ///// temporary
            // value->value_size = dlen;

            if (m_use_iops_model) {
                stat.collect(STAT_READ, copylen);
            }
            ret = KV_SUCCESS;
        } else {
            return KV_ERR_KEY_NOT_EXIST;
        }
    }
    if (m_use_iops_model) {
        //kv_emul_timer.wait_until(start_tick, stat.get_expected_latency_ns(), _kv_emul_queue_latency);
        kv_emul_timer.wait_until2(&begin,stat.get_expected_latency_ns() - _kv_emul_queue_latency);
    }
    return ret;
}


kv_result kv_emulator::kv_exist(const kv_key *key, uint32_t &keycount, uint8_t *buffers, uint32_t &buffer_size, void *ioctx) {
    (void) ioctx;

    int bitpos = 0;

    if (keycount == 0) {
        return KV_SUCCESS;
    }

    const uint32_t bytes_to_write = ((keycount -1) / 8) + 1;

    if (bytes_to_write > buffer_size) {
        return KV_ERR_BUFFER_SMALL;
    }

    memset (buffers, 0, bytes_to_write );

    std::unique_lock<std::mutex> lock(m_map_mutex);

    for (uint32_t i = 0 ; i < keycount ; i++, bitpos++) {
        const int setidx     = (bitpos / 8);
        const int bitoffset  =  bitpos - setidx * 8;

        auto it = m_map.find((kv_key*)&key[i]);
        if (it != m_map.end()) {
            buffers[setidx] |= (1 << bitoffset);
        }
    }

    buffer_size = bytes_to_write;

    return KV_SUCCESS;
}

kv_result kv_emulator::kv_purge(kv_purge_option option, void *ioctx) {
    (void) ioctx;

    if (option != KV_PURGE_OPT_DEFAULT) {
        WRITE_WARN("only default purge option is supported");
        return KV_ERR_OPTION_INVALID;
    }

    std::unique_lock<std::mutex> lock(m_map_mutex);
    for (auto it = m_map.begin(); it != m_map.end(); it++) {
        kv_key *key = it->first;
        m_map.erase(it);
        free(key->key);
        delete key;
    }

    m_available = m_capacity;
    return KV_SUCCESS;
}

kv_result kv_emulator::kv_delete(const kv_key *key, uint8_t option, uint32_t *recovered_bytes, void *ioctx) {
    (void) ioctx;

    if (key == NULL || key->key == NULL) {
        return KV_ERR_KEY_INVALID;
    }

    std::unique_lock<std::mutex> lock(m_map_mutex);
    auto it = m_map.find((kv_key*)key);
    if (it != m_map.end()) {
        kv_key *key = it->first;

        uint32_t len = key->length + it->second.length();
        m_available += len;
        if (recovered_bytes != NULL) {
            *recovered_bytes = len;
        }

        m_map.erase(it);
        free(key->key);
        delete key;
    }

    return KV_SUCCESS;
}

// iterator
kv_result kv_emulator::kv_open_iterator(const kv_iterator_option opt, const kv_group_condition *cond, bool_t keylen_fixed, kv_iterator_handle *iter_hdl, void *ioctx) {
    (void) ioctx;

    if (cond == NULL || iter_hdl == NULL || cond == NULL) {
        return KV_ERR_PARAM_NULL;
    }

    if (m_it_map.size() >= SAMSUNG_MAX_ITERATORS) {
        return KV_ERR_TOO_MANY_ITERATORS_OPEN;
    }

    (*iter_hdl) = new _kv_iterator_handle();
    auto iH = (*iter_hdl);
    iH->it_op = opt;
    iH->it_cond.bitmask = cond->bitmask;
    iH->it_cond.bit_pattern = cond->bit_pattern;
    iH->has_fixed_keylen = keylen_fixed;

    uint32_t prefix = iH->it_cond.bit_pattern & iH->it_cond.bitmask;
    memcpy((void*)iH->current_key, (char *)&prefix, 4);
    iH->keylength = 4;
    iH->end = FALSE;

    //std::bitset<32> set0 (*(uint32_t*)iH->current_key);
    //std::cerr << "minkey = " << set0 << std::endl;

    std::unique_lock<std::mutex> lock(m_it_map_mutex);
    m_it_map.insert(iH);

    return KV_SUCCESS;
}

kv_result kv_emulator::kv_iterator_next_set(kv_iterator_handle iter_hdl, kv_iterator_list *iter_list, void *ioctx) {
    (void) ioctx;

    if (iter_hdl == NULL || iter_list == NULL) {
        return KV_ERR_PARAM_NULL;
    }

    const bool include_value = iter_hdl->it_op == KV_ITERATOR_OPT_KV || iter_hdl->it_op == KV_ITERATOR_OPT_KV_WITH_DELETE;
    const bool delete_value = iter_hdl->it_op == KV_ITERATOR_OPT_KV_WITH_DELETE;

    kv_key key;
    key.key = iter_hdl->current_key;
    key.length = iter_hdl->keylength;

    // 4 leading bytes to match
    uint32_t to_match = iter_hdl->it_cond.bitmask & iter_hdl->it_cond.bit_pattern;

    // treat bitmask of 0 as iterating all keys
    bool iterate_all = (iter_hdl->it_cond.bitmask == 0);

    bool_t end = TRUE;
    iter_list->end = TRUE;

    iter_list->num_entries = 0;
    const uint32_t buffer_size  = iter_list->size;
    char *buffer = (char *) iter_list->it_list;
    uint32_t buffer_pos = 0;
    int counter = 0;

    std::unique_lock<std::mutex> lock(m_map_mutex);

    uint32_t prefix = 0;
    auto it = m_map.lower_bound(&key);
    while (it != m_map.end()) {
        const int klength = it->first->length;
        const int vlength = it->second.length();

        // only to try matching when there is a valid bitmask
        if (!iterate_all) {
            // match leading 4 bytes
            memcpy(&prefix, it->first->key, 4);

            // if no more match, which means we reached the end of matching list
            if (((prefix & iter_hdl->it_cond.bitmask) & iter_hdl->it_cond.bit_pattern) != to_match) {
                iter_list->end = TRUE;
                end = TRUE;
                break;
            }
        }

        // printf("matched 0x%X, current key prefix 0x%X, -- %d\n", to_match, prefix, i);
        // found a key
        size_t datasize = klength;
        if (!iter_hdl->has_fixed_keylen) {
            datasize += sizeof(kv_key::key);
        }
        datasize += (include_value)? (vlength  + sizeof(kv_value::value)):0;

        if ((buffer_pos + datasize) > buffer_size) {
            // save the current key for next iteration
            iter_list->end = FALSE;
            end = FALSE;
            iter_hdl->keylength = klength;
            memcpy(iter_hdl->current_key, it->first->key, klength);
            //std::cerr << "save  key  " << set0 << std::endl;
            // printf("no more buffer space\n");
            break;
        }

        //std::cerr << "found key  " << set0 << std::endl;
        // only output key len when key size is not fixed
        if (!iter_hdl->has_fixed_keylen) {
            memcpy(buffer + buffer_pos, &klength,        sizeof(kv_key_t));
            buffer_pos += sizeof(kv_key_t);
        }
        memcpy(buffer + buffer_pos, it->first->key, klength);
        buffer_pos += klength;

        if (include_value) {
            memcpy(buffer + buffer_pos, &vlength,        sizeof(kv_value_t));
            buffer_pos += sizeof(kv_value_t);

            memcpy(buffer + buffer_pos, it->second.data(), vlength);
            buffer_pos += vlength;
        }
        counter++;

        if (delete_value) {
            it = m_map.erase(it);
        } else {
            it++;
        }
    }

    // EOF
    // printf("emulator internal iterator: XXX got entries %d\n", counter);
    iter_list->num_entries = counter;
    if (end != TRUE) {
        return KV_WRN_MORE;
    }

    return KV_SUCCESS;
}

// match iterator condition, return max of 1 key
kv_result kv_emulator::kv_iterator_next(kv_iterator_handle iter_hdl, kv_key *key, kv_value *value, void *ioctx) {
    (void) ioctx;

    if (key == NULL || key->key == NULL || value == NULL || value->value == NULL) {
        return KV_ERR_PARAM_NULL;
    }

    const bool include_value = iter_hdl->it_op == KV_ITERATOR_OPT_KV || iter_hdl->it_op == KV_ITERATOR_OPT_KV_WITH_DELETE;
    const bool delete_value = iter_hdl->it_op == KV_ITERATOR_OPT_KV_WITH_DELETE;

    kv_key key1;
    key1.key = iter_hdl->current_key;
    key1.length = iter_hdl->keylength;

    // check if the end is set from last iteration
    if (iter_hdl->end) {
        return KV_ERR_ITERATOR_END;
    }

    // 4 leading bytes to match
    uint32_t to_match = iter_hdl->it_cond.bitmask & iter_hdl->it_cond.bit_pattern;

    std::unique_lock<std::mutex> lock(m_map_mutex);

    uint32_t prefix = 0;
    auto it = m_map.lower_bound(&key1);

    // the end
    if (it == m_map.end()) {
        return KV_ERR_ITERATOR_END;
    }

    const uint32_t klength = it->first->length;
    const uint32_t vlength = it->second.length();

    // match leading 4 bytes
    memcpy(&prefix, it->first->key, 4);

    // if no more match, which means we reached the end of matching list
    if (((prefix & iter_hdl->it_cond.bitmask) & iter_hdl->it_cond.bit_pattern) != to_match) {
        return KV_ERR_ITERATOR_END;
    }

    // printf("matched 0x%X, current key prefix 0x%X, -- %d\n", to_match, prefix, i);
    // found a key
    // first check key size
    key->length = klength;
    if (klength > key->length) {
        // first save unused key for next iteration 
        iter_hdl->keylength = klength;
        memcpy(iter_hdl->current_key, it->first->key, klength);
        return KV_ERR_BUFFER_SMALL;
    }

    if (iter_hdl->it_op == KV_ITERATOR_OPT_KV || iter_hdl->it_op == KV_ITERATOR_OPT_KV_WITH_DELETE) {
        ///// temporary
        // value->value_size = vlength;
        if (vlength > value->length) {
            value->length= 0;
            value->offset = 0;
            iter_hdl->keylength = klength;
            // first save unused key for next iteration 
            memcpy(iter_hdl->current_key, it->first->key, klength);
            return KV_ERR_BUFFER_SMALL;
        }
    }

    memcpy(key->key, it->first->key, klength);

    if (include_value) {
        memcpy(value->value, it->second.data(), vlength);
        value->length= vlength;
        ///// temporary
        // value->value_size = vlength;
        value->offset = 0;
    }

    // delete the identified key, it points to next element
    if (delete_value) {
        it = m_map.erase(it);
    } else {
        it++;
    }
    // save next key for next iteration
    if (it != m_map.end()) {
        key->length = klength;
        iter_hdl->keylength = klength;
        memcpy(iter_hdl->current_key, it->first->key, klength);
    } else {
        iter_hdl->end = TRUE;
    }

    return KV_SUCCESS;
}

kv_result kv_emulator::kv_close_iterator(kv_iterator_handle iter_hdl, void *ioctx) {
    (void) ioctx;

    if (iter_hdl == NULL) return KV_ERR_PARAM_NULL;
    {
        std::unique_lock<std::mutex> lock(m_it_map_mutex);
        auto it = m_it_map.find(iter_hdl);
        if (it != m_it_map.end()) {
            m_it_map.erase(it);
        }
    }
    delete iter_hdl;

    return KV_SUCCESS;
}

kv_result kv_emulator::kv_list_iterators(kv_iterator *iter_list, uint32_t *count, void *ioctx) {
    (void) ioctx;

    if (iter_list == NULL || count == NULL) {
        return KV_ERR_PARAM_NULL;
    }

    {
        uint32_t i = 0;
        const uint32_t max = *count;
        std::unique_lock<std::mutex> lock(m_it_map_mutex);
        for (const auto &it : m_it_map) {
            kv_iterator_handle handle = it;
            if (i < max) {
                iter_list[i].itid = i;
                iter_list[i].iter_op = (kv_iterator_option)handle->it_op;
                if (iter_list[i].iter_cond)
                    memcpy(iter_list[i].iter_cond, &handle->it_cond, sizeof(kv_group_condition));
            }
        }
        *count = i;
    }

    return KV_SUCCESS;
}

kv_result kv_emulator::kv_delete_group(kv_group_condition *grp_cond, uint64_t *recovered_bytes, void *ioctx) {
    (void) ioctx;

    uint32_t minkey = grp_cond->bitmask & grp_cond->bit_pattern;

    kv_key key;
    key.key = &minkey;
    key.length = 4;

    // 4 leading bytes to match
    uint32_t to_match = grp_cond->bitmask & grp_cond->bit_pattern;
    emulator_map_t::iterator it_tmp;

    std::unique_lock<std::mutex> lock(m_map_mutex);

    auto it = m_map.lower_bound(&key);
    while (it != m_map.end()) {
        uint32_t prefix = 0;
        memcpy(&prefix, it->first->key, 4);

        // validate, if it no longer match, then we are done
        // as the map is ordered by leading 4 byte as integer
        // in ascending order
        if (((prefix & grp_cond->bitmask) & grp_cond->bit_pattern) != to_match ) {
            return KV_SUCCESS;
        }

        // update reclaimed space first
        kv_key *k = it->first;
        m_available += k->length + it->second.length();

        it_tmp = it;
        it++;
        m_map.erase(it_tmp);
    }

    return KV_SUCCESS;
}

// emulator will set this up at queue level
// shouldn't be called, so just return error.
kv_result kv_emulator::set_interrupt_handler(const kv_interrupt_handler int_hdl) {
    (void) int_hdl;
    return KV_ERR_DEV_INIT;
}

// shouldn't be called, so just return error.
kv_result kv_emulator::poll_completion(uint32_t timeout_usec, uint32_t *num_events) {
    (void) timeout_usec;
    (void) num_events;
    return KV_ERR_DEV_INIT;
}

uint64_t kv_emulator::get_total_capacity() { return m_capacity;  }
uint64_t kv_emulator::get_available() { return m_available; }

} // end of namespace
