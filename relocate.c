/*
 * relocate.c - Performs SCE ELF relocations
 * Copyright 2015 Yifan Lu
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "load.h"
#include "relocate.h"
#include "scefuncs.h"
#include "utils.h"
#include "uvloader.h"

/********************************************//**
 *  \brief Write to loaded segment at offset
 *  
 *  \returns Zero on success, otherwise error
 ***********************************************/
int
uvl_segment_write (Elf32_Phdr_t *seg,   ///< Where to write
                    u32_t offset,       ///< Offset to write to
                    void *value,        ///< What to write
                    u32_t size)         ///< How much
{
    if (offset+size > seg->p_filesz) // don't overflow me plz
    {
        LOG ("Relocation overflows segment");
        return -1;
    }
    if (seg->p_flags & PF_X)
    {
        uvl_unlock_mem ();
    }
    memcpy ((char *)seg->p_vaddr + offset, value, size);
    if (seg->p_flags & PF_X)
    {
        uvl_lock_mem ();
    }
    return 0;
}

/********************************************//**
 *  \brief Perform SCE relocation
 *  
 *  \returns Zero on success, otherwise error
 ***********************************************/
int
uvl_relocate (void *reloc,          ///< Base address of relocation segment
              u32_t size,           ///< Size of relocation segment
              Elf32_Phdr_t *segs)   ///< All segments loaded
{
    sce_reloc_t *entry;
    u32_t pos;
    u16_t r_code;
    u32_t r_offset;
    u32_t r_addend;
    u8_t r_symseg;
    u8_t r_datseg;
    s32_t offset;
    u32_t symval, addend, loc;
    u32_t upper, lower, sign, j1, j2;
    u32_t value;

    pos = 0;
    while (pos < size)
    {
        // get entry
        entry = (sce_reloc_t *)((char *)reloc + pos);
        if (SCE_RELOC_IS_SHORT (*entry))
        {
            r_offset = SCE_RELOC_SHORT_OFFSET (entry->r_short);
            r_addend = SCE_RELOC_SHORT_ADDEND (entry->r_short);
            pos += 8;
        }
        else
        {
            r_offset = SCE_RELOC_LONG_OFFSET (entry->r_long);
            r_addend = SCE_RELOC_LONG_ADDEND (entry->r_long);
            if (SCE_RELOC_LONG_CODE2 (entry->r_long))
            {
                IF_VERBOSE LOG ("Code2 ignored for relocation at %X.", pos);
            }
            pos += 12;
        }

        // get values
        r_symseg = SCE_RELOC_SYMSEG (*entry);
        r_datseg = SCE_RELOC_DATSEG (*entry);
        symval = r_symseg == 15 ? 0 : (u32_t)segs[r_symseg].p_vaddr;
        loc = (u32_t)segs[r_datseg].p_vaddr + r_offset;

        // perform relocation
        // taken from linux/arch/arm/kernel/module.c of Linux Kernel 4.0
        switch (SCE_RELOC_CODE (*entry))
        {
            case R_ARM_V4BX:
                {
                    /* Preserve Rm and the condition code. Alter
                    * other bits to re-code instruction as
                    * MOV PC,Rm.
                    */
                    value = (*(u32_t *)loc & 0xf000000f) | 0x01a0f000;
                }
                break;
            case R_ARM_ABS32:
            case R_ARM_TARGET1:
                {
                    value = r_addend + symval;
                }
                break;
            case R_ARM_REL32:
            case R_ARM_TARGET2:
                {
                    value = r_addend + symval - loc;
                }
                break;
            case R_ARM_THM_CALL:
                {
                    upper = *(u16_t *)loc;
                    lower = *(u16_t *)(loc + 2);

                    /*
                     * 25 bit signed address range (Thumb-2 BL and B.W
                     * instructions):
                     *   S:I1:I2:imm10:imm11:0
                     * where:
                     *   S     = upper[10]   = offset[24]
                     *   I1    = ~(J1 ^ S)   = offset[23]
                     *   I2    = ~(J2 ^ S)   = offset[22]
                     *   imm10 = upper[9:0]  = offset[21:12]
                     *   imm11 = lower[10:0] = offset[11:1]
                     *   J1    = lower[13]
                     *   J2    = lower[11]
                     */
                    sign = (upper >> 10) & 1;
                    j1 = (lower >> 13) & 1;
                    j2 = (lower >> 11) & 1;
                    offset = r_addend + symval - loc;

                    if (offset <= (s32_t)0xff000000 ||
                        offset >= (s32_t)0x01000000) {
                        LOG ("reloc %x out of range: 0x%08X", pos, symval);
                        break;
                    }

                    sign = (offset >> 24) & 1;
                    j1 = sign ^ (~(offset >> 23) & 1);
                    j2 = sign ^ (~(offset >> 22) & 1);
                    upper = (u16_t)((upper & 0xf800) | (sign << 10) |
                                ((offset >> 12) & 0x03ff));
                    lower = (u16_t)((lower & 0xd000) |
                              (j1 << 13) | (j2 << 11) |
                              ((offset >> 1) & 0x07ff));

                    value = ((u32_t)lower << 16) | upper;
                }
                break;
            case R_ARM_CALL:
            case R_ARM_JUMP24:
                {
                    offset = r_addend + symval - loc;
                    if (offset <= (s32_t)0xfe000000 ||
                        offset >= (s32_t)0x02000000) {
                        LOG ("reloc %x out of range: 0x%08X", pos, symval);
                        break;
                    }

                    offset >>= 2;
                    offset &= 0x00ffffff;

                    value = (*(u32_t *)loc & 0xff000000) | offset;
                }
                break;
            case R_ARM_PREL31:
                {
                    offset = r_addend + symval - loc;
                    value = offset & 0x7fffffff;
                }
                break;
            case R_ARM_MOVW_ABS_NC:
            case R_ARM_MOVT_ABS:
                {
                    offset = symval + r_addend;
                    if (SCE_RELOC_CODE (*entry) == R_ARM_MOVT_ABS)
                        offset >>= 16;

                    value = *(u32_t *)loc;
                    value &= 0xfff0f000;
                    value |= ((offset & 0xf000) << 4) |
                          (offset & 0x0fff);
                }
                break;
            case R_ARM_THM_MOVW_ABS_NC:
            case R_ARM_THM_MOVT_ABS:
                {
                    upper = *(u16_t *)loc;
                    lower = *(u16_t *)(loc + 2);

                    /*
                     * MOVT/MOVW instructions encoding in Thumb-2:
                     *
                     * i    = upper[10]
                     * imm4 = upper[3:0]
                     * imm3 = lower[14:12]
                     * imm8 = lower[7:0]
                     *
                     * imm16 = imm4:i:imm3:imm8
                     */
                    offset = r_addend + symval;

                    if (SCE_RELOC_CODE (*entry) == R_ARM_THM_MOVT_ABS)
                        offset >>= 16;

                    upper = (u16_t)((upper & 0xfbf0) |
                              ((offset & 0xf000) >> 12) |
                              ((offset & 0x0800) >> 1));
                    lower = (u16_t)((lower & 0x8f00) |
                              ((offset & 0x0700) << 4) |
                              (offset & 0x00ff));

                    value = ((u32_t)lower << 16) | upper;
                }
                break;
            default:
                {
                    LOG ("Unknown relocation code %u at %x", r_code, pos);
                }
            case R_ARM_NONE:
                continue;
        }

        // write value
        uvl_segment_write (&segs[r_datseg], r_offset, &value, sizeof (value));
    }

    return 0;
}
