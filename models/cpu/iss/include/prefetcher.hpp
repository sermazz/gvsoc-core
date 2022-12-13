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

#ifndef __CPU_ISS_PREFETCHER_HPP
#define __CPU_ISS_PREFETCHER_HPP

#include "types.hpp"
#include <stdio.h>

static inline void prefetcher_init(Iss *iss);
static inline iss_opcode_t prefetcher_fill(Iss *iss, iss_addr_t addr, bool timed);
static inline void __attribute__((always_inline)) prefetcher_fetch(Iss *iss, iss_insn_t *insn);


// This can be called to force the core to refetch the current instruction,
// for example after the pc has been modified
static inline void prefetcher_refetch(Iss *iss)
{
    prefetcher_fetch(iss, iss->cpu.current_insn);
}


static inline int iss_fetch_req(Iss *_this, uint64_t addr, uint8_t *data, uint64_t size, bool is_write)
{
  vp::io_req *req = &_this->fetch_req;

  _this->trace.msg(vp::trace::LEVEL_TRACE, "Fetch request (addr: 0x%x, size: 0x%x)\n", addr, size);

  req->init();
  req->set_addr(addr);
  req->set_size(size);
  req->set_is_write(is_write);
  req->set_data(data);
  vp::io_req_status_e err = _this->fetch.req(req);
  if (err != vp::IO_REQ_OK)
  {
    if (err == vp::IO_REQ_INVALID)
    {
      _this->trace.force_warning("Invalid fetch request (addr: 0x%x, size: 0x%x)\n", addr, size);
      return 0;
    }
    else
    {
      _this->trace.msg(vp::trace::LEVEL_TRACE, "Waiting for asynchronous response\n");
      return -1;
    }
  }

  int cycles = req->get_latency();
  _this->cpu.state.insn_cycles += cycles;
  iss_pccr_account_event(_this, CSR_PCER_IMISS, cycles);

  return 0;
}


static inline int prefetcher_fill(iss_prefetcher_t *prefetcher, Iss *iss, iss_addr_t addr)
{
  uint32_t aligned_addr = addr & ~(ISS_PREFETCHER_SIZE-1);
  prefetcher->addr = aligned_addr;
  return iss_fetch_req(iss, aligned_addr, prefetcher->data, ISS_PREFETCHER_SIZE, false);
}


static inline void prefetcher_fetch_value_resume_1(Iss *iss)
{
  iss_prefetcher_t *prefetcher = &iss->cpu.prefetcher;
  iss_addr_t addr = iss->cpu.prefetch_insn->addr;
  uint32_t next_addr = (addr + ISS_PREFETCHER_SIZE - 1) & ~(ISS_PREFETCHER_SIZE-1);
  // Number of bytes of the opcode which fits the first line
  int nb_bytes = next_addr - addr;

  // And append the second part from second line
  iss->cpu.prefetch_insn->opcode = iss->cpu.state.fetch_stall_opcode | (( *(iss_opcode_t *)&prefetcher->data[0]) << (nb_bytes*8));
  iss_decode_pc_noexec(iss, iss->cpu.prefetch_insn);
}


static inline void prefetcher_fetch_value_after_fill_0(
  iss_prefetcher_t *prefetcher, Iss *iss, iss_insn_t *insn, int index)
{
  iss_addr_t addr = insn->addr;

  if (likely(index + ISS_OPCODE_MAX_SIZE <= ISS_PREFETCHER_SIZE))
  {
    insn->opcode = *(iss_opcode_t *)&prefetcher->data[index];
    iss_decode_pc_noexec(iss, insn);
  }
  else
  {
    // Case where the opcode is between 2 lines. The prefetcher can only store one line so we have
    // to temporarly store the first part to return it with the next one coming from the final line.
    iss_opcode_t opcode = 0;

    // Compute address of next line
    uint32_t next_addr = (addr + ISS_PREFETCHER_SIZE - 1) & ~(ISS_PREFETCHER_SIZE-1);
    // Number of bytes of the opcode which fits the first line
    int nb_bytes = next_addr - addr;
    // Copy first part from first line
    memcpy((void *)&opcode, (void *)&prefetcher->data[index], nb_bytes);
    // Fetch next line
    if (prefetcher_fill(prefetcher, iss, next_addr))
    {
      iss->cpu.state.fetch_stall_callback = prefetcher_fetch_value_resume_1;
      iss->cpu.state.fetch_stall_opcode = opcode;
      iss->cpu.prefetch_insn = insn;
      iss->stalled_inc();
      return;
    }
    // And append the second part from second line
    opcode = opcode | (( *(iss_opcode_t *)&prefetcher->data[0]) << (nb_bytes*8));

    insn->opcode = opcode;
    iss_decode_pc_noexec(iss, insn);
  }
}


static inline void prefetcher_fetch_value_resume_0(Iss *iss)
{
  iss_prefetcher_t *prefetcher = &iss->cpu.prefetcher;
  iss_addr_t addr = iss->cpu.prefetch_insn->addr;
  int index = addr - prefetcher->addr;
  prefetcher_fetch_value_after_fill_0(prefetcher, iss, iss->cpu.prefetch_insn, index);
}


static void __attribute__((noinline)) prefetcher_fetch_value(
  iss_prefetcher_t *prefetcher, Iss *iss, iss_insn_t *insn)
{
  iss_addr_t addr = insn->addr;
  int index = addr - prefetcher->addr;

  if (likely(index >= 0 && index  <= ISS_PREFETCHER_SIZE - sizeof(iss_opcode_t)))
  {
    insn->opcode = *(iss_opcode_t *)&prefetcher->data[index];
    iss_decode_pc_noexec(iss, insn);
    return;
  }

  if (unlikely(index < 0 || index >= ISS_PREFETCHER_SIZE))
  {
    if (prefetcher_fill(prefetcher, iss, addr))
    {
      iss->cpu.state.fetch_stall_callback = prefetcher_fetch_value_resume_0;
      iss->cpu.prefetch_insn = insn;
      iss->stalled_inc();
      return;
    }
    index = addr - prefetcher->addr;
  }

  prefetcher_fetch_value_after_fill_0(prefetcher, iss, insn, index);
}


static inline void prefetcher_fetch_novalue_check_overflow(iss_prefetcher_t *prefetcher, Iss *iss, iss_insn_t *insn, int index)
{
  if (unlikely(index + ISS_OPCODE_MAX_SIZE > ISS_PREFETCHER_SIZE))
  {
    if (prefetcher_fill(prefetcher, iss, prefetcher->addr + ISS_PREFETCHER_SIZE))
    {
      iss->cpu.state.fetch_stall_callback = NULL;
      iss->cpu.prefetch_insn = insn;
      iss->stalled_inc();
      return;
    }
  }
}


static inline void prefetcher_fetch_novalue_resume_0(Iss *iss)
{
  iss_prefetcher_t *prefetcher = &iss->cpu.prefetcher;
  iss_addr_t addr = iss->cpu.prefetch_insn->addr;
  int index = addr - prefetcher->addr;
  prefetcher_fetch_novalue_check_overflow(prefetcher, iss, iss->cpu.prefetch_insn, index);
}


static void __attribute__((noinline)) prefetcher_fetch_novalue_refill(
  iss_prefetcher_t *prefetcher, Iss *iss, iss_insn_t *insn, iss_addr_t addr, int index)
{
  if (unlikely(index < 0 || index >= ISS_PREFETCHER_SIZE))
  {
    if (prefetcher_fill(prefetcher, iss, addr))
    {
      iss->cpu.state.fetch_stall_callback = prefetcher_fetch_novalue_resume_0;
      iss->cpu.state.fetch_stall_callback = NULL;
      iss->cpu.prefetch_insn = insn;
      iss->stalled_inc();
      return;
    }
    index = addr - prefetcher->addr;
  }

  prefetcher_fetch_novalue_check_overflow(prefetcher, iss, insn, index);
}




static inline void __attribute__((always_inline)) prefetcher_fetch_novalue(
  iss_prefetcher_t *prefetcher, Iss *iss, iss_insn_t *insn)
{
  iss_addr_t addr = insn->addr;
  int index = addr - prefetcher->addr;

  if (likely(index >= 0 && index  <= ISS_PREFETCHER_SIZE - sizeof(iss_opcode_t)))
  {
    return;
  }

  prefetcher_fetch_novalue_refill(prefetcher, iss, insn, addr, index);
}





static inline void __attribute__((always_inline)) prefetcher_fetch(Iss *iss, iss_insn_t *insn)
{
  iss->trace.msg(vp::trace::LEVEL_TRACE, "Prefetching instruction (pc: 0x%x)\n", insn->addr);
  if (insn->fetched)
  {
    prefetcher_fetch_novalue(&iss->cpu.prefetcher, iss, insn);
  }
  else
  {
    insn->fetched = true;
    prefetcher_fetch_value(&iss->cpu.prefetcher, iss, insn);
  }
}



static inline void prefetcher_flush(Iss *iss)
{
  iss->cpu.decode_prefetcher.addr = -1;
  iss->cpu.prefetcher.addr = -1;
}



static inline void prefetcher_init(Iss *iss)
{
  prefetcher_flush(iss);
}



#endif
