/* -*-  Mode:C; c-basic-offset:4; tab-width:4 -*-
 ****************************************************************************
 * (C) 2003 - Rolf Neugebauer - Intel Research Cambridge
 ****************************************************************************
 *
 *        File: mm.c
 *      Author: Rolf Neugebauer (neugebar@dcs.gla.ac.uk)
 *     Changes: 
 *              
 *        Date: Aug 2003
 * 
 * Environment: Xen Minimal OS
 * Description: memory management related functions
 *              contains buddy page allocator from Xen.
 *
 ****************************************************************************
 * $Id: c-insert.c,v 1.7 2002/11/08 16:04:34 rn Exp $
 ****************************************************************************
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <os.h>
#include <hypervisor.h>
#include <mm.h>
#include <types.h>
#include <lib.h>

unsigned long *phys_to_machine_mapping;
extern char *stack;
extern char _text, _etext, _edata, _end;

static void init_page_allocator(unsigned long min, unsigned long max);

void init_mm(void)
{

    unsigned long start_pfn, max_pfn, max_free_pfn;

    unsigned long *pgd = (unsigned long *)start_info.pt_base;

    printk("MM: Init\n");

    printk("  _text:        %p\n", &_text);
    printk("  _etext:       %p\n", &_etext);
    printk("  _edata:       %p\n", &_edata);
    printk("  stack start:  %p\n", &stack);
    printk("  _end:         %p\n", &_end);

    /* set up minimal memory infos */
    start_pfn = PFN_UP(__pa(&_end));
    max_pfn = start_info.nr_pages;

    printk("  start_pfn:    %lx\n", start_pfn);
    printk("  max_pfn:      %lx\n", max_pfn);

    /*
     * we know where free tables start (start_pfn) and how many we 
     * have (max_pfn). 
     * 
     * Currently the hypervisor stores page tables it providesin the
     * high region of the this memory range.
     * 
     * next we work out how far down this goes (max_free_pfn)
     * 
     * XXX this assumes the hypervisor provided page tables to be in
     * the upper region of our initial memory. I don't know if this 
     * is always true.
     */

    max_free_pfn = PFN_DOWN(__pa(pgd));
    {
        unsigned long *pgd = (unsigned long *)start_info.pt_base;
        unsigned long  pte;
        int i;
        printk("  pgd(pa(pgd)): %lx(%lx)", (u_long)pgd, __pa(pgd));

        for ( i = 0; i < (HYPERVISOR_VIRT_START>>22); i++ )
        {
            unsigned long pgde = *pgd++;
            if ( !(pgde & 1) ) continue;
            pte = machine_to_phys(pgde & PAGE_MASK);
            printk("  PT(%x): %lx(%lx)", i, (u_long)__va(pte), pte);
            if (PFN_DOWN(pte) <= max_free_pfn) 
                max_free_pfn = PFN_DOWN(pte);
        }
    }
    max_free_pfn--;
    printk("  max_free_pfn: %lx\n", max_free_pfn);

    /*
     * now we can initialise the page allocator
     */
    printk("MM: Initialise page allocator for %lx(%lx)-%lx(%lx)\n",
           (u_long)__va(PFN_PHYS(start_pfn)), PFN_PHYS(start_pfn), 
           (u_long)__va(PFN_PHYS(max_free_pfn)), PFN_PHYS(max_free_pfn));
    init_page_allocator(PFN_PHYS(start_pfn), PFN_PHYS(max_free_pfn));   


    /* Now initialise the physical->machine mapping table. */


    printk("MM: done\n");

    
}

/*********************
 * ALLOCATION BITMAP
 *  One bit per page of memory. Bit set => page is allocated.
 */

static unsigned long *alloc_bitmap;
#define PAGES_PER_MAPWORD (sizeof(unsigned long) * 8)

#define allocated_in_map(_pn) \
(alloc_bitmap[(_pn)/PAGES_PER_MAPWORD] & (1<<((_pn)&(PAGES_PER_MAPWORD-1))))


/*
 * Hint regarding bitwise arithmetic in map_{alloc,free}:
 *  -(1<<n)  sets all bits >= n. 
 *  (1<<n)-1 sets all bits <  n.
 * Variable names in map_{alloc,free}:
 *  *_idx == Index into `alloc_bitmap' array.
 *  *_off == Bit offset within an element of the `alloc_bitmap' array.
 */

static void map_alloc(unsigned long first_page, unsigned long nr_pages)
{
    unsigned long start_off, end_off, curr_idx, end_idx;

    curr_idx  = first_page / PAGES_PER_MAPWORD;
    start_off = first_page & (PAGES_PER_MAPWORD-1);
    end_idx   = (first_page + nr_pages) / PAGES_PER_MAPWORD;
    end_off   = (first_page + nr_pages) & (PAGES_PER_MAPWORD-1);

    if ( curr_idx == end_idx )
    {
        alloc_bitmap[curr_idx] |= ((1<<end_off)-1) & -(1<<start_off);
    }
    else 
    {
        alloc_bitmap[curr_idx] |= -(1<<start_off);
        while ( ++curr_idx < end_idx ) alloc_bitmap[curr_idx] = ~0L;
        alloc_bitmap[curr_idx] |= (1<<end_off)-1;
    }
}


static void map_free(unsigned long first_page, unsigned long nr_pages)
{
    unsigned long start_off, end_off, curr_idx, end_idx;

    curr_idx = first_page / PAGES_PER_MAPWORD;
    start_off = first_page & (PAGES_PER_MAPWORD-1);
    end_idx   = (first_page + nr_pages) / PAGES_PER_MAPWORD;
    end_off   = (first_page + nr_pages) & (PAGES_PER_MAPWORD-1);

    if ( curr_idx == end_idx )
    {
        alloc_bitmap[curr_idx] &= -(1<<end_off) | ((1<<start_off)-1);
    }
    else 
    {
        alloc_bitmap[curr_idx] &= (1<<start_off)-1;
        while ( ++curr_idx != end_idx ) alloc_bitmap[curr_idx] = 0;
        alloc_bitmap[curr_idx] &= -(1<<end_off);
    }
}



/*************************
 * BINARY BUDDY ALLOCATOR
 */

typedef struct chunk_head_st chunk_head_t;
typedef struct chunk_tail_st chunk_tail_t;

struct chunk_head_st {
    chunk_head_t  *next;
    chunk_head_t **pprev;
    int            level;
};

struct chunk_tail_st {
    int level;
};

/* Linked lists of free chunks of different powers-of-two in size. */
#define FREELIST_SIZE ((sizeof(void*)<<3)-PAGE_SHIFT)
static chunk_head_t *free_head[FREELIST_SIZE];
static chunk_head_t  free_tail[FREELIST_SIZE];
#define FREELIST_EMPTY(_l) ((_l)->next == NULL)

#define round_pgdown(_p)  ((_p)&PAGE_MASK)
#define round_pgup(_p)    (((_p)+(PAGE_SIZE-1))&PAGE_MASK)


/*
 * Initialise allocator, placing addresses [@min,@max] in free pool.
 * @min and @max are PHYSICAL addresses.
 */
static void init_page_allocator(unsigned long min, unsigned long max)
{
    int i;
    unsigned long range, bitmap_size;
    chunk_head_t *ch;
    chunk_tail_t *ct;

    for ( i = 0; i < FREELIST_SIZE; i++ )
    {
        free_head[i]       = &free_tail[i];
        free_tail[i].pprev = &free_head[i];
        free_tail[i].next  = NULL;
    }

    min = round_pgup  (min);
    max = round_pgdown(max);

    /* Allocate space for the allocation bitmap. */
    bitmap_size  = (max+1) >> (PAGE_SHIFT+3);
    bitmap_size  = round_pgup(bitmap_size);
    alloc_bitmap = (unsigned long *)__va(min);
    min         += bitmap_size;
    range        = max - min;

    /* All allocated by default. */
    memset(alloc_bitmap, ~0, bitmap_size);
    /* Free up the memory we've been given to play with. */
    map_free(min>>PAGE_SHIFT, range>>PAGE_SHIFT);

    /* The buddy lists are addressed in high memory. */
    min += PAGE_OFFSET;
    max += PAGE_OFFSET;

    while ( range != 0 )
    {
        /*
         * Next chunk is limited by alignment of min, but also
         * must not be bigger than remaining range.
         */
        for ( i = PAGE_SHIFT; (1<<(i+1)) <= range; i++ )
            if ( min & (1<<i) ) break;


        ch = (chunk_head_t *)min;
        min   += (1<<i);
        range -= (1<<i);
        ct = (chunk_tail_t *)min-1;
        i -= PAGE_SHIFT;
        ch->level       = i;
        ch->next        = free_head[i];
        ch->pprev       = &free_head[i];
        ch->next->pprev = &ch->next;
        free_head[i]    = ch;
        ct->level       = i;
    }
}


/* Release a PHYSICAL address range to the allocator. */
void release_bytes_to_allocator(unsigned long min, unsigned long max)
{
    min = round_pgup  (min) + PAGE_OFFSET;
    max = round_pgdown(max) + PAGE_OFFSET;

    while ( min < max )
    {
        __free_pages(min, 0);
        min += PAGE_SIZE;
    }
}


/* Allocate 2^@order contiguous pages. Returns a VIRTUAL address. */
unsigned long __get_free_pages(int order)
{
    int i;
    chunk_head_t *alloc_ch, *spare_ch;
    chunk_tail_t            *spare_ct;


    /* Find smallest order which can satisfy the request. */
    for ( i = order; i < FREELIST_SIZE; i++ ) {
	if ( !FREELIST_EMPTY(free_head[i]) ) 
	    break;
    }

    if ( i == FREELIST_SIZE ) goto no_memory;
 
    /* Unlink a chunk. */
    alloc_ch = free_head[i];
    free_head[i] = alloc_ch->next;
    alloc_ch->next->pprev = alloc_ch->pprev;

    /* We may have to break the chunk a number of times. */
    while ( i != order )
    {
        /* Split into two equal parts. */
        i--;
        spare_ch = (chunk_head_t *)((char *)alloc_ch + (1<<(i+PAGE_SHIFT)));
        spare_ct = (chunk_tail_t *)((char *)spare_ch + (1<<(i+PAGE_SHIFT)))-1;

        /* Create new header for spare chunk. */
        spare_ch->level = i;
        spare_ch->next  = free_head[i];
        spare_ch->pprev = &free_head[i];
        spare_ct->level = i;

        /* Link in the spare chunk. */
        spare_ch->next->pprev = &spare_ch->next;
        free_head[i] = spare_ch;
    }
    
    map_alloc(__pa(alloc_ch)>>PAGE_SHIFT, 1<<order);

    return((unsigned long)alloc_ch);

 no_memory:

    printk("Cannot handle page request order %d!\n", order);

    return 0;
}


/* Free 2^@order pages at VIRTUAL address @p. */
void __free_pages(unsigned long p, int order)
{
    unsigned long size = 1 << (order + PAGE_SHIFT);
    chunk_head_t *ch;
    chunk_tail_t *ct;
    unsigned long pagenr = __pa(p) >> PAGE_SHIFT;

    map_free(pagenr, 1<<order);
    
    /* Merge chunks as far as possible. */
    for ( ; ; )
    {
        if ( (p & size) )
        {
            /* Merge with predecessor block? */
            if ( allocated_in_map(pagenr-1) ) break;
            ct = (chunk_tail_t *)p - 1;
            if ( ct->level != order ) break;
            ch = (chunk_head_t *)(p - size);
            p -= size;
        }
        else
        {
            /* Merge with successor block? */
            if ( allocated_in_map(pagenr+(1<<order)) ) break;
            ch = (chunk_head_t *)(p + size);
            if ( ch->level != order ) break;
        }
        
        /* Okay, unlink the neighbour. */
        *ch->pprev = ch->next;
        ch->next->pprev = ch->pprev;

        order++;
        size <<= 1;
    }

    /* Okay, add the final chunk to the appropriate free list. */
    ch = (chunk_head_t *)p;
    ct = (chunk_tail_t *)(p+size)-1;
    ct->level = order;
    ch->level = order;
    ch->pprev = &free_head[order];
    ch->next  = free_head[order];
    ch->next->pprev = &ch->next;
    free_head[order] = ch;
}