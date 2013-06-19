/*
 * Copyright (c) 2013 Plausible Labs Cooperative, Inc.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "dwarf_cfa_stack.hpp"

using namespace plcrash;

/**
 * Push a state onto the state stack; all existing values will be saved on the stack, and registers
 * will be set to their default state.
 *
 * @return Returns true on success, or false if insufficient space is available on the state
 * stack.
 */
bool dwarf_cfa_stack::push_state (void) {
    PLCF_ASSERT(_table_depth+1 <= DWARF_CFA_STACK_MAX_STATES);
    
    if (_table_depth+1 == DWARF_CFA_STACK_MAX_STATES)
        return false;
    
    _table_depth++;
    _register_count[_table_depth] = 0;
    plcrash_async_memset(_table_stack[_table_depth], DWARF_CFA_STACK_INVALID_ENTRY_IDX, sizeof(_table_stack[0]));
    
    return true;
}

/**
 * Pop a previously saved state from the state stack. All existing values will be discarded on the stack, and registers
 * will be reinitialized from the saved state.
 *
 * @return Returns true on success, or false if no states are available on the state stack.
 */
bool dwarf_cfa_stack::pop_state (void) {
    if (_table_depth == 0)
        return false;
    
    _table_depth--;
    return true;
}

/*
 * Default constructor.
 */
dwarf_cfa_stack::dwarf_cfa_stack (void) {
    /* The size must be smaller than the invalid entry index, which is used as a NULL flag */
    PLCF_ASSERT_STATIC(max_size, DWARF_CFA_STACK_MAX_REGISTERS < DWARF_CFA_STACK_INVALID_ENTRY_IDX);
    
    /* Initialize the free list */
    for (uint8_t i = 0; i < DWARF_CFA_STACK_MAX_REGISTERS; i++)
        _entries[i].next = i+1;
    
    /* Set the terminator flag on the last entry */
    _entries[DWARF_CFA_STACK_MAX_REGISTERS-1].next = DWARF_CFA_STACK_INVALID_ENTRY_IDX;
    
    /* First free entry is _entries[0] */
    _free_list = 0;
    
    /* Initial register count */
    _register_count[0] = 0;
    
    /* Set up the table */
    _table_depth = 0;
    plcrash_async_memset(_table_stack[0], DWARF_CFA_STACK_INVALID_ENTRY_IDX, sizeof(_table_stack[0]));
}

/**
 * Add a new register.
 *
 * @param regnum The DWARF register number.
 * @param rule The DWARF CFA rule for @a regnum.
 * @param value The data value to be used when interpreting @a rule.
 */
bool dwarf_cfa_stack::set_register (uint32_t regnum, plcrash_dwarf_cfa_reg_rule_t rule, int64_t value) {
    PLCF_ASSERT(rule <= UINT8_MAX);
    
    /* Check for an existing entry, or find the target entry off which we'll chain our entry */
    unsigned int bucket = regnum % (sizeof(_table_stack[0]) / sizeof(_table_stack[0][0]));
    
    dwarf_cfa_reg_entry_t *parent = NULL;
    for (uint8_t parent_idx = _table_stack[_table_depth][bucket]; parent_idx != DWARF_CFA_STACK_INVALID_ENTRY_IDX; parent_idx = parent->next) {
        parent = &_entries[parent_idx];
        
        /* If an existing entry is found, we can re-use it directly */
        if (parent->regnum == regnum) {
            parent->value = value;
            parent->rule = rule;
            return true;
        }
        
        /* Otherwise, make sure we terminate with parent == last element */
        if (parent->next == DWARF_CFA_STACK_INVALID_ENTRY_IDX)
            break;
    }
    
    /* 'parent' now either points to the end of the list, or is NULL (in which case the table
     * slot was empty */
    dwarf_cfa_reg_entry *entry = NULL;
    uint8_t entry_idx;
    
    /* Fetch a free entry */
    if (_free_list == DWARF_CFA_STACK_INVALID_ENTRY_IDX) {
        /* No free entries */
        return false;
    }
    entry_idx = _free_list;
    entry = &_entries[entry_idx];
    _free_list = entry->next;
    
    /* Intialize the entry */
    entry->regnum = regnum;
    entry->rule = rule;
    entry->value = value;
    entry->next = DWARF_CFA_STACK_INVALID_ENTRY_IDX;
    
    /* Either insert in the parent, or insert as the first table element */
    if (parent == NULL) {
        _table_stack[_table_depth][bucket] = entry_idx;
    } else {
        parent->next = entry - _entries;
    }
    
    _register_count[_table_depth]++;
    return true;
}

/**
 * Fetch the register entry data for a given DWARF register number, returning
 * true on success, or false if no entry has been added for the register.
 *
 * @param regnum The DWARF register number.
 * @param rule[out] On success, the DWARF CFA rule for @a regnum.
 * @param value[out] On success, the data value to be used when interpreting @a rule.
 */
bool dwarf_cfa_stack::get_register_rule (uint32_t regnum, plcrash_dwarf_cfa_reg_rule_t *rule, int64_t *value) {
    /* Search for the entry */
    unsigned int bucket = regnum % (sizeof(_table_stack[0]) / sizeof(_table_stack[0][0]));
    
    dwarf_cfa_reg_entry_t *entry = NULL;
    for (uint8_t entry_idx = _table_stack[_table_depth][bucket]; entry_idx != DWARF_CFA_STACK_INVALID_ENTRY_IDX; entry_idx = entry->next) {
        entry = &_entries[entry_idx];
        
        if (entry->regnum != regnum) {
            if (entry->next == DWARF_CFA_STACK_INVALID_ENTRY_IDX)
                break;
            
            continue;
        }
        
        /* Existing entry found, we can re-use it directly */
        *value = entry->value;
        *rule = (plcrash_dwarf_cfa_reg_rule_t) entry->rule;
        return true;
    }
    
    /* Not found? */
    return false;
}

/**
 * Remove a register from the current state.
 *
 * @param regnum The DWARF register number to be removed.
 */
void dwarf_cfa_stack::remove_register (uint32_t regnum) {
    /* Search for the entry */
    unsigned int bucket = regnum % (sizeof(_table_stack[0]) / sizeof(_table_stack[0][0]));
    
    dwarf_cfa_reg_entry *prev = NULL;
    dwarf_cfa_reg_entry_t *entry = NULL;
    for (uint8_t entry_idx = _table_stack[_table_depth][bucket]; entry_idx != DWARF_CFA_STACK_INVALID_ENTRY_IDX; entry_idx = entry->next) {
        prev = entry;
        entry = &_entries[entry_idx];
        
        if (entry->regnum != regnum)
            continue;
        
        /* Remove from the bucket chain */
        if (prev != NULL) {
            prev->next = entry->next;
        } else {
            _table_stack[_table_depth][bucket] = entry->next;
        }
        
        /* Re-insert in the free list */
        entry->next = _free_list;
        _free_list = entry_idx;
        
        /* Decrement the register count */
        _register_count[_table_depth]--;
    }
}

/**
 * Return the number of register rules set for the current register state.
 */
uint8_t dwarf_cfa_stack::get_register_count (void) {
    return _register_count[_table_depth];
}

/**
 * Construct an iterator for @a stack. The @a stack <em>must not</em> be mutated
 * during iteration.
 *
 * @param stack The stack to be iterated.
 */
dwarf_cfa_stack_iterator::dwarf_cfa_stack_iterator(dwarf_cfa_stack *stack) {
    _stack = stack;
    _bucket_idx = 0;
    _cur_entry_idx = DWARF_CFA_STACK_INVALID_ENTRY_IDX;
}

/**
 * Enumerate the next register entry. Returns true on success, or false if no additional entries are available.
 *
 * @param regnum[out] On success, the DWARF register number.
 * @param rule[out] On success, the DWARF CFA rule for @a regnum.
 * @param value[out] On success, the data value to be used when interpreting @a rule.
 */
bool dwarf_cfa_stack_iterator::next (uint32_t *regnum, plcrash_dwarf_cfa_reg_rule_t *rule, int64_t *value) {
    /* Fetch the next entry in the bucket chain */
    if (_cur_entry_idx != DWARF_CFA_STACK_INVALID_ENTRY_IDX) {
        _cur_entry_idx = _stack->_entries[_cur_entry_idx].next;
        
        /* Advance to the next bucket if we've reached the end of the current chain */
        if (_cur_entry_idx == DWARF_CFA_STACK_INVALID_ENTRY_IDX)
            _bucket_idx++;
    }
    
    /*
     * On the first iteration, or after the end of a bucket chain has been reached, find the next valid bucket chain.
     * Otherwise, we have a valid bucket chain and simply need the next entry.
     */
    if (_cur_entry_idx == DWARF_CFA_STACK_INVALID_ENTRY_IDX) {
        for (; _bucket_idx < DWARF_CFA_STACK_BUCKET_COUNT; _bucket_idx++) {
            if (_stack->_table_stack[_stack->_table_depth][_bucket_idx] != DWARF_CFA_STACK_INVALID_ENTRY_IDX) {
                _cur_entry_idx = _stack->_table_stack[_stack->_table_depth][_bucket_idx];
                break;
            }
        }
        
        /* If we get here without a valid entry, we've hit the end of all bucket chains. */
        if (_cur_entry_idx == DWARF_CFA_STACK_INVALID_ENTRY_IDX)
            return false;
    }
    
    
    typename dwarf_cfa_stack::dwarf_cfa_reg_entry_t *entry = &_stack->_entries[_cur_entry_idx];
    *regnum = entry->regnum;
    *value = entry->value;
    *rule = (plcrash_dwarf_cfa_reg_rule_t) entry->rule;
    return true;
}