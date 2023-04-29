#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

// Buddy allocator

static int nsizes;  // the number of entries in bd_sizes array

#define LEAF_SIZE 16          // The smallest block size
#define MAXSIZE (nsizes - 1)  // Largest index in bd_sizes array
#define BLK_SIZE(k) ((1L << (k)) * LEAF_SIZE)  // Size of block at size k
#define HEAP_SIZE BLK_SIZE(MAXSIZE)
#define NBLK(k) (1 << (MAXSIZE - k))  // Number of block at size k
#define ROUNDUP(n, sz) \
  (((((n)-1) / (sz)) + 1) * (sz))  // Round up to the next multiple of sz

typedef struct list Bd_list;

// The allocator has sz_info for each size k. Each sz_info has a free
// list, an array alloc to keep track which blocks have been
// allocated, and an split array to to keep track which blocks have
// been split.  The arrays are of type char (which is 1 byte), but the
// allocator uses 1 bit per block (thus, one char records the info of
// 8 blocks).
struct sz_info {
  Bd_list free;
  char *alloc;
  char *split;
};
typedef struct sz_info Sz_info;

static Sz_info *bd_sizes;
static void *bd_base;  // start address of memory managed by the buddy allocator
static struct spinlock lock;

// Return 1 if bit at position index in array is set to 1
int bit_isset(char *array, int index) {
  char b = array[index / 8];
  char m = (1 << (index % 8));
  return (b & m) == m;
}

// Set bit at position index in array to 1
void bit_set(char *array, int index) {
  char b = array[index / 8];
  char m = (1 << (index % 8));
  array[index / 8] = (b | m);
}

// Clear bit at position index in array
void bit_clear(char *array, int index) {
  char b = array[index / 8];
  char m = (1 << (index % 8));
  array[index / 8] = (b & ~m);
}

void bit_flip(char *array, int index) {
  if (bit_isset(array, index)) {
    bit_clear(array, index);
    return;
  }
  bit_set(array, index);
}

// Print a bit vector as a list of ranges of 1 bits
void bd_print_vector(char *vector, int len) {
  int last, lb;

  last = 1;
  lb = 0;
  for (int b = 0; b < len; b++) {
    if (last == bit_isset(vector, b)) continue;
    if (last == 1) printf(" [%d, %d)", lb, b);
    lb = b;
    last = bit_isset(vector, b);
  }
  if (lb == 0 || last == 1) {
    printf(" [%d, %d)", lb, len);
  }
  printf("\n");
}

// Print buddy's data structures
void bd_print() {
  for (int k = 0; k < nsizes; k++) {
    printf("size %d (blksz %d nblk %d): free list: ", k, BLK_SIZE(k), NBLK(k));
    lst_print(&bd_sizes[k].free);
    printf("  alloc:");
    bd_print_vector(bd_sizes[k].alloc, NBLK(k));
    if (k > 0) {
      printf("  split:");
      bd_print_vector(bd_sizes[k].split, NBLK(k));
    }
  }
}

// What is the first k such that 2^k >= n?
int firstk(uint64 n) {
  int k = 0;
  uint64 size = LEAF_SIZE;

  while (size < n) {
    k++;
    size *= 2;
  }
  return k;
}

// Compute the block index for address p at size k
int blk_index(int k, char *p) {
  int n = p - (char *)bd_base;
  return n / BLK_SIZE(k);
}

// Convert a block index at size k back into an address
void *addr(int k, int bi) {
  int n = bi * BLK_SIZE(k);
  return (char *)bd_base + n;
}

// allocate nbytes, but malloc won't return anything smaller than LEAF_SIZE
void *bd_malloc(uint64 nbytes) {
  int fk, k;

  acquire(&lock);

  // Find a free block >= nbytes, starting with smallest k possible
  fk = firstk(nbytes);
  for (k = fk; k < nsizes; k++) {
    if (!lst_empty(&bd_sizes[k].free)) break;
  }
  if (k >= nsizes) {  // No free blocks?
    release(&lock);
    return 0;
  }

  // Found a block; pop it and potentially split it.
  char *p = lst_pop(&bd_sizes[k].free);
  bit_flip(bd_sizes[k].alloc, blk_index(k, p) / 2);
  for (; k > fk; k--) {
    // split a block at size k and mark one half allocated at size k-1
    // and put the buddy on the free list at size k-1
    char *q = p + BLK_SIZE(k - 1);  // p's buddy
    bit_set(bd_sizes[k].split, blk_index(k, p));
    bit_flip(bd_sizes[k - 1].alloc, blk_index(k - 1, p) / 2);
    lst_push(&bd_sizes[k - 1].free, q);
  }
  release(&lock);

  return p;
}

// Find the size of the block that p points to.
int size(char *p) {
  for (int k = 0; k < nsizes; k++) {
    if (bit_isset(bd_sizes[k + 1].split, blk_index(k + 1, p))) {
      return k;
    }
  }
  return 0;
}

// Free memory pointed to by p, which was earlier allocated using
// bd_malloc.
void bd_free(void *p) {
  void *q;
  int k;

  acquire(&lock);
  for (k = size(p); k < MAXSIZE; k++) {
    int bi = blk_index(k, p);
    int buddy = (bi % 2 == 0) ? bi + 1 : bi - 1;
    bit_set(bd_sizes[k].alloc, bi / 2);          // free p at size k
    if (bit_isset(bd_sizes[k].alloc, bi / 2)) {  // is buddy allocated?
      break;                                     // break out of loop
    }
    // budy is free; merge with buddy
    q = addr(k, buddy);
    lst_remove(q);  // remove buddy from free list
    if (buddy % 2 == 0) {
      p = q;
    }
    // at size k+1, mark that the merged buddy pair isn't split
    // anymore
    bit_clear(bd_sizes[k + 1].split, blk_index(k + 1, p));
  }
  lst_push(&bd_sizes[k].free, p);
  release(&lock);
}

// Compute the first block at size k that doesn't contain p
int blk_index_next(int k, char *p) {
  int n = (p - (char *)bd_base) / BLK_SIZE(k);
  if ((p - (char *)bd_base) % BLK_SIZE(k) != 0) n++;
  return n;
}

int log2(uint64 n) {
  int k = 0;
  while (n > 1) {
    k++;
    n = n >> 1;
  }
  return k;
}

// Mark memory from [start, stop), starting at size 0, as allocated.
int bd_mark(void *start, void *stop, int is_left) {
  int bi, bj;
  int free_ret = 0;

  if (((uint64)start % LEAF_SIZE != 0) || ((uint64)stop % LEAF_SIZE != 0))
    panic("bd_mark");

  for (int k = 0; k < nsizes; k++) {
    bi = blk_index(k, start);
    bj = blk_index_next(k, stop);

    if (k < nsizes - 2) {
      if (is_left && bj % 2) {
        lst_push(&bd_sizes[k].free, addr(k, bj));
        free_ret += BLK_SIZE(k);
      }
      if (!is_left && bi % 2) {
        lst_push(&bd_sizes[k].free, addr(k, bi - 1));
        free_ret += BLK_SIZE(k);
      }
    }

    if (bi % 2 != 0) {
      bit_set(bd_sizes[k].alloc, bi / 2);
    }
    if (bj % 2 != 0) {
      bit_set(bd_sizes[k].alloc, bj / 2);
    }

    for (; bi < bj; bi++) {
      if (k > 0) {
        // if a block is allocated at size k, mark it as split too.
        bit_set(bd_sizes[k].split, bi);
        // bit_set(bd_sizes[k].split, bi + 1);
      }
      bit_flip(bd_sizes[k].alloc, bi / 2);
    }
  }
  return free_ret;
}

typedef struct bd_mark_meta {
  int meta;
  int free;
} MARK_META, *PMARK_META;

// Mark the range [bd_base,p) as allocated
void bd_mark_data_structures(char *p, PMARK_META meta) {
  meta->meta = p - (char *)bd_base;
  printf("bd: %d meta bytes for managing %d bytes of memory\n", meta->meta,
         BLK_SIZE(MAXSIZE));
  meta->free = bd_mark(bd_base, p, 1);
}

// Mark the range [end, HEAPSIZE) as allocated
void bd_mark_unavailable(void *end, void *left, PMARK_META meta) {
  meta->meta = BLK_SIZE(MAXSIZE) - (end - bd_base);
  if (meta->meta > 0) meta->meta = ROUNDUP(meta->meta, LEAF_SIZE);
  printf("bd: 0x%x bytes unavailable\n", meta->meta);

  void *bd_end = bd_base + BLK_SIZE(MAXSIZE) - meta->meta;
  meta->free = bd_mark(bd_end, bd_base + BLK_SIZE(MAXSIZE), 0);
}

// Initialize the buddy allocator: it manages memory from [base, end).
void bd_init(void *base, void *end) {
  char *p = (char *)ROUNDUP((uint64)base, LEAF_SIZE);
  int sz;

  initlock(&lock, "buddy");
  bd_base = (void *)p;

  // compute the number of sizes we need to manage [base, end)
  nsizes = log2(((char *)end - p) / LEAF_SIZE) + 1;
  if ((char *)end - p > BLK_SIZE(MAXSIZE)) {
    nsizes++;  // round up to the next power of 2
  }

  printf("bd: memory sz is %d bytes; allocate an size array of length %d\n",
         (char *)end - p, nsizes);

  // allocate bd_sizes array
  bd_sizes = (Sz_info *)p;
  p += sizeof(Sz_info) * nsizes;
  memset(bd_sizes, 0, sizeof(Sz_info) * nsizes);

  // initialize free list and allocate the alloc array for each size k
  for (int k = 0; k < nsizes; k++) {
    lst_init(&bd_sizes[k].free);
    sz = sizeof(char) * ROUNDUP(NBLK(k), 8) / 16;
    bd_sizes[k].alloc = p;
    memset(bd_sizes[k].alloc, 0, sz);
    p += sz;
  }

  // allocate the split array for each size k, except for k = 0, since
  // we will not split blocks of size k = 0, the smallest size.
  for (int k = 1; k < nsizes; k++) {
    sz = sizeof(char) * (ROUNDUP(NBLK(k), 8)) / 8;
    bd_sizes[k].split = p;
    memset(bd_sizes[k].split, 0, sz);
    p += sz;
  }
  p = (char *)ROUNDUP((uint64)p, LEAF_SIZE);

  // done allocating; mark the memory range [base, p) as allocated, so
  // that buddy will not hand out that memory.
  MARK_META m_left, m_right;
  bd_mark_data_structures(p, &m_left);

  // mark the unavailable memory range [end, HEAP_SIZE) as allocated,
  // so that buddy will not hand out that memory.
  bd_mark_unavailable(end, p, &m_right);

  int free = m_left.free + m_right.free;
  if (free != BLK_SIZE(MAXSIZE) - m_left.meta - m_right.meta) {
    printf("free %d %d\n", free,
           BLK_SIZE(MAXSIZE) - m_left.meta - m_right.meta);
    panic("bd_init: free mem");
  }
}