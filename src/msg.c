/*
 * msg.c
 * Brian Bulkowski
 * Citrusleaf, 2008
 * This is a generic binary format message parsing system
 * You create the definition of the message not by an IDL, but by 
 * a .H file. Eventually we're going to need to do a similar thing using java,
 * though, which would promote an IDL-style approach
 * All rights reserved
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#include <arpa/inet.h> /// htonl, htons

#include "cf.h"

int 
msg_create(msg **m_r, const msg_desc *md, size_t md_sz, byte *stack_buf, size_t stack_buf_sz)
{
	// Figure out how many bytes you need
	int md_rows = md_sz / sizeof(msg_desc);
	cf_assert(md_rows > 0, CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_CRITICAL, "msg create: invalid parameter");
	unsigned int max_id = 0;
	for (int i=0;i<md_rows;i++) {
		if (md[i].id >= max_id) {
			max_id = md[i].id;
		}
	}
	max_id++;
	
	// DEBUG - can tell if it's so sparse that we're wasting lots of memory
	if (max_id > md_rows * 2) {
		// It would be nice if there was a human readable string for debugging
		// in the message descriptor
		D("msg_create: found sparse message, %d ids, only %d rows consider recoding",max_id,md_rows);
	}
	
	
	// allocate memory (if necessary)
	size_t m_sz = sizeof(msg_field) * max_id;
	msg *m;
	if ((stack_buf == 0) || (m_sz > stack_buf_sz)) {
		size_t a_sz = sizeof(msg) + m_sz;
		a_sz = ((a_sz / 512) + 1) + 512;
		m = malloc(a_sz);
		cf_assert(m, CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_CRITICAL, "malloc");
		m->len = max_id;
		m->bytes_used = m->bytes_alloc = a_sz;
		m->is_stack = false;
	} else {
		m = (msg *) stack_buf;
		m->len = max_id;
		m->bytes_used = sizeof(msg) + m_sz;
		m->bytes_alloc = stack_buf_sz;
		m->is_stack = true;
	}
	
	m->md = md;
	
	// debug - not strictly necessary, but saves the user if they
	// have an invalid field
	for (int i=0;i<max_id;i++)
		m->f[i].is_valid = false;
	
	// fill in the fields - rather minimalistic, save the dcache
	for (int i=0;i<md_rows;i++) {
		msg_field *f = &(m->f[ md[i].id ] );
		f->id = md[i].id;
		f->type = md[i].type;
		f->is_set = false;
		f->is_valid = true;
	}
	
	*m_r = m;
	
	return(0);
}

// THE MEAT!
// YOU'VE FOUND THE MEAT!
//
// Here's where the wire protocol is defined. All the rest is just copying
// stupid buffers around.
//
// Currently, there aren't a lot of alignment checks here. We assume we're
// running on an intel architecture where the chip does the fastest possible
// thing for us.
//
// Should probably have some kind of version field, because there are better
// ways of doing this on a number of levels
//
// Current protocol:
// uint32_t size-in-bytes (not including this header, network byte order)
//      2 byte - field id
// 		1 byte - field type
//      3 bytes - field size
//      [x] - field
//      (6 + field sz)

// htonll - an 8 byte swap
//   swaps in flight between two buffers
//   ASSUMES htonl(1) != 1 !!! FIXME!
//     (should be ';' if htonl(1) == 1) ;-)
//   ripe for assembly language here!!! this is a little freakin GNARLY!!!

#define htonll_p(__llp_dst, __llp_src) \
	*(uint32_t *) (__llp_dst) = htonl( *(uint32_t *) ( ((byte *)__llp_src) + 4)); \
	*(uint32_t *) (((byte *)__llp_dst) + 4) = htonl( *(uint32_t *) (__llp_src)); 

	


// msg_parse - parse a buffer into a message, which thus can be accessed
int 
msg_parse(msg *m, const byte *buf, const size_t buflen, bool copy)
{
	if (buflen < 4) {
		D("msg_parse: but not enough data! can't handle that yet.");
		return(-2);
	}
	uint32_t len = *(uint32_t *) buf;
	len = htonl(len) ;
	if (buflen < len + 4) {
		D("msg_parse: but not enough data! can't handle that yet.");
		return(-2);
	}
	buf += 4;

	const byte *eob = buf + len;
	
	while (buf < eob) {
		
		// Grab the ID
		uint32_t id = (buf[0] << 8) | buf[1];
		buf += 2;
		
		// find the field in the message
		msg_field *mf;
		if (id >= m->len) {
			D(" received message with id greater than current definition, kind of OK, ignoring field");
			mf = 0;
		}
		else {
			mf = &(m->f[id]);
			if (! mf->is_valid ) {
				D(" received message with id no longer valid, kind of OK, ignoring field");
				mf = 0;
			}
		}
		
		field_type ft = (field_type) *buf;
		uint32_t flen = (buf[1] << 16) | (buf[2] << 8) | (buf[3]);
		buf += 4;
		
		if (mf && (ft != mf->type)) {
			D(" received message with incorrect field type from definition, kind of OK, ignoring field");
			mf = 0;
		}
		
		if (mf) {
		
			switch (mf->type) {
				case M_FT_INT32:
					mf->u.i32 = htonl( *(uint32_t *) buf ); // problem is htonl really only works on unsigned?
					break;
				case M_FT_UINT32:
					mf->u.ui32 = htonl( *(uint32_t *) buf );
					break;
				case M_FT_INT64:
					htonll_p( &(mf->u.i64) , buf);
					break;
				case M_FT_UINT64:
					htonll_p( &(mf->u.i64) , buf);
					break;
				case M_FT_STR:
				case M_FT_BUF:
					mf->field_len = flen;
					if (copy) {
						mf->u.buf = malloc(flen);
						// TODO: add assert
						memcpy(mf->u.buf, buf, flen);
						mf->is_copy = true;
					}
					else {
						mf->u.buf = (byte *) buf; // compiler whinges here correctly, I bless this cast
						mf->is_copy = false;						
					}
					break;
				case M_FT_ARRAY:
				case M_FT_MESSAGE:
				default:
					D("msg_parse: field type not supported, but skipping over anyway: %d",mf->type);
			}
			mf->is_set = true;
		}
		buf += flen;
	};
	
	return(0);
}

//
int
msg_get_size(size_t *size_r, const byte *buf, const size_t buflen)
{
	// enuf bytes to tell yet?
	if (buflen < 4)
		return(-2);
	// grab from the buf, nevermind alignment (see note)
	size_t size = * (uint32_t *) buf;
	// !paws
	size = htonl(size);
	// size does not include this header
	size += 4;
	// bob's you're uncle
	*size_r = size;
	
	return( 0 );

}	


static inline size_t
msg_get_wire_field_size(const msg_field *mf) {
	
	switch (mf->type) {
		case M_FT_INT32:
		case M_FT_UINT32:
			return(4);
		case M_FT_INT64:
		case M_FT_UINT64:
			return(8);
		case M_FT_STR:
		case M_FT_BUF:
			if (mf->field_len >= ( 1 << 24 ))
				D("field length %d too long, not yet supported", mf->field_len);
			return(mf->field_len);
		case M_FT_ARRAY:
		case M_FT_MESSAGE:
		default:
			D("field type not supported, internal error: %d",mf->type);
	}
	return(0);
}

// returns the number of bytes written

static inline size_t
msg_stamp_field(byte *buf, const msg_field *mf)
{
	// Stamp the ID
	if (mf->id >= 1 << 16) {
		D("msg_stamp_field: ID too large!");
		return(0);
	}
	buf[0] = (mf->id >> 8) & 0xff;
	buf[1] = mf->id & 0xff;
	buf += 2;

	// stamp the type
	*buf++ = (field_type) mf->type;
	
	// Stamp the field itself (forward over the length, we'll patch that later
	size_t flen;
	buf += 3;
	switch(mf->type) {
		case M_FT_INT32:
			flen = 4;
			int32_t *b_i32 = (int32_t *)buf;
			*b_i32 = htonl(mf->u.i32);
			break;
			
		case M_FT_UINT32:
			flen = 4;
			uint32_t *b_ui32 = (uint32_t *)buf;
			*b_ui32 = htonl(mf->u.ui32);
			break;
			
		case M_FT_INT64:
			flen = 8;
			htonll_p(buf, (&mf->u.i64) );
			break;

		case M_FT_UINT64:
			flen = 8;
			htonll_p(buf, (&mf->u.ui64) );
			break;
			
		case M_FT_STR:
		case M_FT_BUF:
			flen = mf->field_len;
			memcpy(buf, mf->u.buf, flen); 
			break;
			
		case M_FT_ARRAY:
		case M_FT_MESSAGE:
		default:
			D("field type not supported, internal error: %d",mf->type);
			return(0);
	}
	
	// Now, patch the length back in
	buf[-3] = (flen >> 16) & 0xff;
	buf[-2] = (flen >> 8) & 0xff;
	buf[-1] = flen & 0xff;

	return(6 + flen);
}


// msg_tobuf - parse a message out into a buffer.
//    interesting point: for a sparse buffer, it's better to rip through
//    	the msg description and deference to the table, but for non-sparse
//    	it's kinder to the cache lines to just rip through the actual table
//    	we assume non-sparse at the moment

int 
msg_fillbuf(const msg *m, byte *buf, size_t *buflen)
{
	// debug!
	memset(buf, 0xff, *buflen);
	
	// Figure out the size
	size_t	sz = 4;
	
	for (int i=0;i<m->len;i++) {
		const msg_field *mf = &m->f[i];
		if (mf->is_valid && mf->is_set) {
			sz += 6 + msg_get_wire_field_size(mf);
		}
	}
	
	// validate the size
	if (sz > *buflen) {
		D("msg_fillbuf: passed in size too small");
		return(-2);
	}
	*buflen = sz;
	
	// stamp the size in the buf
	(* (uint32_t *) buf) = htonl(sz - 4);
	buf += 4;
	
	// copy the fields
	for (int i=0;i<m->len;i++) {
		const msg_field *mf = &m->f[i];
		if (mf->is_valid && mf->is_set) {
			buf += msg_stamp_field(buf, mf);
		}		
	}
	
	return(0);
}
	

// Getters and setters
int 
msg_get_uint32(const msg *m, int field_id, uint32_t *r)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field get",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	if ( m->f[field_id].type != M_FT_UINT32 ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch getter field type wants %d has %d",m->f[field_id].type, M_FT_UINT32);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	if ( ! m->f[field_id].is_set ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_NOTICE, "msg: attempt to retrieve unset field %d",field_id);
		return(-2);
	}

	*r = m->f[field_id].u.ui32;
	
	return(0);
}

int msg_get_int32(const msg *m, int field_id, int32_t *r)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field get",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	if ( m->f[field_id].type != M_FT_INT32 ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch getter field type wants %d has %d",m->f[field_id].type, M_FT_INT32);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	if ( ! m->f[field_id].is_set ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_NOTICE, "msg: attempt to retrieve unset field %d",field_id);
		return(-2);
	}
	
	*r = m->f[field_id].u.i32;
	
	return(0);
}

int msg_get_uint64(const msg *m, int field_id, uint64_t *r)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field get",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	if ( m->f[field_id].type != M_FT_UINT64 ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch getter field type wants %d has %d",m->f[field_id].type, M_FT_UINT64);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	if ( ! m->f[field_id].is_set ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_NOTICE, "msg: attempt to retrieve unset field %d",field_id);
		return(-2);
	}
	
	*r = m->f[field_id].u.ui64;
	
	return(0);
}

int msg_get_int64(const msg *m, int field_id, int64_t *r)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field get",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	if ( m->f[field_id].type != M_FT_INT64 ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch getter field type wants %d has %d",m->f[field_id].type, M_FT_INT64);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	if ( ! m->f[field_id].is_set ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_NOTICE, "msg: attempt to retrieve unset field %d",field_id);
		return(-2);
	}
	
	*r = m->f[field_id].u.i64;
	
	return(0);
}


int 
msg_get_str(const msg *m, int field_id, char **r, size_t *len, bool copy)  // this length is strlen+1, the allocated size
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field get",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	if ( m->f[field_id].type != M_FT_STR ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch getter field type wants %d has %d",m->f[field_id].type, M_FT_STR);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	if ( ! m->f[field_id].is_set ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_NOTICE, "msg: attempt to retrieve unset field %d",field_id);
		return(-2);
	}
	
	if (copy) {
		*r = strdup( m->f[field_id].u.str );
		cf_assert(*r, CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "malloc");
	}
	else
		*r = m->f[field_id].u.str;

	*len = m->f[field_id].field_len;
	
	return(0);
}

int 
msg_get_buf(const msg *m, int field_id, byte **r, size_t *len, bool copy)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field get",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	if ( m->f[field_id].type != M_FT_BUF ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch getter field type wants %d has %d",m->f[field_id].type, M_FT_BUF);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	if ( ! m->f[field_id].is_set ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_NOTICE, "msg: attempt to retrieve unset field %d",field_id);
		return(-2);
	}
	
	if (copy) {
		*r = malloc( m->f[field_id].field_len );
		cf_assert(*r, CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "malloc");
		memcpy(*r, m->f[field_id].u.buf, m->f[field_id].field_len );
	}
	else
		*r = m->f[field_id].u.buf;

	*len = m->f[field_id].field_len;
	
	return(0);
}

int 
msg_set_uint32(msg *m, int field_id, const uint32_t v)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field set",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	if ( m->f[field_id].type != M_FT_UINT32 ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch setter field type wants %d has %d",m->f[field_id].type, M_FT_UINT32);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	m->f[field_id].is_set = true;
	m->f[field_id].u.ui32 = v;
	
	return(0);
}

int 
msg_set_int32(msg *m, int field_id, const int32_t v)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field set",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	if ( m->f[field_id].type != M_FT_INT32 ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch setter field type wants %d has %d",m->f[field_id].type, M_FT_INT32);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	m->f[field_id].is_set = true;
	m->f[field_id].u.i32 = v;
	
	return(0);
}

int 
msg_set_uint64(msg *m, int field_id, const uint64_t v)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field set",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	if ( m->f[field_id].type != M_FT_UINT64 ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch setter field type wants %d has %d",m->f[field_id].type, M_FT_UINT64);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	m->f[field_id].is_set = true;
	m->f[field_id].u.ui64 = v;
	
	return(0);
}

int 
msg_set_int64(msg *m, int field_id, const int64_t v)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field set",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	if ( m->f[field_id].type != M_FT_INT64 ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch setter field type wants %d has %d",m->f[field_id].type, M_FT_INT64);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	m->f[field_id].is_set = true;
	m->f[field_id].u.i64 = v;
	
	return(0);
}

int 
msg_set_str(msg *m, int field_id, const char *v, bool copy)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field set",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	if ( m->f[field_id].type != M_FT_STR ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch setter field type wants %d has %d",m->f[field_id].type, M_FT_STR);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	msg_field *mf = &(m->f[field_id]);
	
	// free auld value if necessary
	if (mf->is_set && mf->is_copy) {
		free(mf->u.str);
		mf->u.str = 0;
	}
	
	mf->field_len = strlen(v)+1;
	
	if (copy) {
		size_t len = mf->field_len;
		// If we've got a little extra memory here already, use it
		if (m->bytes_alloc - m->bytes_used >= len) {
			mf->u.str = (char *) (((byte *)m) + m->bytes_used);
			m->bytes_alloc += len;
			mf->is_copy = false;
			memcpy(mf->u.str, v, len);
		}
		else {
			mf->u.str = strdup(v);
			cf_assert(mf->u.str, CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_CRITICAL, "malloc");
			mf->is_copy = true;
		}
	} else {
		mf->u.str = (char *)v; // compiler winges here, correctly, but I bless this -b
		mf->is_copy = false;
	}
		
	mf->is_set = true;
	
	return(0);
}


int msg_set_buf(msg *m, int field_id, const byte *v, size_t len, bool copy)
{
	if (! m->f[field_id].is_valid) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: invalid id %d in field set",field_id);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}
	
	if ( m->f[field_id].type != M_FT_STR ) {
		cf_fault(CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_ERROR, "msg: mismatch setter field type wants %d has %d",m->f[field_id].type, M_FT_STR);
		return(-1); // not sure the meaning of ERROR - will it throw or not?
	}

	msg_field *mf = &(m->f[field_id]);
	
	// free auld value if necessary
	if (mf->is_set && mf->is_copy) {
		free(mf->u.buf);
		mf->u.buf = 0;
	}
	
	mf->field_len = len;
	
	if (copy) {
		// If we've got a little extra memory here already, use it
		if (m->bytes_alloc - m->bytes_used >= len) {
			mf->u.buf = ((byte *)m) + m->bytes_used;
			m->bytes_alloc += len;
			mf->is_copy = false;
		}
		// Or just malloc if we have to. Sad face.
		else {
			mf->u.buf = malloc(len);
			cf_assert(mf->u.buf, CF_FAULT_SCOPE_THREAD, CF_FAULT_SEVERITY_CRITICAL, "malloc");
			mf->is_copy = true;
		}

		memcpy(mf->u.buf, v, len);

	} else {
		mf->u.str = (void *)v; // compiler winges here, correctly, but I bless this -b
		mf->is_copy = false;
	}
		
	mf->is_set = true;
	
	return(0);
}

// very useful for test code! one can encode and decode messages and see if
// they're the same!

int 
msg_compare(const msg *m1, const msg *m2) {
	D("msg_compare: stub");
	return(-2);
}

// And, finally, the destruction of a message
void msg_destroy(msg *m) 
{
	for (int i=0;i<m->len;i++) {
		if (m->f[i].is_valid && m->f[i].is_set && m->f[i].is_copy)
			free(m->f[i].u.buf);
	}
		
	if (! m->is_stack)
		free(m);

	return;
}
