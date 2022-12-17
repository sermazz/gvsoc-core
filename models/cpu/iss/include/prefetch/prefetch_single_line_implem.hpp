/*
 * Copyright (C) 2020 GreenWaves Technologies, SAS, ETH Zurich and
 *                    University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Authors: Germain Haugou, GreenWaves Technologies (germain.haugou@greenwaves-technologies.com)
 */

#pragma once

#include "types.hpp"
#include <stdio.h>

inline void Prefetcher::fetch_novalue(iss_insn_t *insn)
{
    // Compute where the instructions address falls into the prefetch buffer
    iss_addr_t addr = insn->addr;
    int index = addr - this->buffer_start_addr;

    // If it is entirely within the buffer, returns nothing to fake a hit.
    if (likely(index >= 0 && index <= ISS_PREFETCHER_SIZE - sizeof(iss_opcode_t)))
    {
        return;
    }

    // Otherwise, fake a refill
    this->fetch_novalue_refill(insn, addr, index);
}

inline void Prefetcher::fetch(iss_insn_t *insn)
{
    this->trace.msg(vp::trace::LEVEL_TRACE, "Prefetching instruction (pc: 0x%x)\n", insn->addr);

    // This is an optimization, since we are on the critical path.
    // The opcode is refilled the first time the instruction is executed, and then, the fetch is just fake
    // to get the proper timing, without the overhead of the data copy.
    if (likely(insn->fetched))
    {
        this->fetch_novalue(insn);
    }
    else
    {
        insn->fetched = true;
        this->fetch_value(insn);
    }
}

inline void Prefetcher::flush()
{
    // Since the address is an unsigned int, the next index will be negative and will force the prefetcher
    // to refill
    this->buffer_start_addr = -1;
}

inline void Prefetcher::handle_stall(void (*callback)(Prefetcher *), iss_insn_t *current_insn)
{
    Iss *iss = &this->iss;

    // Function to be called when teh refill is done
    this->fetch_stall_callback = callback;
    // Remember the current instruction since the core may switch to a new one while the prefetch buffer
    // is being refilled
    this->prefetch_insn = current_insn;
    // Stall the core
    iss->exec.stalled_inc();
}
