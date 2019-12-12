
#include "util.h"


/**********************
	Pairing heap
 **********************/


/* 
	Make heap2 the first child of heap1.
	Precondition: heap2->next == NULL
 */
rlnode* rheap_link(rlnode* heap1, rlnode* heap2);


/*
	Remove a node from the heap and take it into a heap of its own.
	Precondition: node is in the subtree of heap
	Precondition: heap != node
 */
void rheap_unlink(rlnode* node);


rlnode* rheap_init(rlnode* node) 
{
	/* Mark prev of node */
	node->prev = pointer_marked(node);
	node->next = NULL;
	return node;
}

/* h2 becomes the first child of h1 */
rlnode* rheap_link(rlnode* heap1, rlnode* heap2)
{
	assert(heap2->next == NULL);
	heap2->next = heap1->prev; 
	heap1->prev = heap2;
	return heap1;
}

size_t rheap_size(rlnode* heap)
{
	if(heap==NULL) return 0;
	size_t c = 1;
	for(rlnode* p=heap->prev; ! pointer_is_marked(p); p=p->next)
		c += rheap_size(p);
	return c;
}



void __rheap_unlink(rlnode* node, rlnode* parent)
{	
	assert(node != NULL);
	assert(parent != NULL);
	/* pointer-hop until we find pointer pointing to node */
	rlnode** ptr = &parent->prev;
	assert(! pointer_is_marked(*ptr));
	while( (*ptr) != node ) {
		ptr = & (*ptr)->next;
		assert(! pointer_is_marked(*ptr));
	}
	/* redirect pointer to node->next */
	*ptr = node->next;
	/* mark node as unlinked */
	node->next = NULL;
}

void rheap_unlink(rlnode* node)
{
	rlnode* parent = rheap_parent(node);
	__rheap_unlink(node, parent);
}


rlnode* rheap_meld(rlnode* heap1, rlnode* heap2, rlnode_less_func lessf)
{
	if(heap1 == NULL)
		return heap2;
	else if(heap2 == NULL)
		return heap1;

	assert(heap1!=NULL && heap2!=NULL);
	if(lessf(heap1, heap2)) 
		return rheap_link(heap1, heap2);
	else
		return rheap_link(heap2, heap1);
}


static inline rlnode* __rheap_merge_pairs(rlnode* hlist, rlnode_less_func lessf)
{
	if(hlist==NULL || hlist->next==NULL) return hlist;
	/* two or more nodes in the list ... */
	rlnode* h0 = hlist;
	rlnode* h1 = hlist->next;
	rlnode* hlist2 = h1->next;
	h0->next = h1->next = NULL;
	return rheap_meld( 
			rheap_meld(h0,h1, lessf),
			__rheap_merge_pairs(hlist2, lessf),
			lessf
		);
}

rlnode* rheap_delmin(rlnode* heap, rlnode_less_func lessf)
{
	assert(heap != NULL);

	/* 
		First, we need to make the list of children of heap a proper singly-linked list.
		To do this, we pointer-hop until we locate a marked pointer, and set it to NULL.
	 */
	rlnode **p;
	for(p = &heap->prev;  ! pointer_is_marked(*p); p=&(*p)->next) {};
	rlnode* heapm = *p;
	assert(pointer_unmarked(heapm)==heap);

	/* Make the child list NULL-terminated */
	*p = NULL;  

	/* Save it, this list will be pair-merged */
	rlnode* hlist = heap->prev;

	/* Reset heap node to a legal heap without any children */
	heap->prev = heapm;

	/* This is the most critical step (performance-wise). */
	return __rheap_merge_pairs(hlist, lessf);
}


rlnode* rheap_delete(rlnode* heap, rlnode* node, rlnode_less_func lessf)
{
	/* Base case: node==heap */
	if(node == heap) return rheap_delmin(heap, lessf);

	/* Unlink the node from the heap */
	rheap_unlink(node);

	/* Unlink the node from its children */
	rlnode* nh = rheap_delmin(node, lessf);

	/* Meld children and rest of heap */
	return rheap_meld(heap, nh, lessf);
}


rlnode* rheap_decrease(rlnode* heap, rlnode* node, rlnode_less_func lessf)
{
	if(node == heap) return heap;

	rlnode* parent = rheap_parent(node);
	if( !lessf(node, parent) ) 	return heap;

	/* Sorry, we must do some more work... */
	__rheap_unlink(node, parent);
	return rheap_meld(node, heap, lessf);	
}


rlnode* rheap_from_ring(rlnode* ring, rlnode_less_func lessf)
{
	/* First, things first... */
	if(ring == NULL) return NULL;
	if(ring == ring->next) return rheap_init(ring);

	/* We do this by hand... */
	ring->prev->next = NULL;
	for(rlnode* p = ring; p!=NULL; p=p->next) 
		p->prev = pointer_marked(p);

	return __rheap_merge_pairs(ring, lessf);
}


void __rheap_add_to_list(rlnode* heap, rlnode* L)
{
	rlnode* hmark = pointer_marked(heap);
	while(heap->prev != hmark) {
		rlnode* child = heap->prev;
		heap->prev = child->next;
		__rheap_add_to_list(child, L);
	}
	rlist_push_back(L, rlnode_new(heap));
}


rlnode* rheap_to_ring(rlnode* heap)
{
	if(heap==NULL) return NULL;

	rlnode L;  
	rlnode_new(&L);
	__rheap_add_to_list(heap, &L);

	assert(!is_rlist_empty(&L));
	return rl_splice(&L, L.prev);
}


/**********************
	Dict
 **********************/


static void rdict_init_buckets(rdict_bucket* buckets, unsigned long size)
{
	for(size_t i=0;i<size-1;i++) {
		buckets[i] = pointer_marked(buckets+i);
	}
	buckets[size-1] = NULL; /* sentinel */
}



void rdict_init(rdict* dict, unsigned long buckno)
{
	if(buckno < 16) buckno = 16;

	dict->size = 0;
	dict->bucketno = buckno-1;
	size_t bucket_bytes = buckno * sizeof(rlnode*);
	dict->buckets = (rlnode**)malloc(bucket_bytes);
	rdict_init_buckets(dict->buckets, buckno);
}



void rdict_destroy(rdict* dict)
{
	if(dict->bucketno>0) {
		rdict_clear(dict);
		free(dict->buckets);
		dict->bucketno = 0;
		dict->buckets = NULL;
	}
}


void rdict_clear(rdict* dict)
{
	for(unsigned long i = 0; i < dict->bucketno; i++) {
		while(! pointer_is_marked(dict->buckets[i])) {
			rlnode* elem = dict->buckets[i];
			dict->buckets[i] = elem->next;
			elem->next = elem;
		}
	}

	dict->size = 0;
}



rdict_iterator rdict_begin(rdict* dict)
{
	rdict_iterator iter = dict->buckets;
	for(size_t i=0; i<dict->bucketno; i++) if(!pointer_is_marked(*iter)) return iter;
	return NULL;
}


rdict_iterator rdict_next(rdict_iterator pos)
{
	assert(pos);  /* not null */
	assert(*pos != (*pos)->next); /* not in a removed element */

	if(pointer_is_marked(pos)) {
		while(1) {
			if(pointer_is_marked(*pos)) pos = 1+(rdict_bucket*)pointer_unmarked(*pos);
			else if(*pos == NULL) return NULL; else return pos;
		}
	}
	else
		return &(*pos)->next;
}
static inline void __rdict_size_changed(rdict* dict)
{

}

static inline void __rdict_bucket_insert(rdict_iterator pos, rlnode* elem)
{
	elem->next = *pos;
	*pos = elem;
}

static inline int __rdict_bucket_remove(rdict_iterator pos, rlnode* elem)
{
	for(; !pointer_is_marked(*pos); pos = &(*pos)->next)
		if(*pos == elem) {
			*pos = (*pos)->next;
			elem->next = elem;
			return 1;
		}
	return 0;
}


void rdict_bucket_insert(rdict* dict, rdict_iterator pos, rlnode* elem)
{
	__rdict_bucket_insert(pos, elem);
	dict->size++;
	__rdict_size_changed(dict);
}


rdict_iterator rdict_bucket_find(rdict_iterator pos, rlnode_key key, rdict_equal equalf)
{
	for( ; !rdict_bucket_end(pos); pos = & (*pos)->next ) {
		if(equalf(*pos, key)) return pos;
	}
	return pos;
}


int rdict_bucket_remove(rdict* dict, rdict_iterator pos, rlnode* elem)
{
	if(__rdict_bucket_remove(pos, elem)) {
		dict->size--;
		__rdict_size_changed(dict);
		return 1;
	} else 
		return 0;
}


void rdict_node_update(rlnode* elem, hash_value new_hash, rdict* dict)
{
	if(elem != elem->next) {
		/* size remains unchanged, avoid resizing the hash table */
		assert(dict != NULL);
		int rc = __rdict_bucket_remove(rdict_get_bucket(dict, elem->hash), elem);
		assert(rc);
		if(rc) __rdict_bucket_insert(rdict_get_bucket(dict, new_hash), elem);
	}

	elem->hash = new_hash;
}



/*
	From the C++ libaries
 */
#define NUM_DISTINCT_SIZES
static const size_t prime_hash_table_sizes[NUM_DISTINCT_SIZES] =
    {
      /* 0     */              5ul,
      /* 1     */              11ul, 
      /* 2     */              23ul, 
      /* 3     */              47ul, 
      /* 4     */              97ul, 
      /* 5     */              199ul, 
      /* 6     */              409ul, 
      /* 7     */              823ul, 
      /* 8     */              1741ul, 
      /* 9     */              3469ul, 
      /* 10    */              6949ul, 
      /* 11    */              14033ul, 
      /* 12    */              28411ul, 
      /* 13    */              57557ul, 
      /* 14    */              116731ul, 
      /* 15    */              236897ul,
      /* 16    */              480881ul, 
      /* 17    */              976369ul,
      /* 18    */              1982627ul, 
      /* 19    */              4026031ul,
      /* 20    */              8175383ul, 
      /* 21    */              16601593ul, 
      /* 22    */              33712729ul,
      /* 23    */              68460391ul, 
      /* 24    */              139022417ul, 
      /* 25    */              282312799ul, 
      /* 26    */              573292817ul, 
      /* 27    */              1164186217ul,
      /* 28    */              2364114217ul, 
      /* 29    */              4294967291ul,
      /* 30    */              8589934583ull,
      /* 31    */              17179869143ull,
      /* 32    */              34359738337ull,
      /* 33    */              68719476731ull,
      /* 34    */              137438953447ull,
      /* 35    */              274877906899ull,
      /* 36    */              549755813881ull,
      /* 37    */              1099511627689ull,
      /* 38    */              2199023255531ull,
      /* 39    */              4398046511093ull,
      /* 40    */              8796093022151ull,
      /* 41    */              17592186044399ull,
      /* 42    */              35184372088777ull,
      /* 43    */              70368744177643ull,
      /* 44    */              140737488355213ull,
      /* 45    */              281474976710597ull,
      /* 46    */              562949953421231ull, 
      /* 47    */              1125899906842597ull,
      /* 48    */              2251799813685119ull, 
      /* 49    */              4503599627370449ull,
      /* 50    */              9007199254740881ull, 
      /* 51    */              18014398509481951ull,
      /* 52    */              36028797018963913ull, 
      /* 53    */              72057594037927931ull,
      /* 54    */              144115188075855859ull,
      /* 55    */              288230376151711717ull,
      /* 56    */              576460752303423433ull,
      /* 57    */              1152921504606846883ull,
      /* 58    */              2305843009213693951ull,
      /* 59    */              4611686018427387847ull,
      /* 60    */              9223372036854775783ull,
      /* 61    */              18446744073709551557ull,
    };






/**********************
	Exceptions
 **********************/


void raise_exception(exception_context context)
{
	if(*context) {
		__atomic_signal_fence(__ATOMIC_SEQ_CST);
		longjmp((*context)->jbuf, 1);
	}
}


void exception_unwind(exception_context context, int errcode)
{
	/* Get the top frame */
	struct exception_stack_frame* frame = *context;

	/* handle exception */
	int captured = 0;

	/* First execute catchers one by one */
	while(frame->catchers) {
		captured = 1;
		struct exception_handler_frame *c = frame->catchers;
		/* Pop it from the list, just in case it throws() */
		frame->catchers = c->next;
		c->handler(errcode);
	}

	/* Execute finalizers one by one */
	while(frame->finalizers) {
		struct exception_handler_frame *fin = frame->finalizers;
		frame->finalizers = fin->next;

		fin->handler(errcode);
	}
 	
	/* pop this frame */
	*context = frame->next;

 	/* propagate */
	if(errcode && !captured) 
		raise_exception(context);
}

